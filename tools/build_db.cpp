#include "wordlist.hpp"
#include "pattern.hpp"
#include "solver.hpp"
#include "database.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <format>
#include <future>
#include <iostream>
#include <print>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace wp;
using Clock = std::chrono::steady_clock;

[[noreturn]] static void die(std::string_view msg) {
    std::println(stderr, "build_db error: {}", msg);
    std::exit(1);
}

static double elapsed_s(Clock::time_point t0) {
    return std::chrono::duration<double>(Clock::now() - t0).count();
}

// ---------------------------------------------------------------------------
// Transposition memoization
//
// Many decision-tree paths converge on the same candidate set (transpositions).
// Caching best_guess() results for those sets avoids redundant O(N × |cands|)
// entropy computations.  Only the entropy-greedy path is cached: minimax results
// depend on depth_budget and are only applied to tiny sets (≤15), so caching
// them is not worthwhile.
//
// Cache key: the sorted candidate vector (partition() preserves sort order).
// Cache value: the best-guess word index for that candidate set.
// Bounded by memo_limit: only sets of size ≤ memo_limit are cached.
// Setting memo_limit to 0 disables caching entirely (the default).
// ---------------------------------------------------------------------------
struct CandidateHash {
    std::size_t operator()(const std::vector<uint16_t>& v) const noexcept {
        // FNV-1a-inspired mix over the sorted word-index vector.
        std::size_t seed = v.size();
        for (auto x : v)
            seed ^= x + std::size_t{0x9e3779b9} + (seed << 6) + (seed >> 2);
        return seed;
    }
};
using MemoCache = std::unordered_map<std::vector<uint16_t>, uint16_t, CandidateHash>;

// Result of a beam-search evaluation at a tree node: the best guess found
// across the beam, and the resulting quality metrics for that choice.
struct BeamEvalResult {
    uint16_t best_guess;
    int      worst_depth;  // worst over all candidates reachable from this node
    double   mean_depth;   // mean solve depth over all candidates in this set
};

// ---------------------------------------------------------------------------
// TreeBuilder — depth-first recursive, single-threaded DB writes, parallel
// entropy evaluation at the top levels.
//
// Design:
//   • The pattern matrix is precomputed (parallel).
//   • For the root node, we parallelise the best-guess search over all 14k
//     candidates (the O(N²) hot spot).
//   • All DB inserts are single-threaded (SQLite write serialisation).
//   • At each node, the guess that maximises Shannon entropy is chosen.
//     Ties go to lexicographically earlier words (via EntropySolver).
//
// Beam search (beam_width > 1):
//   • Two-phase build: evaluate all reachable candidate sets using beam
//     search (memoised), then write the DB using a DAG structure that
//     deduplicates shared subtrees.
//   • beam_eval() fills beam_cache with the best guess for every candidate
//     set encountered by the beam.  Memoisation ensures each unique set is
//     evaluated exactly once even when the beam creates transpositions.
//   • build_from_eval() writes the DB using dag_nodes to skip re-inserting
//     subtrees whose candidate set has already been built.
// ---------------------------------------------------------------------------
struct TreeBuilder {
    const WordList&        words;
    const PatternMatrix&   pm;
    Database&              db;
    EntropySolver          solver;
    unsigned               nthreads;
    EntropySolver::WeightFn weight_fn;  // may be null (uniform)
    uint16_t                fixed_root{WordList::NPOS};  // forced first guess, or NPOS

    uint32_t next_id{0};
    uint64_t nodes_written{0};
    int      max_depth_seen{0};
    uint64_t minimax_calls{0};

    // K=1 transposition memo cache (entropy-greedy path only).
    std::size_t memo_limit{0};
    std::size_t cache_hits{0};
    std::size_t cache_misses{0};
    MemoCache   memo_cache;

    // Beam search state (beam_width > 1 only).
    std::size_t beam_width{1};
    // beam_root_candidates: the initial candidate set for beam search.
    // When non-empty, beam search evaluates sub-trees over this set rather
    // than over all words.  Setting this to the curated answer list (2,355
    // words) eliminates the 12,500 non-answer words that otherwise pollute
    // quality estimates and cause the beam to make poor root-level decisions.
    // The GUESS POOL (searched by top_k_guesses) is always all words.
    std::vector<uint16_t> beam_root_candidates;
    std::unordered_map<std::vector<uint16_t>, BeamEvalResult, CandidateHash> beam_cache;
    std::unordered_map<std::vector<uint16_t>, uint32_t,       CandidateHash> dag_nodes;
    std::size_t beam_cache_hits{0};
    std::size_t beam_cache_misses{0};
    std::size_t dag_reuses{0};

