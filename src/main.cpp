#include "wordlist.hpp"
#include "pattern.hpp"
#include "database.hpp"

#include <algorithm>
#include <cstdlib>
#include <format>
#include <iostream>
#include <print>
#include <ranges>
#include <string>
#include <string_view>

using namespace wp;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
[[noreturn]] static void die(std::string_view msg) {
    std::println(stderr, "error: {}", msg);
    std::exit(1);
}

static void print_step(int n, std::string_view word, Pattern p) {
    auto dec = decode_pattern(p);
    std::println("  {:d}. {}  [{}{}{}{}{} ]", n, word,
        dec[0], dec[1], dec[2], dec[3], dec[4]);
}

// ---------------------------------------------------------------------------
// Mode: info — print database metadata
// ---------------------------------------------------------------------------
static void mode_info(const Database& db) {
    auto meta = db.read_metadata();
    if (!meta) die(meta.error());
    auto& m = *meta;
    std::println("wordle-perfect database");
    std::println("  strategy    : {}", m.strategy.empty() ? "(none)" : m.strategy);
    std::println("  start word  : {}", m.start_word.empty() ? "(unknown)" : m.start_word);
    std::println("  words       : {}", m.total_words);
    std::println("  answers     : {}", m.total_answers);
    std::println("  nodes       : {}", db.node_count());
    std::println("  worst case  : {} guesses", m.worst_case_depth);
    std::println("  mean depth  : {:.4f} guesses", m.mean_depth);
    std::println("  words src   : {}", m.words_source);
    std::println("  words date  : {}", m.words_date);
    std::println("  answers src : {}", m.answers_source);
}

// ---------------------------------------------------------------------------
// Mode: dump — human-readable tree dump
// ---------------------------------------------------------------------------
static void mode_dump(const Database& db, const WordList& words) {
    db.dump(words);
}

// ---------------------------------------------------------------------------
// Mode: solve — show precomputed path for a known target word
// ---------------------------------------------------------------------------
static void mode_solve(const Database& db, const WordList& words,
                       const WordList& answers, std::string_view target,
                       int max_rounds, bool full_coverage, double mean_depth) {
    // In full-coverage mode, any word in the guess list is a valid target.
    // In answers-only mode, only curated answer words have precomputed paths.
    if (full_coverage) {
        if (!words.contains(target)) {
            std::println(stderr, "error: '{}' is not in the word list", target);
            std::exit(1);
        }
    } else {
        if (!answers.contains(target)) {
            if (words.contains(target)) {
                std::println(stderr,
                    "error: '{}' is a valid guess but not an answer word; "
                    "solve paths only exist for the {} answer words",
                    target, answers.size());
            } else {
                std::println(stderr, "error: '{}' is not in the word list", target);
            }
            std::exit(1);
        }
    }

    uint32_t node = Database::ROOT_ID;
    int step = 0;

    std::println("solving: {}", target);
    while (true) {
        auto info = db.node_info(node);
        if (!info) die(info.error());
        auto [word_idx, depth] = *info;

        std::string_view guess = words[word_idx].view();
        Pattern p = compute_pattern(guess, target);
        ++step;
        print_step(step, guess, p);

        if (p == PATTERN_SOLVED) break;
        if (step >= max_rounds) {
            std::println(stderr, "  (failed to solve within {} guesses)", max_rounds);
            std::exit(1);
        }

        auto nxt = db.next_node(node, p);
        if (!nxt) die(nxt.error());
        node = *nxt;
    }

    std::println("solved in {} guess{}  (db mean: {:.4f})", step,
        step == 1 ? "" : "es", mean_depth);
}

