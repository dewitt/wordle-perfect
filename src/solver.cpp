#include "solver.hpp"

#include <algorithm>
#include <cmath>
#include <future>
#include <thread>
#include <vector>

namespace wp {

// ---------------------------------------------------------------------------
// PatternMatrix::build
// ---------------------------------------------------------------------------
PatternMatrix PatternMatrix::build(const WordList& words, unsigned nthreads) {
    const std::size_t n = words.size();

    if (nthreads == 0)
        nthreads = std::max(1u, std::thread::hardware_concurrency());

    PatternMatrix pm;
    pm.n_ = n;
    pm.data_.resize(n * n);

    // Split rows (guess indices) across threads
    std::vector<std::thread> threads;
    threads.reserve(nthreads);

    const auto rows_per_thread = (n + nthreads - 1) / nthreads;
    for (unsigned t = 0; t < nthreads; ++t) {
        const auto row_start = t * rows_per_thread;
        const auto row_end   = std::min(row_start + rows_per_thread, n);
        if (row_start >= n) break;

        threads.emplace_back([&pm, &words, n, row_start, row_end] {
            for (std::size_t gi = row_start; gi < row_end; ++gi) {
                auto gv = words[static_cast<uint16_t>(gi)].view();
                for (std::size_t ai = 0; ai < n; ++ai) {
                    pm.data_[gi * n + ai] =
                        compute_pattern(gv, words[static_cast<uint16_t>(ai)].view());
                }
            }
        });
    }
    for (auto& th : threads) th.join();

    return pm;
}

// ---------------------------------------------------------------------------
// EntropySolver::partition
// ---------------------------------------------------------------------------
std::array<std::vector<uint16_t>, PATTERN_COUNT>
EntropySolver::partition(std::span<const uint16_t> candidates,
                         uint16_t                  guess_idx,
                         const PatternMatrix&      pm) {
    std::array<std::vector<uint16_t>, PATTERN_COUNT> buckets;
    for (uint16_t ai : candidates) {
        buckets[pm.get(guess_idx, ai)].push_back(ai);
    }
    return buckets;
}

// ---------------------------------------------------------------------------
// EntropySolver::entropy (private) — weighted Shannon entropy
// ---------------------------------------------------------------------------
double EntropySolver::entropy(std::span<const uint16_t> candidates,
                              uint16_t                  guess_idx,
                              const PatternMatrix&      pm) const noexcept {
    std::array<double, PATTERN_COUNT> bucket_weight{};
    double total_weight = 0.0;

    for (uint16_t ai : candidates) {
        double w = weight_of(ai);
        bucket_weight[pm.get(guess_idx, ai)] += w;
        total_weight += w;
    }

    if (total_weight == 0.0) return 0.0;

    double H = 0.0;
    for (double bw : bucket_weight) {
        if (bw > 0.0) {
            double p = bw / total_weight;
            H -= p * std::log2(p);
        }
    }
    return H;
}

// ---------------------------------------------------------------------------
// EntropySolver::best_guess
// ---------------------------------------------------------------------------
uint16_t EntropySolver::best_guess(std::span<const uint16_t> candidates,
                                   bool                      restrict_to_candidates) const {
    if (candidates.empty()) return WordList::NPOS;
    if (candidates.size() == 1) return candidates[0];

    // If only 2 candidates remain, guess one of them directly
    if (candidates.size() == 2) return candidates[0];

    // Determine which pool of words to search
    std::vector<uint16_t> all_idx;
    std::span<const uint16_t> guess_pool;

    if (restrict_to_candidates) {
        guess_pool = candidates;
    } else {
        all_idx = words_.all_indices();
        guess_pool = all_idx;
    }

    double   best_H            = -1.0;
    uint16_t best_word         = candidates[0];  // fallback
    // Track best_word's candidacy as state instead of re-running binary_search
    // on it every iteration (issue #15); only recomputed when best_word changes.
    bool     best_is_candidate = std::ranges::binary_search(candidates, best_word);

    // Prefer guesses that are themselves still candidates (breaks ties toward
    // an answer rather than a pure information guess)
    for (uint16_t gi : guess_pool) {
        double H = entropy(candidates, gi, patterns_);

        // Tie-break: prefer a candidate over a non-candidate for equal entropy
        bool gi_is_candidate = std::ranges::binary_search(candidates, gi);

        if (H > best_H ||
            (H == best_H && gi_is_candidate && !best_is_candidate) ||
            (H == best_H && gi_is_candidate == best_is_candidate && gi < best_word)) {
            best_H            = H;
            best_word         = gi;
            best_is_candidate = gi_is_candidate;
        }
    }
    return best_word;
}

// ---------------------------------------------------------------------------
// EntropySolver::best_guess_parallel
//
// Same result and tie-breaking as best_guess() over the full vocabulary, but
// the guess pool is split across worker threads. Each worker finds its local
// best (highest entropy; ties → candidate over non-candidate, then lower index)
// and the merge applies the identical rule across workers.
// ---------------------------------------------------------------------------
uint16_t EntropySolver::best_guess_parallel(std::span<const uint16_t> candidates,
                                            unsigned nthreads) const {
    if (candidates.empty()) return WordList::NPOS;
    if (candidates.size() == 1) return candidates[0];
    if (candidates.size() == 2) return candidates[0];
    if (nthreads == 0) nthreads = 1;

    const std::size_t total = words_.size();
    const auto chunk = (total + nthreads - 1) / nthreads;

    struct LocalBest { double H; uint16_t idx; bool is_candidate; };

    auto is_cand = [&](uint16_t gi) {
        return std::ranges::binary_search(candidates, gi);
    };
    auto better = [](double H, uint16_t idx, bool is_c,
                     double bH, uint16_t bIdx, bool bC) {
        return H > bH ||
               (H == bH && is_c && !bC) ||
               (H == bH && is_c == bC && idx < bIdx);
    };

    std::vector<std::future<LocalBest>> futures;
    futures.reserve(nthreads);

    for (unsigned t = 0; t < nthreads; ++t) {
        const auto start = static_cast<uint16_t>(t * chunk);
        if (start >= total) break;
        const auto end = static_cast<uint16_t>(
            std::min(static_cast<std::size_t>(start) + chunk, total));

        futures.push_back(std::async(std::launch::async,
            [this, candidates, start, end, &is_cand, &better]() -> LocalBest {
                double   best_H    = -1.0;
                uint16_t best_word = start;
                bool     best_is_c = false;
                for (uint16_t gi = start; gi < end; ++gi) {
                    double H = entropy(candidates, gi, patterns_);
                    bool   c = is_cand(gi);
                    if (better(H, gi, c, best_H, best_word, best_is_c)) {
                        best_H = H; best_word = gi; best_is_c = c;
                    }
                }
                return {best_H, best_word, best_is_c};
            }));
    }

    double   global_H = -1.0;
    uint16_t global_word = candidates[0];
    bool     global_is_c = false;
    for (auto& f : futures) {
        auto [H, idx, c] = f.get();
        if (better(H, idx, c, global_H, global_word, global_is_c)) {
            global_H = H; global_word = idx; global_is_c = c;
        }
    }
    return global_word;
}

// ---------------------------------------------------------------------------
// EntropySolver::minimax_best_guess / minimax_inner
//
// Public entry point: seed the upper bound with the entropy guess result,
// then call the recursive core. Tries candidates first (they can win
// immediately), then non-candidates.
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// EntropySolver::greedy_worst_depth
// Fast greedy evaluation: follows entropy guesses without branching.
// O(depth × candidates) per call. Used to warmstart minimax alpha-beta.
// ---------------------------------------------------------------------------
int EntropySolver::greedy_worst_depth(std::span<const uint16_t> candidates,
                                      int depth_budget,
                                      int upper_bound) const noexcept {
    if (candidates.empty()) return 0;
    if (candidates.size() == 1) return 1;
    if (depth_budget <= 0)      return DEPTH_IMPOSSIBLE;
    if (upper_bound <= 1)       return DEPTH_IMPOSSIBLE;  // can't beat it

    uint16_t guess   = best_guess(candidates);
    auto     buckets = partition(candidates, guess, patterns_);

    int worst = 0;
    for (Pattern p = 0; p < PATTERN_COUNT - 1; ++p) {
        if (buckets[p].empty()) continue;
        // Child only needs to be probed up to (upper_bound - 1): if its depth
        // would make 1 + d reach upper_bound, this guess is already no better.
        int d = greedy_worst_depth(buckets[p], depth_budget - 1, upper_bound - 1);
        if (d == DEPTH_IMPOSSIBLE) return d;
        worst = std::max(worst, 1 + d);
        if (worst >= upper_bound) return DEPTH_IMPOSSIBLE;  // prune
    }
    return worst;
}

// ---------------------------------------------------------------------------
// EntropySolver::minimax_best_guess
// Public entry: warmstart upper_bound from greedy evaluation, then search.
// ---------------------------------------------------------------------------
std::pair<uint16_t, int>
EntropySolver::minimax_best_guess(std::span<const uint16_t> candidates,
                                  int depth_budget) const {
    if (candidates.empty()) return {WordList::NPOS, 0};
    if (candidates.size() == 1) return {candidates[0], 1};

    // Warmstart: evaluate the greedy path to get a tight initial upper bound.
    // This allows alpha-beta to prune most branches immediately, cutting the
    // search from exponential to near-linear in practice.
    const int seed_depth = greedy_worst_depth(candidates, depth_budget);

    // If greedy already achieves depth ≤ 2 or the budget is blown, skip the
    // full minimax search. No guess can improve on depth 2 for 2+ candidates
    // (you need at least 1 guess to distinguish + 1 more to answer = 2).
    if (seed_depth <= 2 || seed_depth == DEPTH_IMPOSSIBLE)
        return {best_guess(candidates), seed_depth};

    // Search for strictly better than greedy. If nothing improves, the caller
    // falls back to the entropy guess via the NPOS check in build_db.
    return minimax_inner(candidates, depth_budget, seed_depth);
}

// ---------------------------------------------------------------------------
// EntropySolver::best_guess_within_budget
//
// Greedy-first, escalate-on-budget-miss. See header for the rationale.
// ---------------------------------------------------------------------------
uint16_t
EntropySolver::best_guess_within_budget(std::span<const uint16_t> candidates,
                                        int budget,
                                        bool* escalated,
                                        std::size_t escalate_max_candidates,
                                        std::size_t beam_width) const {
    if (escalated) *escalated = false;
    if (candidates.empty()) return WordList::NPOS;
    if (candidates.size() == 1) return candidates[0];
    if (candidates.size() == 2) return candidates[0];

    const uint16_t greedy = best_guess(candidates);

    // Cheap pre-check: how deep does the greedy choice push the worst case? If
    // it already fits the budget we're done. This probe calls best_guess at
    // each level (O(N·K)), so for very large sets we lean on the beam path's
    // own pruning instead of probing first.
    const int greedy_depth = greedy_worst_depth(candidates, budget);
    if (greedy_depth != DEPTH_IMPOSSIBLE && greedy_depth <= budget)
        return greedy;

    // Greedy blows the budget. Escalate.
    if (escalated) *escalated = true;

    if (candidates.size() <= escalate_max_candidates) {
        // Small set: full minimax can find the provably-shallowest guess.
        auto [mm_gi, mm_depth] = minimax_best_guess(candidates, budget);
        if (mm_gi != WordList::NPOS && mm_depth <= budget)
            return mm_gi;
        return greedy;  // unavoidable at this depth
    }

    // Large set: full minimax is intractable (its top level iterates the whole
    // vocabulary). Use the beam re-search — probe the top entropy guesses with
    // the cheap greedy worst-depth evaluator. This is what lets us attack the
    // repeated-letter trap clusters created high in the tree (issue #8).
    auto [beam_gi, beam_depth] = best_guess_beam(candidates, budget, beam_width);
    if (beam_gi != WordList::NPOS && beam_depth < greedy_depth)
        return beam_gi;   // beam improved on greedy
    return greedy;
}

// ---------------------------------------------------------------------------
// EntropySolver::best_guess_beam
//
// Rank all guesses by entropy, take the top `beam_width`, and pick the one
// whose greedy continuation has the smallest worst-case depth. Tractable for
// large candidate sets because only `beam_width` greedy probes are run.
// ---------------------------------------------------------------------------
std::pair<uint16_t, int>
EntropySolver::best_guess_beam(std::span<const uint16_t> candidates,
                               int budget,
                               std::size_t beam_width) const {
    if (candidates.empty()) return {WordList::NPOS, 0};
    if (candidates.size() == 1) return {candidates[0], 1};
    if (candidates.size() == 2) return {candidates[0], 2};

    // Rank every guess in the vocabulary by entropy over the candidate set.
    const std::size_t n = words_.size();
    std::vector<std::pair<double, uint16_t>> scored;
    scored.reserve(n);
    for (std::size_t gi = 0; gi < n; ++gi) {
        const auto gi16 = static_cast<uint16_t>(gi);
        scored.emplace_back(entropy(candidates, gi16, patterns_), gi16);
    }

    const std::size_t k = std::min(beam_width, scored.size());
    // Partial sort: highest entropy first; tie-break lexicographically (lower
    // index) to preserve reproducibility (spec tie_breaking_for_equal_paths).
    std::ranges::partial_sort(
        scored, scored.begin() + static_cast<std::ptrdiff_t>(k),
        [](const auto& a, const auto& b) {
            if (a.first != b.first) return a.first > b.first;
            return a.second < b.second;
        });

    // Probe each beam member's greedy worst-case depth; keep the shallowest.
    // Seed with the plain greedy guess so we never regress.
    const uint16_t greedy = best_guess(candidates);
    int      best_depth = greedy_worst_depth(candidates, budget);
    uint16_t best_gi    = greedy;

    for (std::size_t i = 0; i < k; ++i) {
        const uint16_t gi = scored[i].second;
        auto buckets = partition(candidates, gi, patterns_);

        int worst = 0;
        bool feasible = true;
        // Each child only needs probing deep enough to (possibly) beat the
        // current best: cap the child's worst-depth at best_depth - 1.
        const int child_cap = (best_depth == DEPTH_IMPOSSIBLE)
                            ? budget : best_depth - 1;
        for (Pattern p = 0; p < PATTERN_COUNT - 1; ++p) {
            if (buckets[p].empty()) continue;
            int d = greedy_worst_depth(buckets[p], budget - 1, child_cap);
            if (d == DEPTH_IMPOSSIBLE) { feasible = false; break; }
            worst = std::max(worst, 1 + d);
            if (best_depth != DEPTH_IMPOSSIBLE && worst >= best_depth) {
                feasible = false;  // can't beat current best; prune
                break;
            }
        }
        if (!feasible) continue;

        if (best_depth == DEPTH_IMPOSSIBLE || worst < best_depth ||
            (worst == best_depth && gi < best_gi)) {
            best_depth = worst;
            best_gi    = gi;
        }
    }

    return {best_gi, best_depth};
}

// Recursive minimax: returns {best_guess_idx, worst_depth} where worst_depth
// is the maximum number of guesses (including this one) needed over all
// candidates. Prunes branches whose worst-depth ≥ upper_bound.
//
// restrict_to_candidates: when true, skip the non-candidate guess pool.
// Sub-calls always pass true, keeping recursive cost O(K^depth) rather than
// O(N^depth). The top-level call (from minimax_best_guess) passes false so
// that non-candidate "bridge" guesses can still be found when they help.
std::pair<uint16_t, int>
EntropySolver::minimax_inner(std::span<const uint16_t> candidates,
                             int depth_budget,
                             int upper_bound,
                             bool restrict_to_candidates) const {
    if (candidates.empty()) return {WordList::NPOS, 0};
    if (candidates.size() == 1) return {candidates[0], 1};
    if (depth_budget <= 0)      return {WordList::NPOS, DEPTH_IMPOSSIBLE};

    const std::size_t n = words_.size();

    // Early exit: no guess can achieve depth ≤ 1 for 2+ candidates.
    // Avoid constructing any eval_guess when the target is impossible.
    if (upper_bound <= 2) return {WordList::NPOS, upper_bound};

    uint16_t best_gi    = WordList::NPOS;
    int      best_worst = upper_bound;  // we only want to beat this

    // Helper: evaluate one guess against the candidate set.
    //
    // Key optimisation: instead of calling partition() (which allocates 243
    // std::vector objects on the heap), use a count array to classify buckets.
    // Only when we need to recurse into a multi-candidate bucket do we build
    // that bucket on the fly by re-scanning candidates.  This eliminates the
    // dominant heap-allocation cost (243 vectors × 14,855 guesses per node).
    //
    // Sub-calls always use restrict_to_candidates=true so the inner search
    // stays within the sub-bucket, keeping cost O(K^depth) rather than
    // O(N^depth).
    auto eval_guess = [&](uint16_t gi, int prune_at) -> int {
        // Fast-path: for prune_at ≤ 2, any non-empty non-GGGGG bucket with
        // ≥ 2 candidates gives 1 + 2 = 3 > 2, which exceeds prune_at.
        // But even singleton non-GGGGG buckets give 1 + 1 = 2 ≥ prune_at = 2.
        // So all guesses fail. (Caught by the early exit above, but guard here
        // for recursive calls where prune_at ≤ 2.)
        if (prune_at <= 2) return DEPTH_IMPOSSIBLE;

        // First pass: count how many candidates fall in each pattern bucket.
        // uint16_t (not uint8_t): although the only production caller gates
        // minimax to candidate sets ≤ MINIMAX_THRESHOLD, this is public API and
        // the top-level counts array sees the full candidate set. uint16_t
        // cannot overflow for any list WordList::load accepts (≤ 65,535 words).
        std::array<uint16_t, PATTERN_COUNT> counts{};
        for (uint16_t ai : candidates) {
            ++counts[patterns_.get(gi, ai)];
        }

        int worst = 0;
        for (Pattern p = 0; p < PATTERN_COUNT - 1; ++p) {  // skip GGGGG
            const auto cnt = counts[p];
            if (cnt == 0) continue;

            if (depth_budget - 1 <= 0) return DEPTH_IMPOSSIBLE;

            int sub;
            if (cnt == 1) {
                // Singleton: minimax_inner always returns 1 for size==1.
                sub = 1;
            } else {
                // Multi-candidate: build sub-bucket on demand and recurse.
                // Only reached when prune_at > 2 (ensured by check above),
                // so the sub-call has prune_at-1 ≥ 2, and itself uses the
                // count optimisation recursively.
                if (prune_at - 1 <= 2) {
                    // 1 + sub ≥ 1 + 2 = 3 ≥ prune_at (since prune_at == 3)
                    // → prune immediately without building sub-bucket.
                    return DEPTH_IMPOSSIBLE;
                }
                std::vector<uint16_t> sub_bucket;
                sub_bucket.reserve(cnt);
                for (uint16_t ai : candidates) {
                    if (patterns_.get(gi, ai) == p) sub_bucket.push_back(ai);
                }
                // Sub-calls restrict to candidates only: keeps cost O(K^depth)
                // rather than O(N^depth) for the recursive search.
                auto [_, s] = minimax_inner(sub_bucket, depth_budget - 1,
                                            prune_at - 1, /*restrict=*/true);
                sub = s;
            }

            if (sub == DEPTH_IMPOSSIBLE)
                return DEPTH_IMPOSSIBLE;

            worst = std::max(worst, 1 + sub);
            if (worst >= prune_at) return DEPTH_IMPOSSIBLE; // prune
        }
        return worst;
    };

    // Try candidates first (they can win immediately if they're the answer).
    bool best_is_candidate = false;

    for (uint16_t gi : candidates) {
        int w = eval_guess(gi, best_worst);
        if (w == DEPTH_IMPOSSIBLE) continue;

        bool better = w < best_worst ||
                      (w == best_worst && !best_is_candidate) ||
                      (w == best_worst &&  best_is_candidate && gi < best_gi);
        if (better) {
            best_worst       = w;
            best_gi          = gi;
            best_is_candidate = true;
        }
        if (best_worst == 1) return {best_gi, 1}; // can't do better
    }

    // Then try non-candidates (skip when restrict_to_candidates is set,
    // e.g. in sub-calls where the cost would be O(N^depth)).
    if (!restrict_to_candidates) {
        // Use std::size_t to avoid uint16_t overflow if word list ever grows
        // beyond 65,535. Safe cast to uint16_t at each use since WordList::load
        // rejects lists larger than UINT16_MAX.
        for (std::size_t gi = 0; gi < n; ++gi) {
            const auto gi16 = static_cast<uint16_t>(gi);
            if (std::ranges::binary_search(candidates, gi16)) continue; // already tried

            int w = eval_guess(gi16, best_worst);
            if (w == DEPTH_IMPOSSIBLE) continue;

            bool better = w < best_worst ||
                          (w == best_worst && !best_is_candidate && gi16 < best_gi);
            if (better) {
                best_worst        = w;
                best_gi           = gi16;
                best_is_candidate = false;
            }
            if (best_worst == 1) return {best_gi, 1};
        }
    }

    return {best_gi, best_worst};
}

// ---------------------------------------------------------------------------
// EntropySolver::solve
// ---------------------------------------------------------------------------
SolveResult EntropySolver::solve(uint16_t answer_idx, int max_rounds) const {
    SolveResult result;
    std::vector<uint16_t> candidates = words_.all_indices();

    for (int round = 0; round < max_rounds; ++round) {
        uint16_t guess_idx = best_guess(candidates);
        if (guess_idx == WordList::NPOS) break;

        Pattern p = patterns_.get(guess_idx, answer_idx);
        result.steps.push_back({guess_idx, p});

        if (p == PATTERN_SOLVED) {
            result.solved = true;
            break;
        }

        // Filter candidates to those consistent with this guess+response
        auto buckets = partition(candidates, guess_idx, patterns_);
        candidates   = std::move(buckets[p]);
    }

    return result;
}

// ---------------------------------------------------------------------------
// Optimal (worst-case-bounded) tree construction: is_feasible / best_guess_feasible
// ---------------------------------------------------------------------------
namespace {
std::uint64_t feas_hash(std::span<const uint16_t> s, int depth) {
    std::uint64_t h = 1469598103934665603ULL;
    auto mix = [&](std::uint64_t x){ h ^= x; h *= 1099511628211ULL; };
    for (uint16_t v : s) mix(v + 1u);
    mix(0x5A17u);
    mix(static_cast<std::uint64_t>(depth));
    return h;
}
}  // namespace

double EntropySolver::entropy_simple(std::span<const uint16_t> candidates,
                                     uint16_t guess_idx) const noexcept {
    std::array<int, PATTERN_COUNT> cnt{};
    for (uint16_t ai : candidates) ++cnt[patterns_.get(guess_idx, ai)];
    const double tot = static_cast<double>(candidates.size());
    double H = 0.0;
    for (int c : cnt) if (c) { double p = c / tot; H -= p * std::log2(p); }
    return H;
}

bool EntropySolver::is_feasible(std::span<const uint16_t> candidates, int depth,
                                uint16_t* witness) const {
    const int n = static_cast<int>(candidates.size());
    if (n <= 1) { if (witness && n == 1) *witness = candidates[0]; return true; }
    if (depth <= 1) return false;
    // Admissible bound: with one guess (243 patterns), depth==2 can distinguish
    // at most 243 words into singleton buckets.
    if (depth == 2 && n > PATTERN_COUNT) return false;

    const std::uint64_t key = feas_hash(candidates, depth);
    if (auto it = feas_memo_.find(key); it != feas_memo_.end()) {
        if (it->second == 1 && witness) {
            if (auto w = feas_witness_.find(key); w != feas_witness_.end())
                *witness = w->second;
        }
        return it->second == 1;
    }

    // Order guesses by max-bucket ascending (best splitters first → prune hard).
    const std::size_t W = words_.size();
    std::vector<std::pair<int, uint16_t>> order;
    order.reserve(W);
    for (std::size_t g = 0; g < W; ++g) {
        const auto gi = static_cast<uint16_t>(g);
        std::array<uint16_t, PATTERN_COUNT> cnt{};
        int mb = 0;
        for (uint16_t ai : candidates) { int c = ++cnt[patterns_.get(gi, ai)]; if (c > mb) mb = c; }
        if (mb == n) continue;  // no progress
        order.emplace_back(mb, gi);
    }
    std::ranges::sort(order, [](auto& a, auto& b){
        if (a.first != b.first) return a.first < b.first;
        return a.second < b.second;
    });

    bool ok = false;
    uint16_t chosen = WordList::NPOS;
    for (auto& [mb, gi] : order) {
        if (depth - 1 == 1 && mb > 1) break;  // remaining can't solve in 1 guess
        auto buckets = partition(candidates, gi, patterns_);
        bool all_ok = true;
        for (Pattern p = 0; p < PATTERN_COUNT && all_ok; ++p) {
            if (p == PATTERN_SOLVED) continue;
            if (buckets[p].empty()) continue;
            if (!is_feasible(buckets[p], depth - 1, nullptr)) all_ok = false;
        }
        if (all_ok) { ok = true; chosen = gi; break; }
    }

    feas_memo_[key] = ok ? 1 : 2;
    if (ok) { feas_witness_[key] = chosen; if (witness) *witness = chosen; }
    return ok;
}

int EntropySolver::feasible_total(std::span<const uint16_t> candidates,
                                  int budget) const {
    const int n = static_cast<int>(candidates.size());
    if (n == 0) return 0;
    if (n == 1) return 1;
    if (budget <= 1) return std::numeric_limits<int>::max();

    // Pick the highest-entropy feasible guess (lookahead 1) and accumulate.
    uint16_t gi = best_guess_feasible(candidates, budget, /*lookahead=*/1);
    if (gi == WordList::NPOS) return std::numeric_limits<int>::max();

    auto buckets = partition(candidates, gi, patterns_);
    int total = n;
    for (Pattern p = 0; p < PATTERN_COUNT; ++p) {
        if (p == PATTERN_SOLVED) continue;
        auto& b = buckets[p];
        if (b.empty()) continue;
        total += (b.size() == 1) ? 1 : feasible_total(b, budget - 1);
    }
    return total;
}

int EntropySolver::tree_total_for_opener(std::span<const uint16_t> candidates,
                                         uint16_t opener, int budget,
                                         std::size_t lookahead) const {
    const int n = static_cast<int>(candidates.size());
    if (n == 0) return 0;
    if (budget <= 0) return std::numeric_limits<int>::max();

    auto buckets = partition(candidates, opener, patterns_);
    // The opener must keep every non-trivial bucket feasible within budget-1.
    for (Pattern p = 0; p < PATTERN_COUNT; ++p) {
        if (p == PATTERN_SOLVED) continue;
        auto& b = buckets[p];
        if (b.size() <= 1) continue;
        if (!is_feasible(b, budget - 1, nullptr))
            return std::numeric_limits<int>::max();
    }
    // Accumulate total via the feasibility-constrained policy (lookahead-aware
    // when > 1; lookahead 1 uses the cheap greedy continuation).
    int total = n;
    for (Pattern p = 0; p < PATTERN_COUNT; ++p) {
        if (p == PATTERN_SOLVED) continue;
        auto& b = buckets[p];
        if (b.empty()) continue;
        if (b.size() == 1) { total += 1; continue; }
        if (lookahead > 1) {
            // descend choosing best_guess_feasible with lookahead at each node
            uint16_t g = best_guess_feasible(b, budget - 1, lookahead);
            if (g == WordList::NPOS) return std::numeric_limits<int>::max();
            total += tree_total_for_opener(b, g, budget - 1, lookahead);
        } else {
            total += feasible_total(b, budget - 1);
        }
    }
    return total;
}

uint16_t EntropySolver::best_guess_feasible(std::span<const uint16_t> candidates,
                                            int budget,
                                            std::size_t lookahead) const {
    const int n = static_cast<int>(candidates.size());
    if (n == 0) return WordList::NPOS;
    if (n == 1) return candidates[0];
    if (n == 2) return candidates[0];
    if (budget <= 1) return WordList::NPOS;

    // Memoized choice: the production build calls this once per node, and many
    // candidate sets recur across the tree. Key on (set, budget, lookahead).
    const std::uint64_t ckey =
        feas_hash(candidates, budget) ^ (0xC0FFEEull * lookahead + 1);
    if (auto it = feas_choice_.find(ckey); it != feas_choice_.end())
        return it->second;

    // Guarantee a feasible fallback exists (and warm the feasibility memo for
    // this set). The witness is a guess that keeps every bucket solvable within
    // budget; if the set isn't feasible at all, bail.
    uint16_t witness = WordList::NPOS;
    if (!is_feasible(candidates, budget, &witness)) {
        feas_choice_[ckey] = WordList::NPOS;
        return WordList::NPOS;
    }

    // Rank guesses by entropy desc (low-mean heuristic), then check feasibility
    // on only the top EXPLORE_POOL of them. This bounds per-node cost: feasibility
    // checks (the expensive part) run on a small, promising pool, not all ~15k
    // words. The witness guarantees we always have a feasible choice.
    constexpr std::size_t EXPLORE_POOL = 80;
    const std::size_t W = words_.size();
    std::vector<std::pair<double, uint16_t>> order;
    order.reserve(W);
    for (std::size_t g = 0; g < W; ++g) {
        const auto gi = static_cast<uint16_t>(g);
        std::array<uint16_t, PATTERN_COUNT> cnt{};
        int mb = 0;
        for (uint16_t ai : candidates) { int c = ++cnt[patterns_.get(gi, ai)]; if (c > mb) mb = c; }
        if (mb == n) continue;
        order.emplace_back(entropy_simple(candidates, gi), gi);
    }
    const std::size_t pool = std::min<std::size_t>(EXPLORE_POOL, order.size());
    std::ranges::partial_sort(order, order.begin() + static_cast<std::ptrdiff_t>(pool),
        [](auto& a, auto& b){
            if (a.first != b.first) return a.first > b.first;
            return a.second < b.second;
        });
    order.resize(pool);

    // Expand up to `lookahead` feasible guesses; keep the one with lowest total.
    uint16_t best_gi = WordList::NPOS;
    int      best_total = std::numeric_limits<int>::max();
    std::size_t expanded = 0;

    for (auto& [H, gi] : order) {
        if (expanded >= lookahead && best_gi != WordList::NPOS) break;
        auto buckets = partition(candidates, gi, patterns_);
        bool feasible_here = true;
        for (Pattern p = 0; p < PATTERN_COUNT && feasible_here; ++p) {
            if (p == PATTERN_SOLVED) continue;
            auto& b = buckets[p];
            if (b.size() <= 1) continue;
            if (!is_feasible(b, budget - 1, nullptr)) feasible_here = false;
        }
        if (!feasible_here) continue;
        ++expanded;

        if (lookahead <= 1) { feas_choice_[ckey] = gi; return gi; }

        // Estimate total depth under a greedy continuation for tie-breaking.
        int total = n;
        for (Pattern p = 0; p < PATTERN_COUNT; ++p) {
            if (p == PATTERN_SOLVED) continue;
            auto& b = buckets[p];
            if (b.empty()) continue;
            total += (b.size() == 1) ? 1
                   : feasible_total(b, budget - 1);
        }
        if (total < best_total) { best_total = total; best_gi = gi; }
    }
    // If no high-entropy pool member was feasible, fall back to the guaranteed
    // feasible witness (worst-case correctness over mean).
    if (best_gi == WordList::NPOS) best_gi = witness;
    feas_choice_[ckey] = best_gi;
    return best_gi;
}

// ---------------------------------------------------------------------------
// any_consistent_word — solver-mode consistency check
// ---------------------------------------------------------------------------
bool any_consistent_word(const WordList& candidates,
                         std::span<const GuessResponse> history) noexcept {
    for (const auto& w : candidates.span()) {
        std::string_view av = w.view();
        bool ok = true;
        for (const auto& [g, r] : history) {
            if (compute_pattern(g, av) != r) { ok = false; break; }
        }
        if (ok) return true;
    }
    return false;
}

} // namespace wp
