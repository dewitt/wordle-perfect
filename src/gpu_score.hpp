#pragma once

#include "pattern.hpp"
#include "wordlist.hpp"   // WordIndex

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

namespace wp {

// ---------------------------------------------------------------------------
// GpuScorer — Metal-accelerated "score all guesses against a candidate set".
//
// For a fixed candidate (answer) set, computes for EVERY guess in the
// vocabulary, in one GPU dispatch:
//   • max_bucket : the size of the largest pattern bucket (split quality / a
//                  feasibility lower bound), and
//   • entropy    : the Shannon entropy of the partition (mean-depth heuristic).
//
// This is the single hottest per-node operation in the search (ranking ~15k
// guesses), and it is embarrassingly data-parallel — the GPU sweet spot in a
// hybrid CPU-search + GPU-scoring design.
//
// Construction uploads the full N×N pattern matrix to the device once; each
// score_all() call only uploads the (small) candidate index list. The interface
// leaks no Metal types so the rest of the project builds on any platform; on
// systems without Metal, GpuScorer::create() returns an error and callers fall
// back to the CPU path.
// ---------------------------------------------------------------------------
class GpuScorer {
public:
    struct GuessScore {
        std::uint32_t max_bucket;
        float         entropy;
    };

    // Build a scorer for an N×N pattern matrix (row-major: pm[g*N + a]).
    // Returns an error string if Metal is unavailable or setup fails.
    [[nodiscard]] static std::expected<GpuScorer, std::string>
    create(const Pattern* pattern_matrix, std::uint32_t n);

    ~GpuScorer();
    GpuScorer(GpuScorer&&) noexcept;
    GpuScorer& operator=(GpuScorer&&) noexcept;
    GpuScorer(const GpuScorer&)            = delete;
    GpuScorer& operator=(const GpuScorer&) = delete;

    // Score every guess (0..N-1) against `candidates`. Result indexed by guess.
    [[nodiscard]] std::expected<std::vector<GuessScore>, std::string>
    score_all(std::span<const WordIndex> candidates) const;

    // Batched: score every guess against EACH of several candidate sets in a
    // single GPU dispatch. `packed` holds all sets' candidate indices
    // concatenated; `offsets` (size nsets+1) gives each set's [begin,end) slice.
    // Result is row-major [set * N + guess], size nsets*N. This amortises the
    // per-dispatch latency across a whole frontier of sets — the regime where
    // many small per-set dispatches would otherwise be latency-bound.
    [[nodiscard]] std::expected<std::vector<GuessScore>, std::string>
    score_sets(std::span<const WordIndex> packed,
               std::span<const std::uint32_t> offsets) const;

    [[nodiscard]] std::uint32_t n() const noexcept { return n_; }

private:
    GpuScorer() = default;
    void*         impl_{nullptr};  // opaque Obj-C bridge (GpuScorerImpl*)
    std::uint32_t n_{0};
};

} // namespace wp
