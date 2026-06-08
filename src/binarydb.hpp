#pragma once

#include "database.hpp"
#include "pattern.hpp"
#include "wordlist.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

namespace wp {

// ---------------------------------------------------------------------------
// BinaryDb — flat, mmap'd, read-only decision tree for true O(1) lookup.
//
// Motivation (spec invariant `constant_time_lookup`): the SQLite database does
// an O(log N) B-tree descent plus query overhead per step and a ~5 ms cold
// open. The decision tree is tiny (~16.5 K nodes, ~0.5 MB), so a flat file we
// can mmap and index directly is genuinely O(1), sub-µs, and dependency-free.
//
// On-disk layout (all little-endian; the format is host-endian and validated by
// a magic+version header, so cross-endian portability is intentionally out of
// scope — these artifacts are rebuilt per-host):
//
//   Header (fixed size):
//     magic        u64   "WPBINDB\0"
//     version      u32
//     node_count   u32
//     edge_count   u32
//     root_id      u32   (always 0, stored for clarity)
//     worst_depth  u32
//     mean_depth   f64
//     total_words  u32
//     total_answers u32
//     checksum     u64   FNV-1a over everything after the header
//     meta_len     u32   length of the trailing JSON-ish metadata blob
//     (padding to 8-byte alignment)
//
//   Nodes[node_count]:   { word_idx: u16, depth: u8, _pad: u8, edge_off: u32 }
//                        edge_off = index into Edges of this node's first edge;
//                        the node's edge count is next.edge_off - this.edge_off
//                        (a sentinel node_count entry stores the final offset).
//   Edges[edge_count]:   { pattern: u8, _pad[3], child: u32 }  sorted by
//                        (node, pattern) so a node's slice is sorted by pattern.
//   Meta[meta_len]:      newline-separated "key\tvalue" metadata records.
//
// Lookup: node_info is a direct array index. next_node binary-searches the
// node's (small, ≤241) edge slice by pattern — effectively O(1).
// ---------------------------------------------------------------------------
class BinaryDb {
public:
    static constexpr uint32_t NULL_NODE = UINT32_MAX;
    static constexpr uint32_t ROOT_ID   = 0;
    static constexpr uint64_t MAGIC     = 0x00424454'4e494250ULL;  // "PBINTDB\0"-ish
    static constexpr uint32_t VERSION   = 1;

    // Convert an existing (finalized) SQLite Database into a binary file.
    [[nodiscard]] static std::expected<void, std::string>
    export_from(const Database& db, const DbMetadata& meta, std::string_view path);

    // mmap an existing binary file for reading.
    [[nodiscard]] static std::expected<BinaryDb, std::string>
    open(std::string_view path);

    ~BinaryDb();
    BinaryDb(BinaryDb&&) noexcept;
    BinaryDb& operator=(BinaryDb&&) noexcept;
    BinaryDb(const BinaryDb&)            = delete;
    BinaryDb& operator=(const BinaryDb&) = delete;

    // Recompute the FNV-1a checksum over the body and compare to the header.
    [[nodiscard]] std::expected<void, std::string> verify_integrity() const;

    [[nodiscard]] std::expected<DbMetadata, std::string> read_metadata() const;

    // current node + pattern → child node id. Returns NULL_NODE for GGGGG or a
    // missing edge (mirrors next_node semantics enough for the CLI walk, which
    // checks PATTERN_SOLVED before calling).
    [[nodiscard]] std::expected<uint32_t, std::string>
    next_node(uint32_t node_id, Pattern pattern) const;

    // word_idx + depth for a node.
    [[nodiscard]] std::expected<std::pair<uint16_t, uint8_t>, std::string>
    node_info(uint32_t node_id) const;

    [[nodiscard]] std::expected<uint16_t, std::string> root_word() const;

    // Human-readable dump of nodes + edges (satisfies the spec debug_dump_tool
    // invariant for the proprietary binary format).
    void dump(const WordList& words) const;

    [[nodiscard]] uint32_t node_count() const noexcept { return node_count_; }
    [[nodiscard]] uint32_t edge_count() const noexcept { return edge_count_; }

private:
    BinaryDb() = default;

    // Raw mmap region.
    const std::byte* base_{nullptr};
    std::size_t      size_{0};

    // Decoded views into the mapping (set by open()).
    uint32_t node_count_{0};
    uint32_t edge_count_{0};
};

} // namespace wp
