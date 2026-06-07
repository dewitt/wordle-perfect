#pragma once

#include <array>
#include <cstdint>
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

inline constexpr Pattern PATTERN_SOLVED = 242;   // GGGGG — guess == answer
inline constexpr int     PATTERN_COUNT  = 243;   // 3^5

// Compute the Wordle response for guessing `guess` when the answer is `answer`.
// Both must be exactly WORD_LEN characters.
[[nodiscard]] Pattern
compute_pattern(std::string_view guess, std::string_view answer) noexcept;

// Decode a Pattern into a 5-char array of 'B'/'Y'/'G' (display order, pos 0 first).
[[nodiscard]] std::array<char, 5>
decode_pattern(Pattern p) noexcept;

// Encode a user-supplied response string (e.g. "GYBBB", case-insensitive)
// into a Pattern. Returns 255 on invalid input.
[[nodiscard]] Pattern
encode_response(std::string_view response) noexcept;

// Validate that a response string is syntactically well-formed (exactly 5
// characters, each one of G/Y/B, case-insensitive). Does not check logical
// consistency with prior guesses.
[[nodiscard]] bool
valid_response_string(std::string_view response) noexcept;

} // namespace wp
