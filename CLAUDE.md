# CLAUDE.md — Agent Context for wordle-perfect

This file helps a future agent resume work on this project with full context.

## What this project is

A Wordle solver that precomputes the best-known decision tree over all valid Wordle answer words, then stores it in a database that allows O(1) per-step lookup. The CLI supports solution mode (show precomputed path for a given word), solver mode (tool guesses, user responds with G/Y/B), and batch mode (evaluate a word list for hillclimbing).

## Key design decisions already made

- **Word list**: Valid NYT Wordle guess words and answer words as of June 2026. Past public answers are used as a holdback evaluation set, not as a constraint.
- **Optimality framing**: We hillclimb toward the best tree discoverable within practical compute; we do not claim global optimality. The spec was deliberately written to avoid that claim.
- **Optimization order**: Minimize worst-case solve depth first, then minimize average solve depth.
- **Lookup**: Each step is one database lookup. No runtime tree search.
- **Database**: Format is unconstrained (SQLite preferred; binary OK if it wins on latency/size). Must include a checksum and a metadata record (strategy, start word, worst-case depth, mean depth, word list source + date). If binary, must provide a dump sub-command.
- **Start word**: Selected by the precomputation process, not by hand.
- **Tie-breaking**: Lexicographically first word wins when metrics are equal, for reproducibility.
- **Consistency checking**: In solver mode, logically inconsistent user responses cause an immediate error exit.
- **Response encoding**: G = correct position, Y = correct letter wrong position, B = absent. Case-insensitive.
- **Language**: C or C++ strongly preferred for SIMD/NEON access and memory control. See the style section below.

## Current state

- [x] Behavioral spec written (`spec.md`, dx format)
- [x] Word list acquisition (`data/words.txt` 14,855 words; `data/answers.txt` 2,355 words)
- [x] Pattern matrix precomputation (220M entries, ~210 MB, built in 0.5s on M2)
- [x] Dynamic solver — answer-weighted entropy-greedy (`EntropySolver`, `solver.cpp`)
- [x] Minimax optimizer — alpha-beta for small candidate sets (≤64); seeded by greedy for pruning
- [x] Beam re-search — top-N entropy probe for large candidate sets where minimax is intractable (`best_guess_beam`)
- [x] Budget-aware escalation — `best_guess_within_budget`: greedy unless it blows the depth budget, then minimax (small) or beam (large)
- [x] Precomputation pipeline — `build_db` tool; builds SQLite decision tree (~2 min standard, ~1 min full)
- [x] CLI tool — `wordle` binary; `solve`, `play`, `eval`, `info`, `dump` modes
- [x] Database integrity — FNV-1a checksum on every open
- [x] Binary DB format — flat mmap'd `.bin` (`src/binarydb.*`) for true O(1) lookup; `build_db` exports it alongside the SQLite file; CLI auto-detects format. ~42% smaller, ~2× faster, no runtime SQLite dep
- [x] Hillclimbing — answer-weighted entropy (1000×), minimax, beam, alternative start words
- [x] Test suite — Catch2 v3, 59 tests, `ctest` in ~4s (pattern, wordlist, solver incl. minimax + consistency, database incl. walk_target + real-corruption + e2e, binarydb incl. SQLite-parity + dump)
- [x] Code quality — `DEPTH_IMPOSSIBLE` constant, cached DB statements, uint16_t overflow guard, shared `walk_target`, mtime-derived words_date, mode_solve answers validation, FNV hash correctness
- [x] Full-coverage build mode — `build_db --full` builds `wordle-full.db` covering all 14,855 guess words (worst-case depth 8, 0 failures)

**Latest database results (answer-weighted-beam-v3, start word: tarse):**
- Worst case: 6 guesses (all 2,355 answers solved)
- Mean depth: 3.8144 guesses
- Distribution: 0×1, 11×2, 672×3, 1420×4, 247×5, 5×6
- Database: 16,521 nodes (SQLite `wordle.db` ~455 KB; binary `wordle.bin` ~265 KB)
- Answers source: cfreshman/a03ef2cba789d8cf00c08f767e0fad7b (original embed) + 40 post-acquisition NYT additions from eithan/wordlelist

