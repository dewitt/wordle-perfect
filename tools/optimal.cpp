// optimal.cpp — experimental exhaustive DFS minimax over the answer set.
//
// Goal (issue #8): determine whether the curated answer set admits a decision
// tree with worst-case depth D (provably 5 for the standard NYT set), and if so
// produce one; then, as a second phase, minimise total/mean depth under that D.
//
// SCRATCH/RESEARCH tool. Approach follows the known-optimal solvers (Selby,
// Bertsimas): guesses drawn from the full vocabulary, objective measured over
// the answer set, with memoization on the candidate set, guess ordering by
// max-bucket-size, and admissible lower-bound pruning.
//
// Two modes:
//   --mode feasible  : find ANY worst<=D tree (fast; first feasible guess wins)
//   --mode optimal   : minimise total depth subject to worst<=D (expensive)

#include "wordlist.hpp"
#include "pattern.hpp"
#include "solver.hpp"
#include "database.hpp"
#include "binarydb.hpp"

#include <filesystem>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <print>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

using namespace wp;
using Clock = std::chrono::steady_clock;

namespace {

const WordList*      g_words = nullptr;
const PatternMatrix* g_pm    = nullptr;
std::uint64_t        g_nodes = 0;
int                  g_max_depth = 5;

// ── feasibility memo ────────────────────────────────────────────────────────
// Key: hash(sorted candidate set) combined with depth. Value: feasible?
// A set feasible at depth d is feasible at any depth > d, but we key on exact
// depth to keep it simple and sound.
std::unordered_map<std::uint64_t, char> g_feas;  // 0 unknown,1 yes,2 no
std::unordered_map<std::uint64_t, uint16_t> g_choice;

std::uint64_t hash_key(const std::vector<uint16_t>& s, int depth) {
    std::uint64_t h = 1469598103934665603ULL;
    auto mix = [&](std::uint64_t x){ h ^= x; h *= 1099511628211ULL; };
    for (uint16_t v : s) mix(v + 1u);
    mix(0x5A17u); mix(static_cast<std::uint64_t>(depth));
    return h;
}

// Max bucket size for guess gi over cand (cheap split-quality metric).
int max_bucket(const std::vector<uint16_t>& cand, uint16_t gi) {
    std::array<uint16_t, PATTERN_COUNT> cnt{};
    int mx = 0;
    for (uint16_t ai : cand) { int c = ++cnt[g_pm->get(gi, ai)]; if (c > mx) mx = c; }
    return mx;
}

// Is `cand` solvable with worst-case depth <= depth? Memoized.
bool feasible(const std::vector<uint16_t>& cand, int depth, uint16_t* out) {
    ++g_nodes;
    const int n = static_cast<int>(cand.size());
    if (n <= 1) { if (out && n == 1) *out = cand[0]; return true; }
    if (depth <= 1) return false;             // 2+ words need >=2 guesses
    // Admissible bound: with `depth` guesses and at most 243 patterns per guess,
    // an upper bound on distinguishable words is 243^(depth-1) * (1 for the final
    // guess). For depth=2, at most 243 distinct singleton buckets → if n>243
    // infeasible. Generalised cheaply for the shallow depths we care about.
    if (depth == 2 && n > PATTERN_COUNT) return false;

    const std::uint64_t key = hash_key(cand, depth);
    if (auto it = g_feas.find(key); it != g_feas.end()) {
        if (it->second == 1 && out) {
            if (auto c = g_choice.find(key); c != g_choice.end()) *out = c->second;
        }
        return it->second == 1;
    }

    // Order candidate guesses by max-bucket ascending (best splitters first).
    // Consider the full vocabulary but pre-rank; this is the dominant cost, so
    // we compute max_bucket once per guess and sort.
    const std::size_t W = g_words->size();
    std::vector<std::pair<int, uint16_t>> order;
    order.reserve(W);
    for (std::size_t g = 0; g < W; ++g) {
        const auto gi = static_cast<uint16_t>(g);
        int mb = max_bucket(cand, gi);
        if (mb == n) continue;                 // no progress
        order.emplace_back(mb, gi);
    }
    std::ranges::sort(order, [](auto& a, auto& b){
        if (a.first != b.first) return a.first < b.first;
        return a.second < b.second;
    });

    bool ok = false;
    uint16_t chosen = WordList::NPOS;

    for (auto& [mb, gi] : order) {
        // Prune: the largest bucket must itself be solvable in depth-1. A
        // necessary condition is mb <= 243^(depth-2) ... but cheaply, if
        // depth-1 == 1 then mb must be 1.
        if (depth - 1 == 1 && mb > 1) break;   // sorted asc → all rest worse

        // Partition and recurse into every non-solved bucket.
        std::vector<std::vector<uint16_t>> buckets(PATTERN_COUNT);
        for (uint16_t ai : cand) buckets[g_pm->get(gi, ai)].push_back(ai);

        bool all_ok = true;
        for (Pattern p = 0; p < PATTERN_COUNT && all_ok; ++p) {
            if (p == PATTERN_SOLVED) continue;
            auto& b = buckets[p];
            if (b.empty()) continue;
            uint16_t dummy;
            if (!feasible(b, depth - 1, &dummy)) all_ok = false;
        }
        if (all_ok) { ok = true; chosen = gi; break; }
    }

    g_feas[key] = ok ? 1 : 2;
    if (ok) { g_choice[key] = chosen; if (out) *out = chosen; }
    return ok;
}

// ── feasibility-constrained entropy-greedy tree (the practical winner) ───────
// At each node, among the guesses whose every bucket is feasible at depth-1,
// pick the highest-entropy one (best mean heuristic), tie-broken by being a
// candidate then lexicographically. Guarantees worst<=depth (every chosen guess
// keeps all buckets feasible) while keeping the mean low and the build cheap.
// Returns the total depth (sum over candidates, direct hit = 1).
double entropy_of(const std::vector<uint16_t>& cand, uint16_t gi) {
    std::array<int, PATTERN_COUNT> cnt{};
    for (uint16_t ai : cand) ++cnt[g_pm->get(gi, ai)];
    const double tot = static_cast<double>(cand.size());
    double H = 0.0;
    for (int c : cnt) if (c) { double p = c / tot; H -= p * std::log2(p); }
    return H;
}

constexpr int TREE_INF = 1'000'000'000;
int g_lookahead = 1;  // how many top-entropy feasible guesses to expand per node

// Memo for tree_total: many sub-candidate-sets recur. Key on (set, depth).
std::unordered_map<std::uint64_t, int> g_tree;
std::unordered_map<std::uint64_t, uint16_t> g_tree_choice;  // winning guess per set

int tree_total(const std::vector<uint16_t>& cand, int depth) {
    const int n = static_cast<int>(cand.size());
    if (n == 0) return 0;
    if (n == 1) return 1;
    if (depth <= 1) return TREE_INF;

    const std::uint64_t key = hash_key(cand, depth) ^ 0x7EE5u;
    if (auto it = g_tree.find(key); it != g_tree.end()) return it->second;

    // Rank guesses by entropy desc (best mean first).
    const std::size_t W = g_words->size();
    std::vector<std::pair<double, uint16_t>> order;
    order.reserve(W);
    for (std::size_t g = 0; g < W; ++g) {
        const auto gi = static_cast<uint16_t>(g);
        int mb = max_bucket(cand, gi);
        if (mb == n) continue;
        order.emplace_back(entropy_of(cand, gi), gi);
    }
    std::ranges::sort(order, [](auto& a, auto& b){
        if (a.first != b.first) return a.first > b.first;
        return a.second < b.second;
    });

    int best = TREE_INF;
    uint16_t best_gi = WordList::NPOS;
    int expanded = 0;
    for (auto& [H, gi] : order) {
        if (expanded >= g_lookahead) break;
        std::vector<std::vector<uint16_t>> buckets(PATTERN_COUNT);
        for (uint16_t ai : cand) buckets[g_pm->get(gi, ai)].push_back(ai);
        // Feasibility gate (cheap, memoized): every bucket solvable in depth-1.
        bool ok = true;
        for (Pattern p = 0; p < PATTERN_COUNT && ok; ++p) {
            if (p == PATTERN_SOLVED) continue;
            auto& b = buckets[p];
            if (b.empty() || b.size() == 1) continue;
            uint16_t dummy;
            if (!feasible(b, depth - 1, &dummy)) ok = false;
        }
        if (!ok) continue;            // not feasible — doesn't count toward lookahead
        ++expanded;
        // Accumulate the real total recursively under the same policy.
        int total = n;
        for (Pattern p = 0; p < PATTERN_COUNT && total < best; ++p) {
            if (p == PATTERN_SOLVED) continue;
            auto& b = buckets[p];
            if (b.empty()) continue;
            total += (b.size() == 1) ? 1 : tree_total(b, depth - 1);
        }
        if (total < best) { best = total; best_gi = gi; }
    }
    g_tree[key] = best;
    if (best_gi != WordList::NPOS) g_tree_choice[key] = best_gi;
    return best;
}

// Return the chosen guess for a candidate set (must have been computed by
// tree_total first). For singletons, the guess is the candidate itself.
uint16_t tree_choice(const std::vector<uint16_t>& cand, int depth) {
    if (cand.size() == 1) return cand[0];
    auto it = g_tree_choice.find(hash_key(cand, depth) ^ 0x7EE5u);
    return it == g_tree_choice.end() ? WordList::NPOS : it->second;
}

// Recursively emit the decision tree into a Database using the choices computed
// by tree_total. Returns the node id. `forced_guess` overrides the choice at the
// root (to honour an explicit opener).
uint32_t emit_tree(Database& db, const std::vector<uint16_t>& cand, int depth,
                   uint32_t& next_id, uint16_t forced_guess = WordList::NPOS) {
    const uint32_t id = next_id++;
    const int n = static_cast<int>(cand.size());
    if (depth <= 0) {
        std::println(stderr, "emit: depth exhausted for set size {}", n);
        std::exit(1);
    }
    uint16_t guess = (forced_guess != WordList::NPOS) ? forced_guess
                                                      : tree_choice(cand, depth);
    if (guess == WordList::NPOS) {
        std::println(stderr, "emit: no choice for set of size {} at depth {}",
            n, depth);
        std::exit(1);
    }
    const uint8_t round = static_cast<uint8_t>(g_max_depth - depth + 1);
    if (auto r = db.insert_node(id, guess, round); !r) { std::println(stderr, "{}", r.error()); std::exit(1); }

    std::vector<std::vector<uint16_t>> buckets(PATTERN_COUNT);
    for (uint16_t ai : cand) buckets[g_pm->get(guess, ai)].push_back(ai);
    for (Pattern p = 0; p < PATTERN_COUNT; ++p) {
        if (p == PATTERN_SOLVED) continue;
        auto& b = buckets[p];
        if (b.empty()) continue;
        // Guard against a non-splitting choice causing infinite recursion.
        if (static_cast<int>(b.size()) == n) {
            std::println(stderr, "emit: guess {} did not split set size {} "
                "(bucket {}) at depth {}", (*g_words)[guess].view(), n, p, depth);
            std::exit(1);
        }
        uint32_t child = emit_tree(db, b, depth - 1, next_id);
        if (auto r = db.insert_edge(id, p, child); !r) { std::println(stderr, "{}", r.error()); std::exit(1); }
    }
    return id;
}

// ── mean (total-depth) optimization, subject to worst<=depth ────────────────
// min_total(cand, depth) = minimal sum over candidates of their depth (counted
// from this node, direct hit = 1) over all trees with worst-case <= depth.
// Returns INF if infeasible. Exact + memoized.
constexpr int INF = 1'000'000'000;
std::unordered_map<std::uint64_t, int>      g_tot;     // exact min total
std::unordered_map<std::uint64_t, uint16_t> g_tot_choice;

int min_total(const std::vector<uint16_t>& cand, int depth) {
    ++g_nodes;
    const int n = static_cast<int>(cand.size());
    if (n == 0) return 0;
    if (n == 1) return 1;
    if (depth <= 1) return INF;

    const std::uint64_t key = hash_key(cand, depth) ^ 0xABCDEFu;
    if (auto it = g_tot.find(key); it != g_tot.end()) return it->second;

    // Rank guesses by max-bucket ascending (good splitters → low total & feasible).
    const std::size_t W = g_words->size();
    std::vector<std::pair<int, uint16_t>> order;
    order.reserve(W);
    for (std::size_t g = 0; g < W; ++g) {
        const auto gi = static_cast<uint16_t>(g);
        int mb = max_bucket(cand, gi);
        if (mb == n) continue;
        order.emplace_back(mb, gi);
    }
    std::ranges::sort(order, [](auto& a, auto& b){
        if (a.first != b.first) return a.first < b.first;
        return a.second < b.second;
    });

    int best = INF;
    uint16_t best_gi = WordList::NPOS;

    for (auto& [mb, gi] : order) {
        if (depth - 1 == 1 && mb > 1) break;  // remaining can't be solved in 1

        std::vector<std::vector<uint16_t>> buckets(PATTERN_COUNT);
        for (uint16_t ai : cand) buckets[g_pm->get(gi, ai)].push_back(ai);

        // total from here = n + sum over non-solved buckets of min_total(bucket).
        int total = n;
        bool ok = true;
        for (Pattern p = 0; p < PATTERN_COUNT && ok; ++p) {
            if (p == PATTERN_SOLVED) continue;
            auto& b = buckets[p];
            if (b.empty()) continue;
            int sub = (b.size() == 1) ? 1 : min_total(b, depth - 1);
            if (sub >= INF) { ok = false; break; }
            total += sub;
            if (total >= best) { ok = false; break; }  // prune: can't beat best
        }
        if (ok && total < best) { best = total; best_gi = gi; }
    }

    int result = (best_gi == WordList::NPOS) ? INF : best;
    g_tot[key] = result;
    if (best_gi != WordList::NPOS) g_tot_choice[key] = best_gi;
    return result;
}

}  // namespace