// ---------------------------------------------------------------------------
// Mode: play — interactive solver (tool guesses, user responds)
// ---------------------------------------------------------------------------
static void mode_play(const Database& db, const WordList& words, int max_rounds) {
    auto root_word_res = db.root_word();
    if (!root_word_res) die(root_word_res.error());

    uint32_t node = Database::ROOT_ID;

    // Track previous (guess, response) pairs for consistency checking
    struct Prior { std::string guess; Pattern response; };
    std::vector<Prior> history;

    std::println("wordle-perfect solver  (respond with G/Y/B, e.g. BGGYB)");
    std::println("  G = correct letter, correct position");
    std::println("  Y = correct letter, wrong position");
    std::println("  B = letter not in answer");
    std::println("");

    for (int round = 1; round <= max_rounds; ++round) {
        auto info = db.node_info(node);
        if (!info) die(info.error());
        auto [word_idx, depth] = *info;
        std::string_view guess = words[word_idx].view();

        std::println("guess {}: {}", round, guess);
        std::print("response: ");
        std::cout.flush();

        std::string resp;
        while (true) {
            if (!std::getline(std::cin, resp)) die("unexpected end of input");
            if (!valid_response_string(resp)) {
                std::println(stderr,
                    "invalid response '{}': enter exactly 5 characters, each G, Y, or B",
                    resp);
                std::print("response: ");
                std::cout.flush();
                continue;
            }
            break;
        }

        Pattern p = encode_response(resp);
        if (p == 0xFF) die("internal error: encode_response returned invalid pattern");

        // Consistency check: verify this response is compatible with all prior
        // (guess, response) pairs by ensuring the implied answer constraints agree.
        // We check by computing what pattern each prior guess would produce for a
        // hypothetical word that matches the current response, using candidate filtering.
        // Simplified check: if p == SOLVED, no prior guess should have had a non-SOLVED
        // response claiming the answer is not this guess.
        // Full consistency is enforced by the word list: if no word is consistent
        // with all responses, we error.
        {
            // Build remaining candidates consistent with all prior (guess, response) + this one
            auto all = words.all_indices();

            // Build a temporary pattern matrix for consistency checking
            // (only needed here, not the full 14k×14k matrix)
            // For each candidate, check all prior constraints
            auto consistent = [&](uint16_t ai) {
                std::string_view av = words[ai].view();
                for (auto& [g, r] : history) {
                    if (compute_pattern(g, av) != r) return false;
                }
                // Also check current guess + response
                if (compute_pattern(guess, av) != p) return false;
                return true;
            };

            auto remaining = all | std::views::filter(consistent)
                                 | std::ranges::to<std::vector>();

            if (remaining.empty()) {
                std::println(stderr,
                    "\nerror: response '{}' to '{}' is inconsistent with prior responses",
                    resp, guess);
                std::println(stderr,
                    "no word in the list is consistent with all responses so far");
                std::exit(1);
            }
        }

        history.push_back({std::string(guess), p});

        if (p == PATTERN_SOLVED) {
            std::println("\nsolved in {} guess{}!", round, round == 1 ? "" : "es");
            return;
        }

        if (round == max_rounds) {
            std::println(stderr, "\nfailed to solve within {} guesses", max_rounds);
            std::exit(1);
        }

        auto nxt = db.next_node(node, p);
        if (!nxt) die(nxt.error());
        node = *nxt;
    }
}

