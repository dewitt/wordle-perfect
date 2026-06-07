#include "wordlist.hpp"
#include "pattern.hpp"
#include "solver.hpp"
#include "database.hpp"

#include <algorithm>
#include <atomic>
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
    const WordList&      words;
    const PatternMatrix& pm;
    Database&            db;
    EntropySolver        solver;
    unsigned             nthreads;

    uint32_t next_id{0};
    uint64_t nodes_written{0};
    int      max_depth_seen{0};

    TreeBuilder(const WordList& w, const PatternMatrix& p, Database& d, unsigned t)
        : words{w}, pm{p}, db{d}, solver{w, p}, nthreads{t} {}

    // Entry point: build the entire tree, return root node id (always 0)
    uint32_t build() {
        auto all_candidates = words.all_indices();
        return build_node(all_candidates, 1);
    }

private:
    // Recursively build a subtree for `candidates` at `depth`.
    // Returns the node_id assigned to this node.
    uint32_t build_node(std::vector<uint16_t>& candidates, int depth) {
        uint32_t my_id = next_id++;

        // Choose best guess for this candidate set
        uint16_t guess_idx = (depth == 1)
            ? best_guess_parallel(candidates)   // parallelise the expensive root
            : solver.best_guess(candidates);

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

                    // Check if a given index is in the candidate set (sorted)
                    auto is_cand = [&](uint16_t gi) {
                        return std::ranges::binary_search(candidates, gi);
                    };

                    for (uint16_t gi = start; gi < end; ++gi) {
                        // Compute entropy
                        std::array<int, PATTERN_COUNT> counts{};
                        for (uint16_t ai : candidates) {
                            counts[pm.get(gi, ai)]++;
                        }
                        const double n = static_cast<double>(candidates.size());
                        double H = 0.0;
                        for (int c : counts) {
                            if (c > 0) {
                                double p = static_cast<double>(c) / n;
                                H -= p * std::log2(p);
                            }
                        }

                        bool gi_is_cand = is_cand(gi);
                        bool better =
                            H > best_H ||
                            (H == best_H && gi_is_cand && !best_is_cand) ||
                            (H == best_H && gi_is_cand == best_is_cand && gi < best_word);

                        if (better) {
                            best_H     = H;
                            best_word  = gi;
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
// Evaluate mean/worst depth over answers list
// ---------------------------------------------------------------------------
static std::pair<double, int>
evaluate(const Database& db, const WordList& words, const WordList& answers) {
    double total = 0.0;
    int    worst = 0;
    int    count = 0;

    for (auto& w : answers.span()) {
        auto idx = words.index_of(w.view());
        if (idx == WordList::NPOS) continue;
        ++count;

        uint32_t node  = Database::ROOT_ID;
        int      depth = 0;

        for (int round = 1; round <= 6; ++round) {
            auto info = db.node_info(node);
            if (!info) break;
            auto [word_idx, d] = *info;

            Pattern p = compute_pattern(words[word_idx].view(), w.view());
            depth = round;
            if (p == PATTERN_SOLVED) break;

            auto nxt = db.next_node(node, p);
            if (!nxt) break;
            node = *nxt;
        }

        total += depth;
        if (depth > worst) worst = depth;
    }

    return {count > 0 ? total / count : 0.0, worst};
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

    if (auto v = consume("--words");   !v.empty()) words_path   = v;
    if (auto v = consume("--answers"); !v.empty()) answers_path = v;
    if (auto v = consume("--output");  !v.empty()) out_path     = v;
    if (auto v = consume("--jobs");    !v.empty()) nthreads = static_cast<unsigned>(std::stoi(std::string(v)));

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

    // ── Tree building ─────────────────────────────────────────────────────
    std::println("building decision tree ({} threads)...", nthreads);
    t0 = Clock::now();

    TreeBuilder builder{*wl, pm, *db, nthreads};
    builder.build();

    std::println("tree built in {:.1f}s  ({} nodes, max depth {})",
        elapsed_s(t0), builder.nodes_written, builder.max_depth_seen);

    // ── Evaluate ──────────────────────────────────────────────────────────
    std::print("evaluating against {} answers... ", ans->size());
    std::cout.flush();
    auto [mean, worst] = evaluate(*db, *wl, *ans);
    std::println("worst={} mean={:.4f}", worst, mean);

    // ── Finalize ──────────────────────────────────────────────────────────
    DbMetadata meta{
        .words_source    = "https://gist.github.com/SukkaW/92ff13af03a0117e5bafec6c7f7d6dce",
        .words_date      = "2026-06-07",
        .answers_source  = "https://gist.github.com/cfreshman/a03ef2cba789d8cf00c08f767e0fad7b",
        .strategy        = "entropy-greedy-v1",
        .start_word      = {},
        .worst_case_depth = worst,
        .mean_depth      = mean,
        .total_nodes     = static_cast<int>(builder.nodes_written),
        .total_words     = static_cast<int>(wl->size()),
        .total_answers   = static_cast<int>(ans->size()),
    };

    if (auto ri = db->node_info(Database::ROOT_ID); ri)
        meta.start_word = std::string(wl->operator[](ri->first).view());

    if (auto r = db->finalize(meta); !r) die(r.error());

    std::println("done.");
    std::println("  start word  : {}", meta.start_word);
    std::println("  worst case  : {} guesses", worst);
    std::println("  mean depth  : {:.4f} guesses", mean);
    std::println("  total nodes : {}", builder.nodes_written);

    return 0;
}
