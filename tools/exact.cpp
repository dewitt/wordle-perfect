// exact.cpp — parallel worst-case-bounded opener search (issue #27).
//
// Demonstrates CPU-farm-style parallelism on the optimal-tree search. The
// natural top-level fan-out is over OPENERS: for each candidate first guess we
// compute the total depth of its feasibility-constrained tree (guaranteed
// worst-case <= D), and keep the opener with the lowest mean. These per-opener
// evaluations are independent, so they parallelise cleanly across a thread pool
// with work-stealing (atomic task index).
//
// Each worker uses its OWN EntropySolver instance: the feasibility memo is
// per-instance (not shared), which avoids lock contention entirely at the cost
// of some duplicated feasibility work. This is the embarrassingly-parallel
// regime above the first move.
//
// CAVEAT (known): best_guess_feasible's choice memo (feas_choice_) caches the
// first choice computed for a (set,budget), so a worker's REPORTED per-opener
// total can vary slightly with how earlier openers warmed that worker's memo.
// The headline (best opener + its tree) is always re-emitted and independently
// verified by the production CLI eval, so the SHIPPED result is exact; only the
// intermediate sweep totals are order-sensitive. A fully order-independent total
// would require clearing the choice memo per opener (issue #28).
//
// Usage:
//   exact [--answers F] [--words F] [--max-depth D] [--lookahead K]
//         [--jobs N] [--top M]
//     --top M  : only sweep the top-M openers by entropy (default: all that
//                split the set). M is the main speed/quality knob.

#include "wordlist.hpp"
#include "pattern.hpp"
#include "solver.hpp"
#include "database.hpp"
#include "binarydb.hpp"

#include <filesystem>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <functional>
#include <limits>
#include <mutex>
#include <print>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace wp;
using Clock = std::chrono::steady_clock;

namespace {

constexpr int INF = std::numeric_limits<int>::max();

double entropy_of(const PatternMatrix& pm, const std::vector<uint16_t>& cand,
                  uint16_t gi) {
    std::array<int, PATTERN_COUNT> cnt{};
    for (uint16_t ai : cand) ++cnt[pm.get(gi, ai)];
    const double tot = static_cast<double>(cand.size());
    double H = 0.0;
    for (int c : cnt) if (c) { double p = c / tot; H -= p * std::log2(p); }
    return H;
}

int max_bucket(const PatternMatrix& pm, const std::vector<uint16_t>& cand, uint16_t gi) {
    std::array<uint16_t, PATTERN_COUNT> cnt{};
    int mx = 0;
    for (uint16_t ai : cand) { int c = ++cnt[pm.get(gi, ai)]; if (c > mx) mx = c; }
    return mx;
}

}  // namespace

