#include <catch2/catch_test_macros.hpp>

#include "pattern.hpp"

using namespace wp;

// Helper: build a Pattern from a BYG string using encode_response (the
// canonical encoder), so tests don't hand-compute base-3 values inline.
static Pattern pat(const char* s) {
    Pattern p = encode_response(s);
    REQUIRE(p != 255);  // 255 = encode_response error sentinel
    return p;
}

TEST_CASE("PATTERN_SOLVED is 242 (GGGGG in base-3)", "[pattern]") {
    CHECK(PATTERN_SOLVED == 242);
    CHECK(PATTERN_COUNT  == 243);
}

TEST_CASE("compute_pattern - all green", "[pattern]") {
    CHECK(compute_pattern("crane", "crane") == PATTERN_SOLVED);
    CHECK(compute_pattern("tarse", "tarse") == PATTERN_SOLVED);
}

TEST_CASE("compute_pattern - all black", "[pattern]") {
    // "shift" and "could" share no letters
    CHECK(compute_pattern("shift", "could") == pat("BBBBB"));
}

TEST_CASE("compute_pattern - mixed case: crane→trace", "[pattern]") {
    // GUESS: c r a n e
    // ANSWER: t r a c e
    //   c: not at pos 0 in TRACE; but C is at pos 3 → Y
    //   r: exact match at pos 1 → G
    //   a: exact match at pos 2 → G
    //   n: not in TRACE → B
    //   e: exact match at pos 4 → G
    CHECK(compute_pattern("crane", "trace") == pat("YGGBG"));
}

TEST_CASE("compute_pattern - duplicate letter in guess, fewer in answer", "[pattern]") {
    // GUESS: speed  ANSWER: spell
    //   s pos 0: G
    //   p pos 1: G
    //   e pos 2: G
    //   e pos 3: only 1 E in spell (all taken by green at pos 2) → B
    //   d pos 4: not in spell → B
    CHECK(compute_pattern("speed", "spell") == pat("GGGBB"));
}

TEST_CASE("compute_pattern - duplicate letter in answer, single in guess", "[pattern]") {
    // GUESS: reels  ANSWER: creep
    //   r pos 0: r in CREEP at pos 1, not 0 → Y
    //   e pos 1: e in CREEP at pos 2, not 1 → Y
    //   e pos 2: e in CREEP at pos 2 → G
    //   l pos 3: not in CREEP → B
    //   s pos 4: not in CREEP → B
    CHECK(compute_pattern("reels", "creep") == pat("YYGBB"));
}

TEST_CASE("compute_pattern - duplicate letter consumed from left", "[pattern]") {
    // GUESS: speed  ANSWER: creep
    //   s pos 0: not in CREEP → B
    //   p pos 1: p in CREEP at pos 4, not 1 → Y
    //   e pos 2: e in CREEP at pos 2 → G
    //   e pos 3: e in CREEP at pos 3 → G
    //   d pos 4: not in CREEP → B
    CHECK(compute_pattern("speed", "creep") == pat("BYGGB"));
}

TEST_CASE("compute_pattern - guess has more of a letter than answer", "[pattern]") {
    // GUESS: eerie  ANSWER: their
    //   e pos 0: E in THEIR at pos 2, not 0 → Y
    //   e pos 1: E in THEIR — already consumed one E, THEIR has 1 E → B
    //   r pos 2: R in THEIR at pos 4, not 2 → Y
    //   i pos 3: I in THEIR at pos 3 → G
    //   e pos 4: no E remaining → B
    CHECK(compute_pattern("eerie", "their") == pat("YBYGB"));
}

TEST_CASE("decode_pattern - round-trips with encode_response", "[pattern]") {
    for (const char* s : {"GGGGG", "BBBBB", "YYGBG", "GYBBY", "YYYYG"}) {
        Pattern p = encode_response(s);
        REQUIRE(p != 255);
        auto dec = decode_pattern(p);
        for (int i = 0; i < 5; ++i) {
            CHECK(dec[i] == s[i]);
        }
    }
}

TEST_CASE("encode_response - case insensitive", "[pattern]") {
    CHECK(encode_response("GYGBG") == encode_response("gygbg"));
    CHECK(encode_response("GYGBG") == encode_response("GYgbG"));
}

TEST_CASE("encode_response - wrong length returns 255", "[pattern]") {
    CHECK(encode_response("GGGG")   == 255);
    CHECK(encode_response("GGGGGG") == 255);
    CHECK(encode_response("")       == 255);
}

TEST_CASE("encode_response - invalid character returns 255", "[pattern]") {
    CHECK(encode_response("GGRGG") == 255);
    CHECK(encode_response("GGGGG") != 255);  // sanity
}

TEST_CASE("valid_response_string - accepts valid inputs", "[pattern]") {
    CHECK(valid_response_string("GGGGG"));
    CHECK(valid_response_string("BBBBB"));
    CHECK(valid_response_string("gygbg"));  // lowercase
    CHECK(valid_response_string("GYBBG"));
}

TEST_CASE("valid_response_string - rejects invalid inputs", "[pattern]") {
    CHECK_FALSE(valid_response_string("GGGG"));    // too short
    CHECK_FALSE(valid_response_string("GGGGGG"));  // too long
    CHECK_FALSE(valid_response_string("GGRGG"));   // invalid char
    CHECK_FALSE(valid_response_string(""));
}
