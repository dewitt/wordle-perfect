#pragma once

#include "wordlist.hpp"
#include "pattern.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <limits>
#include <span>
#include <unordered_map>
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

    // Default round budget for the dynamic solve() method. The standard Wordle
    // game allows 6 guesses; full-coverage DBs may need up to 8.
    static constexpr int DEFAULT_MAX_ROUNDS = 6;

    // ── Worst-case-bounded tree construction ────────────────────────────────
    //
    // is_feasible(candidates, depth): true iff `candidates` can be solved with
    // worst-case depth <= `depth`. DFS minimax over the answer set with
    // memoization on the sorted candidate set and max-bucket guess ordering.
    // Optionally returns a witness guess that keeps every bucket feasible.
    [[nodiscard]] bool
    is_feasible(std::span<const uint16_t> candidates, int depth,
                uint16_t* witness = nullptr) const;

    // best_guess_feasible: among guesses whose every resulting bucket is
    // feasible at depth-1, pick the highest-entropy one (low-mean heuristic),
    // tie-broken by candidate-ness then lexicographically. Guarantees the
    // returned guess keeps the whole subtree solvable within `budget` IF the
    // candidate set is feasible at `budget`. Returns NPOS only if infeasible.
    //
    // `lookahead` > 1 expands the top-N feasible guesses and keeps the one whose
    // greedy continuation yields the lowest total depth (better mean).
    [[nodiscard]] uint16_t
    best_guess_feasible(std::span<const uint16_t> candidates, int budget,
                        std::size_t lookahead = 1) const;

    // Total depth (Σ over candidates, direct hit = 1) of the feasibility-
    // constrained entropy-greedy tree when the first guess is fixed to `opener`,
    // under a worst-case cap of `budget`. Returns INT_MAX if `opener` cannot
    // keep the set solvable within `budget`. Used by the parallel opener sweep;
    // each thread must use its own EntropySolver instance (the feasibility memo
    // is per-instance, not shared).
    [[nodiscard]] int
    tree_total_for_opener(std::span<const uint16_t> candidates,
                          uint16_t opener, int budget,
                          std::size_t lookahead = 1) const;

    // Solve for a known target word, returning the full step sequence.
    // answer_idx must be a valid index into the WordList.
    // max_rounds caps the search; defaults to DEFAULT_MAX_ROUNDS (6).
    [[nodiscard]] SolveResult solve(uint16_t answer_idx,
                                    int max_rounds = DEFAULT_MAX_ROUNDS) const;

    // ── Instrumentation (builder performance, issue #28) ─────────────────────
    // Lightweight counters for profiling the worst-case-bounded search. They are
    // per-instance and incremented in the hot paths; cost is a single ++ each.
    struct Stats {
        std::uint64_t feasible_calls   = 0;  // is_feasible() invocations
        std::uint64_t feasible_hits    = 0;  // ... served from the memo
        std::uint64_t feasible_recur   = 0;  // ... that ran the full search
        std::uint64_t partitions       = 0;  // partition() calls (a proxy for work)
        std::uint64_t choice_calls     = 0;  // best_guess_feasible() invocations
        std::uint64_t choice_hits      = 0;  // ... served from the choice memo
    };
    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }
    [[nodiscard]] std::size_t feas_memo_size()   const noexcept { return feas_memo_.size(); }
    [[nodiscard]] std::size_t choice_memo_size() const noexcept { return feas_choice_.size(); }

private:
    // Compute weighted Shannon entropy.
    [[nodiscard]] double entropy(
        std::span<const uint16_t> candidates,
        uint16_t                  guess_idx,
        const PatternMatrix&      pm) const noexcept;

    const WordList&      words_;
    const PatternMatrix& patterns_;
    WeightFn             weight_fn_;  // null → uniform weight

    double weight_of(uint16_t idx) const noexcept {
        return weight_fn_ ? weight_fn_(idx) : 1.0;
    }

    // Feasibility memo for is_feasible(): key = hash(sorted candidate set, depth)
    // → 1 feasible / 2 infeasible, plus a witness guess. Mutable so the public
    // const API can cache.  Cleared lazily is unnecessary; lives for the solver.
    mutable std::unordered_map<std::uint64_t, char>     feas_memo_;
    mutable std::unordered_map<std::uint64_t, uint16_t> feas_witness_;
    // Cache of best_guess_feasible's chosen guess per (candidate set, budget),
    // so the production build (which calls it once per node) doesn't recompute
    // the full-vocabulary entropy ranking for recurring candidate sets.
    mutable std::unordered_map<std::uint64_t, uint16_t> feas_choice_;
    mutable Stats stats_;
    [[nodiscard]] double entropy_simple(std::span<const uint16_t> candidates,
                                        uint16_t guess_idx) const noexcept;
    // Total depth (sum over candidates, direct hit = 1) under the entropy-greedy
    // feasible policy; used by best_guess_feasible's lookahead tie-break.
    [[nodiscard]] int feasible_total(std::span<const uint16_t> candidates,
                                     int budget) const;
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
