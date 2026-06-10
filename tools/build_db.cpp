// build_db — the Wordle decision-tree builder.
//
// Builds a tree that solves every answer within the smallest worst-case depth
// the search can prove reachable (5 for the curated answer set — provably
// optimal; 4 is impossible), then minimises the mean among such trees. A
// parallel opener sweep (feasibility-constrained, multi-threaded) picks the
// lowest-mean opener; the chosen tree is emitted and independently verified.
//
// NOTE on the worst-case-5 claim: minimising the *worst case* to 5 is provably
// optimal for the curated set. The *mean* (≈3.49) is NOT claimed optimal — the
// feasibility-constrained entropy policy is a strong heuristic, not exhaustive
// mean-optimal DP. So we avoid calling the builder itself "optimal".
//
// Flags:
//   --full             : cover every guess word as an answer (worst-case 7).
//   --start-word W     : force the opener (skips the sweep).
//   --lookahead K      : mean refinement on the chosen opener's tree (expand
//                        top-K feasible guesses per node; higher K → lower mean,
//                        slower). Default 1.
//   --sweep-lookahead K: lookahead used while ranking openers (default 1; the
//                        sweep runs it per-opener, so keep it cheap).
//   --top M            : sweep only the top-M openers by entropy (default 50;
//                        0 = all openers).
//   --jobs N           : worker threads for the parallel sweep / matrix build.
//
// Every build is independently verified by an evaluate() pass (the stored
// worst-case/mean come from walking the finished tree, not from the search),
// and produces a SQLite DB + a flat mmap binary (.bin) unless --no-binary.

#include "wordlist.hpp"
#include "pattern.hpp"
#include "solver.hpp"
#include "database.hpp"
#include "binarydb.hpp"
#include "progress.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <functional>
#include <iostream>
#include <limits>
#include <mutex>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <sys/stat.h>

using namespace wp;
using Clock = std::chrono::steady_clock;