int main(int argc, char** argv) {
    std::string words_path = "data/words.txt";
    std::string answers_path = "data/answers.txt";
    int max_depth = 5;
    std::size_t lookahead = 1;
    std::size_t top = 0;  // 0 = all splitting openers
    unsigned nthreads = std::thread::hardware_concurrency();
    std::string emit_path;

    std::vector<std::string_view> args(argv + 1, argv + argc);
    for (std::size_t i = 0; i + 1 < args.size(); ++i) {
        if (args[i] == "--words")     words_path = args[i + 1];
        if (args[i] == "--answers")   answers_path = args[i + 1];
        if (args[i] == "--max-depth") max_depth = std::stoi(std::string(args[i + 1]));
        if (args[i] == "--lookahead") lookahead = std::stoul(std::string(args[i + 1]));
        if (args[i] == "--top")       top = std::stoul(std::string(args[i + 1]));
        if (args[i] == "--jobs")      nthreads = static_cast<unsigned>(std::stoi(std::string(args[i + 1])));
        if (args[i] == "--emit")      emit_path = args[i + 1];
    }
    if (nthreads == 0) nthreads = 1;

    auto wl = WordList::load(words_path);
    if (!wl) { std::println(stderr, "load words: {}", wl.error()); return 1; }
    auto ans = WordList::load(answers_path);
    if (!ans) { std::println(stderr, "load answers: {}", ans.error()); return 1; }

    auto t0 = Clock::now();
    auto pm = PatternMatrix::build(*wl);
    std::println("words={} answers={} max_depth={} lookahead={} jobs={}  (matrix {:.1f}s)",
        wl->size(), ans->size(), max_depth, lookahead, nthreads,
        std::chrono::duration<double>(Clock::now() - t0).count());

    std::vector<uint16_t> cand;
    for (auto& w : ans->span()) {
        auto idx = wl->index_of(w.view());
        if (idx != WordList::NPOS) cand.push_back(idx);
    }
    std::ranges::sort(cand);
    const int n = static_cast<int>(cand.size());

    // Rank openers by entropy (best mean candidates first); optionally keep top-M.
    std::vector<std::pair<double, uint16_t>> openers;
    openers.reserve(wl->size());
    for (std::size_t g = 0; g < wl->size(); ++g) {
        const auto gi = static_cast<uint16_t>(g);
        if (max_bucket(pm, cand, gi) == n) continue;  // no progress
        openers.emplace_back(entropy_of(pm, cand, gi), gi);
    }
    std::ranges::sort(openers, [](auto& a, auto& b){
        if (a.first != b.first) return a.first > b.first;
        return a.second < b.second;
    });
    if (top > 0 && openers.size() > top) openers.resize(top);
    std::println("sweeping {} openers across {} threads...", openers.size(), nthreads);

    // ── Parallel opener sweep (work-stealing via atomic index) ───────────────
    t0 = Clock::now();
    std::atomic<std::size_t> next{0};
    std::atomic<std::size_t> done{0};
    std::mutex best_mtx;
    int best_total = INF;
    uint16_t best_opener = WordList::NPOS;

    auto worker = [&]() {
        // Per-thread solver instance → private feasibility memo, no contention.
        EntropySolver solver{*wl, pm};
        for (;;) {
            std::size_t i = next.fetch_add(1, std::memory_order_relaxed);
            if (i >= openers.size()) return;
            uint16_t gi = openers[i].second;
            int total = solver.tree_total_for_opener(cand, gi, max_depth, lookahead);
            std::size_t d = done.fetch_add(1, std::memory_order_relaxed) + 1;
            if (total != INF) {
                std::lock_guard lock(best_mtx);
                if (total < best_total) {
                    best_total = total;
                    best_opener = gi;
                    std::println("  [{}/{}] {} -> total {} mean {:.4f}  (new best)",
                        d, openers.size(), (*wl)[gi].view(), total,
                        double(total) / n);
                }
            }
        }
    };

    std::vector<std::thread> pool;
    for (unsigned t = 1; t < nthreads; ++t) pool.emplace_back(worker);
    worker();  // main thread participates
    for (auto& th : pool) th.join();

    double s = std::chrono::duration<double>(Clock::now() - t0).count();
    if (best_opener == WordList::NPOS) {
        std::println("no feasible opener at worst-case {}", max_depth);
        return 1;
    }
    std::println("\nBEST: opener={} worst<={} total={} mean={:.4f}  "
        "({} openers swept, {:.1f}s, {} threads)",
        (*wl)[best_opener].view(), max_depth, best_total,
        double(best_total) / n, openers.size(), s, nthreads);

    // ── Optional: emit the best opener's tree to a DB for independent CLI eval ─
    if (!emit_path.empty()) {
        EntropySolver solver{*wl, pm};
        std::error_code ec;
        std::filesystem::remove(emit_path, ec);
        std::filesystem::remove(emit_path + "-wal", ec);
        std::filesystem::remove(emit_path + "-shm", ec);
        auto db = Database::create(emit_path);
        if (!db) { std::println(stderr, "create db: {}", db.error()); return 1; }
        if (auto r = db->begin_transaction(); !r) { std::println(stderr, "{}", r.error()); return 1; }

        uint32_t next_id = 0;
        // Recursive emitter: node guesses via best_guess_feasible (the same policy
        // tree_total_for_opener measured), root forced to best_opener.
        std::function<uint32_t(const std::vector<uint16_t>&, int, uint16_t)> emit =
            [&](const std::vector<uint16_t>& set, int budget, uint16_t forced) -> uint32_t {
            uint32_t id = next_id++;
            uint16_t guess = (forced != WordList::NPOS) ? forced
                : (set.size() == 1 ? set[0]
                   : solver.best_guess_feasible(set, budget, lookahead));
            if (guess == WordList::NPOS) {
                std::println(stderr, "emit: no feasible guess (size {}, budget {})",
                    set.size(), budget);
                std::exit(1);
            }
            uint8_t round = static_cast<uint8_t>(max_depth - budget + 1);
            if (auto r = db->insert_node(id, guess, round); !r) { std::println(stderr, "{}", r.error()); std::exit(1); }
            auto buckets = EntropySolver::partition(set, guess, pm);
            for (Pattern p = 0; p < PATTERN_COUNT; ++p) {
                if (p == PATTERN_SOLVED || buckets[p].empty()) continue;
                uint32_t child = emit(buckets[p], budget - 1, WordList::NPOS);
                if (auto r = db->insert_edge(id, p, child); !r) { std::println(stderr, "{}", r.error()); std::exit(1); }
            }
            return id;
        };
        emit(cand, max_depth, best_opener);
        if (auto r = db->commit_transaction(); !r) { std::println(stderr, "{}", r.error()); return 1; }

        DbMetadata meta{
            .words_source = "https://gist.github.com/SukkaW/92ff13af03a0117e5bafec6c7f7d6dce",
            .words_date = "2026-06-09",
            .answers_source = "curated answers (parallel exact opener sweep)",
            .strategy = std::format("exact-worst{}-lookahead{}", max_depth, lookahead),
            .start_word = std::string((*wl)[best_opener].view()),
            .worst_case_depth = max_depth,
            .mean_depth = double(best_total) / n,
            .total_nodes = static_cast<int>(next_id),
            .total_words = static_cast<int>(wl->size()),
            .total_answers = static_cast<int>(ans->size()),
        };
        if (auto r = db->finalize(meta); !r) { std::println(stderr, "{}", r.error()); return 1; }
        auto dot = emit_path.find_last_of('.');
        std::string bin = (dot == std::string::npos ? emit_path : emit_path.substr(0, dot)) + ".bin";
        if (auto r = BinaryDb::export_from(*db, meta, bin); !r)
            std::println(stderr, "binary export: {}", r.error());
        db = std::unexpected(std::string{});
        std::filesystem::remove(emit_path + "-wal", ec);
        std::filesystem::remove(emit_path + "-shm", ec);
        std::println("emitted DB to {} (+ {})", emit_path, bin);
    }
    return 0;
}
