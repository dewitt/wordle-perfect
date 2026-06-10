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

    double   best_H            = -1.0;
    uint16_t best_word         = candidates[0];  // fallback
    // Track best_word's candidacy as state instead of re-running binary_search
    // on it every iteration; only recomputed when best_word changes.
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
// Worst-case-bounded tree construction: is_feasible / best_guess_feasible
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
    ++stats_.feasible_calls;
    const int n = static_cast<int>(candidates.size());
    if (n <= 1) { if (witness && n == 1) *witness = candidates[0]; return true; }
    if (depth <= 1) return false;
    // Admissible bound: with one guess (243 patterns), depth==2 can distinguish
    // at most 243 words into singleton buckets.
    if (depth == 2 && n > PATTERN_COUNT) return false;

    const std::uint64_t key = feas_hash(candidates, depth);
    if (auto it = feas_memo_.find(key); it != feas_memo_.end()) {
        ++stats_.feasible_hits;
        if (it->second == 1 && witness) {
            if (auto w = feas_witness_.find(key); w != feas_witness_.end())
                *witness = w->second;
        }
        return it->second == 1;
    }
    ++stats_.feasible_recur;

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
        ++stats_.partitions;
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
    ++stats_.choice_calls;
    const int n = static_cast<int>(candidates.size());
    if (n == 0) return WordList::NPOS;
    if (n == 1) return candidates[0];
    if (n == 2) return candidates[0];
    if (budget <= 1) return WordList::NPOS;

    // Memoized choice: the production build calls this once per node, and many
    // candidate sets recur across the tree. Key on (set, budget, lookahead).
    const std::uint64_t ckey =
        feas_hash(candidates, budget) ^ (0xC0FFEEull * lookahead + 1);
    if (auto it = feas_choice_.find(ckey); it != feas_choice_.end()) {
        ++stats_.choice_hits;
        return it->second;
    }

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
