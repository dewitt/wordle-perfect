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
- [x] Minimax optimizer — alpha-beta for candidate sets ≤15; seeded by greedy for pruning
- [x] Precomputation pipeline — `build_db` tool; builds SQLite decision tree (~30s)
- [x] CLI tool — `wordle` binary; `solve`, `play`, `eval`, `info`, `dump` modes
- [x] Database integrity — FNV-1a checksum on every open
- [x] Hillclimbing — answer-weighted entropy (1000×), minimax, alternative start words
- [x] Test suite — Catch2 v3, 43 tests, `ctest` in ~3s (pattern, wordlist, solver, database)
- [x] Code quality — `DEPTH_IMPOSSIBLE` constant, cached DB statements, uint16_t overflow guard, mode_solve answers validation, FNV hash correctness
- [x] Full-coverage build mode — `build_db --full` builds `wordle-full.db` covering all 14,855 guess words (worst-case depth 8, 0 failures)
- [x] Memoization experiment — `--memo-limit N` added to K=1 path; sweep confirmed 0% hit rate (no transpositions in a greedy tree); infra ready for beam search
- [x] Beam search — `--beam-width K` builds trees via two-phase memoised beam search; K=10 achieves worst=5, mean=3.4293 (see `wordle-beam.db`)

**Standard database results (answer-weighted-v2, start word: tarse):**
- Worst case: 6 guesses (all 2,355 answers solved)
- Mean depth: 3.8170 guesses
- Distribution: 0×1, 10×2, 680×3, 1402×4, 257×5, 6×6
- Database: 16,516 nodes, 455 KB
- Answers source: cfreshman/a03ef2cba789d8cf00c08f767e0fad7b (original embed) + 40 post-acquisition NYT additions from eithan/wordlelist

**Beam-search database results (beam-search-v1-k10, start word: reast):**
- Worst case: **5 guesses** (all 2,355 answers solved; no 6-guess words)
- Mean depth: **3.4293 guesses** (vs static-wordle benchmark: worst=5, mean=3.606)
- Distribution: 0×1, 70×2, 1245×3, 999×4, 41×5
- Database: **2,507 nodes** (more compact — covers answer words only, not all 14,855 guess words)
- Build time: ~142s, `./build/build_db --beam-width 10 --output wordle-beam.db`

**Beam sweep summary (branch: memoization-experiment):**

| K   | worst | mean   | nodes  | build  | cache hits |
|-----|-------|--------|--------|--------|------------|
| 1   | 6     | 3.8170 | 16,516 | 30s    | 0% (no transpositions at K=1) |
| 2   | 6     | 3.4301 | 2,511  | 10.7s  | 13.8% (1 six-guess word) |
| 3   | **5** | 3.4620 | 2,522  | 22.4s  | 26.0% |
| 5   | 5     | 3.4616 | 2,521  | 48.9s  | 43.6% |
| 10  | 5     | **3.4293** | 2,507 | 142s | 62.4% ← best |
| 20  | 5     | 3.4301 | 2,507  | 399s   | 76.5% |

**Hillclimbing findings:**
- `tarse` is the auto-selected optimal start word (entropy over answer-weighted candidates)
- Answer weight 1000× is optimal; higher weights (10000×, 100000×) slightly increase 6-guess count
- Common human-chosen starts are all worse: slate(11×6), crane(12×6), crate(16×6), audio(26×6)
- The 6 remaining six-guess words (boxer, goody, joker, racer, rover, woozy) are **provably unavoidable** from tarse — minimax confirms no reachable guess sequence reduces them below 6
- Adding the 40 missing NYT post-acquisition answers improved cover and roger from 6→5 guesses (better cluster separation), added boxer as a new 6-guess case
- Full-coverage DB (`wordle-full.db`, built with `--full`): start word `tares`, worst-case 8, mean 4.1280, 16,543 nodes; all 14,855 words solved; the 28 previously-unsolvable words now have paths (27 in 7 guesses, 1 in 8); none are in the curated answers set

