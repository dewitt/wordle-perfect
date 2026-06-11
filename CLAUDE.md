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
- [x] **Worst-case-5 tree (issues #8, #27)** — `EntropySolver::is_feasible`/`best_guess_feasible`: DFS minimax (memoized on the candidate set) finds a tree that solves the answer set in worst-case 5 (the best known; a 4-guess guarantee appears impossible); a feasibility-constrained entropy-greedy policy + `--lookahead` then lowers the mean (heuristic). A **parallel opener sweep** (per-thread `EntropySolver`, atomic work-stealing, 3–5× multi-core) picks the lowest-mean opener → reast, worst-5, **mean 3.4870**, verified by the built-in evaluate().
 - [x] **Metal GPU node-scoring (issue #12)** — `src/gpu_score.{hpp,mm}`: a compute shader scores all ~15k guesses (max_bucket + entropy) in one dispatch. Transposed-matrix (coalesced) layout → **14.8×** vs CPU (15.7× on the full list), verified identical. `tools/gpu_bench` benchmarks/verifies. Gated by `WP_HAVE_METAL`; non-Apple builds unaffected.
 - [x] Single precomputation tool — `build_db` (worst-5 parallel feasibility sweep; `--full` for full coverage; **`--exact-mean` for the lowest-mean tree the search can find**). SQLite + binary, verified by evaluate(). The earlier entropy/beam ("legacy") builder was removed — it was strictly inferior (worst-6, mean 3.8144).
- [x] **Lowest-mean search (issue #26)** — `EntropySolver::min_total`: exhaustive branch-and-bound + transposition DP for the minimum-mean tree it can find (worst≤5). Corroborated by Selby's independently-computed value (reast on his 2309 list = 7896/3.4197, matching `wordle.cpp`) and a brute-force oracle on small inputs — best known, not a verified global optimum. `build_db --exact-mean --start-word salet` emits a DB at **mean 3.4246** (vs greedy 3.4870), verified by evaluate().
- [x] CLI tool — `wordle` binary; `solve`, `play`, `eval`, `info`, `dump` modes
- [x] Database integrity — FNV-1a checksum on every open
- [x] Binary DB format — flat mmap'd `.bin` (`src/binarydb.*`) for true O(1) lookup; `build_db` exports it alongside the SQLite file; CLI auto-detects format. ~42% smaller, ~2× faster, no runtime SQLite dep
- [x] Test suite — Catch2 v3, 62 tests, `ctest` in ~7s (pattern, wordlist, solver incl. feasibility + consistency + exact `min_total` vs brute-force oracle, database incl. walk_target + real-corruption + e2e, binarydb incl. SQLite-parity + dump)
- [x] Code quality — cached DB statements, uint16_t overflow guard, shared `walk_target`, mtime-derived words_date, mode_solve answers validation, FNV hash correctness
- [x] Full-coverage build mode — `build_db --full` builds `wordle-full.db` covering all 14,855 guess words (worst-case depth 7, 0 failures)

**Database results (default `build_db`, start word: reast):**
- Worst case: **5 guesses** — the best known for the curated answer set (a 4-guess guarantee appears impossible). All 2,355 answers solved, 0 failures.
- Mean depth: **3.4870 guesses** (lookahead 1, parallel opener sweep picks reast). This mean is a heuristic result. The **lowest mean we've found** for the reast opener on our list is **3.4251** (issue #26; see the exact-mean section below). An exact-mean DB (`build_db --exact-mean`) reaches that best-known value.
- Distribution: 41×2, 1176×3, 1072×4, 66×5 (zero 6+)
- Built by `build_db --output wordle.db` (parallel sweep of top-50 openers @ sweep-lookahead 1) in ~1 min on 8 cores; `--lookahead K` refines the winner's tree (e.g. K=30 → 3.4679, ~80s); `--start-word W` skips the sweep; `--top 0` sweeps all openers
- worst/mean are measured by the built-in `evaluate()` (SQLite==binary parity)
- Answers source: cfreshman/a03ef2cba789d8cf00c08f767e0fad7b base (2,315 words) + 40 post-acquisition NYT additions, 0 removals → 2,355. (Relative to Selby's `wordlist_nyt20220830_hidden` (2,309, a strict subset), ours is +46: 2,355 = 2,309 + 46.) See `data/SOURCES.md`.

**Naming note:** we deliberately avoid calling the builder or its output "optimal"
or "proven". Worst-case 5 is the best result we've found (and we believe 4 is
unachievable, though we don't claim a formal proof); the mean is the lowest known,
not a verified minimum. The metadata strategy label is
`minimax-worst{D}-lookahead{K}` (or `exact-mean-worst{D}`) — describing the method
used, not a quality claim.

**Findings:**
- The opener sweep picks `reast` (lowest mean among worst-5 trees at lookahead 1); `--lookahead 30` lowers the winner's mean to 3.4679.
- Full-coverage DB (`build_db --full`, start `tares`): worst-case **7**, mean ≈4.17, 0 failures, zero 8-guess words. (A forced start is used because the full-list opener sweep is too slow.)
- Historical: an earlier answer-weighted entropy + minimax/beam builder reached worst-6 / mean 3.8144. It was removed once the feasibility sweep (worst-5) dominated it on both metrics; recoverable from git history if ever needed.

**Architecture summary:**
- `build_tree` runs a parallel opener sweep — each worker thread (own `EntropySolver`, private feasibility memo, atomic work-stealing) evaluates an opener via `tree_total_for_opener` at the cheap **sweep_lookahead** (default 1); the lowest-mean feasible opener wins, then the tree is emitted node-by-node via `best_guess_feasible` at **emit_lookahead** (`--lookahead`, default 1, winner only). The two lookaheads are separate so a high `--lookahead` refines the final mean without multiplying the sweep cost. `--start-word` skips the sweep; `--top` (default 50) caps it.
- `is_feasible` (DFS minimax, memoized on the sorted candidate set, guesses ordered by max-bucket-size) is what holds the tree to worst-case 5; `best_guess_feasible` then chooses, among budget-feasible guesses, the highest-entropy one (with `--lookahead` expanding the top-N).
- `walk_target()` (database.cpp) is the single shared tree-walk used by both `build_db::evaluate` and the CLI `eval` mode, with a generous `WALK_DEPTH_CAP` so deep-but-valid paths report their true depth (not FAIL).
- All DB writes happen in a single SQLite transaction (~1 min standard, ~40s full); `build_db` removes any prior artifact first so rebuilds to the same path succeed.
- CLI lookup: one SQL query per step, ~5ms cold / µs amortized; `next_node` and `node_info` statements are lazily prepared and cached for the connection lifetime.
- CLI derives `max_rounds` and `full_coverage` from DB metadata on open; word-list size is cross-checked against `total_words` in metadata to catch mismatched word files.
- `words_date` is derived from the words-file mtime (or `--date`); no longer hardcoded.

## Known limitations

- **`EvalResult::dist` is capped at depth 15** in `build_db.cpp`. Words solved at depth ≥ 16 would appear as failures. The current worst case is 7, so this is not a practical concern. (The CLI `eval` mode uses the shared `walk_target` with `WALK_DEPTH_CAP = 16` and reports true depths rather than capping at the DB's worst-case metric — issue #9, fixed.)
- **Production DB mean (3.4870 default, 3.4679 with `--lookahead 30`) is above the lowest known mean.** The worst-case-5 result is the best known, but the production builder's feasibility-constrained entropy-greedy + bounded-lookahead policy doesn't fully minimise the mean. **The lowest-mean tree we can find is now computable** — see below.

### Lowest-mean search (issue #26, merged to `main`)

`EntropySolver::min_total(S, depth, bound)` searches for the **minimum total/mean
solve depth it can find** subject to the worst-case-5 cap, via exhaustive
branch-and-bound + transposition table. It is tractable on the full answer set
with a forced opener (~35–90 s single-core on an M2) after several Selby-style
optimisations: (1) `2|b|-1` per-guess lower bound + |H_i|-ordered partition loop;
(2) LB-ordered guess iteration with whole-loop early break;
(3) **lower-bound caching** — the memo stores a `lower` bound per subset and
child floors use `max(2k-1, cached_lower)` with a tight per-child bound (plus a
stored-set hash-collision guard).

**Corroborated against Selby's independently-computed value (not a proof).** Our
guess list is byte-identical to Selby's `wordlist_nyt20220830_all` (14855); his
hidden list (2309) is a subset of ours (2355 = 2309 + 46 later additions). On
**Selby's exact 2309 list** our search gives reast = **7896 / 3.4197**, the same
value his `wordle.cpp` reports (verified by compiling and running it: 7896,
1.1 s). Two independent implementations agreeing is strong evidence, and we also
match a brute-force reference on small inputs — but we have **not** independently
verified a formal proof of global optimality (neither an audit of our pruning's
admissibility nor of Selby's). Treat these as **best-known** values. This
end-to-end check did catch two bugs (now fixed): a soundness bug that discarded
improving trees found under a finite recursion bound (over-count), and a
`--probe-buckets` display bug that omitted the +1-per-singleton cost
(under-count). The forced-opener and full `min_total(set)` paths were unaffected.

**Lowest known means on our 2355-answer list (worst≤5):** salet **3.4246**
(8065) and reast **3.4251** (8066) are essentially tied (salet ahead by 1 guess);
other openers pending re-measurement with the fixed code. The production greedy
ships 3.4870, so ~0.062 of mean is reclaimed by an exact-mean DB. Driver:
`tools/exact_mean.cpp` (`--start WORD`; `--probe-buckets` for per-bucket timing,
`--verify N` for a brute-force cross-check, `--worst-bucket` to isolate hard
buckets). Full results + method in `BENCHMARKS.md`.

**`build_db --exact-mean` emits a lowest-mean DB** (requires `--start-word`; runs
`min_total` once at the root, then emits via `optimal_guess` per node). Verified
end to end with salet: evaluate() reports **mean 3.4246, worst 5, 0 failures**
(dist 80/1236/998/41), SQLite==binary parity, CLI solves correctly. Strategy
label `exact-mean-worst{D}` (a method name, not a quality claim).

**Remaining:** speed up the harder openers (e.g. tarse has a pathological
size-115 `-er`/`-eed` endgame bucket that runs >100s) so an unforced exact opener
search for the global optimum becomes tractable; consider shipping the
exact-mean DB as the default production artifact.

See the open GitHub issues for the remaining backlog from the code review — notably the SIMD/bitmask pattern path (#12, open). The worst-case-5 tree (#8), the flat mmap'd binary DB (#13), and the exact-mean DB (#26, `build_db --exact-mean`) are implemented.

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
| `src/solver.hpp/cpp` | `EntropySolver`: weighted entropy `best_guess`/`solve`; `is_feasible` (DFS minimax feasibility) + `best_guess_feasible`/`tree_total_for_opener` (worst-case-5 construction); `min_total`/`optimal_guess`/`cached_lower` (exhaustive lowest-mean branch-and-bound DP, issue #26); `any_consistent_word` |
| `src/database.hpp/cpp` | SQLite decision tree + templated `walk_target` helper + bulk `all_nodes`/`all_edges` export (read/write, checksum, metadata, cached hot-path stmts) |
| `src/binarydb.hpp/cpp` | Flat mmap'd `.bin` decision tree (`BinaryDb`): header+checksum, direct-indexed nodes, CSR pattern-sorted edges; `export_from(Database)` + read-only mmap lookup |
| `tools/build_db.cpp` | **The single decision-tree builder** (worst-5 parallel feasibility sweep; `--full` for full coverage; `--exact-mean` for the lowest-mean tree the search can find, requires `--start-word`). Always runs evaluate() for measured metadata; exports SQLite + binary. Flags: `--start-word`, `--exact-mean`, `--lookahead` (emit refinement), `--sweep-lookahead`, `--top` (default 50; 0=all), `--jobs`, `--target-depth`, `--date`, `--binary`, `--no-binary` |
| `tools/exact_mean.cpp` | Exact minimal-mean optimiser driver (`min_total`). `--start WORD`, `--probe-buckets`, `--verify N`, `--worst-bucket WORD`, `--answers F` |
| `src/gpu_score.{hpp,mm}` | **Metal GPU scorer** — one-dispatch scoring of all guesses (max_bucket+entropy); transposed-matrix coalesced layout, 14.8× CPU |
| `tools/gpu_bench.cpp` | GPU-vs-CPU scoring benchmark + parity check |
| `src/main.cpp` | CLI entry point (`solve`, `play`, `eval`, `info`, `dump`); validates solve targets vs answers list |
| `data/words.txt` | 14,855 valid Wordle guesses (NYT, June 2026) |
| `data/answers.txt` | 2,355 valid Wordle answers (NYT, June 2026; includes 40 post-acquisition additions; +46 vs Selby's 2309-word hidden list) |
| `tests/test_pattern.cpp` | Pattern computation tests (all-green, duplicates, encode/decode round-trips) |
| `tests/test_wordlist.cpp` | WordList load, lookup, and edge case tests |
| `tests/test_solver.cpp` | PatternMatrix, partition, best_guess, feasibility (`is_feasible`/`best_guess_feasible`), consistency, exact `min_total` vs brute-force oracle (incl. full-vocabulary regression), and end-to-end solve tests |
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
