# wordle-perfect

A Wordle solver built on precomputed, O(1)-lookup decision trees.

The solver computes a **provably worst-case-optimal** decision tree — every one of the 2,355 curated NYT answers is solved in **at most 5 guesses** (it is mathematically impossible to guarantee 4), with a mean of **3.5495**. Among trees that hit the optimal worst case, it minimizes average depth via a feasibility-constrained, entropy-guided search with bounded lookahead. Precomputed paths are stored so that each guess in a solve requires exactly one O(1) lookup with no runtime search: SQLite is the build-time format, and a flat `mmap`-able binary (`.bin`) is the dependency-free runtime format. The CLI auto-detects which it's given.

## Current results

Two precomputed databases are available. The standard DB is the default; the full-coverage DB is an optional fallback for resilience against future answer-list expansions.

### Optimal database (`wordle.db`) — 2,355 curated answers, worst-case 5

| Metric | Value |
|--------|-------|
| Start word | **trace** |
| Answers covered | 2,355 (all known NYT answer words) |
| Worst case | **5 guesses** (provably optimal — 4 is impossible) |
| Mean depth | **3.5495 guesses** |
| Strategy | `optimal-worst5-lookahead30` |
| Per-query latency | O(1) lookup; mmap'd binary DB (`wordle.bin`) or SQLite (`wordle.db`) |

Distribution:

```
1 guess :     1
2 guesses:    38
3 guesses:  1056
4 guesses:  1186
5 guesses:    74
```