    TreeBuilder(const WordList& w, const PatternMatrix& p, Database& d, unsigned t,
                EntropySolver::WeightFn wf = {}, uint16_t fixed_root = WordList::NPOS,
                std::size_t memo_limit_ = 0, std::size_t beam_width_ = 1)
        : words{w}, pm{p}, db{d}, solver{w, p, wf}, nthreads{t}, weight_fn{wf},
          fixed_root{fixed_root}, memo_limit{memo_limit_}, beam_width{beam_width_} {}

    // Entry point: build the entire tree wrapped in a single transaction.
    uint32_t build() {
        if (auto r = db.begin_transaction(); !r) die(r.error());
        auto all_candidates = words.all_indices();
        uint32_t root;

        if (beam_width > 1) {
            // Phase 1: memoised beam-search evaluation.  Fills beam_cache with
            // the best-guess word for every reachable candidate set.  Each unique
            // set is evaluated exactly once regardless of how many paths the beam
            // explores (memoisation converts the DAG of transpositions to a tree
            // of unique evaluations).
            //
            // Use beam_root_candidates (typically the curated answer list) when
            // set.  This keeps quality estimates clean: non-answer words have
            // weight 1× but answer words have 1000×, so starting from only the
            // 2,355 answer words gives the beam an unambiguous objective without
            // noise from the 12,500 non-answer candidates.
            const auto& beam_init = beam_root_candidates.empty()
                                    ? all_candidates : beam_root_candidates;
            std::print("  phase 1 (eval, beam={}, {} candidates)... ",
                beam_width, beam_init.size());
            std::cout.flush();
            auto t1 = Clock::now();
            beam_eval(beam_init);
            const std::size_t total_evals = beam_cache_hits + beam_cache_misses;
            std::println("done ({:.1f}s)  {} unique sets,  {} cache hits ({:.1f}%)",
                elapsed_s(t1), beam_cache.size(), beam_cache_hits,
                total_evals > 0
                    ? 100.0 * static_cast<double>(beam_cache_hits) / static_cast<double>(total_evals)
                    : 0.0);

            // Phase 2: write DB using the cached decisions.  DAG deduplication
            // reuses node IDs when the same candidate set is reached via multiple
            // paths, keeping the DB compact.
            std::print("  phase 2 (build, DAG)... ");
            std::cout.flush();
            auto t2 = Clock::now();
            root = build_from_eval(beam_init, 1);
            std::println("done ({:.1f}s)  {} unique nodes,  {} paths reused",
                elapsed_s(t2), nodes_written, dag_reuses);
        } else {
            // K=1: original depth-first build (parallel root, minimax for small sets).
            root = build_node(all_candidates, 1);
        }

        if (auto r = db.commit_transaction(); !r) die(r.error());
        return root;
    }

private:
    // Recursively build a subtree for `candidates` at `depth`.
    // Returns the node_id assigned to this node.
    uint32_t build_node(std::vector<uint16_t>& candidates, int depth) {
        uint32_t my_id = next_id++;

        // Choose best guess for this candidate set.
        // At small candidate sets, switch to minimax to minimise worst-case
        // depth rather than just maximising entropy. This eliminates the
        // depth-6 paths that greedy entropy leaves behind for repeated-letter
        // trap families (cover, rover, roger, etc.).
        uint16_t guess_idx;
        if (depth == 1 && fixed_root != WordList::NPOS) {
            guess_idx = fixed_root;   // forced start word
        } else if (depth == 1) {
            guess_idx = best_guess_parallel(candidates);  // parallelise expensive root
        } else if (candidates.size() <= EntropySolver::MINIMAX_THRESHOLD) {
            // Budget: worst-case Wordle is solvable in 6; remaining budget is
            // 6 - (depth - 1) guesses from this node onward.
            // Clamp to 1 so we never pass ≤0 to minimax.
            const int budget = std::max(1, 7 - depth);
            ++minimax_calls;
            auto mm_t0 = Clock::now();
            auto [gi, worst_depth] = solver.minimax_best_guess(candidates, budget);
            double mm_e = elapsed_s(mm_t0);
            if (mm_e > 0.01 || minimax_calls <= 5) {
                std::println(stderr, "[mm #{} d={} c={} b={} t={:.3f}s]",
                    minimax_calls, depth, candidates.size(), budget, mm_e);
            }
            (void)worst_depth;
            guess_idx = (gi != WordList::NPOS) ? gi : solver.best_guess(candidates);
        } else {
            // Entropy-greedy path — check transposition memo cache.
            // Cache is keyed on the sorted candidate vector; partition() preserves
            // sort order so no extra sorting is needed here.
            if (memo_limit > 0 && candidates.size() <= memo_limit) {
                if (auto it = memo_cache.find(candidates); it != memo_cache.end()) {
                    ++cache_hits;
                    guess_idx = it->second;
                } else {
                    ++cache_misses;
                    guess_idx = solver.best_guess(candidates);
                    memo_cache.emplace(candidates, guess_idx);
                }
            } else {
                guess_idx = solver.best_guess(candidates);
            }
        }

        // Insert node
        auto r = db.insert_node(my_id, guess_idx, static_cast<uint8_t>(depth));
        if (!r) die(r.error());
        ++nodes_written;
        if (depth > max_depth_seen) max_depth_seen = depth;

        // Partition candidates by pattern against this guess
        auto buckets = EntropySolver::partition(candidates, guess_idx, pm);

        // For each non-solved partition, recurse and add edge
        for (Pattern p = 0; p < PATTERN_COUNT - 1; ++p) {  // 0..241; skip 242 = GGGGG
            auto& bucket = buckets[p];
            if (bucket.empty()) continue;

            uint32_t child_id = build_node(bucket, depth + 1);

            auto re = db.insert_edge(my_id, p, child_id);
            if (!re) die(re.error());
        }

        return my_id;
    }

