#include "solver.hpp"

#include <algorithm>
#include <cmath>
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

    double   best_H    = -1.0;
    uint16_t best_word = candidates[0];  // fallback

    // Prefer guesses that are themselves still candidates (breaks ties toward
    // an answer rather than a pure information guess)
    for (uint16_t gi : guess_pool) {
        double H = entropy(candidates, gi, patterns_);

        // Tie-break: prefer a candidate over a non-candidate for equal entropy
        bool gi_is_candidate = std::ranges::binary_search(candidates, gi);
        bool best_is_candidate = std::ranges::binary_search(candidates, best_word);

        if (H > best_H ||
            (H == best_H && gi_is_candidate && !best_is_candidate) ||
            (H == best_H && gi_is_candidate == best_is_candidate && gi < best_word)) {
            best_H    = H;
            best_word = gi;
        }
    }
    return best_word;
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
                                      int depth_budget) const noexcept {
    if (candidates.empty()) return 0;
    if (candidates.size() == 1) return 1;
    if (depth_budget <= 0)      return DEPTH_IMPOSSIBLE;

    uint16_t guess   = best_guess(candidates);
    auto     buckets = partition(candidates, guess, patterns_);

    int worst = 0;
    for (Pattern p = 0; p < PATTERN_COUNT - 1; ++p) {
        if (buckets[p].empty()) continue;
        int d = greedy_worst_depth(buckets[p], depth_budget - 1);
        if (d == DEPTH_IMPOSSIBLE) return d;
        worst = std::max(worst, 1 + d);
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
        std::array<uint8_t, PATTERN_COUNT> counts{};
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
SolveResult EntropySolver::solve(uint16_t answer_idx) const {
    SolveResult result;
    std::vector<uint16_t> candidates = words_.all_indices();

    for (int round = 0; round < 6; ++round) {
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

} // namespace wp
