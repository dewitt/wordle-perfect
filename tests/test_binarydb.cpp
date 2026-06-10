#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "binarydb.hpp"
#include "database.hpp"
#include "pattern.hpp"
#include "solver.hpp"
#include "wordlist.hpp"

#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <thread>
#include <vector>

using namespace wp;

namespace {

std::string tmp_path(const char* tag) {
    return (std::filesystem::temp_directory_path() /
            std::format("wptest_bin_{}_{}.bin", tag,
                        std::hash<std::thread::id>{}(std::this_thread::get_id())))
        .string();
}

DbMetadata sample_meta() {
    return DbMetadata{
        .words_source     = "test-src",
        .words_date       = "2026-06-08",
        .answers_source   = "test-ans",
        .strategy         = "test-strategy",
        .start_word       = "crane",
        .worst_case_depth = 3,
        .mean_depth       = 2.5,
        .total_nodes      = 3,
        .total_words      = 10,
        .total_answers    = 5,
    };
}

struct SmallTree {
    Database db;
    Pattern  a, b;
};

// root(0,word=11) --[a]--> 1(word=22) --[b]--> 2(word=33)
SmallTree make_small_sqlite() {
    auto db = Database::create(":memory:");
    REQUIRE(db.has_value());
    const Pattern a = 5, b = 7;
    REQUIRE(db->begin_transaction().has_value());
    REQUIRE(db->insert_node(0, 11, 1).has_value());
    REQUIRE(db->insert_node(1, 22, 2).has_value());
    REQUIRE(db->insert_node(2, 33, 3).has_value());
    REQUIRE(db->insert_edge(0, a, 1).has_value());
    REQUIRE(db->insert_edge(1, b, 2).has_value());
    REQUIRE(db->commit_transaction().has_value());
    REQUIRE(db->finalize(sample_meta()).has_value());
    return {std::move(*db), a, b};
}

}  // namespace

TEST_CASE("BinaryDb - export then open round-trips nodes and edges", "[binarydb]") {
    auto tree = make_small_sqlite();
    std::string path = tmp_path("rt");
    std::filesystem::remove(path);

    REQUIRE(BinaryDb::export_from(tree.db, sample_meta(), path).has_value());

    auto bin = BinaryDb::open(path);
    REQUIRE(bin.has_value());

    CHECK(bin->node_count() == 3);
    CHECK(bin->edge_count() == 2);

    auto r0 = bin->node_info(0);
    REQUIRE(r0.has_value());
    CHECK(r0->first == 11);
    CHECK(r0->second == 1);

    auto r2 = bin->node_info(2);
    REQUIRE(r2.has_value());
    CHECK(r2->first == 33);

    auto rw = bin->root_word();
    REQUIRE(rw.has_value());
    CHECK(*rw == 11);

    auto n1 = bin->next_node(0, tree.a);
    REQUIRE(n1.has_value());
    CHECK(*n1 == 1);
    auto n2 = bin->next_node(1, tree.b);
    REQUIRE(n2.has_value());
    CHECK(*n2 == 2);

    auto solved = bin->next_node(0, PATTERN_SOLVED);
    REQUIRE(solved.has_value());
    CHECK(*solved == BinaryDb::NULL_NODE);
    CHECK_FALSE(bin->next_node(0, 99).has_value());
    CHECK_FALSE(bin->node_info(999).has_value());

    std::filesystem::remove(path);
}

TEST_CASE("BinaryDb - metadata round-trips", "[binarydb]") {
    auto tree = make_small_sqlite();
    std::string path = tmp_path("meta");
    std::filesystem::remove(path);
    auto meta = sample_meta();
    REQUIRE(BinaryDb::export_from(tree.db, meta, path).has_value());

    auto bin = BinaryDb::open(path);
    REQUIRE(bin.has_value());
    auto rm = bin->read_metadata();
    REQUIRE(rm.has_value());
    CHECK(rm->strategy         == meta.strategy);
    CHECK(rm->start_word       == meta.start_word);
    CHECK(rm->words_source     == meta.words_source);
    CHECK(rm->words_date       == meta.words_date);
    CHECK(rm->answers_source   == meta.answers_source);
    CHECK(rm->worst_case_depth == meta.worst_case_depth);
    CHECK(rm->total_words      == meta.total_words);
    CHECK(rm->total_answers    == meta.total_answers);
    CHECK(rm->mean_depth == Catch::Approx(meta.mean_depth));

    std::filesystem::remove(path);
}

