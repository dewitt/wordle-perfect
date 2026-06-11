#pragma once

#include "wordlist.hpp"
#include "pattern.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <limits>
#include <mutex>
#include <span>
#include <unordered_map>
#include <vector>

namespace wp {

class GpuScorer;  // optional Metal accelerator (src/gpu_score.hpp); fwd-declared
                  // so core doesn't depend on Metal headers.

// ---------------------------------------------------------------------------
// FeasibilityCache — thread-safe, sharded memo of is_feasible() results.
//
// is_feasible(candidate-set, depth) is a PURE function (independent of the
// opener or weights), so when many EntropySolver workers evaluate different
// openers in parallel they recompute the same feasibility facts over heavily
// overlapping subtrees. Sharing one cache across the workers eliminates that
// duplication. Sharded by hash to keep lock contention low.
//
// Stores: status (1 feasible / 2 infeasible) packed with a witness guess.
// ---------------------------------------------------------------------------
class FeasibilityCache {
public:
    struct Entry { char status; WordIndex witness; };

    // Returns the cached entry for `key`, or nullopt. Cheap shared-path lock.
    [[nodiscard]] bool get(std::uint64_t key, Entry& out) const {
        auto& sh = shard(key);
        std::lock_guard lk{sh.m};
        auto it = sh.map.find(key);
        if (it == sh.map.end()) return false;
        out = it->second;
        return true;
    }

    void put(std::uint64_t key, Entry e) {
        auto& sh = shard(key);
        std::lock_guard lk{sh.m};
        sh.map.emplace(key, e);   // first writer wins; entries are immutable facts
    }

    [[nodiscard]] std::size_t size() const {
        std::size_t total = 0;
        for (auto& sh : shards_) { std::lock_guard lk{sh.m}; total += sh.map.size(); }
        return total;
    }

private:
    static constexpr std::size_t SHARDS = 64;  // power of two
    struct Shard {
        mutable std::mutex m;
        std::unordered_map<std::uint64_t, Entry> map;
    };
    std::array<Shard, SHARDS> shards_;
    Shard& shard(std::uint64_t key) const {
        // Mix high bits down; the low bits are used elsewhere too.
        return const_cast<Shard&>(shards_[(key ^ (key >> 32)) & (SHARDS - 1)]);
    }
};

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
    std::vector<Pattern> data_;   // row-major N×N, data_[gi*N + ai]
    std::size_t          n_{};
};

// ---------------------------------------------------------------------------
// SolveStep / SolveResult
// ---------------------------------------------------------------------------
struct SolveStep {
    WordIndex word_idx;  // index into the WordList
    Pattern   response;  // the pattern returned for this guess
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
    using WeightFn = std::function<double(WordIndex)>;

    explicit EntropySolver(const WordList& words, const PatternMatrix& patterns,
                           WeightFn weight_fn = {})
        : words_{words}, patterns_{patterns}, weight_fn_{std::move(weight_fn)} {}

    // Share a feasibility cache across solver instances (e.g. parallel sweep
    // workers). When set, is_feasible() reads/writes this cache instead of the
    // per-instance memo, so workers reuse each other's (pure) feasibility facts.
    void set_feasibility_cache(FeasibilityCache* cache) noexcept {
        shared_feas_ = cache;
    }

    // Optional GPU scorer (matrix uploaded once, reused). When set, is_feasible
    // batches each tree node's sibling-bucket rankings into a single GPU
    // score_sets dispatch instead of CPU-scanning all ~15k guesses per bucket.
    // Result is identical to the CPU path (same ranking → same selection).
    void set_gpu_scorer(const GpuScorer* gpu) noexcept { gpu_ = gpu; }

    // Partition `candidates` by their pattern against `guess_idx`.
    [[nodiscard]] static std::array<std::vector<WordIndex>, PATTERN_COUNT>
    partition(std::span<const WordIndex> candidates,
              WordIndex                  guess_idx,
              const PatternMatrix&       pm);

    // Best guess from the full word list (or restricted to candidates if
    // restrict_to_candidates == true). Returns NPOS on empty candidates.
    [[nodiscard]] WordIndex best_guess(
        std::span<const WordIndex> candidates,
        bool                       restrict_to_candidates = false) const;

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
    is_feasible(std::span<const WordIndex> candidates, int depth,
                WordIndex* witness = nullptr) const;

    // best_guess_feasible: among guesses whose every resulting bucket is
    // feasible at depth-1, pick the highest-entropy one (low-mean heuristic),
    // tie-broken by candidate-ness then lexicographically. Guarantees the
    // returned guess keeps the whole subtree solvable within `budget` IF the
    // candidate set is feasible at `budget`. Returns NPOS only if infeasible.
    //
    // `lookahead` > 1 expands the top-N feasible guesses and keeps the one whose
    // greedy continuation yields the lowest total depth (better mean).
    [[nodiscard]] WordIndex
    best_guess_feasible(std::span<const WordIndex> candidates, int budget,
                        std::size_t lookahead = 1) const;

    // Total depth (Σ over candidates, direct hit = 1) of the feasibility-
    // constrained entropy-greedy tree when the first guess is fixed to `opener`,
    // under a worst-case cap of `budget`. Returns INT_MAX if `opener` cannot
    // keep the set solvable within `budget`. Used by the parallel opener sweep;
    // each thread must use its own EntropySolver instance (the feasibility memo
    // is per-instance, not shared).
    [[nodiscard]] int
    tree_total_for_opener(std::span<const WordIndex> candidates,
                          WordIndex opener, int budget,
                          std::size_t lookahead = 1) const;

