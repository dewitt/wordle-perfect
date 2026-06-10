// exact_mean.cpp — exact minimal-mean optimiser (branch-and-bound DP).
//
// Computes the provably minimal total/mean solve depth subject to a worst-case
// cap, via EntropySolver::min_total (transposition + alpha-beta). With a forced
// --start it fixes the opener; otherwise it searches all openers (expensive).
//
// Usage: exact_mean [--start WORD] [--max-depth D] [--words F] [--answers F]

#include "wordlist.hpp"
#include "pattern.hpp"
#include "solver.hpp"

#include <algorithm>
#include <chrono>
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
    std::vector<std::string_view> a(argv + 1, argv + argc);
    for (std::size_t i = 0; i + 1 < a.size(); ++i) {
        if (a[i] == "--words") words_path = a[i + 1];
        if (a[i] == "--answers") answers_path = a[i + 1];
        if (a[i] == "--start") start = a[i + 1];
        if (a[i] == "--max-depth") max_depth = std::stoi(std::string(a[i + 1]));
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

    t0 = Clock::now();
    int total = EntropySolver::MIN_TOTAL_INFEASIBLE;
    WordIndex root = WordList::NPOS;

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