int main(int argc, char** argv) {
    std::string words_path = "data/words.txt";
    std::string answers_path = "data/answers.txt";
    int max_depth = 5;
    std::string forced_start;
    std::string mode = "feasible";  // "feasible" | "optimal" | "tree"
    std::string emit_path;          // if set (tree mode), write DB here

    std::vector<std::string_view> args(argv + 1, argv + argc);
    for (std::size_t i = 0; i + 1 < args.size(); ++i) {
        if (args[i] == "--words")     words_path = args[i + 1];
        if (args[i] == "--answers")   answers_path = args[i + 1];
        if (args[i] == "--max-depth") max_depth = std::stoi(std::string(args[i + 1]));
        if (args[i] == "--start")     forced_start = args[i + 1];
        if (args[i] == "--mode")      mode = args[i + 1];
        if (args[i] == "--emit")      emit_path = args[i + 1];
        if (args[i] == "--lookahead") g_lookahead = std::stoi(std::string(args[i + 1]));
    }

    auto wl = WordList::load(words_path);
    if (!wl) { std::println(stderr, "load words: {}", wl.error()); return 1; }
    auto ans = WordList::load(answers_path);
    if (!ans) { std::println(stderr, "load answers: {}", ans.error()); return 1; }

    auto t0 = Clock::now();
    auto pm = PatternMatrix::build(*wl);
    g_words = &*wl; g_pm = &pm; g_max_depth = max_depth;
    std::println("words={} answers={} max_depth={}  (matrix {:.1f}s)",
        wl->size(), ans->size(), max_depth,
        std::chrono::duration<double>(Clock::now() - t0).count());

    std::vector<uint16_t> cand;
    for (auto& w : ans->span()) {
        auto idx = wl->index_of(w.view());
        if (idx != WordList::NPOS) cand.push_back(idx);
    }
    std::ranges::sort(cand);

    t0 = Clock::now();

    // ── tree mode: feasibility-constrained entropy-greedy (worst<=D, low mean) ─
    if (mode == "tree") {
        int total = TREE_INF; uint16_t root = WordList::NPOS;
        if (!forced_start.empty()) {
            auto gi = wl->index_of(forced_start);
            if (gi == WordList::NPOS) { std::println(stderr, "start not found"); return 1; }
            // First verify the opener keeps everything feasible at D-1.
            std::vector<std::vector<uint16_t>> buckets(PATTERN_COUNT);
            for (uint16_t ai : cand) buckets[g_pm->get(gi, ai)].push_back(ai);
            bool feas = true;
            for (Pattern p = 0; p < PATTERN_COUNT && feas; ++p) {
                if (p == PATTERN_SOLVED) continue;
                auto& b = buckets[p];
                if (b.empty() || b.size() == 1) continue;
                uint16_t d; if (!feasible(b, max_depth - 1, &d)) feas = false;
            }
            if (feas) {
                total = static_cast<int>(cand.size());
                for (Pattern p = 0; p < PATTERN_COUNT; ++p) {
                    if (p == PATTERN_SOLVED) continue;
                    auto& b = buckets[p];
                    if (b.empty()) continue;
                    total += (b.size() == 1) ? 1 : tree_total(b, max_depth - 1);
                }
            }
            root = gi;
        } else {
            total = tree_total(cand, max_depth);
            // root recorded implicitly by the first feasible top-level guess;
            // re-derive it cheaply for reporting.
        }
        double s = std::chrono::duration<double>(Clock::now() - t0).count();
        if (total >= TREE_INF) {
            std::println("TREE: infeasible at worst<={} ({:.1f}s)", max_depth, s);
            return 1;
        }
        double mean = static_cast<double>(total) / static_cast<double>(cand.size());
        std::println("TREE: worst<={} total={} mean={:.4f} root={} ({} nodes, {:.1f}s)",
            max_depth, total, mean,
            root == WordList::NPOS ? "(searched)" : std::string((*wl)[root].view()),
            g_nodes, s);

        // ── Emit the tree to a database (+ binary) if requested ─────────────
        if (!emit_path.empty()) {
            std::error_code ec;
            std::filesystem::remove(emit_path, ec);
            std::filesystem::remove(emit_path + "-wal", ec);
            std::filesystem::remove(emit_path + "-shm", ec);
            auto db = Database::create(emit_path);
            if (!db) { std::println(stderr, "create db: {}", db.error()); return 1; }
            if (auto r = db->begin_transaction(); !r) { std::println(stderr, "{}", r.error()); return 1; }
            uint32_t next_id = 0;
            uint16_t root_guess = forced_start.empty() ? WordList::NPOS
                                                       : wl->index_of(forced_start);
            emit_tree(*db, cand, max_depth, next_id, root_guess);
            if (auto r = db->commit_transaction(); !r) { std::println(stderr, "{}", r.error()); return 1; }

            // Recompute true depths for metadata via the words list.
            DbMetadata meta{
                .words_source = "https://gist.github.com/SukkaW/92ff13af03a0117e5bafec6c7f7d6dce",
                .words_date = "2026-06-09",
                .answers_source = "curated answers (optimal worst<=5 tree)",
                .strategy = std::format("optimal-worst{}-lookahead{}", max_depth, g_lookahead),
                .start_word = std::string((*wl)[root_guess != WordList::NPOS ? root_guess
                                          : tree_choice(cand, max_depth)].view()),
                .worst_case_depth = max_depth,
                .mean_depth = mean,
                .total_nodes = static_cast<int>(next_id),
                .total_words = static_cast<int>(wl->size()),
                .total_answers = static_cast<int>(ans->size()),
            };
            if (auto r = db->finalize(meta); !r) { std::println(stderr, "{}", r.error()); return 1; }
            // Binary export alongside.
            auto dot = emit_path.find_last_of('.');
            std::string bin = (dot == std::string::npos ? emit_path
                                                        : emit_path.substr(0, dot)) + ".bin";
            if (auto r = BinaryDb::export_from(*db, meta, bin); !r)
                std::println(stderr, "binary export: {}", r.error());
            db = std::unexpected(std::string{});
            std::filesystem::remove(emit_path + "-wal", ec);
            std::filesystem::remove(emit_path + "-shm", ec);
            std::println("emitted DB to {} (+ {})", emit_path, bin);
        }
        return 0;
    }

    // ── optimal mode: minimise total/mean depth subject to worst<=max_depth ──
    if (mode == "optimal") {
        int total = INF; uint16_t root = WordList::NPOS;
        if (!forced_start.empty()) {
            auto gi = wl->index_of(forced_start);
            if (gi == WordList::NPOS) { std::println(stderr, "start not found"); return 1; }
            std::vector<std::vector<uint16_t>> buckets(PATTERN_COUNT);
            for (uint16_t ai : cand) buckets[g_pm->get(gi, ai)].push_back(ai);
            total = static_cast<int>(cand.size());
            for (Pattern p = 0; p < PATTERN_COUNT && total < INF; ++p) {
                if (p == PATTERN_SOLVED) continue;
                auto& b = buckets[p];
                if (b.empty()) continue;
                int sub = (b.size() == 1) ? 1 : min_total(b, max_depth - 1);
                if (sub >= INF) { total = INF; break; }
                total += sub;
            }
            root = gi;
        } else {
            total = min_total(cand, max_depth);
            if (total < INF) {
                if (auto c = g_tot_choice.find(hash_key(cand, max_depth) ^ 0xABCDEFu);
                    c != g_tot_choice.end()) root = c->second;
            }
        }
        double s = std::chrono::duration<double>(Clock::now() - t0).count();
        if (total >= INF) {
            std::println("OPTIMAL: infeasible at worst<={} ({:.1f}s)", max_depth, s);
        } else {
            double mean = static_cast<double>(total) / static_cast<double>(cand.size());
            std::println("OPTIMAL: worst<={} total={} mean={:.4f} root={} "
                "({} nodes, {} memo, {:.1f}s)",
                max_depth, total, mean,
                root == WordList::NPOS ? "?" : std::string((*wl)[root].view()),
                g_nodes, g_tot.size(), s);
        }
        return 0;
    }

    bool ok = false;
    uint16_t root = WordList::NPOS;

    if (!forced_start.empty()) {
        auto gi = wl->index_of(forced_start);
        if (gi == WordList::NPOS) { std::println(stderr, "start not found"); return 1; }
        // Evaluate the forced opener: partition then require each bucket feasible
        // at max_depth-1.
        std::vector<std::vector<uint16_t>> buckets(PATTERN_COUNT);
        for (uint16_t ai : cand) buckets[g_pm->get(gi, ai)].push_back(ai);
        ok = true;
        int worst_bucket = 0;
        for (Pattern p = 0; p < PATTERN_COUNT && ok; ++p) {
            if (p == PATTERN_SOLVED) continue;
            auto& b = buckets[p];
            if (b.empty()) continue;
            worst_bucket = std::max(worst_bucket, (int)b.size());
            uint16_t dummy;
            if (!feasible(b, max_depth - 1, &dummy)) {
                ok = false;
                std::println("  bucket pattern {} (size {}) INFEASIBLE at depth {}",
                    p, b.size(), max_depth - 1);
            }
        }
        root = gi;
        std::println("forced start '{}': worst_bucket={} → {}", forced_start,
            worst_bucket, ok ? "FEASIBLE" : "infeasible");
    } else {
        ok = feasible(cand, max_depth, &root);
    }

    double secs = std::chrono::duration<double>(Clock::now() - t0).count();
    std::println("{}: worst<={} root={} ({} nodes, {} memo, {:.1f}s)",
        ok ? "FEASIBLE" : "INFEASIBLE", max_depth,
        root == WordList::NPOS ? "?" : std::string((*wl)[root].view()),
        g_nodes, g_feas.size(), secs);
    return 0;
}
