#include "database.hpp"

#include <array>
#include <cstdio>
#include <cstring>
#include <format>
#include <print>
#include <sstream>
#include <string>

#include <sqlite3.h>

namespace wp {

// ---------------------------------------------------------------------------
// RAII helpers
// ---------------------------------------------------------------------------
namespace {

struct StmtGuard {
    sqlite3_stmt* s{};
    ~StmtGuard() { if (s) sqlite3_finalize(s); }
    sqlite3_stmt** operator&() { return &s; }
    sqlite3_stmt*  operator*() { return s; }
};

std::string db_errmsg(sqlite3* db) {
    return sqlite3_errmsg(db);
}

// Simple FNV-1a 64-bit hash used as the content checksum
uint64_t fnv1a_update(uint64_t h, const void* data, std::size_t len) noexcept {
    static constexpr uint64_t FNV_PRIME  = 0x00000100000001B3ULL;
    static constexpr uint64_t FNV_OFFSET = 0xcbf29ce484222325ULL;
    if (h == 0) h = FNV_OFFSET;
    const auto* p = static_cast<const uint8_t*>(data);
    for (std::size_t i = 0; i < len; ++i) {
        h ^= p[i];
        h *= FNV_PRIME;
    }
    return h;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------
Database::~Database() {
    if (db_) sqlite3_close(db_);
}

Database::Database(Database&& o) noexcept : db_{o.db_} { o.db_ = nullptr; }
Database& Database::operator=(Database&& o) noexcept {
    if (this != &o) { if (db_) sqlite3_close(db_); db_ = o.db_; o.db_ = nullptr; }
    return *this;
}

std::expected<Database, std::string> Database::open(std::string_view path) {
    sqlite3* db{};
    if (sqlite3_open_v2(std::string(path).c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        std::string err = db ? sqlite3_errmsg(db) : "sqlite3_open failed";
        if (db) sqlite3_close(db);
        return std::unexpected(err);
    }
    return Database{db};
}

std::expected<Database, std::string> Database::create(std::string_view path) {
    sqlite3* db{};
    if (sqlite3_open_v2(std::string(path).c_str(), &db,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK) {
        std::string err = db ? sqlite3_errmsg(db) : "sqlite3_open_v2 failed";
        if (db) sqlite3_close(db);
        return std::unexpected(err);
    }
    Database d{db};
    if (auto r = d.init_schema(); !r) return std::unexpected(r.error());
    return d;
}

std::expected<void, std::string> Database::init_schema() {
    static const char* sql = R"sql(
        PRAGMA journal_mode = WAL;
        PRAGMA synchronous  = NORMAL;

        CREATE TABLE IF NOT EXISTS metadata (
            key   TEXT NOT NULL PRIMARY KEY,
            value TEXT NOT NULL
        );

        CREATE TABLE IF NOT EXISTS nodes (
            id       INTEGER NOT NULL PRIMARY KEY,
            word_idx INTEGER NOT NULL,
            depth    INTEGER NOT NULL
        );

        CREATE TABLE IF NOT EXISTS edges (
            parent  INTEGER NOT NULL,
            pattern INTEGER NOT NULL CHECK(pattern BETWEEN 0 AND 241),
            child   INTEGER NOT NULL,
            PRIMARY KEY (parent, pattern)
        ) WITHOUT ROWID;

        CREATE TABLE IF NOT EXISTS checksum (
            hash TEXT NOT NULL
        );
    )sql";

    char* errmsg{};
    if (sqlite3_exec(db_, sql, nullptr, nullptr, &errmsg) != SQLITE_OK) {
        std::string err = errmsg;
        sqlite3_free(errmsg);
        return std::unexpected(err);
    }
    return {};
}

// ---------------------------------------------------------------------------
// Integrity
// ---------------------------------------------------------------------------
std::string Database::compute_content_hash() const {
    // Hash all (id, word_idx, depth) rows then all (parent, pattern, child) rows
    uint64_t h = 0;
    {
        StmtGuard st;
        sqlite3_prepare_v2(db_, "SELECT id, word_idx, depth FROM nodes ORDER BY id", -1, &st, nullptr);
        while (sqlite3_step(*st) == SQLITE_ROW) {
            int64_t vals[3] = {
                sqlite3_column_int64(*st, 0),
                sqlite3_column_int64(*st, 1),
                sqlite3_column_int64(*st, 2),
            };
            h = fnv1a_update(h, vals, sizeof(vals));
        }
    }
    {
        StmtGuard st;
        sqlite3_prepare_v2(db_, "SELECT parent, pattern, child FROM edges ORDER BY parent, pattern", -1, &st, nullptr);
        while (sqlite3_step(*st) == SQLITE_ROW) {
            int64_t vals[3] = {
                sqlite3_column_int64(*st, 0),
                sqlite3_column_int64(*st, 1),
                sqlite3_column_int64(*st, 2),
            };
            h = fnv1a_update(h, vals, sizeof(vals));
        }
    }
    return std::format("{:016x}", h);
}

std::expected<void, std::string> Database::verify_integrity() const {
    StmtGuard st;
    if (sqlite3_prepare_v2(db_, "SELECT hash FROM checksum LIMIT 1", -1, &st, nullptr) != SQLITE_OK)
        return std::unexpected("no checksum table: " + db_errmsg(db_));
    if (sqlite3_step(*st) != SQLITE_ROW)
        return std::unexpected("checksum table is empty");

    std::string stored = reinterpret_cast<const char*>(sqlite3_column_text(*st, 0));
    std::string computed = compute_content_hash();

    if (stored != computed)
        return std::unexpected(std::format(
            "checksum mismatch: stored={} computed={}", stored, computed));
    return {};
}

// ---------------------------------------------------------------------------
// Metadata
// ---------------------------------------------------------------------------
std::expected<DbMetadata, std::string> Database::read_metadata() const {
    auto get = [&](const char* key) -> std::string {
        StmtGuard st;
        sqlite3_prepare_v2(db_, "SELECT value FROM metadata WHERE key=?", -1, &st, nullptr);
        sqlite3_bind_text(*st, 1, key, -1, SQLITE_STATIC);
        if (sqlite3_step(*st) != SQLITE_ROW) return {};
        return reinterpret_cast<const char*>(sqlite3_column_text(*st, 0));
    };

    DbMetadata m;
    m.words_source    = get("words_source");
    m.words_date      = get("words_date");
    m.answers_source  = get("answers_source");
    m.strategy        = get("strategy");
    m.start_word      = get("start_word");
    m.worst_case_depth = std::stoi(get("worst_case_depth").empty() ? "0" : get("worst_case_depth"));
    auto md = get("mean_depth");
    m.mean_depth      = md.empty() ? 0.0 : std::stod(md);
    auto tn = get("total_nodes");
    m.total_nodes     = tn.empty() ? 0 : std::stoi(tn);
    auto tw = get("total_words");
    m.total_words     = tw.empty() ? 0 : std::stoi(tw);
    auto ta = get("total_answers");
    m.total_answers   = ta.empty() ? 0 : std::stoi(ta);
    return m;
}

std::expected<void, std::string> Database::write_metadata(const DbMetadata& m) {
    auto set = [&](const char* key, const std::string& val) -> bool {
        StmtGuard st;
        int rc = sqlite3_prepare_v2(db_,
            "INSERT OR REPLACE INTO metadata(key,value) VALUES(?,?)", -1, &st, nullptr);
        if (rc != SQLITE_OK) return false;
        sqlite3_bind_text(*st, 1, key, -1, SQLITE_STATIC);
        sqlite3_bind_text(*st, 2, val.c_str(), -1, SQLITE_TRANSIENT);
        return sqlite3_step(*st) == SQLITE_DONE;
    };

    char* errmsg{};
    sqlite3_exec(db_, "BEGIN", nullptr, nullptr, &errmsg);

    set("words_source",    m.words_source);
    set("words_date",      m.words_date);
    set("answers_source",  m.answers_source);
    set("strategy",        m.strategy);
    set("start_word",      m.start_word);
    set("worst_case_depth", std::to_string(m.worst_case_depth));
    set("mean_depth",      std::format("{:.6f}", m.mean_depth));
    set("total_nodes",     std::to_string(m.total_nodes));
    set("total_words",     std::to_string(m.total_words));
    set("total_answers",   std::to_string(m.total_answers));

    sqlite3_exec(db_, "COMMIT", nullptr, nullptr, &errmsg);
    return {};
}

// ---------------------------------------------------------------------------
// Lookup (hot path)
// ---------------------------------------------------------------------------
std::expected<uint32_t, std::string>
Database::next_node(uint32_t node_id, Pattern pattern) const {
    if (pattern == PATTERN_SOLVED) return NULL_NODE;

    StmtGuard st;
    if (sqlite3_prepare_v2(db_,
            "SELECT child FROM edges WHERE parent=? AND pattern=?",
            -1, &st, nullptr) != SQLITE_OK)
        return std::unexpected(db_errmsg(db_));

    sqlite3_bind_int(*st, 1, static_cast<int>(node_id));
    sqlite3_bind_int(*st, 2, static_cast<int>(pattern));

    if (sqlite3_step(*st) != SQLITE_ROW)
        return std::unexpected(std::format(
            "no edge from node {} for pattern {}", node_id, pattern));

    return static_cast<uint32_t>(sqlite3_column_int(*st, 0));
}

std::expected<uint16_t, std::string>
Database::next_word(uint32_t node_id, Pattern pattern) const {
    auto child = next_node(node_id, pattern);
    if (!child) return std::unexpected(child.error());
    if (*child == NULL_NODE) return static_cast<uint16_t>(NULL_NODE);

    auto info = node_info(*child);
    if (!info) return std::unexpected(info.error());
    return info->first;
}

std::expected<std::pair<uint16_t, uint8_t>, std::string>
Database::node_info(uint32_t node_id) const {
    StmtGuard st;
    if (sqlite3_prepare_v2(db_,
            "SELECT word_idx, depth FROM nodes WHERE id=?",
            -1, &st, nullptr) != SQLITE_OK)
        return std::unexpected(db_errmsg(db_));

    sqlite3_bind_int(*st, 1, static_cast<int>(node_id));
    if (sqlite3_step(*st) != SQLITE_ROW)
        return std::unexpected(std::format("node {} not found", node_id));

    auto word_idx = static_cast<uint16_t>(sqlite3_column_int(*st, 0));
    auto depth    = static_cast<uint8_t>(sqlite3_column_int(*st, 1));
    return std::pair{word_idx, depth};
}

std::expected<uint16_t, std::string> Database::root_word() const {
    auto info = node_info(ROOT_ID);
    if (!info) return std::unexpected(info.error());
    return info->first;
}

// ---------------------------------------------------------------------------
// Build helpers
// ---------------------------------------------------------------------------
std::expected<void, std::string> Database::begin_transaction() {
    char* errmsg{};
    if (sqlite3_exec(db_, "BEGIN", nullptr, nullptr, &errmsg) != SQLITE_OK) {
        std::string err = errmsg; sqlite3_free(errmsg);
        return std::unexpected(err);
    }
    return {};
}

std::expected<void, std::string> Database::commit_transaction() {
    char* errmsg{};
    if (sqlite3_exec(db_, "COMMIT", nullptr, nullptr, &errmsg) != SQLITE_OK) {
        std::string err = errmsg; sqlite3_free(errmsg);
        return std::unexpected(err);
    }
    return {};
}

std::expected<uint32_t, std::string>
Database::insert_node(uint32_t id, uint16_t word_idx, uint8_t depth) {
    StmtGuard st;
    if (sqlite3_prepare_v2(db_,
            "INSERT INTO nodes(id, word_idx, depth) VALUES(?,?,?)",
            -1, &st, nullptr) != SQLITE_OK)
        return std::unexpected(db_errmsg(db_));

    sqlite3_bind_int(*st, 1, static_cast<int>(id));
    sqlite3_bind_int(*st, 2, static_cast<int>(word_idx));
    sqlite3_bind_int(*st, 3, static_cast<int>(depth));

    if (sqlite3_step(*st) != SQLITE_DONE)
        return std::unexpected(db_errmsg(db_));
    return id;
}

std::expected<void, std::string>
Database::insert_edge(uint32_t parent, Pattern pattern, uint32_t child) {
    StmtGuard st;
    if (sqlite3_prepare_v2(db_,
            "INSERT INTO edges(parent, pattern, child) VALUES(?,?,?)",
            -1, &st, nullptr) != SQLITE_OK)
        return std::unexpected(db_errmsg(db_));

    sqlite3_bind_int(*st, 1, static_cast<int>(parent));
    sqlite3_bind_int(*st, 2, static_cast<int>(pattern));
    sqlite3_bind_int(*st, 3, static_cast<int>(child));

    if (sqlite3_step(*st) != SQLITE_DONE)
        return std::unexpected(db_errmsg(db_));
    return {};
}

std::expected<void, std::string> Database::finalize(const DbMetadata& meta) {
    // Create index on edges for fast lookup
    char* errmsg{};
    sqlite3_exec(db_,
        "CREATE INDEX IF NOT EXISTS idx_edges ON edges(parent, pattern)",
        nullptr, nullptr, &errmsg);

    // Write metadata
    if (auto r = write_metadata(meta); !r) return r;

    // Compute and store checksum
    std::string hash = compute_content_hash();
    sqlite3_exec(db_, "DELETE FROM checksum", nullptr, nullptr, &errmsg);
    StmtGuard st;
    sqlite3_prepare_v2(db_, "INSERT INTO checksum(hash) VALUES(?)", -1, &st, nullptr);
    sqlite3_bind_text(*st, 1, hash.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(*st) != SQLITE_DONE)
        return std::unexpected("failed to write checksum");
    return {};
}

// ---------------------------------------------------------------------------
// Stats
// ---------------------------------------------------------------------------
int64_t Database::node_count() const {
    StmtGuard st;
    sqlite3_prepare_v2(db_, "SELECT COUNT(*) FROM nodes", -1, &st, nullptr);
    return sqlite3_step(*st) == SQLITE_ROW ? sqlite3_column_int64(*st, 0) : 0;
}

int64_t Database::edge_count() const {
    StmtGuard st;
    sqlite3_prepare_v2(db_, "SELECT COUNT(*) FROM edges", -1, &st, nullptr);
    return sqlite3_step(*st) == SQLITE_ROW ? sqlite3_column_int64(*st, 0) : 0;
}

// ---------------------------------------------------------------------------
// Dump
// ---------------------------------------------------------------------------
void Database::dump(const WordList& words) const {
    std::println("=== nodes ({}) ===", node_count());
    {
        StmtGuard st;
        sqlite3_prepare_v2(db_, "SELECT id, word_idx, depth FROM nodes ORDER BY id", -1, &st, nullptr);
        while (sqlite3_step(*st) == SQLITE_ROW) {
            auto id       = static_cast<uint32_t>(sqlite3_column_int(*st, 0));
            auto word_idx = static_cast<uint16_t>(sqlite3_column_int(*st, 1));
            auto depth    = sqlite3_column_int(*st, 2);
            std::println("  node {:6d}  word={:5}  depth={}",
                id,
                words.size() > word_idx ? words[word_idx].view() : "?????",
                depth);
        }
    }
    std::println("=== edges ({}) ===", edge_count());
    {
        StmtGuard st;
        sqlite3_prepare_v2(db_,
            "SELECT parent, pattern, child FROM edges ORDER BY parent, pattern",
            -1, &st, nullptr);
        while (sqlite3_step(*st) == SQLITE_ROW) {
            auto parent  = sqlite3_column_int(*st, 0);
            auto pattern = static_cast<Pattern>(sqlite3_column_int(*st, 1));
            auto child   = sqlite3_column_int(*st, 2);
            auto dec     = decode_pattern(pattern);
            std::println("  {:6d} --[{}{}{}{}{} ]--> {:6d}",
                parent,
                dec[0], dec[1], dec[2], dec[3], dec[4],
                child);
        }
    }
}

} // namespace wp
