#include <catch2/catch_test_macros.hpp>

#include "wordlist.hpp"

#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

using namespace wp;

// Write a word list to a temporary file and return its path.
// The file is owned by the caller; delete it when done.
static std::string make_temp_wordlist(const std::vector<std::string>& words) {
    auto path = std::filesystem::temp_directory_path() /
        std::format("wptest_wl_{}.txt",
                    std::hash<std::thread::id>{}(std::this_thread::get_id()));
    std::ofstream f(path);
    for (const auto& w : words) f << w << "\n";
    return path.string();
}

TEST_CASE("WordList::load - loads valid file", "[wordlist]") {
    // data/words.txt is available in the build directory (configured by CMake)
    auto wl = WordList::load("data/words.txt");
    REQUIRE(wl.has_value());
    CHECK(wl->size() > 10'000);  // ~14,855 guess words
    CHECK_FALSE(wl->empty());
}

TEST_CASE("WordList::load - answers subset is smaller than full word list", "[wordlist]") {
    auto words   = WordList::load("data/words.txt");
    auto answers = WordList::load("data/answers.txt");
    REQUIRE(words.has_value());
    REQUIRE(answers.has_value());
    CHECK(answers->size() < words->size());
    CHECK(answers->size() > 2'000);  // ~2,355 curated answers
}

TEST_CASE("WordList::load - fails on missing file", "[wordlist]") {
    auto wl = WordList::load("/nonexistent/path/to/nowhere.txt");
    CHECK_FALSE(wl.has_value());
}

TEST_CASE("WordList::load - skips invalid lines", "[wordlist]") {
    std::string path = make_temp_wordlist({
        "crane",
        "slate",
        "UPPER",   // uppercase — skipped
        "ab",      // too short — skipped
        "toolong", // too long — skipped
        "trail",
    });
    auto wl = WordList::load(path);
    std::filesystem::remove(path);

    REQUIRE(wl.has_value());
    CHECK(wl->size() == 3);  // crane, slate, trail
    CHECK(wl->contains("crane"));
    CHECK(wl->contains("slate"));
    CHECK(wl->contains("trail"));
    CHECK_FALSE(wl->contains("UPPER"));
}

TEST_CASE("WordList::load - rejects empty file", "[wordlist]") {
    std::string path = make_temp_wordlist({});
    auto wl = WordList::load(path);
    std::filesystem::remove(path);
    CHECK_FALSE(wl.has_value());
}

TEST_CASE("WordList - words are sorted (binary search is valid)", "[wordlist]") {
    auto wl = WordList::load("data/answers.txt");
    REQUIRE(wl.has_value());
    auto sp = wl->span();
    for (std::size_t i = 1; i < sp.size(); ++i) {
        CHECK(sp[i - 1] < sp[i]);
    }
}

TEST_CASE("WordList::index_of - known and unknown words", "[wordlist]") {
    std::string path = make_temp_wordlist({"crane", "slate", "trail"});
    auto wl = WordList::load(path);
    std::filesystem::remove(path);
    REQUIRE(wl.has_value());

    // All three words must be findable
    auto ci = wl->index_of("crane");
    auto si = wl->index_of("slate");
    auto ti = wl->index_of("trail");
    CHECK(ci != WordList::NPOS);
    CHECK(si != WordList::NPOS);
    CHECK(ti != WordList::NPOS);

    // All indices must be distinct and in range
    CHECK(ci < wl->size());
    CHECK(si < wl->size());
    CHECK(ti < wl->size());
    CHECK(ci != si);
    CHECK(ci != ti);
    CHECK(si != ti);

    // Unknown word returns NPOS
    CHECK(wl->index_of("zzzzz") == WordList::NPOS);
    CHECK(wl->index_of("") == WordList::NPOS);

    // Lookup by index round-trips
    CHECK(wl->operator[](ci).view() == "crane");
}

TEST_CASE("WordList::contains - delegates to index_of", "[wordlist]") {
    std::string path = make_temp_wordlist({"crane", "slate"});
    auto wl = WordList::load(path);
    std::filesystem::remove(path);
    REQUIRE(wl.has_value());

    CHECK(wl->contains("crane"));
    CHECK(wl->contains("slate"));
    CHECK_FALSE(wl->contains("trail"));
}

TEST_CASE("WordList::all_indices - size and content", "[wordlist]") {
    std::string path = make_temp_wordlist({"crane", "slate", "trail"});
    auto wl = WordList::load(path);
    std::filesystem::remove(path);
    REQUIRE(wl.has_value());

    auto idx = wl->all_indices();
    CHECK(idx.size() == wl->size());
    for (std::size_t i = 0; i < idx.size(); ++i) {
        CHECK(idx[i] == static_cast<uint16_t>(i));
    }
}
