// exact_mean.cpp — lowest-mean tree search (exhaustive branch-and-bound DP).
//
// Searches for the minimum total/mean solve depth it can find subject to a
// worst-case cap, via EntropySolver::min_total (transposition + alpha-beta). The
// result is best-known, not a verified global optimum (corroborated by Selby's
// wordle.cpp and a brute-force oracle — see BENCHMARKS.md). With a forced --start
// it fixes the opener; otherwise it searches all openers (expensive).
//
// Usage: exact_mean [--start WORD] [--max-depth D] [--words F] [--answers F]

#include "wordlist.hpp"
#include "pattern.hpp"
#include "solver.hpp"

#include <algorithm>
#include <chrono>
#include <functional>
#include <cstdint>
#include <print>
#include <string>
#include <string_view>
#include <vector>

using namespace wp;
using Clock = std::chrono::steady_clock;

int main(int argc, char** argv) {
    std::string words_path = "data/words.txt", answers_path = "data/answers.txt";
    std::string start;
    int max_depth = 5;
    bool probe = false;
    int verify = 0;  // if >0, cross-check min_total vs brute force on first N answers
    std::string worst_bucket;  // if set, isolate+solve the single largest bucket of this opener
    std::vector<std::string_view> a(argv + 1, argv + argc);
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i] == "--probe-buckets") probe = true;
        if (i + 1 >= a.size()) continue;
        if (a[i] == "--words") words_path = a[i + 1];
        if (a[i] == "--answers") answers_path = a[i + 1];
        if (a[i] == "--start") start = a[i + 1];
        if (a[i] == "--max-depth") max_depth = std::stoi(std::string(a[i + 1]));
        if (a[i] == "--verify") verify = std::stoi(std::string(a[i + 1]));
        if (a[i] == "--worst-bucket") worst_bucket = a[i + 1];
    }

    auto wl = WordList::load(words_path);
    if (!wl) { std::println(stderr, "{}", wl.error()); return 1; }
    auto ans = WordList::load(answers_path);
    if (!ans) { std::println(stderr, "{}", ans.error()); return 1; }
    auto t0 = Clock::now();
    auto pm = PatternMatrix::build(*wl);
    EntropySolver solver{*wl, pm};
    std::println("words={} answers={} max_depth={}  (matrix {:.1f}s)",
        wl->size(), ans->size(), max_depth,
        std::chrono::duration<double>(Clock::now() - t0).count());

    std::vector<WordIndex> cand;
    for (auto& w : ans->span()) {
        auto idx = wl->index_of(w.view());
        if (idx != WordList::NPOS) cand.push_back(idx);
    }
    std::ranges::sort(cand);
    const int n = static_cast<int>(cand.size());

    if (!worst_bucket.empty()) {
        // Isolate the single largest non-trivial bucket of `worst_bucket` and
        // time min_total on it alone — a fast iteration loop for tuning the
        // large-bucket search. Also reports the greedy upper bound (the seed)
        // vs the exact optimum, to gauge seed quality.
        auto gi = wl->index_of(worst_bucket);
        if (gi == WordList::NPOS) { std::println(stderr, "opener not found"); return 1; }
        auto buckets = EntropySolver::partition(cand, gi, pm);
        Pattern bestp = 0; std::size_t bestsz = 0;
        for (Pattern p = 0; p < PATTERN_COUNT; ++p) {
            if (p == PATTERN_SOLVED) continue;
            if (buckets[p].size() > bestsz) { bestsz = buckets[p].size(); bestp = p; }
        }
        (void)bestp; (void)bestsz;
        // Solve every bucket of size >= 100 in isolation, DESCENDING by size,
        // each on a FRESH solver (cold memo) so per-bucket times are comparable.
        std::vector<std::pair<int, Pattern>> big;
        for (Pattern p = 0; p < PATTERN_COUNT; ++p) {
            if (p == PATTERN_SOLVED) continue;
            if (buckets[p].size() >= 100)
                big.emplace_back(static_cast<int>(buckets[p].size()), p);
        }
        std::ranges::sort(big, [](auto& x, auto& y){ return x.first > y.first; });
        std::println("opener {}: {} buckets of size>=100 (depth budget {})",
            worst_bucket, big.size(), max_depth - 1);
        std::fflush(stdout);
        for (auto& [sz, p] : big) {
            // Show a sample of the bucket's words to reveal its structure.
            std::string sample;
            for (std::size_t i = 0; i < buckets[p].size() && i < 20; ++i)
                sample += std::string((*wl)[buckets[p][i]].view()) + " ";
            std::println("  size={:4d} sample: {}{}", sz, sample,
                buckets[p].size() > 20 ? "..." : "");
            std::fflush(stdout);
            EntropySolver fresh{*wl, pm};
            const int ub = fresh.greedy_total(buckets[p], max_depth - 1);
            std::println("  size={:4d}  greedyUB={}  (searching...)", sz, ub);
            std::fflush(stdout);
            auto bt0 = Clock::now();
            int mt = fresh.min_total(buckets[p], max_depth - 1);
            double s = std::chrono::duration<double>(Clock::now() - bt0).count();
            const auto& st = fresh.stats();
            std::println("  size={:4d}  greedyUB={:5d}  min_total={:5d}  ({:7.2f}s)  "
                "nodes={} memo_hits={} memo={}",
                sz, ub, mt, s, st.mintotal_calls, st.mintotal_hits, fresh.tot_memo_size());
            std::fflush(stdout);
        }
        return 0;
    }

    if (verify > 0) {
        // Independent brute force with only the 2k-1 admissible alpha-beta —
        // shares no machinery with min_total. Cross-check on the first N answers.
        std::function<int(std::span<const WordIndex>, int, int)> brute =
            [&](std::span<const WordIndex> c, int d, int bound) -> int {
            const int m = static_cast<int>(c.size());
            if (m == 0) return 0;
            if (m == 1) return 1;
            if (d <= 1) return EntropySolver::MIN_TOTAL_INFEASIBLE;
            if (2 * m - 1 >= bound) return EntropySolver::MIN_TOTAL_INFEASIBLE;
            int best = bound;
            for (WordIndex gi = 0; gi < static_cast<WordIndex>(wl->size()); ++gi) {
                auto bk = EntropySolver::partition(c, gi, pm);
                if (bk[PATTERN_SOLVED].size() == static_cast<std::size_t>(m)) continue;
                long fl = m;
                for (Pattern p = 0; p < PATTERN_COUNT; ++p) {
                    if (p == PATTERN_SOLVED) continue;
                    const long sz = static_cast<long>(bk[p].size());
                    fl += (sz >= 2) ? 2 * sz - 1 : sz;
                }
                if (fl >= best) continue;
                bool ok = true; long tot = m;
                for (Pattern p = 0; p < PATTERN_COUNT && ok; ++p) {
                    if (p == PATTERN_SOLVED || bk[p].empty()) continue;
                    int sub = (bk[p].size() == 1) ? 1 : brute(bk[p], d - 1, best - static_cast<int>(tot));
                    if (sub >= EntropySolver::MIN_TOTAL_INFEASIBLE) { ok = false; break; }
                    tot += sub;
                    if (tot >= best) { ok = false; break; }
                }
                if (ok && tot < best) best = static_cast<int>(tot);
            }
            return (best < bound) ? best : EntropySolver::MIN_TOTAL_INFEASIBLE;
        };
        std::vector<WordIndex> sub(cand.begin(),
            cand.begin() + std::min<std::size_t>(verify, cand.size()));
        EntropySolver fresh{*wl, pm};
        int mt = fresh.min_total(sub, max_depth);
        int bf = brute(sub, max_depth, EntropySolver::MIN_TOTAL_INFEASIBLE);
        std::println("VERIFY n={} min_total={} brute={} {}",
            sub.size(), mt, bf, (mt == bf ? "OK" : "*** MISMATCH ***"));
        return 0;
    }

    t0 = Clock::now();
    int total = EntropySolver::MIN_TOTAL_INFEASIBLE;
    WordIndex root = WordList::NPOS;

    if (probe && !start.empty()) {
        // Partition on `start` and time min_total per non-solved bucket so we
        // can localise the cost. Process buckets ascending by size.
        auto gi = wl->index_of(start);
        if (gi == WordList::NPOS) { std::println(stderr, "start not found"); return 1; }
        auto buckets = EntropySolver::partition(cand, gi, pm);
        std::vector<std::pair<int, Pattern>> order;
        for (Pattern p = 0; p < PATTERN_COUNT; ++p) {
            if (p == PATTERN_SOLVED || buckets[p].size() <= 1) continue;
            order.emplace_back(static_cast<int>(buckets[p].size()), p);
        }
        std::ranges::sort(order);
        std::println("opener {} -> {} non-trivial buckets", start, order.size());
        {
            std::string sizes;
            for (auto& [sz, p] : order) sizes += std::to_string(sz) + " ";
            std::println("bucket sizes: {}", sizes);
            std::fflush(stdout);
        }
        // Account for EVERY word's depth, matching min_total's convention:
        //   • opener guess: 1 per word                              → +n
        //   • SOLVED bucket (opener == answer): already covered by the +n
        //   • singleton bucket: the word is determined but still needs one more
        //     guess to enter it → cost 2 → +1 beyond the +n already counted
        //   • non-trivial bucket: + min_total(bucket, depth-1)
        // The earlier version skipped the singleton +1, under-counting the total
        // by exactly the number of singleton buckets.
        int run = n;
        int singletons = 0;
        for (Pattern p = 0; p < PATTERN_COUNT; ++p) {
            if (p == PATTERN_SOLVED) continue;
            if (buckets[p].size() == 1) ++singletons;
        }
        run += singletons;  // each singleton word needs a 2nd guess
        for (auto& [sz, p] : order) {
            auto bt0 = Clock::now();
            int sub = solver.min_total(buckets[p], max_depth - 1);
            double bs = std::chrono::duration<double>(Clock::now() - bt0).count();
            std::println("  bucket size={:4d}  min_total={:5d}  ({:6.2f}s)  memo={}",
                sz, sub, bs, solver.choice_memo_size());
            std::fflush(stdout);
            run += sub;
        }
        std::println("probe done: opener {} singletons={} total={} mean={:.4f}",
            start, singletons, run, double(run) / n);
        return 0;
    }

    if (!start.empty()) {
        // Fix the opener: total = n + Σ over the opener's non-solved buckets of
        // min_total(bucket, max_depth-1).
        auto gi = wl->index_of(start);
        if (gi == WordList::NPOS) { std::println(stderr, "start not found"); return 1; }
        auto buckets = EntropySolver::partition(cand, gi, pm);
        total = n;
        for (Pattern p = 0; p < PATTERN_COUNT && total < EntropySolver::MIN_TOTAL_INFEASIBLE; ++p) {
            if (p == PATTERN_SOLVED || buckets[p].empty()) continue;
            int sub = (buckets[p].size() == 1) ? 1
                    : solver.min_total(buckets[p], max_depth - 1);
            if (sub >= EntropySolver::MIN_TOTAL_INFEASIBLE) { total = EntropySolver::MIN_TOTAL_INFEASIBLE; break; }
            total += sub;
        }
        root = gi;
    } else {
        total = solver.min_total(cand, max_depth);
        root = solver.optimal_guess(cand, max_depth);
    }

    double s = std::chrono::duration<double>(Clock::now() - t0).count();
    if (total >= EntropySolver::MIN_TOTAL_INFEASIBLE) {
        std::println("INFEASIBLE at worst<={}  ({:.1f}s)", max_depth, s);
    } else {
        std::println("OPTIMAL-MEAN: worst<={} total={} mean={:.4f} root={}  ({:.1f}s)",
            max_depth, total, double(total) / n,
            root == WordList::NPOS ? "?" : std::string((*wl)[root].view()), s);
    }
    return 0;
}