    // Parallel best-guess search over all words (used only at root).
    // Splits the guess pool across threads; each thread finds its local best,
    // then we pick the global best.
    uint16_t best_guess_parallel(std::span<const uint16_t> candidates) const {
        const std::size_t total = words.size();
        const auto chunk = (total + nthreads - 1) / nthreads;

        struct LocalBest { double H; uint16_t idx; bool is_candidate; };

        std::vector<std::future<LocalBest>> futures;
        futures.reserve(nthreads);

        for (unsigned t = 0; t < nthreads; ++t) {
            const auto start = static_cast<uint16_t>(t * chunk);
            const auto end   = static_cast<uint16_t>(
                std::min(static_cast<std::size_t>(start) + chunk, total));
            if (start >= total) break;

            futures.push_back(std::async(std::launch::async,
                [this, candidates, start, end]() -> LocalBest {
                    double   best_H    = -1.0;
                    uint16_t best_word = start;
                    bool     best_is_cand = false;

                    auto is_cand = [&](uint16_t gi) {
                        return std::ranges::binary_search(candidates, gi);
                    };
                    auto w = [this](uint16_t ai) -> double {
                        return weight_fn ? weight_fn(ai) : 1.0;
                    };

                    for (uint16_t gi = start; gi < end; ++gi) {
                        // Weighted Shannon entropy
                        std::array<double, PATTERN_COUNT> bw{};
                        double total_w = 0.0;
                        for (uint16_t ai : candidates) {
                            double wi = w(ai);
                            bw[pm.get(gi, ai)] += wi;
                            total_w += wi;
                        }
                        double H = 0.0;
                        if (total_w > 0.0) {
                            for (double b : bw) {
                                if (b > 0.0) {
                                    double p = b / total_w;
                                    H -= p * std::log2(p);
                                }
                            }
                        }

                        bool gi_is_cand = is_cand(gi);
                        bool better =
                            H > best_H ||
                            (H == best_H && gi_is_cand && !best_is_cand) ||
                            (H == best_H && gi_is_cand == best_is_cand && gi < best_word);

                        if (better) {
                            best_H       = H;
                            best_word    = gi;
                            best_is_cand = gi_is_cand;
                        }
                    }
                    return {best_H, best_word, best_is_cand};
                }));
        }

        // Merge local bests
        double   global_H    = -1.0;
        uint16_t global_word = 0;
        bool     global_is_cand = false;

        for (auto& f : futures) {
            auto [H, idx, is_cand] = f.get();
            bool better =
                H > global_H ||
                (H == global_H && is_cand && !global_is_cand) ||
                (H == global_H && is_cand == global_is_cand && idx < global_word);
            if (better) {
                global_H       = H;
                global_word    = idx;
                global_is_cand = is_cand;
            }
        }
        return global_word;
    }