Every answer is solved in **at most 5 guesses** — matching the proven optimum for
the curated answer set (it is mathematically impossible to guarantee 4 or fewer).
The tree is produced by a feasibility-constrained DFS minimax: at each node it
takes the highest-entropy guess (low mean) among those that keep every branch
solvable within the remaining depth, with a bounded lookahead to lower the mean.
The mean (3.5495) can be pushed toward the ~3.42 theoretical optimum with a wider
lookahead at higher build cost (see issue #8). Build it with:

```sh
./build/optimal --mode tree --start trace --max-depth 5 --lookahead 30 --emit wordle.db
```

The earlier answer-weighted entropy/beam strategy (`answer-weighted-beam-v3`,
`build_db`) reaches worst-case 6 / mean 3.8144 and remains available; the optimal
tree above supersedes it.

### Full-coverage database — all 14,855 guess words

Two builds are available; the optimal one minimizes the worst case (the project's
top priority), the legacy one minimizes the mean:

| Build | Start | Worst case | Mean | Notes |
|-------|-------|-----------|------|-------|
| **Optimal** (`optimal --answers data/words.txt --max-depth 7`) | tares | **7 guesses** | 4.2732 | 0 failures; zero 8-guess words; worst-case-optimal-leaning |
| Legacy (`build_db --full`) | tares | 8 guesses | **4.1280** | 0 failures; lower mean, one 8-guess word |

Optimal full distribution: 1×1, 41×2, 1679×3, 8003×4, 4490×5, 590×6, 51×7.

Build the optimal full tree with:

```sh
./build/optimal --mode tree --answers data/words.txt --max-depth 7 --lookahead 1 --emit wordle-full.db
```

All deep words are obscure guess-list-only entries (coxed, waqfs, zills, …) that
have never been NYT answers.

## Build

Requires [Nix](https://nixos.org/) with flakes enabled. [direnv](https://direnv.net/) is optional but recommended.

```sh
# Enter the hermetic dev environment
nix develop        # or: direnv allow

# Build everything
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build

# Precompute the OPTIMAL worst-case-5 tree (~1.5 min, mean 3.5495)
./build/optimal --mode tree --start trace --max-depth 5 --lookahead 30 --emit wordle.db

# (alternative) answer-weighted entropy/beam tree, worst-case 6 (~2 min)
./build/build_db --output wordle.db

# Precompute the full-coverage tree (~1 min, all 14,855 guess words as answers)
./build/build_db --full --output wordle-full.db
```

## Testing

```sh
# Run the full test suite (62 tests, ~4s)
ctest --test-dir build --output-on-failure

# Or run the test binary directly for more verbose output
./build/wordle_tests
```

Tests cover pattern computation (including duplicate-letter edge cases), word list loading, solver correctness, and database round-trips and integrity checking.

## Usage

```sh
# Show the precomputed path for a word
./build/wordle solve thumb
# solving: thumb
#   1. tarse  [GBBBB]
#   2. yogin  [BBBBB]
#   3. thumb  [GGGGG]
# solved in 3 guesses  (db mean: 3.8144)

# Interactive solver mode (tool guesses, you supply G/Y/B responses)
./build/wordle play

# Evaluate all answer words and show statistics (auto-selects target list from DB type)
./build/wordle eval

# Show database metadata
./build/wordle info

# Dump the full decision tree (human-readable)
./build/wordle dump
```

All commands accept `--db <path>` (default: `wordle.db`), `--words <path>` (default: `data/words.txt`), and `--answers <path>` (default: `data/answers.txt`) to override file locations.

## Architecture

- **Pattern matrix** — precomputed N×N table of Wordle response patterns (220M entries, ~210 MB in memory, built in ~0.5s using all CPU cores). Allows all partition and entropy computations to be pure memory accesses.
- **EntropySolver** — dynamic solver used during precomputation. At each node, picks the guess maximising weighted Shannon entropy over the remaining candidate set. Answer-list words are weighted 1000× to bias the tree toward better performance on likely answers.
- **Optimal worst-5 builder** (`optimal --mode tree`, `src/solver.cpp` `is_feasible`/`best_guess_feasible`) — the headline result. A DFS minimax over the answer set establishes which candidate sets are solvable within a depth bound (memoized on the sorted candidate set, guesses ordered by max-bucket-size for hard alpha-beta pruning). The tree builder then, at each node, picks the **highest-entropy guess among those that keep every branch feasible** within the remaining depth, with a bounded `--lookahead` that expands the top-N feasible guesses and keeps the lowest-mean subtree. This guarantees worst-case 5 (the proven optimum) while driving the mean toward optimal. Emit a DB with `optimal ... --emit`.
- **Budget-aware escalation** — the legacy `build_db` work-horse (`best_guess_within_budget`). It takes the fast greedy guess unless that guess's worst-case continuation would exceed the remaining depth budget. Only then does it escalate:
  - **small candidate sets (≤ 64):** full alpha-beta **minimax**, seeded by the greedy worst-depth for tight pruning, sub-calls restricted to the candidate pool (O(K^depth)).
  - **large candidate sets:** a **beam re-search** that probes the top-24 entropy guesses with a pruned greedy worst-depth evaluator. Unlike minimax this never iterates the whole vocabulary, so it stays tractable and can attack the repeated-letter trap clusters that form high in the tree. This is what reduced the depth-6 residue from 6 words to 5 and improved the mean.
 - **build_db** — precomputation pipeline. Builds the full decision tree depth-first and writes it to SQLite in a single transaction, then exports a flat binary alongside it (`<output>.bin`; disable with `--no-binary`). Root guess is found in parallel; all other nodes are single-threaded. Tunable via `--target-depth`, `--min-escalation-depth`, `--beam-width`, `--start-word`, `--answer-weight`, `--date`, and `--binary`.
- **Database (SQLite)** — the build-time format. FNV-1a checksum verified on every open. Nodes, edges, and metadata in three tables. ~16,521 nodes (standard DB) / ~16,543 nodes (full-coverage DB). Finalized as a single self-contained file (`journal_mode=DELETE`, no `-wal`/`-shm` sidecars). Hot-path lookup statements (`next_node`, `node_info`) are lazily prepared, cached, and reset after each read.
- **BinaryDb (flat mmap)** — the runtime format (`src/binarydb.*`). A single `mmap`-able file: fixed header (magic, version, counts, FNV-1a checksum, metadata) + a direct-indexed node array + a CSR-style edge array (per-node offset + pattern-sorted slice). `node_info` is a direct array index and `next_node` binary-searches a node's tiny edge slice — genuinely O(1), no SQLite dependency at runtime. ~42% smaller than the SQLite file and ~2× faster end-to-end. This is the format the `constant_time_lookup` invariant intends.
- **wordle CLI** — thin layer over either backend (auto-detected by magic / `.bin` extension). Each solve step is one O(1) lookup. All commands — including `dump` — work against both formats. Solution mode validates targets against the answers list and gives a helpful error for valid-guess-but-not-answer inputs.

## Spec

See [spec.md](spec.md) for the full behavioral specification in [dx format](https://github.com/dewitt/dx).

## Development

See [CLAUDE.md](CLAUDE.md) for agent-oriented context on resuming this project.
