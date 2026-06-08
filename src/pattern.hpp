#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace wp {

// ---------------------------------------------------------------------------
// Pattern encoding
//
// Each of the 5 positions holds one of three values:
//   B = 0  (letter absent from answer)
//   Y = 1  (letter present, wrong position)
//   G = 2  (letter correct, correct position)
//
// Encoded as a base-3 number with position 0 as the least-significant digit:
//   pattern = sum(digit_i * 3^i)  for i in [0, 4]
//
// Range: 0 (BBBBB) … 242 (GGGGG)
// ---------------------------------------------------------------------------

using Pattern = uint8_t;

inline constexpr Pattern PATTERN_SOLVED  = 242;   // GGGGG — guess == answer
inline constexpr int     PATTERN_COUNT   = 243;   // 3^5
inline constexpr Pattern PATTERN_INVALID = 255;   // encode_response error sentinel

// Compute the Wordle response for guessing `guess` when the answer is `answer`.
//
// Precondition: both arguments are exactly WORD_LEN (5) lowercase ASCII letters
// ('a'..'z'). This is guaranteed by WordList::load, which filters the input.
// Passing anything else is undefined behavior (the letter-tally array is indexed
// by `c - 'a'` without bounds checks). Violations are caught by an assert in
// debug builds.
[[nodiscard]] Pattern
compute_pattern(std::string_view guess, std::string_view answer) noexcept;

// Decode a Pattern into a 5-char array of 'B'/'Y'/'G' (display order, pos 0 first).
[[nodiscard]] std::array<char, 5>
decode_pattern(Pattern p) noexcept;

// Format a Pattern as a 5-char "BYGGB"-style display string.
[[nodiscard]] std::string
format_pattern(Pattern p) noexcept;

// Encode a user-supplied response string (e.g. "GYBBB", case-insensitive)
// into a Pattern. Returns PATTERN_INVALID on invalid input.
[[nodiscard]] Pattern
encode_response(std::string_view response) noexcept;

// Validate that a response string is syntactically well-formed (exactly 5
// characters, each one of G/Y/B, case-insensitive). Does not check logical
// consistency with prior guesses.
[[nodiscard]] bool
valid_response_string(std::string_view response) noexcept;

} // namespace wp