    // -----------------------------------------------------------------------
    // top_k_guesses — return the K word indices with highest weighted entropy
    // over `candidates`.  Parallelises the scan for large candidate sets.
    // -----------------------------------------------------------------------
    std::vector<uint16_t> top_k_guesses(std::span<const uint16_t> candidates,
                                        std::size_t k) const {
        struct Scored { double H; uint16_t idx; bool is_cand; };
        const std::size_t total_words = words.size();
        std::vector<Scored> all(total_words);

        // Entropy computation: split guess pool across threads for large sets.
        // For small candidate sets (≤ threshold) the inner loop is cheap enough
        // that thread setup overhead dominates — use a single thread instead.
        static constexpr std::size_t PARALLEL_THRESHOLD = 500;
        const unsigned thr =
            (candidates.size() >= PARALLEL_THRESHOLD && nthreads > 1) ? nthreads : 1u;
        const auto chunk = (total_words + thr - 1) / thr;

        auto compute_range = [&](std::size_t start, std::size_t end) {
            for (std::size_t gi = start; gi < end; ++gi) {
                const auto gi16 = static_cast<uint16_t>(gi);
                std::array<double, PATTERN_COUNT> bw{};
                double total_w = 0.0;
                for (uint16_t ai : candidates) {
                    const double wi = weight_fn ? weight_fn(ai) : 1.0;
                    bw[pm.get(gi16, ai)] += wi;
                    total_w += wi;
                }
                double H = 0.0;
                if (total_w > 0.0) {
                    for (double b : bw)
                        if (b > 0.0) { const double p = b / total_w; H -= p * std::log2(p); }
                }
                all[gi] = {H, gi16, std::ranges::binary_search(candidates, gi16)};
            }
        };

        if (thr > 1) {
            std::vector<std::thread> threads;
            threads.reserve(thr);
            for (unsigned t = 0; t < thr; ++t) {
                const auto start = t * chunk;
                const auto end   = std::min(start + chunk, total_words);
                if (start >= total_words) break;
                threads.emplace_back(compute_range, start, end);
            }
            for (auto& th : threads) th.join();
        } else {
            compute_range(0, total_words);
        }

        // Partial sort: highest entropy first; candidates preferred; lex tie-break.
        const std::size_t n = std::min(k, all.size());
        std::ranges::partial_sort(all, all.begin() + static_cast<std::ptrdiff_t>(n),
            [](const Scored& a, const Scored& b) {
                if (a.H != b.H)            return a.H > b.H;
                if (a.is_cand != b.is_cand) return a.is_cand;
                return a.idx < b.idx;
            });

        std::vector<uint16_t> result;
        result.reserve(n);
        for (std::size_t i = 0; i < n; ++i) result.push_back(all[i].idx);
        return result;
    }