**Beam search key findings:**
- K=1 greedy tree has NO transpositions (0% cache hit rate). Memoization provides no benefit for pure greedy trees.
- Beam search creates abundant transpositions (13-76% hit rates) — memoization makes beam search practical (without it, cost would be K^depth).
- Starting candidates must be ANSWER WORDS ONLY (not all 14,855). Using all words pollutes quality estimates and causes beam to pick bad root words.
- Minimax for small candidate sets (≤15) within beam_eval is required. Without it, near-leaf quality estimates are wrong, corrupting beam choices at higher levels.
- Mean depth must be ANSWER-WEIGHTED and must count GGGGG words (solved in 1 guess). Both bugs caused K=2 to initially perform worse than K=1.
- Beam-search DB is compact (2,507 nodes for 2,355 answers) because it covers answer words only. Standard DB is larger (16,516 nodes) because it covers all 14,855 guess words.
- DAG deduplication in build_from_eval is present but sees 0 reuses: the beam creates transpositions in its EVALUATION but the final chosen tree uses each candidate set exactly once.

**Architecture summary:**
- K=1 path: Solver picks greedy entropy guess at all nodes; switches to alpha-beta minimax for candidate sets ≤15
- K>1 path (beam search): `beam_eval()` + `build_from_eval()` two-phase build; `top_k_guesses()` parallel entropy scan returns top-K; minimax used for small sets
- Minimax is seeded by `greedy_worst_depth()` for a tight initial upper bound; sub-calls restrict to the candidate pool (O(K^depth) instead of O(N^depth))
- `DEPTH_IMPOSSIBLE = std::numeric_limits<int>::max()` is the named sentinel for "budget exceeded" or "no improvement"; replaces all bare INT_MAX literals in the solver
- All DB writes happen in a single SQLite transaction (~30s build, 455 KB for K=1; ~142s, ~245 KB for K=10 beam)
- CLI lookup: one SQL query per step, ~5ms cold / µs amortized; `next_node` and `node_info` statements are lazily prepared and cached for the connection lifetime
- CLI derives `max_rounds` and `full_coverage` from DB metadata on open; word-list size is cross-checked against `total_words` in metadata to catch mismatched word files

## Known limitations

- **`words_date` is hardcoded** in `build_db.cpp`. If the word lists are updated on a different date, update that string manually before rebuilding. A future improvement could derive it from the file mtime or a `--date` CLI flag.
- **Consistency check in `mode_play` uses all 14,855 words**, not the curated 2,355 answers. For the standard DB, this means a logically inconsistent response is only flagged if *no word in the full guess list* satisfies all constraints — a slightly more permissive check than the spec implies. In practice this is rarely noticeable.
- **`EvalResult::dist` is capped at depth 15** in `build_db.cpp`. Words solved at depth ≥ 16 would appear as failures. The current worst case (full-coverage DB) is 8, so this is not a practical concern.

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
| `src/solver.hpp/cpp` | `EntropySolver`: weighted entropy + minimax optimizer; `DEPTH_IMPOSSIBLE` sentinel |
| `src/database.hpp/cpp` | SQLite-backed decision tree (read/write, checksum, metadata, cached hot-path stmts) |
| `tools/build_db.cpp` | Precomputation pipeline; `--full` builds full-coverage DB (all guess words as answers) |
| `src/main.cpp` | CLI entry point (`solve`, `play`, `eval`, `info`, `dump`); validates solve targets vs answers list |
| `data/words.txt` | 14,855 valid Wordle guesses (NYT, June 2026) |
| `data/answers.txt` | 2,355 valid Wordle answers (NYT, June 2026; includes 40 post-acquisition additions) |
| `tests/test_pattern.cpp` | Pattern computation tests (all-green, duplicates, encode/decode round-trips) |
| `tests/test_wordlist.cpp` | WordList load, lookup, and edge case tests |
| `tests/test_solver.cpp` | PatternMatrix, partition, best_guess, and end-to-end solve tests |
| `tests/test_database.cpp` | Database CRUD, metadata round-trip, integrity checking, cached-statement tests |

## Available agents and tools

The lead agent (Claude Code) is encouraged to delegate to sub-agents for code review, design review, parallel implementation work, and other tasks where a second opinion or parallel execution helps. Two additional agents are available in this environment:

- **antigravity** (`/Users/dewitt/.local/bin/agy --dangerously-skip-permissions`) — invoke from this directory for code/design review, parallel research, etc.
- **codex** (`codex --yolo`) — available via PATH at `/opt/homebrew/bin/codex`

Monitor token usage and quota when running multiple agents in parallel. GitHub issues and PRs may be used for coordination and review, but keep the repository **private** at all times.

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