// ---------------------------------------------------------------------------
// Mode: eval — batch evaluation
// ---------------------------------------------------------------------------
static void mode_eval(const Database& db, const WordList& words,
                      std::string_view eval_file, int max_rounds) {
    // Load evaluation word list
    auto eval_wl = WordList::load(eval_file);
    if (!eval_wl) die(eval_wl.error());

    std::vector<int> depths;
    int failures = 0;

    for (auto& w : eval_wl->span()) {
        std::string_view target = w.view();

        auto idx = words.index_of(target);
        if (idx == WordList::NPOS) {
            std::println("SKIP  {}  (not in word list)", target);
            continue;
        }

        uint32_t node = Database::ROOT_ID;
        int depth = 0;
        bool solved = false;

        for (int round = 1; round <= max_rounds && !solved; ++round) {
            auto info = db.node_info(node);
            if (!info) { std::println(stderr, "db error: {}", info.error()); break; }
            auto [word_idx, d] = *info;

            std::string_view guess = words[word_idx].view();
            Pattern p = compute_pattern(guess, target);
            depth = round;

            if (p == PATTERN_SOLVED) { solved = true; break; }

            auto nxt = db.next_node(node, p);
            if (!nxt) { std::println(stderr, "missing edge for {}", target); break; }
            node = *nxt;
        }

        if (solved) {
            depths.push_back(depth);
            std::println("{:5}  {}", target, depth);
        } else {
            ++failures;
            std::println("{:5}  FAIL", target);
        }
    }

    if (depths.empty()) { std::println("no results"); return; }

    double sum = 0.0;
    int worst = 0;
    std::vector<int> dist(max_rounds + 1, 0);  // dist[1..max_rounds]
    for (int d : depths) {
        sum += d;
        worst = std::max(worst, d);
        if (d >= 1 && d <= max_rounds) dist[d]++;
    }
    double mean = sum / static_cast<double>(depths.size());

    std::println("");
    std::println("--- statistics ---");
    std::println("  words evaluated : {}", depths.size() + failures);
    std::println("  solved          : {}", depths.size());
    std::println("  failed          : {}", failures);
    std::println("  worst case      : {} guesses", worst);
    std::println("  mean depth      : {:.4f} guesses", mean);
    std::println("  distribution    :");
    for (int i = 1; i <= max_rounds; ++i) {
        if (dist[i] > 0)
            std::println("    {} guesses: {:5d}", i, dist[i]);
    }
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
    std::string db_path      = "wordle.db";
    std::string words_path   = "data/words.txt";
    std::string answers_path = "data/answers.txt";

    // Parse global flags
    std::vector<std::string_view> args{argv + 1, argv + argc};
    auto consume = [&](std::string_view flag) -> std::string_view {
        for (auto it = args.begin(); it != args.end(); ++it) {
            if (*it == flag && std::next(it) != args.end()) {
                auto val = *std::next(it);
                args.erase(it, it + 2);
                return val;
            }
        }
        return {};
    };

    if (auto v = consume("--db");      !v.empty()) db_path      = v;
    if (auto v = consume("--words");   !v.empty()) words_path   = v;
    if (auto v = consume("--answers"); !v.empty()) answers_path = v;

    if (args.empty()) {
        std::println(stderr,
            "usage: wordle [--db <path>] [--words <path>] [--answers <path>] <command> [args]\n"
            "commands:\n"
            "  solve <word>   show precomputed path to <word>\n"
            "  play           interactive solver (you provide responses)\n"
            "  eval [<file>]  evaluate word list against database\n"
            "  info           show database metadata\n"
            "  dump           dump full decision tree (debug)\n");
        return 1;
    }

    std::string_view cmd = args[0];

    // Load word lists
    auto wl_res = WordList::load(words_path);
    if (!wl_res) die("loading word list: " + wl_res.error());
    const WordList& words = *wl_res;

    auto ans_res = WordList::load(answers_path);
    if (!ans_res) die("loading answers list: " + ans_res.error());
    const WordList& answers = *ans_res;

    // All commands except future 'build' need the database
    if (cmd != "build") {
        auto db_res = Database::open(db_path);
        if (!db_res) die("opening database: " + db_res.error());
        Database& db = *db_res;

        // Integrity check on every launch
        if (auto r = db.verify_integrity(); !r) {
            std::println(stderr, "error: database integrity check failed: {}", r.error());
            return 1;
        }

        // Read metadata once; derive runtime parameters from it
        auto meta = db.read_metadata();
        if (!meta) {
            std::println(stderr, "warning: could not read database metadata: {}",
                         meta.error());
        }

        // Sanity-check: if the runtime word list has a different size than what
        // the DB was built with, word-index values will silently index wrong words.
        if (meta && static_cast<int>(words.size()) != meta->total_words) {
            std::println(stderr,
                "error: word list has {} words but database was built with {}; "
                "use the same words file that was used to build the database",
                words.size(), meta->total_words);
            return 1;
        }

        const int    max_rounds   = (meta && meta->worst_case_depth > 0)
                                  ? meta->worst_case_depth : 6;
        const bool   full_coverage = meta && (meta->total_answers == meta->total_words);
        const double mean_depth    = meta ? meta->mean_depth : 0.0;

        if (cmd == "info") {
            mode_info(db);
        } else if (cmd == "dump") {
            mode_dump(db, words);
        } else if (cmd == "solve") {
            if (args.size() < 2) die("solve requires a target word");
            mode_solve(db, words, answers, args[1], max_rounds, full_coverage, mean_depth);
        } else if (cmd == "play") {
            mode_play(db, words, max_rounds);
        } else if (cmd == "eval") {
            // Default eval target: all words for full-coverage DB, answers otherwise
            std::string eval_path = args.size() >= 2
                ? std::string(args[1])
                : (full_coverage ? words_path : answers_path);
            mode_eval(db, words, eval_path, max_rounds);
        } else {
            std::println(stderr, "unknown command: {}", cmd);
            return 1;
        }
    }

    return 0;
}