    // -----------------------------------------------------------------------
    // beam_eval — memoised recursive beam-search evaluation.
    //
    // For each candidate set, picks the guess that minimises
    // (worst_depth, answer-weighted mean_depth) over the full subtree.
    // Uses minimax for small sets (≤ MINIMAX_THRESHOLD) and beam search
    // for larger sets.  Results are memoised by candidate set, so
    // transpositions (same set reached via multiple beam paths) are
    // evaluated exactly once.
    //
    // Mean depth is ANSWER-WEIGHTED (using weight_fn) to stay aligned with
    // the entropy bias used by best_guess().  It also correctly counts
    // GGGGG words (solved in 1 guess by this node's guess itself) which
    // would otherwise be missed since we don't recurse into the GGGGG bucket.
    // -----------------------------------------------------------------------
    BeamEvalResult beam_eval(const std::vector<uint16_t>& candidates) {
        if (candidates.empty()) return {WordList::NPOS, 0, 0.0};
        if (candidates.size() == 1) return {candidates[0], 1, 1.0};

        if (auto it = beam_cache.find(candidates); it != beam_cache.end()) {
            ++beam_cache_hits;
            return it->second;
        }
        ++beam_cache_misses;

        // Compute quality metrics for a specific guess applied to this set.
        // Returns {worst_depth, answer_weighted_mean_depth}.
        //
        // Correctly handles the GGGGG bucket (words solved in one guess, not
        // recursed into) and uses weight_fn for the mean so the comparison
        // objective matches the answer-weighted entropy ordering.
        auto eval_guess = [&](uint16_t gi) -> std::pair<int, double> {
            auto buckets = EntropySolver::partition(candidates, gi, pm);

            int    worst     = 0;
            double total_w_d = 0.0;
            double total_w   = 0.0;

            // GGGGG bucket: guess IS the answer → depth from this node = 1.
            for (uint16_t ai : buckets[PATTERN_SOLVED]) {
                const double w = weight_fn ? weight_fn(ai) : 1.0;
                total_w   += w;
                total_w_d += w * 1.0;
            }

            // Non-GGGGG buckets: depth = 1 (this guess) + sub-tree depth.
            for (Pattern p = 0; p < PATTERN_COUNT - 1; ++p) {
                const auto& bucket = buckets[p];
                if (bucket.empty()) continue;

                const auto sub = beam_eval(bucket);
                if (sub.worst_depth == EntropySolver::DEPTH_IMPOSSIBLE)
                    return {EntropySolver::DEPTH_IMPOSSIBLE, 1e18};

                worst = std::max(worst, 1 + sub.worst_depth);

                double bw = 0.0;
                for (uint16_t ai : bucket) bw += (weight_fn ? weight_fn(ai) : 1.0);
                total_w   += bw;
                total_w_d += bw * (1.0 + sub.mean_depth);
            }

            return {worst, total_w > 0.0 ? total_w_d / total_w : 0.0};
        };

        // Update best with a new (guess, worst, mean) triple if it's better.
        uint16_t best_gi    = WordList::NPOS;
        int      best_worst = EntropySolver::DEPTH_IMPOSSIBLE;
        double   best_mean  = 1e18;

        auto update_best = [&](uint16_t gi, int worst, double mean) {
            if (best_gi == WordList::NPOS) {
                best_gi = gi; best_worst = worst; best_mean = mean; return;
            }
            const bool is_gi   = std::ranges::binary_search(candidates, gi);
            const bool is_best = std::ranges::binary_search(candidates, best_gi);
            const bool better  =
                worst < best_worst ||
                (worst == best_worst && mean < best_mean - 1e-9) ||
                (worst == best_worst && std::abs(mean - best_mean) < 1e-9 &&
                 is_gi && !is_best) ||
                (worst == best_worst && std::abs(mean - best_mean) < 1e-9 &&
                 is_gi == is_best && gi < best_gi);
            if (better) { best_gi = gi; best_worst = worst; best_mean = mean; }
        };

        // Small sets: minimax finds the worst-case-optimal guess exhaustively.
        // We still call eval_guess on its result so children enter beam_cache.
        if (candidates.size() <= EntropySolver::MINIMAX_THRESHOLD) {
            constexpr int MINIMAX_BUDGET = 10;
            auto [gi_mm, wdepth] = solver.minimax_best_guess(candidates, MINIMAX_BUDGET);
            (void)wdepth;
            const uint16_t gi = (gi_mm != WordList::NPOS)
                                 ? gi_mm : solver.best_guess(candidates);
            auto [worst, mean] = eval_guess(gi);
            return beam_cache[candidates] = {gi, worst, mean};
        }

        // Large sets: beam search over top-K entropy guesses.
        for (uint16_t gi : top_k_guesses(candidates, beam_width)) {
            auto [worst, mean] = eval_guess(gi);
            if (worst != EntropySolver::DEPTH_IMPOSSIBLE)
                update_best(gi, worst, mean);
        }

        if (best_gi == WordList::NPOS) {
            // All beam guesses infeasible — shouldn't occur; fall back to
            // entropy rank-1 to avoid returning a broken tree.
            const auto fb = top_k_guesses(candidates, 1);
            best_gi = fb.empty() ? candidates[0] : fb[0];
        }

        return beam_cache[candidates] = {best_gi, best_worst, best_mean};
    }

