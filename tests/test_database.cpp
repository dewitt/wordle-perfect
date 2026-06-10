#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "database.hpp"
#include "pattern.hpp"
#include "solver.hpp"
#include "wordlist.hpp"

#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <span>
#include <thread>
#include <vector>

#include <sqlite3.h>

using namespace wp;

// Minimal metadata for use in finalize() calls during tests.
static DbMetadata test_meta() {
    return DbMetadata{
        .words_source    = "test",
        .words_date      = "2026-01-01",
        .answers_source  = "test",
        .strategy        = "test-strategy",
        .start_word      = "crane",
        .worst_case_depth = 4,
        .mean_depth      = 3.5,
        .total_nodes     = 3,
        .total_words     = 10,
        .total_answers   = 5,
    };
}

// Path for temp DB files that need to persist across open()/create() calls.
static std::string temp_db_path() {
    return (std::filesystem::temp_directory_path() /
            std::format("wptest_db_{}.sqlite",
                        std::hash<std::thread::id>{}(std::this_thread::get_id())))
        .string();
}

// ─────────────────────────────────────────────────────────────────────────────
// Basic CRUD round-trip
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Database - node and edge round-trip", "[database]") {
    auto db = Database::create(":memory:");
    REQUIRE(db.has_value());

    REQUIRE(db->begin_transaction().has_value());
    REQUIRE(db->insert_node(0, 42, 1).has_value());   // root: word_idx=42, depth=1
    REQUIRE(db->insert_node(1, 99, 2).has_value());   // child: word_idx=99, depth=2
    const Pattern edge_pat = 5;
    REQUIRE(db->insert_edge(0, edge_pat, 1).has_value());
    REQUIRE(db->commit_transaction().has_value());

    // node_info
    auto root = db->node_info(Database::ROOT_ID);
    REQUIRE(root.has_value());
    CHECK(root->first  == 42);  // word_idx
    CHECK(root->second == 1);   // depth

    auto child = db->node_info(1);
    REQUIRE(child.has_value());
    CHECK(child->first  == 99);
    CHECK(child->second == 2);

    // next_node
    auto nxt = db->next_node(0, edge_pat);
    REQUIRE(nxt.has_value());
    CHECK(*nxt == 1);

    // GGGGG → NULL_NODE
    auto solved = db->next_node(0, PATTERN_SOLVED);
    REQUIRE(solved.has_value());
    CHECK(*solved == Database::NULL_NODE);
}

TEST_CASE("Database - root_word returns word_idx at root node", "[database]") {
    auto db = Database::create(":memory:");
    REQUIRE(db.has_value());

    REQUIRE(db->begin_transaction().has_value());
    REQUIRE(db->insert_node(0, 7, 1).has_value());
    REQUIRE(db->commit_transaction().has_value());

    auto rw = db->root_word();
    REQUIRE(rw.has_value());
    CHECK(*rw == 7);
}

TEST_CASE("Database - missing node returns error", "[database]") {
    auto db = Database::create(":memory:");
    REQUIRE(db.has_value());

    auto info = db->node_info(9999);
    CHECK_FALSE(info.has_value());
}

TEST_CASE("Database - missing edge returns error", "[database]") {
    auto db = Database::create(":memory:");
    REQUIRE(db.has_value());

    REQUIRE(db->begin_transaction().has_value());
    REQUIRE(db->insert_node(0, 1, 1).has_value());
    REQUIRE(db->commit_transaction().has_value());

    auto nxt = db->next_node(0, 5);  // pattern 5 has no edge
    CHECK_FALSE(nxt.has_value());
}

// ─────────────────────────────────────────────────────────────────────────────
// Metadata round-trip
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Database - metadata write/read round-trip", "[database]") {
    auto db = Database::create(":memory:");
    REQUIRE(db.has_value());

    REQUIRE(db->begin_transaction().has_value());
    REQUIRE(db->insert_node(0, 1, 1).has_value());
    REQUIRE(db->commit_transaction().has_value());

    auto meta = test_meta();
    REQUIRE(db->finalize(meta).has_value());

    auto read = db->read_metadata();
    REQUIRE(read.has_value());
    CHECK(read->strategy         == meta.strategy);
    CHECK(read->start_word       == meta.start_word);
    CHECK(read->worst_case_depth == meta.worst_case_depth);
    CHECK(read->total_words      == meta.total_words);
    CHECK(read->total_answers    == meta.total_answers);
    // mean_depth stored as 6dp float — allow tiny rounding error
    CHECK(read->mean_depth == Catch::Approx(meta.mean_depth).epsilon(1e-5));
}

