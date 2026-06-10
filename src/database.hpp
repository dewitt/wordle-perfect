#pragma once

#include "wordlist.hpp"
#include "pattern.hpp"

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

struct sqlite3;
struct sqlite3_stmt;

namespace wp {

// Depth/round within the decision tree: the root guess is depth 1, its children
// depth 2, etc. One byte suffices (worst case is 5; full-coverage 7).
using Depth = std::uint8_t;

// Identifier of a node in the decision tree (dense, 0-based; ROOT_ID == 0).
using NodeId = std::uint32_t;

// ---------------------------------------------------------------------------
// DbMetadata — self-describing artifact record
// ---------------------------------------------------------------------------
struct DbMetadata {
    std::string words_source;       // URL/description of words.txt
    std::string words_date;         // ISO date words list was retrieved
    std::string answers_source;     // URL/description of answers.txt
    std::string strategy;           // e.g. "entropy-greedy-v1"
    std::string start_word;         // the first guess the tree always makes
    int         worst_case_depth{};
    double      mean_depth{};
    int         total_nodes{};
    int         total_words{};      // size of words.txt used to build
    int         total_answers{};    // size of answers.txt used to evaluate
};

// ---------------------------------------------------------------------------
// Database — SQLite-backed precomputed decision tree
//
// Schema:
//   metadata(key TEXT PK, value TEXT)
//   nodes(id INTEGER PK, word_idx INTEGER, depth INTEGER)
//   edges(parent INTEGER, pattern INTEGER, child INTEGER, PK(parent,pattern))
//   checksum(hash TEXT)   — SHA-256 of nodes+edges content, set on finalize()
//
// Lookup: SELECT child FROM edges WHERE parent=? AND pattern=?
// O(log N) via B-tree primary key; effectively O(1) for the bounded tree depth.
// ---------------------------------------------------------------------------
class Database {
public:
    static constexpr NodeId NULL_NODE = UINT32_MAX;
    static constexpr NodeId ROOT_ID   = 0;

    static std::expected<Database, std::string> open(std::string_view path);
    static std::expected<Database, std::string> create(std::string_view path);

    ~Database();
    Database(Database&&) noexcept;
    Database& operator=(Database&&) noexcept;
    Database(const Database&)            = delete;
    Database& operator=(const Database&) = delete;

    // Verify stored checksum against recomputed hash of nodes+edges
    [[nodiscard]] std::expected<void, std::string> verify_integrity() const;

    // Metadata access
    [[nodiscard]] std::expected<DbMetadata, std::string> read_metadata()  const;
    [[nodiscard]] std::expected<void, std::string>       write_metadata(const DbMetadata& m);

    [[nodiscard]] std::expected<NodeId, std::string>
    next_node(NodeId node_id, Pattern pattern) const;

    // Node info (word_idx and depth) for display
    [[nodiscard]] std::expected<std::pair<WordIndex, Depth>, std::string>
    node_info(NodeId node_id) const;

    // Word index at root (= optimal first guess)
    [[nodiscard]] std::expected<WordIndex, std::string> root_word() const;

    // Build helpers — used by build_db during precomputation
    [[nodiscard]] std::expected<void, std::string> begin_transaction();
    [[nodiscard]] std::expected<void, std::string> commit_transaction();

    [[nodiscard]] std::expected<NodeId, std::string>
    insert_node(NodeId id, WordIndex word_idx, Depth depth);

    [[nodiscard]] std::expected<void, std::string>
    insert_edge(NodeId parent, Pattern pattern, NodeId child);

    // Call after all inserts: creates indices and writes checksum
    [[nodiscard]] std::expected<void, std::string> finalize(const DbMetadata& meta);

    // Print tree to stdout for debugging (honours --db path via dump sub-command)
    void dump(const WordList& words) const;

    [[nodiscard]] int64_t node_count()  const;
    [[nodiscard]] int64_t edge_count()  const;

    // Bulk export of the tree for conversion to other formats (e.g. BinaryDb).
    // NodeRow is ordered by id; EdgeRow is ordered by (parent, pattern).
    struct NodeRow { NodeId id; WordIndex word_idx; Depth depth; };
    struct EdgeRow { NodeId parent; Pattern pattern; NodeId child; };
    [[nodiscard]] std::expected<std::vector<NodeRow>, std::string> all_nodes() const;
    [[nodiscard]] std::expected<std::vector<EdgeRow>, std::string> all_edges() const;

private:
    explicit Database(sqlite3* db) : db_{db} {}

    [[nodiscard]] std::expected<void, std::string> init_schema();
    [[nodiscard]] std::string                      compute_content_hash() const;

    sqlite3* db_{};

    // Cached prepared statements for the hot-path lookup functions.
    // Prepared lazily on first use; mutable so the cache works from const methods.
    // Finalized in ~Database and nulled in move operations.
    mutable sqlite3_stmt* stmt_next_node_{};
    mutable sqlite3_stmt* stmt_node_info_{};
};

// ---------------------------------------------------------------------------
// walk_target — follow the decision tree for a known target word.
//
// Shared by the build-time evaluator and the CLI eval/solve modes so the
// walk semantics (cap handling, missing-edge detection) never diverge.
//
// `max_rounds` is a hard safety cap on the number of steps, NOT a solve
// guarantee: it should be set generously (e.g. WALK_DEPTH_CAP) so a valid
// path longer than the database's recorded worst case is still reported with
// its true depth rather than being misclassified as a failure.
// ---------------------------------------------------------------------------
struct WalkOutcome {
    enum class Status { Solved, MissingEdge, ExceededCap, DbError };
    Status status{Status::DbError};
    int    depth{0};   // number of guesses taken (valid when Solved)

    [[nodiscard]] bool solved() const noexcept { return status == Status::Solved; }
};

// Generous default cap. The deepest known path (full-coverage DB) is 8; any
// valid Wordle tree is bounded well below this. Distinguishes a genuinely
// missing path from one that merely exceeds the database's worst-case metric.
inline constexpr int WALK_DEPTH_CAP = 16;

// Templated on the database type so the same walk works for both the SQLite
// Database and the mmap'd BinaryDb (both expose node_info/next_node and a
// ROOT_ID). Any type with those members and that interface satisfies it.
template <class DB>
[[nodiscard]] WalkOutcome
walk_target(const DB& db, const WordList& words, std::string_view target,
            int max_rounds = WALK_DEPTH_CAP) {
    WalkOutcome out;
    NodeId node = DB::ROOT_ID;

    for (int round = 1; round <= max_rounds; ++round) {
        auto info = db.node_info(node);
        if (!info) { out.status = WalkOutcome::Status::DbError; return out; }
        auto [word_idx, depth] = *info;

        Pattern p = compute_pattern(words[word_idx].view(), target);
        out.depth = round;

        if (p == PATTERN_SOLVED) {
            out.status = WalkOutcome::Status::Solved;
            return out;
        }

        auto nxt = db.next_node(node, p);
        if (!nxt) { out.status = WalkOutcome::Status::MissingEdge; return out; }
        node = *nxt;
    }

    out.status = WalkOutcome::Status::ExceededCap;
    return out;
}

} // namespace wp
