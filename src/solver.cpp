#include "solver.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <mutex>
#include <numeric>
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
// EntropySolver::entropy (private)
// ---------------------------------------------------------------------------
double EntropySolver::entropy(std::span<const uint16_t> candidates,
                              uint16_t                  guess_idx,
                              const PatternMatrix&      pm) noexcept {
    std::array<int, PATTERN_COUNT> counts{};
    for (uint16_t ai : candidates) {
        counts[pm.get(guess_idx, ai)]++;
    }
    const double n = static_cast<double>(candidates.size());
    double H = 0.0;
    for (int c : counts) {
        if (c > 0) {
            double p = static_cast<double>(c) / n;
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

    const std::size_t n = words_.size();

    // Determine which pool of words to search
    std::span<const uint16_t> cand_span = candidates;
    std::vector<uint16_t> all_idx;
    std::span<const uint16_t> guess_pool;

    if (restrict_to_candidates) {
        guess_pool = cand_span;
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
    (void)n;
    return best_word;
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