// ─────────────────────────────────────────────────────────────────────────────
// Integrity checking
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Database - verify_integrity passes after finalize", "[database]") {
    auto db = Database::create(":memory:");
    REQUIRE(db.has_value());

    REQUIRE(db->begin_transaction().has_value());
    REQUIRE(db->insert_node(0, 1, 1).has_value());
    REQUIRE(db->insert_node(1, 2, 2).has_value());
    REQUIRE(db->insert_edge(0, 7, 1).has_value());
    REQUIRE(db->commit_transaction().has_value());

    REQUIRE(db->finalize(test_meta()).has_value());
    CHECK(db->verify_integrity().has_value());
}

TEST_CASE("Database - verify_integrity fails before finalize (no checksum)", "[database]") {
    auto db = Database::create(":memory:");
    REQUIRE(db.has_value());

    // Don't call finalize — checksum table is empty
    auto r = db->verify_integrity();
    CHECK_FALSE(r.has_value());
}

TEST_CASE("Database - open fails on missing file", "[database]") {
    auto db = Database::open("/nonexistent/path/wordle.db");
    CHECK_FALSE(db.has_value());
}

TEST_CASE("Database - verify_integrity detects post-finalize tampering", "[database]") {
    // Build and finalize a valid DB, then mutate a node row out-of-band via raw
    // sqlite3 while leaving the stored checksum stale. verify_integrity() must
    // then fail. This exercises the database_corruption_detection contract end
    // to end.
    std::string path = temp_db_path();
    std::filesystem::remove(path);

    {
        auto db = Database::create(path);
        REQUIRE(db.has_value());
        REQUIRE(db->begin_transaction().has_value());
        REQUIRE(db->insert_node(0, 1, 1).has_value());
        REQUIRE(db->insert_node(1, 2, 2).has_value());
        REQUIRE(db->insert_edge(0, 7, 1).has_value());
        REQUIRE(db->commit_transaction().has_value());
        REQUIRE(db->finalize(test_meta()).has_value());
        CHECK(db->verify_integrity().has_value());  // valid before tampering
    }

    // Tamper: change a node's word_idx but NOT the checksum row.
    {
        sqlite3* raw{};
        REQUIRE(sqlite3_open(path.c_str(), &raw) == SQLITE_OK);
        REQUIRE(sqlite3_exec(raw, "UPDATE nodes SET word_idx = 999 WHERE id = 1",
                             nullptr, nullptr, nullptr) == SQLITE_OK);
        sqlite3_close(raw);
    }

    // Re-open and verify: the stored checksum no longer matches the content.
    {
        auto db = Database::open(path);
        REQUIRE(db.has_value());
        auto r = db->verify_integrity();
        CHECK_FALSE(r.has_value());  // tampering detected
    }

    std::filesystem::remove(path);
}

// ─────────────────────────────────────────────────────────────────────────────
// walk_target — shared tree walk (used by build_db::evaluate and CLI eval)
// ─────────────────────────────────────────────────────────────────────────────

// A 3-word list ("aaaaa","bbbbb","ccccc") plus a tiny hand-built tree:
//   root (guess aaaaa) --[BBBBB]--> n1 (guess bbbbb) --[BBBBB]--> n2 (ccccc)
// So: aaaaa solves in 1, bbbbb in 2, ccccc in 3.
static std::string make_word_file(const std::vector<std::string>& words) {
    auto p = (std::filesystem::temp_directory_path() /
              std::format("wptest_walk_{}.txt",
                          std::hash<std::thread::id>{}(std::this_thread::get_id())))
                 .string();
    std::ofstream f(p);
    for (auto& w : words) f << w << "\n";
    return p;
}

TEST_CASE("walk_target - reports correct depth and missing-edge status", "[database]") {
    std::string wf = make_word_file({"aaaaa", "bbbbb", "ccccc"});
    auto wl = WordList::load(wf);
    std::filesystem::remove(wf);
    REQUIRE(wl.has_value());

    const uint16_t ia = wl->index_of("aaaaa");
    const uint16_t ib = wl->index_of("bbbbb");
    const uint16_t ic = wl->index_of("ccccc");
    REQUIRE(ia != WordList::NPOS);

    // Patterns for the guesses we place at each node, against each answer.
    const Pattern a_vs_b = compute_pattern("aaaaa", "bbbbb");
    const Pattern b_vs_c = compute_pattern("bbbbb", "ccccc");

    auto db = Database::create(":memory:");
    REQUIRE(db.has_value());
    REQUIRE(db->begin_transaction().has_value());
    REQUIRE(db->insert_node(0, ia, 1).has_value());  // root guesses aaaaa
    REQUIRE(db->insert_node(1, ib, 2).has_value());  // then bbbbb
    REQUIRE(db->insert_node(2, ic, 3).has_value());  // then ccccc
    REQUIRE(db->insert_edge(0, a_vs_b, 1).has_value());
    REQUIRE(db->insert_edge(1, b_vs_c, 2).has_value());
    REQUIRE(db->commit_transaction().has_value());

    auto oa = walk_target(*db, *wl, "aaaaa");
    CHECK(oa.solved());
    CHECK(oa.depth == 1);

    auto ob = walk_target(*db, *wl, "bbbbb");
    CHECK(ob.solved());
    CHECK(ob.depth == 2);

    auto oc = walk_target(*db, *wl, "ccccc");
    CHECK(oc.solved());
    CHECK(oc.depth == 3);
}