TEST_CASE("BinaryDb - verify_integrity passes clean, fails on tamper", "[binarydb]") {
    auto tree = make_small_sqlite();
    std::string path = tmp_path("integ");
    std::filesystem::remove(path);
    REQUIRE(BinaryDb::export_from(tree.db, sample_meta(), path).has_value());

    {
        auto bin = BinaryDb::open(path);
        REQUIRE(bin.has_value());
        CHECK(bin->verify_integrity().has_value());
    }

    // Flip a byte in the body (past the 64-byte header) → checksum must fail.
    {
        std::fstream f(path, std::ios::in | std::ios::out | std::ios::binary);
        REQUIRE(f.good());
        char c{};
        f.seekg(70); f.get(c);
        c = static_cast<char>(c ^ 0xFF);
        f.seekp(70); f.put(c);
    }
    {
        auto bin = BinaryDb::open(path);
        REQUIRE(bin.has_value());
        CHECK_FALSE(bin->verify_integrity().has_value());
    }

    std::filesystem::remove(path);
}

TEST_CASE("BinaryDb - dump runs and lists nodes/edges (debug_dump_tool)", "[binarydb]") {
    // Spec debug_dump_tool invariant: the binary format must offer a dump.
    auto wl = WordList::load("data/answers.txt");
    REQUIRE(wl.has_value());

    auto tree = make_small_sqlite();
    std::string path = tmp_path("dump");
    std::filesystem::remove(path);
    REQUIRE(BinaryDb::export_from(tree.db, sample_meta(), path).has_value());
    auto bin = BinaryDb::open(path);
    REQUIRE(bin.has_value());

    // Just exercise the code path (output goes to stdout); a crash/throw fails.
    bin->dump(*wl);
    SUCCEED("dump completed");

    std::filesystem::remove(path);
}

TEST_CASE("BinaryDb - open rejects bad magic and missing files", "[binarydb]") {
    std::string path = tmp_path("bad");
    std::filesystem::remove(path);
    {
        std::ofstream f(path, std::ios::binary);
        f << "not a binary db file at all, but long enough to exceed the header";
    }
    CHECK_FALSE(BinaryDb::open(path).has_value());
    std::filesystem::remove(path);

    CHECK_FALSE(BinaryDb::open("/nonexistent/path/x.bin").has_value());
}

TEST_CASE("BinaryDb - lookups match SQLite on the real answers tree", "[binarydb][slow]") {
    // Build a real greedy tree into SQLite, export to binary, and confirm binary
    // lookups agree with SQLite for every node reached while walking all answers.
    auto wl = WordList::load("data/answers.txt");
    REQUIRE(wl.has_value());
    auto pm = PatternMatrix::build(*wl);
    EntropySolver solver{*wl, pm};

    auto db = Database::create(":memory:");
    REQUIRE(db.has_value());
    REQUIRE(db->begin_transaction().has_value());

    // Iterative greedy build mirroring build_db's structure.
    uint32_t next_id = 0;
    std::function<uint32_t(std::vector<uint16_t>, int)> build =
        [&](std::vector<uint16_t> cand, int depth) -> uint32_t {
        uint32_t id = next_id++;
        uint16_t guess = solver.best_guess(cand);
        REQUIRE(db->insert_node(id, guess, static_cast<Depth>(depth)).has_value());
        auto buckets = EntropySolver::partition(cand, guess, pm);
        for (Pattern p = 0; p < PATTERN_COUNT - 1; ++p) {
            if (buckets[p].empty()) continue;
            uint32_t child = build(buckets[p], depth + 1);
            REQUIRE(db->insert_edge(id, p, child).has_value());
        }
        return id;
    };
    build(wl->all_indices(), 1);
    REQUIRE(db->commit_transaction().has_value());

    auto meta = sample_meta();
    meta.total_nodes = static_cast<int>(db->node_count());
    REQUIRE(db->finalize(meta).has_value());

    std::string path = tmp_path("parity");
    std::filesystem::remove(path);
    REQUIRE(BinaryDb::export_from(*db, meta, path).has_value());
    auto bin = BinaryDb::open(path);
    REQUIRE(bin.has_value());
    REQUIRE(bin->verify_integrity().has_value());

    int mismatches = 0;
    for (const auto& w : wl->span()) {
        std::string_view target = w.view();
        uint32_t sq_node = Database::ROOT_ID;
        uint32_t bn_node = BinaryDb::ROOT_ID;
        for (int round = 1; round <= 12; ++round) {
            auto si = db->node_info(sq_node);
            auto bi = bin->node_info(bn_node);
            REQUIRE(si.has_value());
            REQUIRE(bi.has_value());
            if (si->first != bi->first || si->second != bi->second) ++mismatches;

            Pattern p = compute_pattern((*wl)[si->first].view(), target);
            if (p == PATTERN_SOLVED) break;

            auto sn = db->next_node(sq_node, p);
            auto bnn = bin->next_node(bn_node, p);
            REQUIRE(sn.has_value());
            REQUIRE(bnn.has_value());
            if (*sn != *bnn) ++mismatches;
            sq_node = *sn;
            bn_node = *bnn;
        }
    }
    CHECK(mismatches == 0);

    std::filesystem::remove(path);
}
