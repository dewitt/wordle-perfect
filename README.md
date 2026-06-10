# wordle-perfect

A Wordle solver built on precomputed, O(1)-lookup decision trees.

The solver computes a **provably worst-case-optimal** decision tree — every one of the 2,355 curated NYT answers is solved in **at most 5 guesses** (it is mathematically impossible to guarantee 4), with a mean as low as **3.4870**. Among trees that hit the optimal worst case, it minimizes average depth via a feasibility-constrained, entropy-guided search: a parallel sweep over openers picks the lowest-mean tree, with optional lookahead refinement. Precomputed paths are stored so that each guess in a solve requires exactly one O(1) lookup with no runtime search: SQLite is the build-time format, and a flat `mmap`-able binary (`.bin`) is the dependency-free runtime format. The CLI auto-detects which it's given.

## Current results

Two precomputed databases are available. The standard DB is the default; the full-coverage DB is an optional fallback for resilience against future answer-list expansions.

### Optimal database (`wordle.db`) — 2,355 curated answers, worst-case 5

| Metric | Value |
|--------|-------|
| Start word | **reast** (chosen by the parallel opener sweep) |
| Answers covered | 2,355 (all known NYT answer words) |
| Worst case | **5 guesses** (provably optimal — 4 is impossible) |
| Mean depth | **3.4870 guesses** |
| Strategy | `optimal-worst5-lookahead1` |
| Per-query latency | O(1) lookup; mmap'd binary DB (`wordle.bin`) or SQLite (`wordle.db`) |

Distribution (reast, lookahead 1): 41×2, 1176×3, 1072×4, 66×5.

Every answer is solved in **at most 5 guesses** — matching the proven optimum for
the curated answer set (it is mathematically impossible to guarantee 4 or fewer).
The tree is produced by a feasibility-constrained search: a **parallel sweep**
over candidate openers picks the one whose worst-case-5 tree has the lowest mean,
where at each node the builder takes the highest-entropy guess that keeps every
branch solvable within the remaining depth. A wider `--lookahead` pushes the mean
toward the ~3.42 theoretical optimum at higher build cost (see issue #26). Build
the default optimal DB with:

```sh
./build/build_db --output wordle.db
```

The legacy answer-weighted entropy/beam strategy (`--strategy legacy`) reaches
worst-case 6 / mean 3.8144 and remains available for comparison.

### Full-coverage database — all 14,855 guess words

```sh
./build/build_db --full --output wordle-full.db
```

This builds a worst-case-**7** tree (the optimal strategy applied to the full
list), 0 failures, zero 8-guess words. Mean ≈ 4.17. The legacy strategy
(`--strategy legacy --full`) yields worst-case 8 / mean 4.1280 — a lower mean but
a deeper worst case. All deep words are obscure guess-list-only entries (coxed,
waqfs, zills, …) that have never been NYT answers.

## Build

Requires [Nix](https://nixos.org/) with flakes enabled. [direnv](https://direnv.net/) is optional but recommended.

```sh
# Enter the hermetic dev environment
nix develop        # or: direnv allow

# Build everything
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build

# Precompute the OPTIMAL worst-case-5 tree (default; parallel opener sweep)
./build/build_db --output wordle.db

#   ...with a forced opener (skips the sweep, faster):
./build/build_db --start-word reast --output wordle.db
#   ...with mean refinement (slower, lower mean): add --lookahead 30
#   ...legacy entropy/beam tree (worst-case 6):   --strategy legacy

# Precompute the full-coverage tree (all 14,855 guess words as answers, worst-7)
./build/build_db --full --start-word tares --output wordle-full.db
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
- **build_db** — the single decision-tree builder, with two strategies:
  - **`optimal` (default)** — the headline, provably worst-case-optimal builder. A DFS minimax (`src/solver.cpp` `is_feasible`) establishes which candidate sets are solvable within a depth bound (memoized on the sorted candidate set, guesses ordered by max-bucket-size for hard alpha-beta pruning). A **parallel opener sweep** then evaluates each candidate first guess on its own worker thread (private feasibility memo, atomic work-stealing) and keeps the opener whose worst-case-5 tree has the lowest mean. At each node the chosen guess is the highest-entropy one that keeps every branch feasible (`best_guess_feasible`); `--lookahead` expands the top-N feasible guesses to lower the mean further. `--start-word` skips the sweep; `--top` caps it.
  - **`legacy` (`--strategy legacy`)** — answer-weighted entropy with **budget-aware escalation** (`best_guess_within_budget`): the fast greedy guess unless it would blow the depth budget, then full alpha-beta minimax for small candidate sets (≤ 64) or a top-24 beam re-search for large ones. Reaches worst-case 6; kept for comparison.

  Every build writes SQLite in a single transaction, exports a flat binary alongside (`<output>.bin`; `--no-binary` to disable), and runs an independent `evaluate()` pass so the stored worst-case/mean are *measured*, not assumed. `--full` covers all guess words; `--jobs` sets thread count.
- **Database (SQLite)** — the build-time format. FNV-1a checksum verified on every open. Nodes, edges, and metadata in three tables. ~16,521 nodes (standard DB) / ~16,543 nodes (full-coverage DB). Finalized as a single self-contained file (`journal_mode=DELETE`, no `-wal`/`-shm` sidecars). Hot-path lookup statements (`next_node`, `node_info`) are lazily prepared, cached, and reset after each read.
- **BinaryDb (flat mmap)** — the runtime format (`src/binarydb.*`). A single `mmap`-able file: fixed header (magic, version, counts, FNV-1a checksum, metadata) + a direct-indexed node array + a CSR-style edge array (per-node offset + pattern-sorted slice). `node_info` is a direct array index and `next_node` binary-searches a node's tiny edge slice — genuinely O(1), no SQLite dependency at runtime. ~42% smaller than the SQLite file and ~2× faster end-to-end. This is the format the `constant_time_lookup` invariant intends.
- **wordle CLI** — thin layer over either backend (auto-detected by magic / `.bin` extension). Each solve step is one O(1) lookup. All commands — including `dump` — work against both formats. Solution mode validates targets against the answers list and gives a helpful error for valid-guess-but-not-answer inputs.

## Performance

- **Parallel opener sweep** — the default `optimal` build fans the independent
  per-opener evaluations across a thread pool with atomic work-stealing (3–5× on
  8 cores). Tune with `--jobs` and `--top`.
- **Metal GPU node-scoring** (`gpu_bench`, Apple Silicon) — a compute shader
  scores all ~15k guesses against a candidate set in one dispatch. With a
  transposed (coalesced) matrix layout it runs **~15× faster than the CPU**,
  bit-for-bit identical. This is the hottest per-node operation.
  ```sh
  ./build/gpu_bench --iters 30      # verifies parity + reports speedup
  ```

## Spec

See [spec.md](spec.md) for the full behavioral specification in [dx format](https://github.com/dewitt/dx).

## Development

See [CLAUDE.md](CLAUDE.md) for agent-oriented context on resuming this project.