    // -----------------------------------------------------------------------
    // build_from_eval — write DB nodes using beam_eval's cached decisions.
    //
    // DAG deduplication: if the same candidate set is reached via a second
    // tree path, we return the already-inserted node_id instead of creating
    // a duplicate subtree.  This keeps the DB compact even when beam search
    // creates many transpositions.
    // -----------------------------------------------------------------------
    uint32_t build_from_eval(const std::vector<uint16_t>& candidates, int depth) {
        if (auto it = dag_nodes.find(candidates); it != dag_nodes.end()) {
            ++dag_reuses;
            return it->second;
        }

        // Get best guess from eval cache.
        const uint16_t guess_idx = [&]() -> uint16_t {
            if (candidates.size() == 1) return candidates[0];
            const auto it = beam_cache.find(candidates);
            if (it == beam_cache.end())
                die("build_from_eval: candidate set not in beam_cache");
            return it->second.best_guess;
        }();

        if (guess_idx == WordList::NPOS)
            die("build_from_eval: beam_eval returned NPOS for non-trivial set");

        const uint32_t my_id = next_id++;
        // Clamp depth to uint8_t range (depths > 255 are theoretically impossible
        // for Wordle, but guard against it anyway).
        const auto depth_u8 = static_cast<uint8_t>(
            std::min(depth, static_cast<int>(std::numeric_limits<uint8_t>::max())));
        if (auto r = db.insert_node(my_id, guess_idx, depth_u8); !r) die(r.error());
        ++nodes_written;
        if (depth > max_depth_seen) max_depth_seen = depth;

        // Register before recursing so that any cycle (impossible in Wordle,
        // but safe to handle) would resolve to the current node.
        dag_nodes[candidates] = my_id;

        auto buckets = EntropySolver::partition(candidates, guess_idx, pm);
        for (Pattern p = 0; p < PATTERN_COUNT - 1; ++p) {
            const auto& bucket = buckets[p];
            if (bucket.empty()) continue;
            uint32_t child_id = build_from_eval(bucket, depth + 1);
            if (auto re = db.insert_edge(my_id, p, child_id); !re) die(re.error());
        }

        return my_id;
    }
};

// ---------------------------------------------------------------------------
// Evaluate mean/worst depth over answers list — no artificial round cap
// ---------------------------------------------------------------------------
struct EvalResult {
    double mean;
    int    worst;
    int    failures;   // words not solved (missing tree path)
    int    solved;
    // dist[depth] = count; depth 0 unused.  Size 16 is the effective max-depth
    // cap: words solved at depth ≥ 16 would appear as failures.  Current
    // worst-case (full-coverage DB) is 8 — well within bounds.
    std::array<int, 16> dist{};
};

