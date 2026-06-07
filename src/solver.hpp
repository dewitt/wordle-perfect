#pragma once

#include "wordlist.hpp"
#include "pattern.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace wp {

// ---------------------------------------------------------------------------
// PatternMatrix — precomputed N×N pattern table
//
// patterns_[guess_idx * N + answer_idx] = compute_pattern(words[guess_idx],
//                                                          words[answer_idx])
//
// Building this matrix once (220 MB for 14,855 words) allows all subsequent
// partition and entropy computations to be pure memory accesses.
// ---------------------------------------------------------------------------
class PatternMatrix {
public:
    // Build the full N×N matrix. This is the expensive one-time cost (~1–2s
    // single-threaded; parallelised via std::thread).
    static PatternMatrix build(const WordList& words, unsigned nthreads = 0);

    [[nodiscard]] Pattern get(uint16_t gi, uint16_t ai) const noexcept {
        return data_[static_cast<std::size_t>(gi) * n_ + ai];
    }

    [[nodiscard]] std::size_t n() const noexcept { return n_; }

private:
    std::vector<uint8_t> data_;
    std::size_t          n_{};
};

// ---------------------------------------------------------------------------
// SolveStep / SolveResult
// ---------------------------------------------------------------------------
struct SolveStep {
    uint16_t word_idx;   // index into the WordList
    Pattern  response;   // the pattern returned for this guess
};

struct SolveResult {
    std::vector<SolveStep> steps;
    bool                   solved{false};
    int                    depth() const noexcept { return static_cast<int>(steps.size()); }
};

// ---------------------------------------------------------------------------
// EntropySolver — greedy, information-theoretic dynamic solver
//
// At each step, picks the guess that maximises the weighted Shannon entropy of
// the partition of remaining candidates by pattern. Ties broken by preferring
// candidates over non-candidates, then lexicographically.
//
// Weighted entropy: each candidate word carries a weight. Setting higher
// weights for words in the curated answer list biases the tree toward
// minimising solve depth for those words specifically, while still building
// valid paths to all words.
//
// This is the intermediate-artifact solver used for:
//   • Interactive solver mode (fallback when database is absent)
//   • Driving the precomputation pipeline
//   • Consistency checking
// ---------------------------------------------------------------------------
class EntropySolver {
public:
    // weight_fn(word_idx) → weight for that word. Default: uniform weight 1.
    // Answer-weighted: return 1000 if word is in answers list, 1 otherwise.
    using WeightFn = std::function<double(uint16_t)>;

    explicit EntropySolver(const WordList& words, const PatternMatrix& patterns,
                           WeightFn weight_fn = {})
        : words_{words}, patterns_{patterns}, weight_fn_{std::move(weight_fn)} {}

    // Partition `candidates` by their pattern against `guess_idx`.
    [[nodiscard]] static std::array<std::vector<uint16_t>, PATTERN_COUNT>
    partition(std::span<const uint16_t> candidates,
              uint16_t                  guess_idx,
              const PatternMatrix&      pm);

    // Best guess from the full word list (or restricted to candidates if
    // restrict_to_candidates == true). Returns NPOS on empty candidates.
    [[nodiscard]] uint16_t best_guess(
        std::span<const uint16_t> candidates,
        bool                      restrict_to_candidates = false) const;

    // Minimax best guess: finds the guess that minimises worst-case depth
    // (number of additional guesses needed, including this one) over
    // `candidates`. Uses alpha-beta pruning seeded by the entropy guess.
    //
    // `depth_budget` is the maximum additional guesses allowed (including
    // the current one). Returns {NPOS, INT_MAX} if no solution exists
    // within budget.
    //
    // Intended for small candidate sets (≤ MINIMAX_THRESHOLD) where the
    // O(N·K^depth) cost is acceptable. For larger sets, fall back to
    // best_guess().
    // Minimax is applied to candidate sets up to this size. Below the
    // threshold, the solver searches for the guess that minimises worst-case
    // depth rather than just maximising entropy.  Sub-calls restrict the
    // guess pool to candidates only (O(K^depth) instead of O(N^depth)).
    static constexpr std::size_t MINIMAX_THRESHOLD = 15;
    [[nodiscard]] std::pair<uint16_t, int>
    minimax_best_guess(std::span<const uint16_t> candidates,
                       int depth_budget) const;

    // Solve for a known target word, returning the full step sequence.
    // answer_idx must be a valid index into the WordList.
    [[nodiscard]] SolveResult solve(uint16_t answer_idx) const;

private:
    // Compute weighted Shannon entropy.
    [[nodiscard]] double entropy(
        std::span<const uint16_t> candidates,
        uint16_t                  guess_idx,
        const PatternMatrix&      pm) const noexcept;

    // Recursive minimax core; upper_bound is the best worst-depth seen so
    // far (alpha-beta: skip guesses that exceed it).
    //
    // restrict_to_candidates: when true, only search candidate words (not the
    // full 14k vocabulary). Used for sub-calls to keep cost O(K^depth) where
    // K = candidate count. The top-level call always searches all words.
    [[nodiscard]] std::pair<uint16_t, int>
    minimax_inner(std::span<const uint16_t> candidates,
                  int depth_budget,
                  int upper_bound,
                  bool restrict_to_candidates = false) const;

    // Fast greedy worst-depth: follows entropy-greedy choices (no branching
    // over alternative guesses). Used to seed alpha-beta upper bound so that
    // minimax_inner prunes aggressively from the start.
    [[nodiscard]] int greedy_worst_depth(std::span<const uint16_t> candidates,
                                         int depth_budget) const noexcept;

    const WordList&      words_;
    const PatternMatrix& patterns_;
    WeightFn             weight_fn_;  // null → uniform weight

    double weight_of(uint16_t idx) const noexcept {
        return weight_fn_ ? weight_fn_(idx) : 1.0;
    }
};

} // namespace wp
