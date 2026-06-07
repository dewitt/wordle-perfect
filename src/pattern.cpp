#include "pattern.hpp"

#include <array>
#include <cctype>
#include <cstdint>

namespace wp {

Pattern compute_pattern(std::string_view guess, std::string_view answer) noexcept {
    // Count of each answer letter available for yellow matching
    // (only non-green positions contribute)
    std::array<int, 26> remaining{};
    std::array<uint8_t, 5> result{};  // 0=B, 1=Y, 2=G

    // First pass: identify greens, tally remaining answer letters
    for (int i = 0; i < 5; ++i) {
        if (guess[i] == answer[i]) {
            result[i] = 2;  // G
        } else {
            remaining[static_cast<unsigned char>(answer[i]) - 'a']++;
        }
    }

    // Second pass: identify yellows among non-green guess positions
    for (int i = 0; i < 5; ++i) {
        if (result[i] == 2) continue;  // already green
        int c = static_cast<unsigned char>(guess[i]) - 'a';
        if (remaining[c] > 0) {
            result[i] = 1;  // Y
            remaining[c]--;
        }
        // else stays 0 (B)
    }

    // Encode as base-3: position 0 is least-significant digit
    Pattern p = 0;
    uint8_t mult = 1;
    for (int i = 0; i < 5; ++i) {
        p = static_cast<Pattern>(p + result[i] * mult);
        mult = static_cast<uint8_t>(mult * 3);
    }
    return p;
}

std::array<char, 5> decode_pattern(Pattern p) noexcept {
    static constexpr std::array<char, 3> sym{'B', 'Y', 'G'};
    std::array<char, 5> out{};
    for (int i = 0; i < 5; ++i) {
        out[i] = sym[p % 3];
        p = static_cast<Pattern>(p / 3);
    }
    return out;
}

Pattern encode_response(std::string_view response) noexcept {
    if (response.size() != 5) return 255;
    Pattern p = 0;
    uint8_t mult = 1;
    for (int i = 0; i < 5; ++i) {
        auto c = static_cast<char>(std::toupper(static_cast<unsigned char>(response[i])));
        uint8_t digit;
        switch (c) {
            case 'G': digit = 2; break;
            case 'Y': digit = 1; break;
            case 'B': digit = 0; break;
            default:  return 255;
        }
        p = static_cast<Pattern>(p + digit * mult);
        mult = static_cast<uint8_t>(mult * 3);
    }
    return p;
}

bool valid_response_string(std::string_view response) noexcept {
    if (response.size() != 5) return false;
    for (char c : response) {
        auto u = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        if (u != 'G' && u != 'Y' && u != 'B') return false;
    }
    return true;
}

} // namespace wp
