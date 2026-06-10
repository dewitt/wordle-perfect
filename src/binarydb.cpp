#include "binarydb.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <format>
#include <fstream>
#include <print>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace wp {

// On-disk record layouts. Kept #pragma-pack-free by using only naturally-aligned
// fields and explicit padding, so the structs match the documented byte layout
// without relying on compiler packing.
namespace {

struct Header {
    uint64_t magic;
    uint32_t version;
    uint32_t node_count;
    uint32_t edge_count;
    uint32_t root_id;
    uint32_t worst_depth;
    uint32_t total_words;
    uint32_t total_answers;
    uint32_t meta_len;
    uint32_t _pad;          // align mean_depth/checksum to 8 bytes
    double   mean_depth;
    uint64_t checksum;      // FNV-1a over everything after the header
};
// 64 bytes: 8 (magic) + 9×4 (u32 fields incl. _pad) + 4 align + 8 (double) +
// 8 (checksum). The exact size is fixed by static_assert; the format is also
// guarded by magic+version at open() so any drift is caught.
static_assert(sizeof(Header) == 64, "Header layout drift");

struct NodeRec {
    uint16_t word_idx;
    Depth    depth;
    uint8_t  _pad;
    uint32_t edge_off;      // first edge index; count = next.edge_off - edge_off
};
static_assert(sizeof(NodeRec) == 8, "NodeRec layout drift");

struct EdgeRec {
    Pattern  pattern;
    uint8_t  _pad[3];
    uint32_t child;
};
static_assert(sizeof(EdgeRec) == 8, "EdgeRec layout drift");

constexpr uint64_t FNV_OFFSET = 1469598103934665603ULL;
constexpr uint64_t FNV_PRIME  = 1099511628211ULL;

uint64_t fnv1a(const std::byte* data, std::size_t len) noexcept {
    uint64_t h = FNV_OFFSET;
    for (std::size_t i = 0; i < len; ++i) {
        h ^= static_cast<uint64_t>(std::to_integer<uint8_t>(data[i]));
        h *= FNV_PRIME;
    }
    return h;
}

std::string serialize_meta(const DbMetadata& m) {
    // newline-separated key\tvalue records; read_metadata parses these back.
    return std::format(
        "words_source\t{}\n""words_date\t{}\n""answers_source\t{}\n"
        "strategy\t{}\n""start_word\t{}\n""worst_case_depth\t{}\n"
        "mean_depth\t{}\n""total_nodes\t{}\n""total_words\t{}\n""total_answers\t{}\n",
        m.words_source, m.words_date, m.answers_source, m.strategy, m.start_word,
        m.worst_case_depth, m.mean_depth, m.total_nodes, m.total_words,
        m.total_answers);
}

}  // namespace

// ---------------------------------------------------------------------------
// export_from — SQLite Database → flat binary file
// ---------------------------------------------------------------------------
std::expected<void, std::string>
BinaryDb::export_from(const Database& db, const DbMetadata& meta,
                      std::string_view path) {
    auto nodes = db.all_nodes();
    if (!nodes) return std::unexpected(nodes.error());
    auto edges = db.all_edges();
    if (!edges) return std::unexpected(edges.error());

    const uint32_t node_count = static_cast<uint32_t>(nodes->size());
    const uint32_t edge_count = static_cast<uint32_t>(edges->size());

    // Nodes must be a dense 0..node_count-1 id space (build_db assigns ids this
    // way). Verify so the direct-index assumption holds.
    for (uint32_t i = 0; i < node_count; ++i) {
        if ((*nodes)[i].id != i)
            return std::unexpected(std::format(
                "node ids are not dense/sequential (row {} has id {})",
                i, (*nodes)[i].id));
    }

    // Build the NodeRec array with CSR-style edge offsets. We need one extra
    // sentinel slot so the last node's edge count is computable.
    std::vector<NodeRec> nrecs(node_count + 1);
    std::vector<EdgeRec> erecs(edge_count);

    // Edges arrive ordered by (parent, pattern). Walk them, filling per-node
    // offsets. Count edges per node first.
    std::vector<uint32_t> per_node(node_count, 0);
    for (const auto& e : *edges) {
        if (e.parent >= node_count)
            return std::unexpected("edge references out-of-range parent");
        ++per_node[e.parent];
    }
    uint32_t running = 0;
    for (uint32_t i = 0; i < node_count; ++i) {
        nrecs[i].word_idx = (*nodes)[i].word_idx;
        nrecs[i].depth    = (*nodes)[i].depth;
        nrecs[i]._pad     = 0;
        nrecs[i].edge_off = running;
        running += per_node[i];
    }
    nrecs[node_count] = {0, 0, 0, running};  // sentinel: final offset == edge_count

    // Fill edges in order (already sorted by parent,pattern → slices are sorted).
    std::vector<uint32_t> cursor(node_count);
    for (uint32_t i = 0; i < node_count; ++i) cursor[i] = nrecs[i].edge_off;
    for (const auto& e : *edges) {
        EdgeRec& r = erecs[cursor[e.parent]++];
        r.pattern = e.pattern;
        r._pad[0] = r._pad[1] = r._pad[2] = 0;
        r.child   = e.child;
    }

    // Assemble the body (nodes + edges + meta), checksum it, then write header.
    const std::string meta_blob = serialize_meta(meta);

    const std::size_t nodes_bytes = nrecs.size() * sizeof(NodeRec);
    const std::size_t edges_bytes = erecs.size() * sizeof(EdgeRec);
    const std::size_t body_bytes  = nodes_bytes + edges_bytes + meta_blob.size();

    std::vector<std::byte> body(body_bytes);
    std::memcpy(body.data(), nrecs.data(), nodes_bytes);
    std::memcpy(body.data() + nodes_bytes, erecs.data(), edges_bytes);
    std::memcpy(body.data() + nodes_bytes + edges_bytes, meta_blob.data(),
                meta_blob.size());

    Header h{};
    h.magic         = MAGIC;
    h.version       = VERSION;
    h.node_count    = node_count;
    h.edge_count    = edge_count;
    h.root_id       = ROOT_ID;
    h.worst_depth   = static_cast<uint32_t>(meta.worst_case_depth);
    h.total_words   = static_cast<uint32_t>(meta.total_words);
    h.total_answers = static_cast<uint32_t>(meta.total_answers);
    h.meta_len      = static_cast<uint32_t>(meta_blob.size());
    h.mean_depth    = meta.mean_depth;
    h.checksum      = fnv1a(body.data(), body.size());

    std::ofstream out{std::string(path), std::ios::binary | std::ios::trunc};
    if (!out) return std::unexpected(std::format("cannot open {} for writing", path));
    out.write(reinterpret_cast<const char*>(&h), sizeof(h));
    out.write(reinterpret_cast<const char*>(body.data()),
              static_cast<std::streamsize>(body.size()));
    if (!out) return std::unexpected(std::format("write failed for {}", path));
    return {};
}