**Hillclimbing findings:**
- `tarse` is the auto-selected optimal start word (entropy over answer-weighted candidates)
- Answer weight 1000× is optimal; higher weights (10000×, 100000×) slightly increase 6-guess count
- Common human-chosen starts are all worse: slate(11×6), crane(12×6), crate(16×6), audio(26×6)
- The budget-aware beam re-search (depth ≥ 2, width 24) reduced the depth-6 residue from 6 words to 5 and improved mean 3.8170 → 3.8144. The remaining 5 (boxer, bunny, fuzzy, joker, rover) are **not** claimed globally unavoidable — only that greedy+beam from tarse finds no shorter path within practical compute. A worst-case-5 answer-set tree is known to exist under exhaustive minimax (future work, issue #8).
- Escalating at depth 2 is the dominant build cost (≈2× build time) but is required for the 6→5 improvement; `--min-escalation-depth 3` reverts to the faster 6-word result. Wider beams (48) gave no further gain.
- Forced strong openers under greedy+beam did NOT beat tarse for worst-case (e.g. salet/target-depth-5 → 9×6); reaching worst=5 needs true depth-first minimax, not greedy.
- Full-coverage DB (`wordle-full.db`, built with `--full`): worst-case 8, mean 4.1280, 16,543 nodes; all 14,855 words solved (27 in 7 guesses, 1 in 8); none of the deep words are in the curated answers set.

**Architecture summary:**
- Guess selection is driven by `EntropySolver::best_guess_within_budget`: take the greedy entropy guess, but if its greedy worst-case continuation would exceed the remaining depth budget, escalate.
- Escalation: candidate sets ≤ `ESCALATE_MAX_CANDIDATES` (64) use full alpha-beta `minimax_best_guess`; larger sets use `best_guess_beam` (probe the top-`beam_width` entropy guesses with a pruned `greedy_worst_depth`).
- Minimax/greedy probes are seeded/pruned by `greedy_worst_depth()` (now with an `upper_bound` alpha cutoff); sub-calls restrict to the candidate pool (O(K^depth) instead of O(N^depth)).
- `DEPTH_IMPOSSIBLE = std::numeric_limits<int>::max()` is the named sentinel for "budget exceeded" or "no improvement".
- `walk_target()` (database.cpp) is the single shared tree-walk used by both `build_db::evaluate` and the CLI `eval` mode, with a generous `WALK_DEPTH_CAP` so deep-but-valid paths report their true depth (not FAIL).
- All DB writes happen in a single SQLite transaction (~2 min build standard, ~1 min full); `build_db` removes any prior artifact first so rebuilds to the same path succeed.
- CLI lookup: one SQL query per step, ~5ms cold / µs amortized; `next_node` and `node_info` statements are lazily prepared and cached for the connection lifetime.
- CLI derives `max_rounds` and `full_coverage` from DB metadata on open; word-list size is cross-checked against `total_words` in metadata to catch mismatched word files.
- `words_date` is derived from the words-file mtime (or `--date`); no longer hardcoded.

## Known limitations

- **`EvalResult::dist` is capped at depth 15** in `build_db.cpp`. Words solved at depth ≥ 16 would appear as failures. The current worst case is 8, so this is not a practical concern. (The CLI `eval` mode uses the shared `walk_target` with `WALK_DEPTH_CAP = 16` and reports true depths rather than capping at the DB's worst-case metric — issue #9, fixed.)
- **Worst-case is 6, not 5** for the standard answer set. Greedy + beam from the auto-selected `tarse` opener cannot reach the known worst-case-5 tree; that needs exhaustive depth-first minimax over the answer set (issue #8, open).

See the open GitHub issues for the remaining backlog from the code review — notably the SIMD/bitmask pattern path (#12, open) and the worst-case-5 exhaustive minimax (#8, open). The flat mmap'd binary DB (#13) is now implemented.

## Spec format

`spec.md` uses the [dx specification format](https://github.com/dewitt/dx). dx specs are structured Markdown with five section types: Intent (goals), Invariants (non-negotiable observable properties), Assumptions (documented decisions), Contracts (Given/When/Then black-box tests), and Unconstrained (explicit implementation freedoms). Read the dx README before editing `spec.md`.

## Files

| File | Purpose |
|------|---------|
| `spec.md` | Behavioral specification (dx format) — source of truth for what the system must do |
| `README.md` | Project overview and usage |
| `CLAUDE.md` | This file |
| `src/pattern.hpp/cpp` | Wordle pattern computation (G/Y/B encoding, 243 patterns) |
| `src/wordlist.hpp/cpp` | Sorted word list with O(log N) lookup; rejects lists > 65,535 entries |
| `src/solver.hpp/cpp` | `EntropySolver`: weighted entropy, minimax, beam re-search, `best_guess_within_budget`; `DEPTH_IMPOSSIBLE` sentinel |
| `src/database.hpp/cpp` | SQLite decision tree + templated `walk_target` helper + bulk `all_nodes`/`all_edges` export (read/write, checksum, metadata, cached hot-path stmts) |
| `src/binarydb.hpp/cpp` | Flat mmap'd `.bin` decision tree (`BinaryDb`): header+checksum, direct-indexed nodes, CSR pattern-sorted edges; `export_from(Database)` + read-only mmap lookup |
| `tools/build_db.cpp` | Precomputation pipeline; budget-aware escalation; exports SQLite + binary; flags incl. `--full`, `--target-depth`, `--min-escalation-depth`, `--beam-width`, `--date`, `--binary`, `--no-binary` |
| `src/main.cpp` | CLI entry point (`solve`, `play`, `eval`, `info`, `dump`); validates solve targets vs answers list |
| `data/words.txt` | 14,855 valid Wordle guesses (NYT, June 2026) |
| `data/answers.txt` | 2,355 valid Wordle answers (NYT, June 2026; includes 40 post-acquisition additions) |
| `tests/test_pattern.cpp` | Pattern computation tests (all-green, duplicates, encode/decode round-trips) |
| `tests/test_wordlist.cpp` | WordList load, lookup, and edge case tests |
| `tests/test_solver.cpp` | PatternMatrix, partition, best_guess, minimax, and end-to-end solve tests |
| `tests/test_database.cpp` | Database CRUD, metadata, integrity + real-corruption, `walk_target`, cached-statement tests |
| `tests/test_binarydb.cpp` | BinaryDb export/open round-trip, metadata, integrity + tamper, bad-magic rejection, SQLite-parity over the answers tree |

## Available agents and tools

The lead agent (Claude Code) is encouraged to delegate to sub-agents for code review, design review, parallel implementation work, and other tasks where a second opinion or parallel execution helps. Two additional agents are available in this environment:

- **antigravity** (`/Users/dewitt/.local/bin/agy --dangerously-skip-permissions`) — invoke from this directory for code/design review, parallel research, etc.
- **codex** (`codex --yolo`) — available via PATH at `/opt/homebrew/bin/codex`

Monitor token usage and quota when running multiple agents in parallel. GitHub issues and PRs may be used for coordination and review. The repository is **public** by design; never commit secrets or credentials.

## Environment

This project uses **Nix flakes** for hermetic, reproducible builds. The `flake.nix` declares all toolchains and dependencies. **direnv** (`.envrc`) activates the environment automatically on `cd`. Never install tools globally for this project — everything goes in the flake.

## Language and style

The user has a strong preference for **modern, idiomatic code**. If using C++:

- Target **C++23** minimum; use **C++26** features where the toolchain supports them.
- Use the modern standard library fully: ranges, `std::expected`, `std::mdspan`, `std::print`, `std::flat_map`, structured bindings, `if constexpr`, concepts, modules where practical.
- Do **not** write C-with-classes style. Avoid raw pointers, manual memory management, `#define` constants, or pre-C++11 patterns unless there is a hard technical reason (e.g., a specific SIMD intrinsic).
- Prefer expressive, readable code. Modern C++ should read cleanly — if it looks like C from 1999, rewrite it.

This preference applies equally to any other language chosen: use the current idioms of that language, not its legacy subset.

## Conventions

- Keep `spec.md` up to date if requirements change. Update `CLAUDE.md` whenever significant architectural decisions are made or implementation milestones are reached.
- Every database artifact should be self-describing (metadata sub-command) so results across hillclimbing runs can be compared.
- Commit incrementally with descriptive messages; this project uses its git history as a paper trail for optimization progress.
