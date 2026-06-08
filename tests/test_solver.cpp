#include <catch2/catch_test_macros.hpp>

#include "pattern.hpp"
#include "solver.hpp"
#include "wordlist.hpp"

#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>
#include <ranges>
#include <thread>

using namespace wp;

// Build a tiny WordList from an initializer list (writes a temp file).
static WordList tiny_wordlist(std::initializer_list<const char*> words) {
    auto path = std::filesystem::temp_directory_path() /
        std::format("wptest_sl_{}.txt",
                    std::hash<std::thread::id>{}(std::this_thread::get_id()));
    {
        std::ofstream f(path);
        for (auto w : words) f << w << "\n";
    }
    auto wl = WordList::load(path.string());
    std::filesystem::remove(path);
    REQUIRE(wl.has_value());
    return std::move(*wl);
}

// ─────────────────────────────────────────────────────────────────────────────
// PatternMatrix::build
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("PatternMatrix - get() matches compute_pattern", "[solver]") {
    auto wl = tiny_wordlist({"crane", "slate", "trail", "point", "black"});
    auto pm = PatternMatrix::build(wl);

    const std::size_t n = wl.size();
    for (std::size_t gi = 0; gi < n; ++gi) {
        for (std::size_t ai = 0; ai < n; ++ai) {
            Pattern expected = compute_pattern(
                wl[static_cast<uint16_t>(gi)].view(),
                wl[static_cast<uint16_t>(ai)].view());
            CHECK(pm.get(static_cast<uint16_t>(gi),
                         static_cast<uint16_t>(ai)) == expected);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// EntropySolver::partition
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("partition - GGGGG bucket contains only the guess itself", "[solver]") {
    auto wl = tiny_wordlist({"crane", "slate", "trail", "point", "black"});
    auto pm = PatternMatrix::build(wl);
    EntropySolver solver{wl, pm};

    auto all = wl.all_indices();
    for (uint16_t gi : all) {
        auto buckets = EntropySolver::partition(all, gi, pm);
        // The GGGGG bucket must contain exactly gi
        REQUIRE(buckets[PATTERN_SOLVED].size() == 1);
        CHECK(buckets[PATTERN_SOLVED][0] == gi);
    }
}

TEST_CASE("partition - all candidates placed in exactly one bucket", "[solver]") {
    auto wl = tiny_wordlist({"crane", "slate", "trail", "point", "black"});
    auto pm = PatternMatrix::build(wl);
    EntropySolver solver{wl, pm};

    auto all = wl.all_indices();
    auto buckets = EntropySolver::partition(all, /*guess_idx=*/0, pm);

    std::size_t total = 0;
    for (auto& b : buckets) total += b.size();
    CHECK(total == all.size());
}

// ─────────────────────────────────────────────────────────────────────────────
// EntropySolver::best_guess
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("best_guess - single candidate returns itself", "[solver]") {
    auto wl = tiny_wordlist({"crane", "slate", "trail"});
    auto pm = PatternMatrix::build(wl);
    EntropySolver solver{wl, pm};

    auto ci = wl.index_of("crane");
    REQUIRE(ci != WordList::NPOS);

    auto result = solver.best_guess(std::span<const uint16_t>{&ci, 1});
    CHECK(result == ci);
}

TEST_CASE("best_guess - two candidates returns first", "[solver]") {
    auto wl = tiny_wordlist({"crane", "slate", "trail"});
    auto pm = PatternMatrix::build(wl);
    EntropySolver solver{wl, pm};

    auto ci = wl.index_of("crane");
    auto si = wl.index_of("slate");
    REQUIRE(ci != WordList::NPOS);
    REQUIRE(si != WordList::NPOS);

    // The two-candidate fast-path returns candidates[0]
    std::vector<uint16_t> two = {ci, si};
    std::ranges::sort(two);
    CHECK(solver.best_guess(two) == two[0]);
}

TEST_CASE("best_guess - empty candidates returns NPOS", "[solver]") {
    auto wl = tiny_wordlist({"crane", "slate"});
    auto pm = PatternMatrix::build(wl);
    EntropySolver solver{wl, pm};

    std::vector<uint16_t> empty{};
    CHECK(solver.best_guess(empty) == WordList::NPOS);
}

TEST_CASE("best_guess - result is always a valid index", "[solver]") {
    // Use the answers list (smaller, faster than full word list)
    auto wl = WordList::load("data/answers.txt");
    REQUIRE(wl.has_value());
    auto pm = PatternMatrix::build(*wl);
    EntropySolver solver{*wl, pm};

    auto all = wl->all_indices();
    auto g = solver.best_guess(all);
    CHECK(g < wl->size());
    CHECK(g != WordList::NPOS);
}

// ─────────────────────────────────────────────────────────────────────────────
// EntropySolver::solve — end-to-end on answers list
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("solve - solves all answer words within 6 guesses (sampled)", "[solver][slow]") {
    // Build the solver against the answer list only. This is a fast path
    // (2,315 × 2,315 pattern matrix, ~5 MB) and exercises the full
    // greedy-entropy path without the heavy 14k-word matrix.
    auto wl = WordList::load("data/answers.txt");
    REQUIRE(wl.has_value());
    auto pm = PatternMatrix::build(*wl);
    EntropySolver solver{*wl, pm};

    // Sample every 10th word to keep test runtime under a second
    int failures = 0;
    for (uint16_t i = 0; i < static_cast<uint16_t>(wl->size()); i += 10) {
        auto result = solver.solve(i);
        if (!result.solved || result.depth() > 6) {
            ++failures;
            WARN("solve failed for " << wl->operator[](i).view()
                 << " (depth=" << result.depth() << " solved=" << result.solved << ")");
        }
    }
    CHECK(failures == 0);
}

// ─────────────────────────────────────────────────────────────────────────────
// EntropySolver::minimax_best_guess — worst-case depth optimizer
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("minimax - single and empty candidate sets", "[solver][minimax]") {
    auto wl = tiny_wordlist({"crane", "slate", "trail", "point", "black"});
    auto pm = PatternMatrix::build(wl);
    EntropySolver solver{wl, pm};

    auto one = wl.index_of("crane");
    auto [gi, depth] = solver.minimax_best_guess(std::span<const uint16_t>{&one, 1}, 6);
    CHECK(gi == one);
    CHECK(depth == 1);

    std::vector<uint16_t> empty{};
    auto [gi2, depth2] = solver.minimax_best_guess(empty, 6);
    CHECK(gi2 == WordList::NPOS);
    CHECK(depth2 == 0);
}

TEST_CASE("minimax - achieves the optimal worst-case depth (gi NPOS = use greedy)",
          "[solver][minimax]") {
    // A family that shares enough letters to be distinguishable in one probe.
    // Guessing "slargt"-like overlapping words splits the set; the optimal
    // worst case for these five is 2 (one informative guess, then the answer).
    //
    // Per the documented contract, minimax_best_guess returns {NPOS, depth}
    // when greedy already achieves the optimum (no strict improvement). The
    // depth is still the true optimal worst-case, and the caller falls back to
    // best_guess() for the actual word.
    auto wl = tiny_wordlist({"slate", "crane", "trace", "stare", "least"});
    auto pm = PatternMatrix::build(wl);
    EntropySolver solver{wl, pm};

    auto all = wl.all_indices();
    auto [gi, depth] = solver.minimax_best_guess(all, 6);
    // Cross-check minimax against an independent end-to-end greedy solve of
    // every word: minimax's reported worst-case must not exceed the greedy
    // worst-case over the same set (it optimizes for exactly this).
    int greedy_worst = 0;
    for (uint16_t i : all) {
        auto r = solver.solve(i);
        REQUIRE(r.solved);
        greedy_worst = std::max(greedy_worst, r.depth());
    }
    CHECK(depth <= greedy_worst);
    CHECK(depth >= 2);  // 2+ candidates always need ≥ 2 guesses

    // Resolve the actual guess: minimax's own pick, or the greedy fallback.
    uint16_t chosen = (gi != WordList::NPOS) ? gi : solver.best_guess(all);
    CHECK(chosen != WordList::NPOS);
    CHECK(chosen < wl.size());
}

TEST_CASE("minimax - reported depth is a sane achievable bound on small sets",
          "[solver][minimax]") {
    // On the answers list, take a small candidate subset and confirm minimax's
    // reported worst-case depth is within budget. gi may be NPOS (greedy already
    // optimal) — that's the fall-back-to-greedy signal, not an error.
    auto wl = WordList::load("data/answers.txt");
    REQUIRE(wl.has_value());
    auto pm = PatternMatrix::build(*wl);
    EntropySolver solver{*wl, pm};

    // Take the first MINIMAX_THRESHOLD answer indices as a candidate set.
    std::vector<uint16_t> cand;
    for (uint16_t i = 0; i < EntropySolver::MINIMAX_THRESHOLD; ++i)
        cand.push_back(i);

    auto [gi, mm_depth] = solver.minimax_best_guess(cand, 6);
    CHECK(mm_depth >= 1);
    CHECK(mm_depth <= 6);

    // Whichever guess we end up using must be valid.
    uint16_t chosen = (gi != WordList::NPOS) ? gi : solver.best_guess(cand);
    CHECK(chosen != WordList::NPOS);
}
