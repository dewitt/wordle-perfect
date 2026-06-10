#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wp {

inline constexpr int WORD_LEN = 5;

// Index into a WordList. Word indices are stored as uint16_t throughout (the
// list is capped at 65,535 entries), so this alias documents that role without
// changing any layout.
using WordIndex = std::uint16_t;

// ---------------------------------------------------------------------------
// Word — exactly 5 lowercase ASCII letters
// ---------------------------------------------------------------------------
struct Word {
    std::array<char, WORD_LEN> letters{};

    constexpr Word() = default;
    explicit constexpr Word(std::string_view sv) noexcept {
        for (int i = 0; i < WORD_LEN; ++i) letters[i] = sv[i];
    }

    [[nodiscard]] constexpr std::string_view view() const noexcept {
        return {letters.data(), WORD_LEN};
    }

    [[nodiscard]] constexpr bool operator==(const Word& o) const noexcept = default;
    [[nodiscard]] constexpr auto operator<=>(const Word& o) const noexcept = default;
};

// ---------------------------------------------------------------------------
// WordList — sorted, immutable list with O(log N) index lookup
// ---------------------------------------------------------------------------
class WordList {
public:
    static constexpr WordIndex NPOS = UINT16_MAX;

    static std::expected<WordList, std::string> load(std::string_view path);

    [[nodiscard]] std::size_t           size()                     const noexcept { return words_.size(); }
    [[nodiscard]] bool                  empty()                    const noexcept { return words_.empty(); }
    [[nodiscard]] const Word&           operator[](WordIndex idx)  const noexcept { return words_[idx]; }
    [[nodiscard]] std::span<const Word> span()                     const noexcept { return words_; }

    // Binary search — words_ is kept sorted by load()
    [[nodiscard]] WordIndex index_of(std::string_view word) const noexcept;
    [[nodiscard]] bool      contains(std::string_view word) const noexcept {
        return index_of(word) != NPOS;
    }

    // Build a parallel index list (all indices) for use as initial candidate set
    [[nodiscard]] std::vector<WordIndex> all_indices() const;

private:
    std::vector<Word> words_;
};

} // namespace wp
