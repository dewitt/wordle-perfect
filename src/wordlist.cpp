#include "wordlist.hpp"

#include <algorithm>
#include <fstream>
#include <numeric>
#include <string>

namespace wp {

std::expected<WordList, std::string> WordList::load(std::string_view path) {
    std::ifstream file{std::string(path)};
    if (!file) {
        return std::unexpected("cannot open file: " + std::string(path));
    }

    WordList wl;
    std::string line;
    while (std::getline(file, line)) {
        // Handle Windows-style line endings
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.size() != static_cast<std::size_t>(WORD_LEN)) continue;

        // Validate: all lowercase ASCII letters
        bool valid = std::ranges::all_of(line, [](unsigned char c) {
            return c >= 'a' && c <= 'z';
        });
        if (!valid) continue;

        wl.words_.emplace_back(line);
    }

    if (wl.words_.empty()) {
        return std::unexpected("word list is empty or contains no valid 5-letter words");
    }

    // Sort for binary-search index_of
    std::ranges::sort(wl.words_);

    return wl;
}

uint16_t WordList::index_of(std::string_view word) const noexcept {
    if (word.size() != static_cast<std::size_t>(WORD_LEN)) return NPOS;
    auto it = std::ranges::lower_bound(words_, Word{word});
    if (it == words_.end() || it->view() != word) return NPOS;
    return static_cast<uint16_t>(it - words_.begin());
}

std::vector<uint16_t> WordList::all_indices() const {
    std::vector<uint16_t> idx(words_.size());
    std::iota(idx.begin(), idx.end(), uint16_t{0});
    return idx;
}

} // namespace wp
