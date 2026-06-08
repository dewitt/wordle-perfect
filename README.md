# wordle-perfect

A Wordle solver built on precomputed, O(1)-lookup decision trees.

The system hillclimbs toward the best-known solution tree — minimizing worst-case solve depth first, then average solve depth — across the full set of valid Wordle words. Precomputed paths are stored so that each guess in a solve requires exactly one O(1) lookup with no runtime search: SQLite is the build-time format, and a flat `mmap`-able binary (`.bin`) is the dependency-free runtime format. The CLI auto-detects which it's given.

## Current results

Two precomputed databases are available. The standard DB is the default; the full-coverage DB is an optional fallback for resilience against future answer-list expansions.

### Standard database (`wordle.db`) — 2,355 curated answers

| Metric | Value |
|--------|-------|
| Start word | **tarse** (auto-selected) |
| Answers covered | 2,355 (all known NYT answer words) |
| Worst case | **6 guesses** |
| Mean depth | **3.8144 guesses** |
| Strategy | `answer-weighted-beam-v3` |
| Per-query latency | O(1) lookup; mmap'd binary DB (`wordle.bin`, ~265 KB) or SQLite (`wordle.db`, ~455 KB) |

Distribution:

```
2 guesses:    11
3 guesses:   672
4 guesses:  1420
5 guesses:   247
6 guesses:     5
```

The 5 six-guess words (boxer, bunny, fuzzy, joker, rover) are the residue left
after the budget-aware beam re-search (see Architecture). They are not claimed
to be globally unavoidable — only that, from the auto-selected `tarse` opening,
the greedy + beam optimizer finds no shorter path within practical compute. A
worst-case-5 tree for the answer set is known to exist under exhaustive
depth-first minimax, which is future work (see issue #8).

### Full-coverage database (`wordle-full.db`) — all 14,855 guess words

| Metric | Value |
|--------|-------|
| Start word | **tares** (auto-selected) |
| Words covered | 14,855 (every valid guess word) |
| Worst case | **8 guesses** (27 words need 7, 1 needs 8) |
| Mean depth | **4.1280 guesses** |
| Failures | **0** (all 14,855 words solved) |

The 28 previously-unsolvable words (coxed, waqfs, zills, etc.) are all obscure guess-list-only entries that have never been NYT answers.

## Build

Requires [Nix](https://nixos.org/) with flakes enabled. [direnv](https://direnv.net/) is optional but recommended.

```sh
# Enter the hermetic dev environment
nix develop        # or: direnv allow

# Build everything
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build

# Precompute the standard decision tree (~2 min, 2,355 curated answers)
./build/build_db --output wordle.db

# Precompute the full-coverage tree (~1 min, all 14,855 guess words as answers)
./build/build_db --full --output wordle-full.db
```

## Testing

```sh
# Run the full test suite (58 tests, ~4s)
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
- **Budget-aware escalation** — the build work-horse (`best_guess_within_budget`). It takes the fast greedy guess unless that guess's worst-case continuation would exceed the remaining depth budget. Only then does it escalate:
  - **small candidate sets (≤ 64):** full alpha-beta **minimax**, seeded by the greedy worst-depth for tight pruning, sub-calls restricted to the candidate pool (O(K^depth)).
  - **large candidate sets:** a **beam re-search** that probes the top-24 entropy guesses with a pruned greedy worst-depth evaluator. Unlike minimax this never iterates the whole vocabulary, so it stays tractable and can attack the repeated-letter trap clusters that form high in the tree. This is what reduced the depth-6 residue from 6 words to 5 and improved the mean.
 - **build_db** — precomputation pipeline. Builds the full decision tree depth-first and writes it to SQLite in a single transaction, then exports a flat binary alongside it (`<output>.bin`; disable with `--no-binary`). Root guess is found in parallel; all other nodes are single-threaded. Tunable via `--target-depth`, `--min-escalation-depth`, `--beam-width`, `--start-word`, `--answer-weight`, `--date`, and `--binary`.
- **Database (SQLite)** — the build-time format. FNV-1a checksum verified on every open. Nodes, edges, and metadata in three tables. ~16,521 nodes (standard DB) / ~16,543 nodes (full-coverage DB). Hot-path lookup statements (`next_node`, `node_info`) are lazily prepared and cached for the connection lifetime.
- **BinaryDb (flat mmap)** — the runtime format (`src/binarydb.*`). A single `mmap`-able file: fixed header (magic, version, counts, FNV-1a checksum, metadata) + a direct-indexed node array + a CSR-style edge array (per-node offset + pattern-sorted slice). `node_info` is a direct array index and `next_node` binary-searches a node's tiny edge slice — genuinely O(1), no SQLite dependency at runtime. ~42% smaller than the SQLite file and ~2× faster end-to-end. This is the format the `constant_time_lookup` invariant intends.
- **wordle CLI** — thin layer over either backend (auto-detected by magic / `.bin` extension). Each solve step is one O(1) lookup. All commands — including `dump` — work against both formats. Solution mode validates targets against the answers list and gives a helpful error for valid-guess-but-not-answer inputs.

## Spec

See [spec.md](spec.md) for the full behavioral specification in [dx format](https://github.com/dewitt/dx).

## Development

See [CLAUDE.md](CLAUDE.md) for agent-oriented context on resuming this project.