    // ── Exact mean optimisation: minimal total depth subject to worst<=depth ──
    //
    // min_total(S, depth) = the MINIMUM achievable Σ-over-answers-in-S of their
    // solve depth (counted from this node; a direct hit = 1), over all decision
    // trees whose worst-case depth is <= `depth`. Returns MIN_TOTAL_INFEASIBLE
    // if S cannot be solved within `depth`. This is the provably-optimal mean
    // (not the entropy heuristic), found by branch-and-bound:
    //   (a) worst-case cap `depth` bounds the search (we already prove D*=5);
    //   (b) alpha-beta on the running total — abandon a guess once its partial
    //       sum reaches the incumbent / passed-in `bound`;
    //   (c) TRANSPOSITION: memoised on the sorted candidate set (the value is
    //       independent of the guess order that produced S), keyed exactly.
    // The optimal first guess for S is recorded in the transposition table and
    // retrievable via optimal_guess().
    static constexpr int MIN_TOTAL_INFEASIBLE = std::numeric_limits<int>::max();
    [[nodiscard]] int min_total(std::span<const WordIndex> candidates,
                                int depth,
                                int bound = MIN_TOTAL_INFEASIBLE) const;
    // After min_total has solved `candidates` at `depth`, the optimal first guess
    // (or NPOS). For tree emission.
    [[nodiscard]] WordIndex optimal_guess(std::span<const WordIndex> candidates,
                                          int depth) const;

    // Solve for a known target word, returning the full step sequence.
    // answer_idx must be a valid index into the WordList.
    // max_rounds caps the search; defaults to DEFAULT_MAX_ROUNDS (6).
    [[nodiscard]] SolveResult solve(WordIndex answer_idx,
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
        std::span<const WordIndex> candidates,
        WordIndex                  guess_idx,
        const PatternMatrix&       pm) const noexcept;

    const WordList&      words_;
    const PatternMatrix& patterns_;
    WeightFn             weight_fn_;  // null → uniform weight

    double weight_of(WordIndex idx) const noexcept {
        return weight_fn_ ? weight_fn_(idx) : 1.0;
    }

    // Feasibility memo for is_feasible(): key = hash(sorted candidate set, depth)
    // → 1 feasible / 2 infeasible, plus a witness guess. Mutable so the public
    // const API can cache.  Cleared lazily is unnecessary; lives for the solver.
    mutable std::unordered_map<std::uint64_t, char>      feas_memo_;
    mutable std::unordered_map<std::uint64_t, WordIndex> feas_witness_;
    // Cache of best_guess_feasible's chosen guess per (candidate set, budget),
    // so the production build (which calls it once per node) doesn't recompute
    // the full-vocabulary entropy ranking for recurring candidate sets.
    mutable std::unordered_map<std::uint64_t, WordIndex> feas_choice_;
    // Transposition table for the exact min_total DP: key = hash(sorted set,
    // depth) → {proven lower bound, exact total (if solved), optimal guess}.
    //   lower : best proven lower bound for this subset (always valid; only
    //           grows). Used to tighten child-bucket floors during pruning.
    //   total : the exact optimum if proven (== lower), else INFEASIBLE meaning
    //           "not yet solved exactly" (the lower bound is still usable).
    //   guess : optimal first guess (valid only when total is exact).
    // A subset's lower bound is a sound floor regardless of the αβ `bound` the
    // call ran under, so it is always safe to record and reuse.
    struct TotEntry {
        int       lower = 0;
        int       total = std::numeric_limits<int>::max();  // INFEASIBLE = unsolved
        WordIndex guess = WordList::NPOS;
        // The exact candidate set this entry describes, so a 64-bit hash
        // collision can be detected (different set, same key) and rejected
        // rather than silently corrupting the optimal total.
        std::vector<WordIndex> set;
    };
    mutable std::unordered_map<std::uint64_t, TotEntry> tot_memo_;
    // Optional cross-instance feasibility cache (set via set_feasibility_cache);
    // when non-null it supersedes feas_memo_/feas_witness_ for is_feasible().
    FeasibilityCache* shared_feas_{nullptr};
    const GpuScorer*  gpu_{nullptr};  // optional Metal scorer for batched ranking
    // When the GPU path is active, a node's guess ranking (the (max_bucket, gi)
    // list, sorted) is precomputed for a whole batch of sibling buckets in one
    // dispatch and stashed here keyed by the set hash, so the recursive
    // is_feasible() consumes it instead of CPU-scanning ~15k guesses per node.
    mutable std::unordered_map<std::uint64_t, std::vector<std::pair<int, WordIndex>>> rank_cache_;
    // Build (or fetch) the max-bucket-ordered guess ranking for `candidates`.
    void build_ranking(std::span<const WordIndex> candidates, int n,
                       std::vector<std::pair<int, WordIndex>>& out) const;
    // GPU-batch the rankings for many sets at once into rank_cache_.
    void prime_rankings_gpu(const std::vector<std::span<const WordIndex>>& sets) const;
    mutable Stats stats_;
    // Total depth (sum over candidates, direct hit = 1) under the entropy-greedy
    // feasible policy; used by best_guess_feasible's lookahead tie-break.
    [[nodiscard]] int feasible_total(std::span<const WordIndex> candidates,
                                     int budget) const;
    // Best known admissible lower bound on min_total(b, depth): max of the
    // structural floor 2|b|-1 and any tighter bound cached in tot_memo_.
    [[nodiscard]] int cached_lower(std::span<const WordIndex> b, int depth) const;
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
