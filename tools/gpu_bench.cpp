// gpu_bench.cpp — verify + benchmark the Metal GPU node-scorer vs the CPU.
//
// Scores all ~15k guesses against a candidate (answer) set both ways and:
//   • checks max_bucket matches exactly and entropy matches within tolerance,
//   • times repeated GPU dispatches vs the equivalent CPU loop.
//
// Builds and runs on any platform; without Metal (WP_HAVE_METAL undefined) it
// runs the CPU path only and reports that the GPU is unavailable.

#include "wordlist.hpp"
#include "pattern.hpp"
#include "solver.hpp"
#ifdef WP_HAVE_METAL
#include "gpu_score.hpp"
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <format>
#include <print>
#include <string>
#include <string_view>
#include <vector>

using namespace wp;
using Clock = std::chrono::steady_clock;

namespace {

struct CpuScore { std::uint32_t max_bucket; float entropy; };

// CPU reference: same computation the shader does, per guess.
std::vector<CpuScore> cpu_score_all(const PatternMatrix& pm, std::uint32_t N,
                                    const std::vector<uint16_t>& cand) {
    std::vector<CpuScore> out(N);
    const float total = static_cast<float>(cand.size());
    for (std::uint32_t g = 0; g < N; ++g) {
        std::array<std::uint16_t, PATTERN_COUNT> hist{};
        std::uint32_t maxb = 0;
        for (uint16_t a : cand) {
            std::uint16_t c = ++hist[pm.get(static_cast<uint16_t>(g), a)];
            if (c > maxb) maxb = c;
        }
        float H = 0.0f;
        for (std::uint16_t c : hist)
            if (c) { float pr = c / total; H -= pr * std::log2(pr); }
        out[g] = {maxb, H};
    }
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    std::string words_path = "data/words.txt";
    std::string answers_path = "data/answers.txt";
    int iters = 20;
    std::vector<std::string_view> args(argv + 1, argv + argc);
    for (std::size_t i = 0; i + 1 < args.size(); ++i) {
        if (args[i] == "--words")   words_path = args[i + 1];
        if (args[i] == "--answers") answers_path = args[i + 1];
        if (args[i] == "--iters")   iters = std::stoi(std::string(args[i + 1]));
    }

    auto wl = WordList::load(words_path);
    if (!wl) { std::println(stderr, "load words: {}", wl.error()); return 1; }
    auto ans = WordList::load(answers_path);
    if (!ans) { std::println(stderr, "load answers: {}", ans.error()); return 1; }

    auto pm = PatternMatrix::build(*wl);
    const std::uint32_t N = static_cast<std::uint32_t>(wl->size());

    std::vector<uint16_t> cand;
    for (auto& w : ans->span()) {
        auto idx = wl->index_of(w.view());
        if (idx != WordList::NPOS) cand.push_back(idx);
    }
    std::ranges::sort(cand);
    std::println("N={} candidates={} iters={}", N, cand.size(), iters);

    // ── CPU reference + timing ───────────────────────────────────────────────
    auto t0 = Clock::now();
    std::vector<CpuScore> cpu;
    for (int it = 0; it < iters; ++it) cpu = cpu_score_all(pm, N, cand);
    double cpu_ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count() / iters;
    std::println("CPU: {:.2f} ms/iter (score all {} guesses)", cpu_ms, N);

#ifdef WP_HAVE_METAL
    // Flatten the matrix into a contiguous row-major buffer for the GPU upload.
    static std::vector<Pattern> flat(static_cast<std::size_t>(N) * N);
    for (std::uint32_t g = 0; g < N; ++g)
        for (std::uint32_t a = 0; a < N; ++a)
            flat[static_cast<std::size_t>(g) * N + a] =
                pm.get(static_cast<uint16_t>(g), static_cast<uint16_t>(a));

    auto scorer = GpuScorer::create(flat.data(), N);
    if (!scorer) { std::println("GPU unavailable: {}", scorer.error()); return 0; }

    // Warm up (shader pipeline + first dispatch), then time.
    auto warm = scorer->score_all(cand);
    if (!warm) { std::println("GPU score failed: {}", warm.error()); return 1; }

    t0 = Clock::now();
    std::vector<GpuScorer::GuessScore> gpu;
    for (int it = 0; it < iters; ++it) {
        auto r = scorer->score_all(cand);
        if (!r) { std::println("GPU score failed: {}", r.error()); return 1; }
        gpu = std::move(*r);
    }
    double gpu_ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count() / iters;
    std::println("GPU: {:.2f} ms/iter  ({:.1f}x vs CPU)", gpu_ms, cpu_ms / gpu_ms);

    // ── Verify parity ────────────────────────────────────────────────────────
    int mb_mismatch = 0; double max_H_diff = 0.0; int H_mismatch = 0;
    for (std::uint32_t g = 0; g < N; ++g) {
        if (cpu[g].max_bucket != gpu[g].max_bucket) ++mb_mismatch;
        double d = std::abs(double(cpu[g].entropy) - double(gpu[g].entropy));
        max_H_diff = std::max(max_H_diff, d);
        if (d > 1e-3) ++H_mismatch;
    }
    std::println("verify: max_bucket mismatches = {}, entropy mismatches(>1e-3) = {}, "
                 "max |ΔH| = {:.6f}", mb_mismatch, H_mismatch, max_H_diff);
    if (mb_mismatch == 0 && H_mismatch == 0)
        std::println("PARITY OK — GPU matches CPU");
    else
        std::println("PARITY FAILED");
#else
    std::println("Metal not compiled in (WP_HAVE_METAL undefined) — CPU only.");
#endif
    return 0;
}
