#pragma once

// Progress — a tiny, dependency-free terminal progress reporter.
//
// Two modes:
//   • indeterminate: spinner + a running count + elapsed time + rate, used when
//     the total amount of work is not known in advance (e.g. the depth-first
//     decision-tree build, where the node count is only discovered as it runs).
//   • determinate:   a [#####-----] xx% bar, used when the total is known
//     (e.g. the per-answer evaluation loop).
//
// Output goes to stderr (keeping stdout clean for the build summary). Updates
// are time-throttled (~10 Hz) so they never dominate runtime. When stderr is
// NOT a TTY (redirected to a file/pipe) the in-place `\r` redraw is suppressed
// and occasional plain lines are emitted instead, so logs stay readable.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <format>
#include <string>
#include <string_view>

#include <unistd.h>  // isatty, STDERR_FILENO

namespace wp {

class Progress {
public:
    explicit Progress(std::string_view label, std::uint64_t total = 0)
        : label_{label}, total_{total},
          start_{Clock::now()}, last_draw_{start_},
          tty_{::isatty(STDERR_FILENO) != 0} {}

    // Report current progress (count of items done so far). Throttled.
    void update(std::uint64_t done) {
        const auto now = Clock::now();
        if (now - last_draw_ < kInterval) return;
        last_draw_ = now;
        draw(done, now, /*final=*/false);
    }

    // Final redraw + newline. Always emitted.
    void finish(std::uint64_t done) {
        draw(done, Clock::now(), /*final=*/true);
    }

private:
    using Clock = std::chrono::steady_clock;
    static constexpr auto kInterval = std::chrono::milliseconds(100);
    static constexpr const char* kSpin = "|/-\\";

    void draw(std::uint64_t done, Clock::time_point now, bool final) {
        const double secs = std::chrono::duration<double>(now - start_).count();
        const double rate = secs > 0 ? done / secs : 0.0;

        std::string line;
        if (total_ > 0) {
            const double frac = total_ ? double(done) / double(total_) : 0.0;
            const int pct = static_cast<int>(frac * 100.0 + 0.5);
            constexpr int W = 24;
            const int fill = static_cast<int>(frac * W + 0.5);
            std::string bar(fill, '#');
            bar.append(W - fill, '-');
            line = std::format("{}: [{}] {:3d}%  {}/{}  {:.1f}s",
                               label_, bar, pct, done, total_, secs);
        } else {
            const char spin = kSpin[(spin_++) & 3];
            line = std::format("{} {}  {} nodes  {:.1f}s  ({:.0f}/s)",
                               label_, spin, done, secs, rate);
        }

        if (tty_) {
            // In-place redraw; pad to clear any leftover from a longer prior line.
            std::fprintf(stderr, "\r%-72s", line.c_str());
            if (final) std::fputc('\n', stderr);
            std::fflush(stderr);
        } else if (final || (now - last_log_) >= std::chrono::seconds(2)) {
            // Redirected: emit occasional plain lines instead of \r churn.
            last_log_ = now;
            std::fprintf(stderr, "%s\n", line.c_str());
            std::fflush(stderr);
        }
    }

    std::string         label_;
    std::uint64_t       total_;
    Clock::time_point   start_;
    Clock::time_point   last_draw_;
    Clock::time_point   last_log_{};
    bool                tty_;
    unsigned            spin_{0};
};

} // namespace wp
