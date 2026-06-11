# wordle-perfect

A Wordle solver built on precomputed, O(1)-lookup decision trees.

The solver computes a decision tree that solves every one of the 2,355 curated NYT answers in **at most 5 guesses**. The worst case of 5 is **provably optimal** for this answer set — it is mathematically impossible to guarantee 4. The default DB uses an entropy-greedy policy (mean **3.4870**); an opt-in `--exact-mean` build computes the **provably minimal mean** (e.g. **3.4246** for the salet opener), validated against Alex Selby's independently-proven optimum. Among worst-case-5 trees, a parallel sweep over openers picks the lowest-mean one. Precomputed paths are stored so that each guess in a solve requires exactly one O(1) lookup with no runtime search: SQLite is the build-time format, and a flat `mmap`-able binary (`.bin`) is the dependency-free runtime format. The CLI auto-detects which it's given.

## Current results

Two precomputed databases are available. The standard DB is the default; the full-coverage DB is an optional fallback for resilience against future answer-list expansions.

### Standard database (`wordle.db`) — 2,355 curated answers, worst-case 5

| Metric | Value |
|--------|-------|
| Start word | **reast** (chosen by the parallel opener sweep) |
| Answers covered | 2,355 (all known NYT answer words) |
| Worst case | **5 guesses** (provably optimal — 4 is impossible) |
| Mean depth | **3.4870 guesses** (entropy-greedy; the exact optimum is 3.4251 for reast / 3.4246 for salet — see `--exact-mean`) |
| Per-query latency | O(1) lookup; mmap'd binary DB (`wordle.bin`) or SQLite (`wordle.db`) |

Distribution (reast, lookahead 1): 41×2, 1176×3, 1072×4, 66×5.

The tree is produced by a feasibility-constrained search: a **parallel sweep**
over candidate openers picks the one whose worst-case-5 tree has the lowest mean,
where at each node the builder takes the highest-entropy guess that keeps every
branch solvable within the remaining depth. The default build takes ~1 minute.
A wider `--lookahead` refines the winning opener's tree toward the optimum at
higher build cost (e.g. `--lookahead 30` → mean **3.4679** in ~80s). Build the
default DB with:

```sh
./build/build_db --output wordle.db
```

#### Exact minimal-mean database (`--exact-mean`)

For the **provably minimal mean** (not the entropy heuristic), `--exact-mean`
runs a branch-and-bound + transposition-table DP (`EntropySolver::min_total`)
that computes the optimal tree subject to the worst-case-5 cap. It currently
requires a forced opener:

```sh
./build/build_db --exact-mean --start-word salet --output wordle-exact.db
```

This emits a tree at the exact optimum — **mean 3.4246** for salet (vs the greedy
3.4870), worst-case 5, 0 failures, distribution 80×2 / 1236×3 / 998×4 / 41×5.
The optimiser is validated against Alex Selby's independently-proven results: on
Selby's exact 2,309-word list our `min_total` reproduces his proven optimum for
reast (7896 total / mean 3.4197) byte-for-byte. See [BENCHMARKS.md](BENCHMARKS.md)
for the method and full results. (The DP is single-core ~2 min; pathological
endgame buckets in some openers — e.g. tarse — are not yet fast enough for an
unforced global opener search.)

### Full-coverage database — all 14,855 guess words

```sh
./build/build_db --full --output wordle-full.db
```

This builds a worst-case-**7** tree over the full list, 0 failures, zero 8-guess
words, mean ≈ 4.17. All deep words are obscure guess-list-only entries (coxed,
waqfs, zills, …) that have never been NYT answers.

## Build

Requires [Nix](https://nixos.org/) with flakes enabled. [direnv](https://direnv.net/) is optional but recommended.

```sh
# Enter the hermetic dev environment
nix develop        # or: direnv allow

# Build everything
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build

# Precompute the worst-case-5 tree (default; parallel opener sweep, ~1 min)
./build/build_db --output wordle.db

#   ...with a forced opener (skips the sweep, faster):
./build/build_db --start-word reast --output wordle.db
#   ...with mean refinement (slower, lower mean): add --lookahead 30

# Precompute the full-coverage tree (all 14,855 guess words as answers, worst-7)
./build/build_db --full --output wordle-full.db
```

## Testing

```sh
# Run the full test suite (62 tests, ~7s)
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
#   1. reast  [BBBBY]
#   2. tinto  [GBBBB]
#   3. thumb  [GGGGG]
# solved in 3 guesses  (db mean: 3.4870)

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
- **build_db** — the decision-tree builder. A DFS minimax (`src/solver.cpp` `is_feasible`) establishes which candidate sets are solvable within a depth bound (memoized on the sorted candidate set, guesses ordered by max-bucket-size for hard alpha-beta pruning) — this is what makes the worst-case-5 result provable. A **parallel opener sweep** then evaluates the top-`--top` (default 50) entropy-ranked openers, each on its own worker thread (private feasibility memo, atomic work-stealing), and keeps the one whose worst-case-5 tree has the lowest mean (the sweep runs at lookahead 1, so it's ~1 min). At each node the chosen guess is the highest-entropy one that keeps every branch feasible (`best_guess_feasible`). `--lookahead K` refines **only the winning opener's** tree by expanding the top-K feasible guesses per node (lower mean, more time). `--start-word` skips the sweep; `--sweep-lookahead` tunes the sweep (rarely needed). Note this default path minimises the worst case (provably optimal at 5); its mean is a heuristic, not claimed optimal. For the provably minimal mean, use `--exact-mean` (below), which replaces the entropy policy with the `min_total` DP.

  Every build writes SQLite in a single transaction, exports a flat binary alongside (`<output>.bin`; `--no-binary` to disable), and runs an independent `evaluate()` pass so the stored worst-case/mean are *measured*, not assumed. `--full` covers all guess words; `--jobs` sets thread count; `--gpu` offloads bucket ranking to Metal (Apple Silicon, byte-identical tree); `--exact-mean` (with a forced `--start-word`) emits the provably minimal-mean tree via the `min_total` DP instead of the entropy-greedy policy.
- **Database (SQLite)** — the build-time format. FNV-1a checksum verified on every open. Nodes, edges, and metadata in three tables. ~2,648 nodes (standard reast DB) / ~17,129 nodes (full-coverage DB). Finalized as a single self-contained file (`journal_mode=DELETE`, no `-wal`/`-shm` sidecars). Hot-path lookup statements (`next_node`, `node_info`) are lazily prepared, cached, and reset after each read.
- **BinaryDb (flat mmap)** — the runtime format (`src/binarydb.*`). A single `mmap`-able file: fixed header (magic, version, counts, FNV-1a checksum, metadata) + a direct-indexed node array + a CSR-style edge array (per-node offset + pattern-sorted slice). `node_info` is a direct array index and `next_node` binary-searches a node's tiny edge slice — genuinely O(1), no SQLite dependency at runtime. ~42% smaller than the SQLite file and ~2× faster end-to-end. This is the format the `constant_time_lookup` invariant intends.
- **wordle CLI** — thin layer over either backend (auto-detected by magic / `.bin` extension). Each solve step is one O(1) lookup. All commands — including `dump` — work against both formats. Solution mode validates targets against the answers list and gives a helpful error for valid-guess-but-not-answer inputs.

## Performance

- **Parallel opener sweep** — the build fans the independent per-opener
  evaluations across a thread pool with atomic work-stealing (3–5× on 8 cores).
  Tune with `--jobs` and `--top`.
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
