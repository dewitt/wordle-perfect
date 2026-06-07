# wordle-perfect

A Wordle solver built on precomputed, O(1)-lookup decision trees.

The system hillclimbs toward the best-known solution tree — minimizing worst-case solve depth first, then average solve depth — across the full set of valid Wordle words. Precomputed paths are stored in a SQLite database so that each guess in a solve requires exactly one lookup with no runtime search.

## Current results

Two precomputed databases are available. The standard DB is the default; the full-coverage DB is an optional fallback for resilience against future answer-list expansions.

### Standard database (`wordle.db`) — 2,355 curated answers

| Metric | Value |
|--------|-------|
| Start word | **tarse** (auto-selected) |
| Answers covered | 2,355 (all known NYT answer words) |
| Worst case | **6 guesses** |
| Mean depth | **3.8170 guesses** |
| Database size | **455 KB** |
| Per-query latency | ~5 ms (cold DB open); µs amortized |

Distribution:

```
2 guesses:    10
3 guesses:   680
4 guesses:  1402
5 guesses:   257
6 guesses:     6
```

The 6 six-guess words (boxer, goody, joker, racer, rover, woozy) are provably unavoidable from the tarse opening — minimax confirms no guess sequence reduces them below 6.

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

# Precompute the standard decision tree (~30s, 2,355 curated answers)
./build/build_db --output wordle.db

# Precompute the full-coverage tree (~34s, all 14,855 guess words as answers)
./build/build_db --full --output wordle-full.db
```

## Testing

```sh
# Run the full test suite (43 tests, ~3s)
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
#   1. tarse  [GBBBB ]
#   2. pinch  [BBBBY ]
#   3. abbot  [BYBBY ]
#   4. thumb  [GGGGG ]
# solved in 4 guesses  (db mean: 3.8170)

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
- **Minimax optimizer** — for candidate sets of ≤15 words, switches from greedy entropy to alpha-beta minimax to minimise worst-case depth rather than expected depth. Seeded by the greedy result for aggressive early pruning; sub-calls restrict search to the candidate pool to keep cost O(K^depth).
- **build_db** — precomputation pipeline. Builds the full decision tree depth-first and writes it to SQLite in a single transaction. Root guess is found in parallel; all other nodes are single-threaded.
- **Database** — SQLite with FNV-1a checksum verified on every open. Nodes, edges, and metadata in three tables. 16,516 nodes (standard DB) / 16,543 nodes (full-coverage DB). Hot-path lookup statements (`next_node`, `node_info`) are lazily prepared and cached for the lifetime of the connection.
- **wordle CLI** — thin layer over the database. Each solve step is one SQL lookup. Solution mode validates targets against the answers list and gives a helpful error for valid-guess-but-not-answer inputs.

## Spec

See [spec.md](spec.md) for the full behavioral specification in [dx format](https://github.com/dewitt/dx).

## Development

See [CLAUDE.md](CLAUDE.md) for agent-oriented context on resuming this project.
