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

    TreeBuilder(const WordList& w, const PatternMatrix& p, Database& d, unsigned t,
                EntropySolver::WeightFn wf = {}, uint16_t fixed_root = WordList::NPOS)
        : words{w}, pm{p}, db{d}, solver{w, p, wf}, nthreads{t}, weight_fn{wf},
          fixed_root{fixed_root} {}

    // Entry point: build the entire tree wrapped in a single transaction.
    uint32_t build() {
        // Wrapping in one transaction turns ~16k individual auto-commits into a
        // single fsync — reduces build time by 100x–1000x on SQLite.
        if (auto r = db.begin_transaction(); !r) die(r.error());
        auto all_candidates = words.all_indices();
        uint32_t root = build_node(all_candidates, 1);
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
            guess_idx = solver.best_guess(candidates);
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
    std::optional<std::string> answers_explicit;      // set if --answers was given

    if (auto v = consume("--words");         !v.empty()) words_path           = v;
    if (auto v = consume("--answers");       !v.empty()) answers_explicit     = std::string(v);
    if (auto v = consume("--output");        !v.empty()) out_path             = v;
    if (auto v = consume("--strategy");      !v.empty()) strategy             = v;
    if (auto v = consume("--start-word");    !v.empty()) start_word           = v;
    if (auto v = consume("--answer-weight"); !v.empty()) answer_weight = std::stod(std::string(v));

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

    TreeBuilder builder{*wl, pm, *db, nthreads, weight_fn, fixed_root};
    builder.build();

    std::println("tree built in {:.1f}s  ({} nodes, max depth {})",
        elapsed_s(t0), builder.nodes_written, builder.max_depth_seen);

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