// ---------------------------------------------------------------------------
// open — mmap the file read-only
// ---------------------------------------------------------------------------
std::expected<BinaryDb, std::string> BinaryDb::open(std::string_view path) {
    const std::string p{path};
    int fd = ::open(p.c_str(), O_RDONLY);
    if (fd < 0) return std::unexpected(std::format("cannot open {}", path));

    struct ::stat st{};
    if (::fstat(fd, &st) != 0) { ::close(fd); return std::unexpected("fstat failed"); }
    const auto size = static_cast<std::size_t>(st.st_size);
    if (size < sizeof(Header)) {
        ::close(fd);
        return std::unexpected("file too small to be a BinaryDb");
    }

    void* m = ::mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
    ::close(fd);  // mapping survives close
    if (m == MAP_FAILED) return std::unexpected("mmap failed");

    const auto* h = reinterpret_cast<const Header*>(m);
    if (h->magic != MAGIC) {
        ::munmap(m, size);
        return std::unexpected("bad magic (not a BinaryDb file)");
    }
    if (h->version != VERSION) {
        ::munmap(m, size);
        return std::unexpected(std::format(
            "unsupported BinaryDb version {} (expected {})", h->version, VERSION));
    }

    // Bounds sanity: declared sections must fit within the mapping.
    const std::size_t nodes_bytes =
        (static_cast<std::size_t>(h->node_count) + 1) * sizeof(NodeRec);
    const std::size_t edges_bytes =
        static_cast<std::size_t>(h->edge_count) * sizeof(EdgeRec);
    if (sizeof(Header) + nodes_bytes + edges_bytes + h->meta_len > size) {
        ::munmap(m, size);
        return std::unexpected("declared sections exceed file size");
    }

    BinaryDb db;
    db.base_       = static_cast<const std::byte*>(m);
    db.size_       = size;
    db.node_count_ = h->node_count;
    db.edge_count_ = h->edge_count;
    return db;
}

BinaryDb::~BinaryDb() {
    if (base_) ::munmap(const_cast<std::byte*>(base_), size_);
}

BinaryDb::BinaryDb(BinaryDb&& o) noexcept
    : base_{o.base_}, size_{o.size_},
      node_count_{o.node_count_}, edge_count_{o.edge_count_} {
    o.base_ = nullptr;
    o.size_ = 0;
}

BinaryDb& BinaryDb::operator=(BinaryDb&& o) noexcept {
    if (this != &o) {
        if (base_) ::munmap(const_cast<std::byte*>(base_), size_);
        base_ = o.base_; size_ = o.size_;
        node_count_ = o.node_count_; edge_count_ = o.edge_count_;
        o.base_ = nullptr; o.size_ = 0;
    }
    return *this;
}

