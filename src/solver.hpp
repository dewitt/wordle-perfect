#pragma once

#include "wordlist.hpp"
#include "pattern.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <limits>
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

    // Parallel best_guess over the full vocabulary, splitting the guess pool
    // across `nthreads` workers. Identical result and tie-breaking to
    // best_guess(); used to parallelise the expensive root search. Shares the
    // same entropy/tie-break logic so the two can't diverge.
    [[nodiscard]] uint16_t best_guess_parallel(
        std::span<const uint16_t> candidates,
        unsigned                  nthreads) const;

    // Sentinel returned as the depth component when no solution exists within
    // budget, or when minimax finds no improvement over the greedy baseline.
    // Callers distinguish the two cases by the returned word index: a valid
    // index means minimax found a strictly better guess; NPOS means fall back
    // to best_guess().
    static constexpr int DEPTH_IMPOSSIBLE = std::numeric_limits<int>::max();

    // Default round budget for the dynamic solve() method. The standard Wordle
    // game allows 6 guesses; full-coverage DBs may need up to 8.
    static constexpr int DEFAULT_MAX_ROUNDS = 6;

    // Minimax best guess: finds the guess that minimises worst-case depth
    // (number of additional guesses needed, including this one) over
    // `candidates`. Uses alpha-beta pruning seeded by the entropy guess.
    //
    // `depth_budget` is the maximum additional guesses allowed (including
    // the current one). Returns {NPOS, DEPTH_IMPOSSIBLE} if no solution
    // exists within budget, or {NPOS, seed_depth} if minimax found no
    // improvement over greedy.
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

    // Budget-aware guess selection (the precomputation work-horse).
    //
    // Strategy: take the fast greedy-entropy guess; if its worst-case depth
    // already fits within `budget`, use it. Only when greedy would BLOW the
    // budget do we escalate to the (expensive) minimax search for a guess that
    // fits. This focuses minimax exactly on the hard nodes — the repeated-letter
    // trap families that greedy leaves at depth 6 — regardless of candidate-set
    // size, while keeping the common case fast.
    //
    // `budget` is the number of guesses allowed from this node onward
    // (including the guess returned). Returns a valid word index always (falls
    // back to the greedy guess if even minimax cannot fit the budget, i.e. the
    // depth is genuinely unavoidable).
    //
    // `escalated` (optional out-param) is set to true when minimax was invoked,
    // for build-time instrumentation.
    //
    // `escalate_max_candidates` bounds the candidate-set size at which we are
    // willing to pay for minimax. Minimax's top-level iterates the entire ~15k
    // guess vocabulary, so it is only tractable when the candidate set (and
    // therefore the per-guess partition + restricted recursion) is small.
    // Above this size we keep the greedy guess even if it misses budget.
    static constexpr std::size_t ESCALATE_MAX_CANDIDATES = 64;
    // Default beam width for the large-candidate-set escalation path.
    static constexpr std::size_t DEFAULT_BEAM_WIDTH = 24;
    [[nodiscard]] uint16_t
    best_guess_within_budget(std::span<const uint16_t> candidates,
                             int budget,
                             bool* escalated = nullptr,
                             std::size_t escalate_max_candidates =
                                 ESCALATE_MAX_CANDIDATES,
                             std::size_t beam_width = DEFAULT_BEAM_WIDTH) const;

    // Beam re-search for large candidate sets where full minimax is intractable.
    //
    // When the greedy guess would blow the budget, evaluate the top-`beam_width`
    // guesses (ranked by entropy) using the cheap greedy_worst_depth probe and
    // return the one with the smallest worst-case depth. Unlike minimax, this
    // does NOT iterate the full vocabulary — it only probes a fixed number of
    // promising guesses — so it stays tractable even for large candidate sets,
    // letting us attack the trap clusters created high in the tree.
    //
    // Returns {best_guess_idx, achieved_worst_depth}. Falls back to the greedy
    // guess (and its depth) if no beam member improves on it.
    [[nodiscard]] std::pair<uint16_t, int>
    best_guess_beam(std::span<const uint16_t> candidates,
                    int budget,
                    std::size_t beam_width) const;

    // Solve for a known target word, returning the full step sequence.
    // answer_idx must be a valid index into the WordList.
    // max_rounds caps the search; defaults to DEFAULT_MAX_ROUNDS (6).
    [[nodiscard]] SolveResult solve(uint16_t answer_idx,
                                    int max_rounds = DEFAULT_MAX_ROUNDS) const;

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
    //
    // `upper_bound`: if the partial worst-depth reaches this value the function
    // returns DEPTH_IMPOSSIBLE early (the result would not beat a known-better
    // alternative). Pass DEPTH_IMPOSSIBLE to disable pruning.
    [[nodiscard]] int greedy_worst_depth(
        std::span<const uint16_t> candidates,
        int depth_budget,
        int upper_bound = DEPTH_IMPOSSIBLE) const noexcept;

    const WordList&      words_;
    const PatternMatrix& patterns_;
    WeightFn             weight_fn_;  // null → uniform weight

    double weight_of(uint16_t idx) const noexcept {
        return weight_fn_ ? weight_fn_(idx) : 1.0;
    }
};

// ---------------------------------------------------------------------------
// Consistency checking (solver mode)
// ---------------------------------------------------------------------------

// A (guess, response) pair observed during interactive solving.
struct GuessResponse {
    std::string_view guess;
    Pattern          response;
};

// Returns true iff at least one word in `candidates` would produce every
// observed (guess, response) pattern — i.e. the responses are mutually
// consistent with some possible secret word. Used by solver mode to reject
// logically impossible user input.
//
// `candidates` should be the set of *possible secret words* (the answers list,
// or the full word list for a full-coverage database), NOT the full guess
// vocabulary — using the guess vocabulary makes the check too permissive.
[[nodiscard]] bool
any_consistent_word(const WordList& candidates,
                    std::span<const GuessResponse> history) noexcept;

} // namespace wp