TEST_CASE("walk_target - missing edge yields MissingEdge, not a long FAIL", "[database]") {
    std::string wf = make_word_file({"aaaaa", "bbbbb", "ccccc"});
    auto wl = WordList::load(wf);
    std::filesystem::remove(wf);
    REQUIRE(wl.has_value());

    const uint16_t ia = wl->index_of("aaaaa");

    // Root guesses aaaaa but has NO outgoing edges. Any non-solving target
    // should return MissingEdge immediately.
    auto db = Database::create(":memory:");
    REQUIRE(db.has_value());
    REQUIRE(db->begin_transaction().has_value());
    REQUIRE(db->insert_node(0, ia, 1).has_value());
    REQUIRE(db->commit_transaction().has_value());

    auto o = walk_target(*db, *wl, "ccccc");
    CHECK_FALSE(o.solved());
    CHECK(o.status == WalkOutcome::Status::MissingEdge);
}

// ─────────────────────────────────────────────────────────────────────────────
// Hot-path caching (next_node and node_info called many times)
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Database - repeated next_node calls return consistent results", "[database]") {
    auto db = Database::create(":memory:");
    REQUIRE(db.has_value());

    REQUIRE(db->begin_transaction().has_value());
    REQUIRE(db->insert_node(0, 1, 1).has_value());
    REQUIRE(db->insert_node(1, 2, 2).has_value());
    REQUIRE(db->insert_edge(0, 10, 1).has_value());
    REQUIRE(db->commit_transaction().has_value());

    // Call many times — exercises the statement reset path
    for (int i = 0; i < 50; ++i) {
        auto r = db->next_node(0, 10);
        REQUIRE(r.has_value());
        CHECK(*r == 1);
    }
}

TEST_CASE("Database - repeated node_info calls return consistent results", "[database]") {
    auto db = Database::create(":memory:");
    REQUIRE(db.has_value());

    REQUIRE(db->begin_transaction().has_value());
    REQUIRE(db->insert_node(0, 42, 1).has_value());
    REQUIRE(db->commit_transaction().has_value());

    for (int i = 0; i < 50; ++i) {
        auto r = db->node_info(0);
        REQUIRE(r.has_value());
        CHECK(r->first  == 42);
        CHECK(r->second == 1);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// End-to-end: build a real greedy decision tree into a DB, then assert every
// answer word is solvable via walk_target (the same walk the CLI uses). This
// exercises the full pipeline (solver → DB writes → DB lookups) in one test.
// ─────────────────────────────────────────────────────────────────────────────

namespace {
// Recursively build a greedy decision tree for `candidates` into `db`, mirroring
// the build_db pipeline but using only public library APIs. Returns the node id.
uint32_t build_greedy_tree(Database& db, const EntropySolver& solver,
                           const PatternMatrix& pm,
                           std::vector<uint16_t> candidates, int depth,
                           uint32_t& next_id) {
    const uint32_t my_id = next_id++;
    const uint16_t guess = solver.best_guess(candidates);
    REQUIRE(db.insert_node(my_id, guess, static_cast<uint8_t>(depth)).has_value());

    auto buckets = EntropySolver::partition(candidates, guess, pm);
    for (Pattern p = 0; p < PATTERN_COUNT - 1; ++p) {  // skip GGGGG
        if (buckets[p].empty()) continue;
        uint32_t child = build_greedy_tree(db, solver, pm, buckets[p],
                                           depth + 1, next_id);
        REQUIRE(db.insert_edge(my_id, p, child).has_value());
    }
    return my_id;
}
}  // namespace

TEST_CASE("end-to-end - built tree solves every answer via walk_target", "[database][e2e]") {
    auto wl = WordList::load("data/answers.txt");
    REQUIRE(wl.has_value());
    auto pm = PatternMatrix::build(*wl);
    EntropySolver solver{*wl, pm};

    auto db = Database::create(":memory:");
    REQUIRE(db.has_value());

    REQUIRE(db->begin_transaction().has_value());
    uint32_t next_id = 0;
    build_greedy_tree(*db, solver, pm, wl->all_indices(), 1, next_id);
    REQUIRE(db->commit_transaction().has_value());

    // Every answer word must be reachable and solved by the shared CLI walk.
    int worst = 0, failures = 0;
    for (const auto& w : wl->span()) {
        auto out = walk_target(*db, *wl, w.view());
        if (out.solved()) worst = std::max(worst, out.depth);
        else ++failures;
    }
    CHECK(failures == 0);
    CHECK(worst >= 1);
    CHECK(worst <= 10);  // greedy over the answers-only matrix stays shallow
}
