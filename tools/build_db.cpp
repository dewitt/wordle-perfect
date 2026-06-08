#include "wordlist.hpp"
#include "pattern.hpp"
#include "solver.hpp"
#include "database.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <future>
#include <iostream>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <sys/stat.h>

using namespace wp;
using Clock = std::chrono::steady_clock;

[[noreturn]] static void die(std::string_view msg) {
    std::println(stderr, "build_db error: {}", msg);
    std::exit(1);
}

static double elapsed_s(Clock::time_point t0) {
    return std::chrono::duration<double>(Clock::now() - t0).count();
}

// Derive an ISO-8601 (YYYY-MM-DD) date from a file's last-modified time.
// Used to stamp the word-list retrieval date into DB metadata so it can never
// silently go stale. Returns "unknown" if the file can't be stat'd.
//
// Uses POSIX stat() rather than std::chrono::clock_cast: the latter is not
// available in the Apple-clang libc++ this project builds against on macOS
// (the same toolchain caveat documented in flake.nix).
static std::string file_mtime_date(const std::string& path) {
    struct ::stat st{};
    if (::stat(path.c_str(), &st) != 0) return "unknown";
    const std::chrono::sys_seconds tp{std::chrono::seconds{st.st_mtime}};
    return std::format("{:%Y-%m-%d}", std::chrono::floor<std::chrono::days>(tp));
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
    int                     target_depth{6};  // worst-case depth we try to hit
    int                     min_escalation_depth{2};  // don't escalate above this
    std::size_t             beam_width{EntropySolver::DEFAULT_BEAM_WIDTH};

    uint32_t next_id{0};
    uint64_t nodes_written{0};
    int      max_depth_seen{0};
    uint64_t minimax_calls{0};

    TreeBuilder(const WordList& w, const PatternMatrix& p, Database& d, unsigned t,
                EntropySolver::WeightFn wf = {}, uint16_t fixed_root = WordList::NPOS,
                int target_depth = 6, int min_escalation_depth = 2,
                std::size_t beam_width = EntropySolver::DEFAULT_BEAM_WIDTH)
        : words{w}, pm{p}, db{d}, solver{w, p, wf}, nthreads{t}, weight_fn{wf},
          fixed_root{fixed_root}, target_depth{target_depth},
          min_escalation_depth{min_escalation_depth}, beam_width{beam_width} {}

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
        } else {
            // Budget-aware selection. Remaining budget is target_depth - (depth-1)
            // guesses from this node onward. Greedy is used unless it would blow
            // the budget, in which case we escalate to minimax to break the
            // repeated-letter trap clusters that leave depth-6 paths.
            const int budget = std::max(1, target_depth + 1 - depth);
            bool escalated = false;
            auto mm_t0 = Clock::now();
            if (depth < min_escalation_depth) {
                // Too shallow to escalate (very large sets, costly probe with
                // little payoff): take the plain greedy guess.
                guess_idx = solver.best_guess(candidates);
            } else {
                guess_idx = solver.best_guess_within_budget(
                    candidates, budget, &escalated,
                    EntropySolver::ESCALATE_MAX_CANDIDATES, beam_width);
            }
            if (escalated) {
                ++minimax_calls;
                double mm_e = elapsed_s(mm_t0);
                if (mm_e > 0.05 || minimax_calls <= 5) {
                    std::println(stderr, "[mm #{} d={} c={} b={} t={:.3f}s]",
                        minimax_calls, depth, candidates.size(), budget, mm_e);
                }
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

        // Shared walk: generous cap so deep-but-valid paths report their true
        // depth instead of being misclassified as failures.
        auto outcome = walk_target(db, words, w.view(),
                                   static_cast<int>(res.dist.size()) - 1);

        if (outcome.solved()) {
            ++res.solved;
            total += outcome.depth;
            if (outcome.depth > res.worst) res.worst = outcome.depth;
            if (outcome.depth < static_cast<int>(res.dist.size()))
                res.dist[outcome.depth]++;
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

    std::string strategy    = full_mode ? "full-coverage-v2" : "answer-weighted-beam-v3";
    std::string start_word;                           // empty = let solver choose
    double      answer_weight = 1000.0;               // multiplier for answer words
    std::optional<std::string> answers_explicit;      // set if --answers was given
    std::string words_date_override;                  // --date override (ISO 8601)
    // Worst-case depth the builder aims for. Default 6 (standard Wordle); the
    // builder escalates to minimax only at nodes where greedy would exceed this.
    // Full-coverage builds need a looser target since some obscure guess-only
    // words are genuinely unreachable in 6.
    int target_depth = full_mode ? 8 : 6;
    // Minimum tree depth at which the budget-aware beam/minimax escalation runs.
    // Escalating at depth 2 (very large candidate sets) is the dominant build
    // cost; default to 3 where the trap clusters are already isolated.
    int min_escalation_depth = 2;
    std::size_t beam_width = EntropySolver::DEFAULT_BEAM_WIDTH;

    if (auto v = consume("--words");         !v.empty()) words_path           = v;
    if (auto v = consume("--answers");       !v.empty()) answers_explicit     = std::string(v);
    if (auto v = consume("--output");        !v.empty()) out_path             = v;
    if (auto v = consume("--strategy");      !v.empty()) strategy             = v;
    if (auto v = consume("--start-word");    !v.empty()) start_word           = v;
    if (auto v = consume("--date");          !v.empty()) words_date_override  = v;
    if (auto v = consume("--target-depth");  !v.empty()) target_depth = std::stoi(std::string(v));
    if (auto v = consume("--min-escalation-depth"); !v.empty()) min_escalation_depth = std::stoi(std::string(v));
    if (auto v = consume("--beam-width"); !v.empty()) beam_width = static_cast<std::size_t>(std::stoul(std::string(v)));
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
    // Start from a clean slate: building over a populated DB would hit a UNIQUE
    // constraint on nodes.id. Remove any prior artifact (and its WAL sidecars)
    // so a rebuild to the same path always succeeds.
    {
        std::error_code ec;
        std::filesystem::remove(out_path, ec);
        std::filesystem::remove(out_path + "-wal", ec);
        std::filesystem::remove(out_path + "-shm", ec);
    }
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

    TreeBuilder builder{*wl, pm, *db, nthreads, weight_fn, fixed_root,
                        target_depth, min_escalation_depth, beam_width};
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
    // Word-list date: explicit --date wins; otherwise derive from the words
    // file mtime so the metadata can never silently go stale (issue #20).
    const std::string words_date = !words_date_override.empty()
                                 ? words_date_override
                                 : file_mtime_date(words_path);

    DbMetadata meta{
        .words_source    = "https://gist.github.com/SukkaW/92ff13af03a0117e5bafec6c7f7d6dce",
        .words_date      = words_date,
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