namespace {

[[noreturn]] void die(std::string_view msg) {
    std::println(stderr, "build_db error: {}", msg);
    std::exit(1);
}

double elapsed_s(Clock::time_point t0) {
    return std::chrono::duration<double>(Clock::now() - t0).count();
}

// Derive an ISO-8601 (YYYY-MM-DD) date from a file's last-modified time, so the
// stamped metadata can never silently go stale. POSIX stat() because
// std::chrono::clock_cast is unavailable in this toolchain's libc++ on macOS.
std::string file_mtime_date(const std::string& path) {
    struct ::stat st{};
    if (::stat(path.c_str(), &st) != 0) return "unknown";
    const std::chrono::sys_seconds tp{std::chrono::seconds{st.st_mtime}};
    return std::format("{:%Y-%m-%d}", std::chrono::floor<std::chrono::days>(tp));
}

int max_bucket(const PatternMatrix& pm, const std::vector<uint16_t>& cand, uint16_t gi) {
    std::array<uint16_t, PATTERN_COUNT> cnt{};
    int mx = 0;
    for (uint16_t ai : cand) { int c = ++cnt[pm.get(gi, ai)]; if (c > mx) mx = c; }
    return mx;
}

double entropy_of(const PatternMatrix& pm, const std::vector<uint16_t>& cand, uint16_t gi) {
    std::array<int, PATTERN_COUNT> cnt{};
    for (uint16_t ai : cand) ++cnt[pm.get(gi, ai)];
    const double tot = static_cast<double>(cand.size());
    double H = 0.0;
    for (int c : cnt) if (c) { double p = c / tot; H -= p * std::log2(p); }
    return H;
}

// ---------------------------------------------------------------------------
// Builder: parallel feasibility-constrained opener sweep + emit.
//
// 1. Rank openers by entropy; sweep the top `top` of them in parallel (each
//    worker has its own EntropySolver / private feasibility memo, so no lock
//    contention) using tree_total_for_opener at `sweep_lookahead` — kept cheap
//    (default 1) because it runs once per candidate opener.
// 2. Pick the opener with the lowest total (mean), or honour --start-word.
// 3. Emit that opener's tree node-by-node via best_guess_feasible at
//    `emit_lookahead` — this runs only on the single chosen opener, so a higher
//    value here refines the final mean without paying it across the whole sweep.
//
// NOTE: the two lookaheads are deliberately separate. Using a high lookahead in
// the sweep multiplies its cost by ~the lookahead and the opener count — a
// 14,855-opener × lookahead-30 sweep is ~tens of hours. Keep the sweep at 1.
// ---------------------------------------------------------------------------
struct BuildResult {
    uint16_t      opener;
    std::uint64_t nodes;
    std::size_t   openers_swept   = 0;   // 0 if a forced opener skipped the sweep
    double        sweep_s         = 0.0;
    double        emit_s          = 0.0;
    // Aggregated solver instrumentation (sweep workers + emit solver).
    EntropySolver::Stats sweep_stats{};
    EntropySolver::Stats emit_stats{};
    std::size_t   emit_feas_memo  = 0;
    std::size_t   emit_choice_memo = 0;
    std::size_t   sweep_feas_cache = 0;  // shared feasibility cache size
};

void accumulate(EntropySolver::Stats& acc, const EntropySolver::Stats& s) {
    acc.feasible_calls += s.feasible_calls;
    acc.feasible_hits  += s.feasible_hits;
    acc.feasible_recur += s.feasible_recur;
    acc.partitions     += s.partitions;
    acc.choice_calls   += s.choice_calls;
    acc.choice_hits    += s.choice_hits;
}

BuildResult build_tree(const WordList& wl, const PatternMatrix& pm,
                       Database& db, const std::vector<uint16_t>& cand,
                       int worst_cap, std::size_t sweep_lookahead,
                       std::size_t emit_lookahead, std::size_t top,
                       uint16_t forced_root, unsigned nthreads) {
    const int n = static_cast<int>(cand.size());
    BuildResult result;

    uint16_t opener = forced_root;
    if (opener == WordList::NPOS) {
        // Rank candidate openers by entropy (best mean first); optionally cap.
        std::vector<std::pair<double, uint16_t>> openers;
        openers.reserve(wl.size());
        for (std::size_t g = 0; g < wl.size(); ++g) {
            const auto gi = static_cast<uint16_t>(g);
            if (max_bucket(pm, cand, gi) == n) continue;  // no progress
            openers.emplace_back(entropy_of(pm, cand, gi), gi);
        }
        std::ranges::sort(openers, [](auto& a, auto& b){
            if (a.first != b.first) return a.first > b.first;
            return a.second < b.second;
        });
        if (top > 0 && openers.size() > top) openers.resize(top);
        result.openers_swept = openers.size();

        std::println("  sweeping {} openers across {} threads (sweep lookahead {})...",
            openers.size(), nthreads, sweep_lookahead);
        Progress prog{"  opener sweep", static_cast<std::uint64_t>(openers.size())};
        const auto sweep_t0 = Clock::now();

        std::atomic<std::size_t> next{0};
        std::atomic<std::size_t> done{0};
        std::mutex best_mtx;
        int best_total = std::numeric_limits<int>::max();
        uint16_t best_opener = WordList::NPOS;

        // Shared feasibility cache: is_feasible() is opener-independent, so all
        // workers reuse each other's results instead of recomputing overlapping
        // subtrees (the dominant cost in the baseline sweep).
        FeasibilityCache feas_cache;

        auto worker = [&]() {
            EntropySolver solver{wl, pm};
            solver.set_feasibility_cache(&feas_cache);
            for (;;) {
                std::size_t i = next.fetch_add(1, std::memory_order_relaxed);
                if (i >= openers.size()) break;
                uint16_t gi = openers[i].second;
                int total = solver.tree_total_for_opener(cand, gi, worst_cap, sweep_lookahead);
                std::size_t d = done.fetch_add(1, std::memory_order_relaxed) + 1;
                prog.update(d);
                if (total != std::numeric_limits<int>::max()) {
                    std::lock_guard lock(best_mtx);
                    if (total < best_total) { best_total = total; best_opener = gi; }
                }
            }
            // Fold this worker's solver counters into the shared accumulator.
            std::lock_guard lock(best_mtx);
            accumulate(result.sweep_stats, solver.stats());
        };
        std::vector<std::thread> pool;
        for (unsigned t = 1; t < nthreads; ++t) pool.emplace_back(worker);
        worker();
        for (auto& th : pool) th.join();
        prog.finish(done.load());
        result.sweep_s = elapsed_s(sweep_t0);
        result.sweep_feas_cache = feas_cache.size();

        if (best_opener == WordList::NPOS)
            die(std::format("no opener solves the set within worst-case {}", worst_cap));
        opener = best_opener;
        std::println("  best opener: {} (sweep mean {:.4f})",
            wl[opener].view(), double(best_total) / n);
    } else {
        std::println("  forced opener: {}", wl[opener].view());
    }

    // ── Emit the chosen opener's tree (emit_lookahead applied here only) ──────
    EntropySolver solver{wl, pm};
    if (auto r = db.begin_transaction(); !r) die(r.error());
    Progress prog{"  building tree", std::uint64_t{0}, "nodes"};
    const auto emit_t0 = Clock::now();
    std::uint64_t nodes = 0;
    uint32_t next_id = 0;

    std::function<uint32_t(const std::vector<uint16_t>&, int, uint16_t)> emit =
        [&](const std::vector<uint16_t>& set, int budget, uint16_t forced) -> uint32_t {
        uint32_t id = next_id++;
        uint16_t guess = (forced != WordList::NPOS) ? forced
            : (set.size() == 1 ? set[0]
               : solver.best_guess_feasible(set, budget, emit_lookahead));
        if (guess == WordList::NPOS)
            die(std::format("no feasible guess (set {}, budget {})",
                            set.size(), budget));
        Depth round = static_cast<Depth>(worst_cap - budget + 1);
        if (auto r = db.insert_node(id, guess, round); !r) die(r.error());
        prog.update(++nodes);
        auto buckets = EntropySolver::partition(set, guess, pm);
        for (Pattern p = 0; p < PATTERN_COUNT; ++p) {
            if (p == PATTERN_SOLVED || buckets[p].empty()) continue;
            uint32_t child = emit(buckets[p], budget - 1, WordList::NPOS);
            if (auto r = db.insert_edge(id, p, child); !r) die(r.error());
        }
        return id;
    };
    emit(cand, worst_cap, opener);
    prog.finish(nodes);
    if (auto r = db.commit_transaction(); !r) die(r.error());
    result.emit_s          = elapsed_s(emit_t0);
    result.emit_stats      = solver.stats();
    result.emit_feas_memo  = solver.feas_memo_size();
    result.emit_choice_memo = solver.choice_memo_size();

    result.opener = opener;
    result.nodes  = nodes;
    return result;
}

// ---------------------------------------------------------------------------
// Evaluate mean/worst depth over the answers list — no artificial round cap.
// The stored metadata comes from this independent walk of the finished tree.
// ---------------------------------------------------------------------------
struct EvalResult {
    double mean;
    int    worst;
    int    failures;
    int    solved;
    std::array<int, 16> dist{};
};

EvalResult evaluate(const Database& db, const WordList& words, const WordList& answers) {
    double total = 0.0;
    EvalResult res{};
    Progress prog{"  evaluating", static_cast<std::uint64_t>(answers.span().size())};
    std::uint64_t processed = 0;

    for (auto& w : answers.span()) {
        auto idx = words.index_of(w.view());
        if (idx == WordList::NPOS) { prog.update(++processed); continue; }
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
        prog.update(++processed);
    }
    prog.finish(processed);
    res.mean = res.solved > 0 ? total / res.solved : 0.0;
    return res;
}

}  // namespace

