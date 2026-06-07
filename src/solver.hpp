#pragma once

#include "wordlist.hpp"
#include "pattern.hpp"

#include <array>
#include <cstdint>
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
// At each step, picks the guess that maximises the Shannon entropy of the
// partition of remaining candidates by pattern. Ties broken lexicographically.
//
// This is the intermediate-artifact solver used for:
//   • Interactive solver mode (fallback when database is absent)
//   • Driving the precomputation pipeline
//   • Consistency checking
// ---------------------------------------------------------------------------
class EntropySolver {
public:
    EntropySolver(const WordList& words, const PatternMatrix& patterns)
        : words_{words}, patterns_{patterns} {}

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

    // Solve for a known target word, returning the full step sequence.
    // answer_idx must be a valid index into the WordList.
    [[nodiscard]] SolveResult solve(uint16_t answer_idx) const;

private:
    // Compute Shannon entropy of a bucket-size distribution.
    [[nodiscard]] static double entropy(
        std::span<const uint16_t> candidates,
        uint16_t                  guess_idx,
        const PatternMatrix&      pm) noexcept;

    const WordList&      words_;
    const PatternMatrix& patterns_;
};

} // namespace wp