// --- accessors into the mapping ---------------------------------------------
namespace {
const Header* hdr(const std::byte* base) {
    return reinterpret_cast<const Header*>(base);
}
const NodeRec* nodes_of(const std::byte* base) {
    return reinterpret_cast<const NodeRec*>(base + sizeof(Header));
}
const EdgeRec* edges_of(const std::byte* base, uint32_t node_count) {
    return reinterpret_cast<const EdgeRec*>(
        base + sizeof(Header) + (static_cast<std::size_t>(node_count) + 1) * sizeof(NodeRec));
}
}  // namespace

std::expected<void, std::string> BinaryDb::verify_integrity() const {
    const auto* h = hdr(base_);
    const std::byte* body = base_ + sizeof(Header);
    const std::size_t body_len = size_ - sizeof(Header);
    if (fnv1a(body, body_len) != h->checksum)
        return std::unexpected("checksum mismatch (binary db corrupted)");
    return {};
}

std::expected<DbMetadata, std::string> BinaryDb::read_metadata() const {
    const auto* h = hdr(base_);
    DbMetadata m;
    m.worst_case_depth = static_cast<int>(h->worst_depth);
    m.mean_depth       = h->mean_depth;
    m.total_words      = static_cast<int>(h->total_words);
    m.total_answers    = static_cast<int>(h->total_answers);
    m.total_nodes      = static_cast<int>(h->node_count);

    // Parse the trailing key\tvalue metadata blob (overrides where present).
    const std::byte* meta =
        base_ + size_ - h->meta_len;
    std::string_view blob{reinterpret_cast<const char*>(meta), h->meta_len};
    auto assign = [&](std::string_view key, std::string_view val) {
        if      (key == "words_source")   m.words_source   = val;
        else if (key == "words_date")     m.words_date     = val;
        else if (key == "answers_source") m.answers_source = val;
        else if (key == "strategy")       m.strategy       = val;
        else if (key == "start_word")     m.start_word     = val;
    };
    std::size_t pos = 0;
    while (pos < blob.size()) {
        auto nl = blob.find('\n', pos);
        if (nl == std::string_view::npos) nl = blob.size();
        auto line = blob.substr(pos, nl - pos);
        auto tab = line.find('\t');
        if (tab != std::string_view::npos)
            assign(line.substr(0, tab), line.substr(tab + 1));
        pos = nl + 1;
    }
    return m;
}

std::expected<std::pair<uint16_t, Depth>, std::string>
BinaryDb::node_info(uint32_t node_id) const {
    if (node_id >= node_count_)
        return std::unexpected(std::format("node {} out of range", node_id));
    const NodeRec& n = nodes_of(base_)[node_id];
    return std::pair<uint16_t, Depth>{n.word_idx, n.depth};
}

std::expected<uint32_t, std::string>
BinaryDb::next_node(uint32_t node_id, Pattern pattern) const {
    if (node_id >= node_count_)
        return std::unexpected(std::format("node {} out of range", node_id));
    if (pattern == PATTERN_SOLVED) return NULL_NODE;

    const NodeRec* nodes = nodes_of(base_);
    const EdgeRec* edges = edges_of(base_, node_count_);
    const uint32_t begin = nodes[node_id].edge_off;
    const uint32_t end   = nodes[node_id + 1].edge_off;

    // Edge slice is sorted by pattern → binary search. Effectively O(1) given
    // the tiny per-node branching factor.
    const EdgeRec* lo = edges + begin;
    const EdgeRec* hi = edges + end;
    auto it = std::lower_bound(lo, hi, pattern,
        [](const EdgeRec& e, Pattern p) { return e.pattern < p; });
    if (it == hi || it->pattern != pattern)
        return std::unexpected(std::format(
            "no edge from node {} for pattern {}", node_id, pattern));
    return it->child;
}

std::expected<uint16_t, std::string> BinaryDb::root_word() const {
    auto info = node_info(ROOT_ID);
    if (!info) return std::unexpected(info.error());
    return info->first;
}

void BinaryDb::dump(const WordList& words) const {
    const NodeRec* nodes = nodes_of(base_);
    const EdgeRec* edges = edges_of(base_, node_count_);

    std::println("=== nodes ({}) ===", node_count_);
    for (uint32_t id = 0; id < node_count_; ++id) {
        const NodeRec& n = nodes[id];
        std::println("  node {:6d}  word={:5}  depth={}",
            id,
            words.size() > n.word_idx ? words[n.word_idx].view() : "?????",
            static_cast<int>(n.depth));
    }
    std::println("=== edges ({}) ===", edge_count_);
    for (uint32_t id = 0; id < node_count_; ++id) {
        for (uint32_t e = nodes[id].edge_off; e < nodes[id + 1].edge_off; ++e) {
            std::println("  {:6d} --[{}]--> {:6d}",
                id, format_pattern(edges[e].pattern), edges[e].child);
        }
    }
}

} // namespace wp