static EvalResult
evaluate(const Database& db, const WordList& words, const WordList& answers) {
    double total = 0.0;
    EvalResult res{};

    for (auto& w : answers.span()) {
        auto idx = words.index_of(w.view());
        if (idx == WordList::NPOS) continue;

        uint32_t node  = Database::ROOT_ID;
        int      depth = 0;
        bool     solved = false;

        // Follow the tree until GGGGG or a missing edge (cap at dist array size)
        for (int round = 1; round < static_cast<int>(res.dist.size()); ++round) {
            auto info = db.node_info(node);
            if (!info) break;
            auto [word_idx, d] = *info;

            Pattern p = compute_pattern(words[word_idx].view(), w.view());
            depth = round;

            if (p == PATTERN_SOLVED) { solved = true; break; }

            auto nxt = db.next_node(node, p);
            if (!nxt) break;   // missing edge — tree incomplete for this word
            node = *nxt;
        }

        if (solved) {
            ++res.solved;
            total += depth;
            if (depth > res.worst) res.worst = depth;
            if (depth < static_cast<int>(res.dist.size())) res.dist[depth]++;
        } else {
            ++res.failures;
        }
    }

    res.mean = res.solved > 0 ? total / res.solved : 0.0;
    return res;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
    std::string words_path   = "data/words.txt";
    std::string answers_path = "data/answers.txt";
    std::string out_path     = "wordle.db";
    unsigned    nthreads     = std::thread::hardware_concurrency();

    std::vector<std::string_view> args{argv + 1, argv + argc};
    auto consume = [&](std::string_view flag) -> std::string_view {
        for (auto it = args.begin(); it != args.end(); ++it) {
            if (*it == flag && std::next(it) != args.end()) {
                auto val = *std::next(it);
                args.erase(it, it + 2);
                return val;
            }
        }
        return {};
    };

    // --full: build a tree that covers every word in the guess list as a
    // potential answer.  Overrides --answers default and switches strategy to
    // full-coverage-v1.  Explicit --answers or --strategy flags still win.
    bool full_mode = [&args]() {
        auto it = std::ranges::find(args, std::string_view{"--full"});
        if (it == args.end()) return false;
        args.erase(it);
        return true;
    }();

    std::string strategy    = full_mode ? "full-coverage-v1" : "answer-weighted-v2";
    std::string start_word;                           // empty = let solver choose
    double      answer_weight = 1000.0;               // multiplier for answer words
    std::size_t memo_limit    = 0;                    // 0 = memoization disabled
    std::size_t beam_width    = 1;                    // 1 = existing greedy/minimax path
    std::optional<std::string> answers_explicit;      // set if --answers was given

    if (auto v = consume("--words");         !v.empty()) words_path           = v;
    if (auto v = consume("--answers");       !v.empty()) answers_explicit     = std::string(v);
    if (auto v = consume("--output");        !v.empty()) out_path             = v;
    if (auto v = consume("--strategy");      !v.empty()) strategy             = v;
    if (auto v = consume("--start-word");    !v.empty()) start_word           = v;
    if (auto v = consume("--answer-weight"); !v.empty()) answer_weight = std::stod(std::string(v));
    // --memo-limit N: cache best_guess() for candidate sets of size ≤ N (K=1 path only).
    if (auto v = consume("--memo-limit");    !v.empty()) memo_limit = static_cast<std::size_t>(std::stoull(std::string(v)));
    // --beam-width K: try top-K entropy guesses at every node and keep the
    // one that minimises worst-case depth.  K=1 uses the existing greedy path.
    if (auto v = consume("--beam-width");    !v.empty()) beam_width = static_cast<std::size_t>(std::stoull(std::string(v)));

    // Reflect beam search in the strategy tag if the user hasn't overridden it.
    if (beam_width > 1 && strategy == (full_mode ? "full-coverage-v1" : "answer-weighted-v2"))
        strategy = std::format("beam-search-v1-k{}", beam_width);

    // Apply --full defaults: answers = all words, unless --answers was explicit
    if (full_mode) answers_path = answers_explicit.value_or(words_path);
    else if (answers_explicit)  answers_path = *answers_explicit;
    if (auto v = consume("--jobs");          !v.empty()) {
        nthreads = static_cast<unsigned>(std::stoi(std::string(v)));
        if (nthreads == 0) nthreads = std::max(1u, std::thread::hardware_concurrency());
    }

    // ── Word lists ────────────────────────────────────────────────────────
    std::print("loading words from {}... ", words_path);
    std::cout.flush();
    auto wl = WordList::load(words_path);
    if (!wl) die(wl.error());
    std::println("{} words", wl->size());

    std::print("loading answers from {}... ", answers_path);
    std::cout.flush();
    auto ans = WordList::load(answers_path);
    if (!ans) die(ans.error());
    std::println("{} answers", ans->size());

    // ── Pattern matrix ────────────────────────────────────────────────────
    const auto N = wl->size();
    std::print("building pattern matrix ({} × {} = {} entries, {} MB)... ",
        N, N,
        static_cast<uint64_t>(N) * N,
        static_cast<uint64_t>(N) * N / (1024 * 1024));
    std::cout.flush();
    auto t0 = Clock::now();
    auto pm = PatternMatrix::build(*wl, nthreads);
    std::println("done ({:.1f}s)", elapsed_s(t0));

    // ── Database ──────────────────────────────────────────────────────────
    std::print("creating {}... ", out_path);
    std::cout.flush();
    auto db = Database::create(out_path);
    if (!db) die(db.error());
    std::println("ok");

    // ── Weight function ───────────────────────────────────────────────────
    // Build a set of answer indices for O(1) membership test
    std::vector<uint16_t> answer_indices;
    for (auto& w : ans->span()) {
        auto idx = wl->index_of(w.view());
        if (idx != WordList::NPOS) answer_indices.push_back(idx);
    }
    std::ranges::sort(answer_indices);

    EntropySolver::WeightFn weight_fn;
    if (strategy == "entropy-greedy-v1") {
        weight_fn = {};  // uniform
    } else {
        // answer-weighted: curated answer words count answer_weight× more than others
        weight_fn = [&answer_indices, answer_weight](uint16_t idx) -> double {
            return std::ranges::binary_search(answer_indices, idx) ? answer_weight : 1.0;
        };
    }

    // ── Fixed start word (optional) ───────────────────────────────────────
    uint16_t fixed_root = WordList::NPOS;
    if (!start_word.empty()) {
        fixed_root = wl->index_of(start_word);
        if (fixed_root == WordList::NPOS)
            die(std::format("--start-word '{}' not found in word list", start_word));
        std::println("using forced start word: {}", start_word);
    }

    // ── Tree building ─────────────────────────────────────────────────────
    std::println("building decision tree (strategy={}, {} threads)...", strategy, nthreads);
    t0 = Clock::now();

    TreeBuilder builder{*wl, pm, *db, nthreads, weight_fn, fixed_root, memo_limit, beam_width};

    // For beam search in standard mode (not full-coverage): evaluate sub-trees
    // over answer words only.  This eliminates noise from non-answer candidates
    // and aligns the beam's quality estimates with the evaluation objective.
    const bool beam_standard_mode = (ans->size() != wl->size());
    if (beam_width > 1 && beam_standard_mode) {
        builder.beam_root_candidates = answer_indices;
    }

    builder.build();

    std::println("tree built in {:.1f}s  ({} nodes, max depth {})",
        elapsed_s(t0), builder.nodes_written, builder.max_depth_seen);

    // Report memo cache stats (only when caching was enabled).
    if (memo_limit > 0) {
        const auto total_lookups = builder.cache_hits + builder.cache_misses;
        const double hit_rate = total_lookups > 0
            ? 100.0 * static_cast<double>(builder.cache_hits) / static_cast<double>(total_lookups)
            : 0.0;
        std::println("memo cache: {} hits / {} lookups ({:.1f}%),  {} unique sets cached  (limit={})",
            builder.cache_hits, total_lookups, hit_rate,
            builder.memo_cache.size(), memo_limit);
    }

    // ── Evaluate (no depth cap — real worst case) ─────────────────────────
    std::print("evaluating against {} answers... ", ans->size());
    std::cout.flush();
    auto ev = evaluate(*db, *wl, *ans);
    std::println("worst={} mean={:.4f} solved={} failures={}",
        ev.worst, ev.mean, ev.solved, ev.failures);

    // ── Finalize ──────────────────────────────────────────────────────────
    const bool is_full_coverage = (ans->size() == wl->size());
    DbMetadata meta{
        .words_source    = "https://gist.github.com/SukkaW/92ff13af03a0117e5bafec6c7f7d6dce",
        .words_date      = "2026-06-07",
        .answers_source  = is_full_coverage
                         ? "all valid guess words (same as words source)"
                         : "cfreshman/a03ef2cba789d8cf00c08f767e0fad7b (original embed) + eithan/wordlelist (40 post-acquisition NYT additions)",
        .strategy        = strategy,
        .start_word      = {},
        .worst_case_depth = ev.worst,
        .mean_depth      = ev.mean,
        .total_nodes     = static_cast<int>(builder.nodes_written),
        .total_words     = static_cast<int>(wl->size()),
        .total_answers   = static_cast<int>(ans->size()),
    };

    if (auto ri = db->node_info(Database::ROOT_ID); ri)
        meta.start_word = std::string(wl->operator[](ri->first).view());

    if (auto r = db->finalize(meta); !r) die(r.error());

    std::println("done.");
    std::println("  start word  : {}", meta.start_word);
    std::println("  worst case  : {} guesses", ev.worst);
    std::println("  mean depth  : {:.4f} guesses", ev.mean);
    std::println("  failures    : {}", ev.failures);
    std::println("  total nodes : {}", builder.nodes_written);
    std::println("  distribution:");
    for (int i = 1; i < static_cast<int>(ev.dist.size()); ++i) {
        if (ev.dist[i] > 0) std::println("    {} guesses: {}", i, ev.dist[i]);
    }

    return 0;
}
