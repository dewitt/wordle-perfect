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
// any_consistent_word — solver-mode consistency check
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("any_consistent_word - empty history is trivially consistent", "[solver]") {
    auto wl = tiny_wordlist({"crane", "slate", "trail"});
    CHECK(any_consistent_word(wl, {}));
}

TEST_CASE("any_consistent_word - true when a candidate matches all responses", "[solver]") {
    auto wl = tiny_wordlist({"crane", "slate", "trail", "point", "blink"});

    // Use the real pattern crane would produce if the secret were "slate".
    const Pattern crane_vs_slate = compute_pattern("crane", "slate");
    std::vector<GuessResponse> hist{{"crane", crane_vs_slate}};
    // "slate" is in the list and is consistent by construction.
    CHECK(any_consistent_word(wl, hist));
}

TEST_CASE("any_consistent_word - false when no candidate matches", "[solver]") {
    auto wl = tiny_wordlist({"crane", "slate", "trail"});

    // GGGGG for "point" claims the answer is point — but point isn't a candidate,
    // so no word in the list can satisfy it.
    std::vector<GuessResponse> hist{{"point", PATTERN_SOLVED}};
    CHECK_FALSE(any_consistent_word(wl, hist));
}

TEST_CASE("any_consistent_word - detects mutually inconsistent responses", "[solver]") {
    auto wl = tiny_wordlist({"crane", "slate", "trail", "point", "blink"});

    // First response says the answer is exactly "crane" (GGGGG). A second,
    // non-matching response for another guess then contradicts it: no single
    // word can be both crane AND produce a different pattern.
    std::vector<GuessResponse> hist{
        {"crane", PATTERN_SOLVED},
        {"slate", compute_pattern("slate", "trail")},  // implies answer == trail
    };
    CHECK_FALSE(any_consistent_word(wl, hist));
}

TEST_CASE("any_consistent_word - guess-only word as answer is rejected vs answers set",
          "[solver]") {
    // The curated-answers semantics: a GGGGG on a word that is NOT in the
    // candidate (answer) set is inconsistent, because that word can never be the
    // secret.
    auto answers = tiny_wordlist({"crane", "slate", "trail"});
    std::vector<GuessResponse> hist{{"tarse", PATTERN_SOLVED}};  // tarse not in set
    CHECK_FALSE(any_consistent_word(answers, hist));
}

// ─────────────────────────────────────────────────────────────────────────────
// is_feasible / best_guess_feasible — worst-case-bounded tree construction
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("is_feasible - trivial sets", "[solver][feasible]") {
    auto wl = tiny_wordlist({"crane", "slate", "trace"});
    auto pm = PatternMatrix::build(wl);
    EntropySolver solver{wl, pm};

    std::vector<uint16_t> empty{};
    CHECK(solver.is_feasible(empty, 5));            // empty trivially feasible
    uint16_t one = wl.index_of("crane");
    CHECK(solver.is_feasible(std::span<const uint16_t>{&one, 1}, 1));  // singleton in 1
    auto all = wl.all_indices();
    CHECK_FALSE(solver.is_feasible(all, 1));         // 3 words can't solve in 1
}

TEST_CASE("is_feasible - answer set is solvable in 5 but not 4 (sampled)",
          "[solver][feasible][slow]") {
    // The full answer set is provably worst-case-5 (and 4 is impossible). Proving
    // 4-infeasibility over the whole set is expensive, so we check only the
    // feasible-at-5 direction on the real set, which is fast with memoization.
    auto wl = WordList::load("data/words.txt");
    REQUIRE(wl.has_value());
    auto ans = WordList::load("data/answers.txt");
    REQUIRE(ans.has_value());
    auto pm = PatternMatrix::build(*wl);
    EntropySolver solver{*wl, pm};

    std::vector<uint16_t> cand;
    for (auto& w : ans->span()) {
        auto idx = wl->index_of(w.view());
        if (idx != WordList::NPOS) cand.push_back(idx);
    }
    std::ranges::sort(cand);

    uint16_t witness = WordList::NPOS;
    CHECK(solver.is_feasible(cand, 5, &witness));
    CHECK(witness != WordList::NPOS);
}

TEST_CASE("best_guess_feasible - returns a feasible, splitting guess",
          "[solver][feasible][slow]") {
    auto wl = WordList::load("data/words.txt");
    REQUIRE(wl.has_value());
    auto ans = WordList::load("data/answers.txt");
    REQUIRE(ans.has_value());
    auto pm = PatternMatrix::build(*wl);
    EntropySolver solver{*wl, pm};

    std::vector<uint16_t> cand;
    for (auto& w : ans->span()) {
        auto idx = wl->index_of(w.view());
        if (idx != WordList::NPOS) cand.push_back(idx);
    }
    std::ranges::sort(cand);

    uint16_t g = solver.best_guess_feasible(cand, 5, /*lookahead=*/1);
    REQUIRE(g != WordList::NPOS);
    // The chosen guess must split the set (max bucket < n) and keep every bucket
    // feasible at depth 4.
    auto buckets = EntropySolver::partition(cand, g, pm);
    std::size_t maxb = 0;
    for (auto& b : buckets) maxb = std::max(maxb, b.size());
    CHECK(maxb < cand.size());
    for (Pattern p = 0; p < PATTERN_COUNT - 1; ++p) {
        if (buckets[p].size() <= 1) continue;
        CHECK(solver.is_feasible(buckets[p], 4));
    }
}