int main(int argc, char** argv) {
    std::string words_path   = "data/words.txt";
    std::string answers_path = "data/answers.txt";
    std::string out_path     = "wordle.db";
    unsigned    nthreads     = std::thread::hardware_concurrency();
    if (nthreads == 0) nthreads = 1;

    std::vector<std::string_view> args{argv + 1, argv + argc};
    auto consume = [&](std::string_view flag) -> std::string_view {
        for (auto it = args.begin(); it != args.end(); ++it)
            if (*it == flag && std::next(it) != args.end()) {
                auto val = *std::next(it);
                args.erase(it, it + 2);
                return val;
            }
        return {};
    };
    auto has_flag = [&](std::string_view flag) {
        auto it = std::ranges::find(args, flag);
        if (it == args.end()) return false;
        args.erase(it);
        return true;
    };

    // ── Modifiers / flags ────────────────────────────────────────────────────
    const bool full_mode = has_flag("--full");
    const bool no_binary = has_flag("--no-binary");

    std::string start_word;
    std::string words_date_override;
    std::string binary_path;
    std::optional<std::string> answers_explicit;
    // Two independent lookaheads (see build_tree): the sweep ranks openers
    // cheaply (default 1, runs per-opener), the emit refines only the winner's
    // tree (default 1; raise for a lower mean at modest cost). --top caps the
    // sweep to the top-N entropy openers so the default build is ~1 min, not
    // hours — the best opener is reliably near the top of the entropy ranking.
    std::size_t emit_lookahead       = 1;          // --lookahead (winner only)
    std::size_t sweep_lookahead      = 1;          // --sweep-lookahead (per opener)
    std::size_t top                  = 50;         // --top (0 = all openers)
    int         target_depth         = 0;          // 0 = depth default
    bool        target_depth_explicit = false;

    if (auto v = consume("--words");         !v.empty()) words_path          = v;
    if (auto v = consume("--answers");       !v.empty()) answers_explicit    = std::string(v);
    if (auto v = consume("--output");        !v.empty()) out_path            = v;
    if (auto v = consume("--start-word");    !v.empty()) start_word          = v;
    if (auto v = consume("--date");          !v.empty()) words_date_override = v;
    if (auto v = consume("--binary");        !v.empty()) binary_path         = v;
    if (auto v = consume("--lookahead");       !v.empty()) emit_lookahead  = std::stoul(std::string(v));
    if (auto v = consume("--sweep-lookahead"); !v.empty()) sweep_lookahead = std::stoul(std::string(v));
    if (auto v = consume("--top");             !v.empty()) top             = std::stoul(std::string(v));
    if (auto v = consume("--target-depth");  !v.empty()) { target_depth = std::stoi(std::string(v)); target_depth_explicit = true; }
    if (auto v = consume("--jobs");          !v.empty()) {
        nthreads = static_cast<unsigned>(std::stoi(std::string(v)));
        if (nthreads == 0) nthreads = std::max(1u, std::thread::hardware_concurrency());
    }

    // Worst-case target defaults: curated answers = 5 (proven optimum),
    // full coverage = 7.
    if (!target_depth_explicit) target_depth = full_mode ? 7 : 5;

    // --full: answers = all words (unless --answers given explicitly).
    if (full_mode) answers_path = answers_explicit.value_or(words_path);
    else if (answers_explicit)  answers_path = *answers_explicit;

    // ── Word lists ────────────────────────────────────────────────────────────
    std::print("loading words from {}... ", words_path); std::cout.flush();
    auto wl = WordList::load(words_path);
    if (!wl) die(wl.error());
    std::println("{} words", wl->size());

    std::print("loading answers from {}... ", answers_path); std::cout.flush();
    auto ans = WordList::load(answers_path);
    if (!ans) die(ans.error());
    std::println("{} answers", ans->size());

    const auto wall_t0 = Clock::now();

    // ── Pattern matrix ──────────────────────────────────────────────────────────
    const auto N = wl->size();
    std::print("building pattern matrix ({} × {} = {} entries, {} MB)... ",
        N, N, static_cast<uint64_t>(N) * N, static_cast<uint64_t>(N) * N / (1024 * 1024));
    std::cout.flush();
    auto t0 = Clock::now();
    auto pm = PatternMatrix::build(*wl, nthreads);
    const double matrix_s = elapsed_s(t0);
    std::println("done ({:.1f}s)", matrix_s);

    // ── Database ──────────────────────────────────────────────────────────────
    std::print("creating {}... ", out_path); std::cout.flush();
    {
        std::error_code ec;
        std::filesystem::remove(out_path, ec);
        std::filesystem::remove(out_path + "-wal", ec);
        std::filesystem::remove(out_path + "-shm", ec);
    }
    auto db = Database::create(out_path);
    if (!db) die(db.error());
    std::println("ok");

    // Answer indices (the optimisation objective set).
    std::vector<uint16_t> answer_indices;
    for (auto& w : ans->span()) {
        auto idx = wl->index_of(w.view());
        if (idx != WordList::NPOS) answer_indices.push_back(idx);
    }
    std::ranges::sort(answer_indices);

    // For --full, the opener sweep over the 14,855-candidate set is very slow
    // (each per-opener evaluation is heavy), and `tares` is the known-good
    // worst-7 opener, so default to it unless the user forces another. The
    // curated-answers build is cheap enough to always sweep.
    if (start_word.empty() && full_mode)
        start_word = "tares";

    uint16_t fixed_root = WordList::NPOS;
    if (!start_word.empty()) {
        fixed_root = wl->index_of(start_word);
        if (fixed_root == WordList::NPOS)
            die(std::format("--start-word '{}' not found in word list", start_word));
    }

    if (binary_path.empty() && !no_binary) {
        auto dot = out_path.find_last_of('.');
        binary_path = (dot == std::string::npos ? out_path : out_path.substr(0, dot)) + ".bin";
    }

    // ── Build ───────────────────────────────────────────────────────────────────
    t0 = Clock::now();
    std::println("building decision tree (worst<={}, emit-lookahead={}, {} threads)...",
        target_depth, emit_lookahead, nthreads);
    auto res = build_tree(*wl, pm, *db, answer_indices, target_depth,
                          sweep_lookahead, emit_lookahead, top,
                          fixed_root, nthreads);
    const std::uint64_t nodes_written = res.nodes;
    const double build_s = elapsed_s(t0);
    // Label records *what was done*, not a quality claim: "minimax" = worst-case
    // minimised (provably optimal at 5); the mean is heuristic, not claimed optimal.
    const std::string strategy_label =
        std::format("minimax-worst{}-lookahead{}", target_depth, emit_lookahead);
    std::println("tree built in {:.1f}s  ({} nodes)", build_s, nodes_written);

    // ── Evaluate (independent verification → stored metadata) ─────────────────
    std::println("evaluating against {} answers...", ans->size());
    t0 = Clock::now();
    auto ev = evaluate(*db, *wl, *ans);
    const double eval_s = elapsed_s(t0);
    std::println("evaluation: worst={} mean={:.4f} solved={} failures={}",
        ev.worst, ev.mean, ev.solved, ev.failures);

    // ── Finalize ──────────────────────────────────────────────────────────────
    const bool is_full = (ans->size() == wl->size());
    const std::string words_date = !words_date_override.empty()
                                 ? words_date_override : file_mtime_date(words_path);
    DbMetadata meta{
        .words_source    = "https://gist.github.com/SukkaW/92ff13af03a0117e5bafec6c7f7d6dce",
        .words_date      = words_date,
        .answers_source  = is_full
                         ? "all valid guess words (same as words source)"
                         : "cfreshman/a03ef2cba789d8cf00c08f767e0fad7b (original embed) + eithan/wordlelist (40 post-acquisition NYT additions)",
        .strategy        = strategy_label,
        .start_word      = {},
        .worst_case_depth = ev.worst,
        .mean_depth      = ev.mean,
        .total_nodes     = static_cast<int>(nodes_written),
        .total_words     = static_cast<int>(wl->size()),
        .total_answers   = static_cast<int>(ans->size()),
    };
    if (auto ri = db->node_info(Database::ROOT_ID); ri)
        meta.start_word = std::string(wl->operator[](ri->first).view());
    t0 = Clock::now();
    if (auto r = db->finalize(meta); !r) die(r.error());
    const double finalize_s = elapsed_s(t0);

    double binary_s = 0.0;
    if (!binary_path.empty()) {
        std::print("exporting binary db to {}... ", binary_path); std::cout.flush();
        t0 = Clock::now();
        if (auto r = BinaryDb::export_from(*db, meta, binary_path); !r) die(r.error());
        binary_s = elapsed_s(t0);
        std::println("ok");
    }

    // Close the connection (finalize switched journal to DELETE) and remove any
    // residual WAL sidecars so the published .db is a single self-contained file.
    db = std::unexpected(std::string{});
    {
        std::error_code ec;
        std::filesystem::remove(out_path + "-wal", ec);
        std::filesystem::remove(out_path + "-shm", ec);
    }

    const double wall_s = elapsed_s(wall_t0);

    std::println("done.");
    std::println("  strategy    : {}", strategy_label);
    std::println("  start word  : {}", meta.start_word);
    std::println("  worst case  : {} guesses", ev.worst);
    std::println("  mean depth  : {:.4f} guesses", ev.mean);
    std::println("  failures    : {}", ev.failures);
    std::println("  total nodes : {}", nodes_written);
    std::println("  distribution:");
    for (int i = 1; i < static_cast<int>(ev.dist.size()); ++i)
        if (ev.dist[i] > 0) std::println("    {} guesses: {}", i, ev.dist[i]);

    // ── Builder performance metrics (issue #28) ──────────────────────────────
    // Aggregate solver work across the sweep + emit phases.
    EntropySolver::Stats agg = res.sweep_stats;
    accumulate(agg, res.emit_stats);
    const double feas_hit_rate = agg.feasible_calls
        ? double(agg.feasible_hits) / double(agg.feasible_calls) : 0.0;
    const double sweep_rate = (res.openers_swept && res.sweep_s > 0.0)
        ? double(res.openers_swept) / res.sweep_s : 0.0;
    const double emit_rate = res.emit_s > 0.0 ? double(nodes_written) / res.emit_s : 0.0;

    std::println("");
    std::println("--- builder performance ---");
    std::println("  phase times (s): matrix={:.2f} sweep={:.2f} emit={:.2f} "
                 "eval={:.2f} finalize={:.2f} binary={:.2f} build={:.2f} wall={:.2f}",
        matrix_s, res.sweep_s, res.emit_s, eval_s, finalize_s, binary_s, build_s, wall_s);
    std::println("  sweep: {} openers @ {:.1f}/s ({} threads)",
        res.openers_swept, sweep_rate, nthreads);
    std::println("  emit : {} nodes @ {:.0f}/s", nodes_written, emit_rate);
    std::println("  feasibility: {} calls, {:.1f}% memo-hit, {} recursions, {} partitions",
        agg.feasible_calls, feas_hit_rate * 100.0, agg.feasible_recur, agg.partitions);
    std::println("  choice memo: {} calls, {} hits; memo sizes feas={} choice={} shared_feas={}",
        agg.choice_calls, agg.choice_hits, res.emit_feas_memo, res.emit_choice_memo,
        res.sweep_feas_cache);

    // Machine-readable single line for tracking/diffing across runs. Stable
    // key=value schema; append-only. Grep-friendly prefix "WPMETRICS".
    std::println(
        "WPMETRICS schema=1 strategy={} start={} worst={} mean={:.4f} nodes={} "
        "answers={} words={} jobs={} top={} sweep_lookahead={} emit_lookahead={} "
        "openers_swept={} matrix_s={:.3f} sweep_s={:.3f} emit_s={:.3f} eval_s={:.3f} "
        "finalize_s={:.3f} binary_s={:.3f} build_s={:.3f} wall_s={:.3f} "
        "sweep_openers_per_s={:.2f} emit_nodes_per_s={:.1f} "
        "feas_calls={} feas_hits={} feas_recur={} feas_hit_rate={:.4f} "
        "partitions={} choice_calls={} choice_hits={} feas_memo={} choice_memo={} "
        "shared_feas_cache={}",
        strategy_label, meta.start_word, ev.worst, ev.mean, nodes_written,
        ans->size(), wl->size(), nthreads, top, sweep_lookahead, emit_lookahead,
        res.openers_swept, matrix_s, res.sweep_s, res.emit_s, eval_s,
        finalize_s, binary_s, build_s, wall_s,
        sweep_rate, emit_rate,
        agg.feasible_calls, agg.feasible_hits, agg.feasible_recur, feas_hit_rate,
        agg.partitions, agg.choice_calls, agg.choice_hits,
        res.emit_feas_memo, res.emit_choice_memo, res.sweep_feas_cache);
    return 0;
}
