# wordle-perfect

A Wordle solver built on precomputed, O(1)-lookup decision trees.

The system hillclimbs toward the best-known solution tree — minimizing worst-case solve depth first, then average solve depth — across the full set of valid Wordle words. Precomputed paths are stored in a SQLite database so that each guess in a solve requires exactly one lookup with no runtime search.

## Current results

| Metric | Value |
|--------|-------|
| Start word | **tarse** (auto-selected) |
| Words covered | 14,855 valid guesses / 2,315 answers |
| Worst case | **6 guesses** (all 2,315 answers solved) |
| Mean depth | **3.82 guesses** |
| Database size | **614 KB** |
| Per-query latency | ~5 ms (cold DB open); µs amortized |

Distribution over 2,315 answer words:

```
2 guesses:    10
3 guesses:   655
4 guesses:  1396
5 guesses:   247
6 guesses:     7
```

The 7 six-guess words (cover, goody, joker, racer, roger, rover, woozy) are provably unavoidable from the tarse opening given this word list — they form clusters that cannot be distinguished sooner.

## Build

Requires [Nix](https://nixos.org/) with flakes enabled. [direnv](https://direnv.net/) is optional but recommended.

```sh
# Enter the hermetic dev environment
nix develop        # or: direnv allow

# Build everything
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build

# Precompute the decision tree (~30s)
./build/build_db --output wordle.db
```

## Usage

```sh
# Show the precomputed path for a word
./build/wordle solve --db wordle.db thumb
# solving: thumb
#   1. tarse  [GBBBB ]
#   2. pinch  [BBBBY ]
#   3. abbot  [BYBBY ]
#   4. thumb  [GGGGG ]
# solved in 4 guesses  (db mean: 3.8212)

# Interactive solver mode (tool guesses, you supply G/Y/B responses)
./build/wordle play --db wordle.db

# Evaluate all answer words and show statistics
./build/wordle eval --db wordle.db

# Show database metadata
./build/wordle info --db wordle.db

# Dump the full decision tree (human-readable)
./build/wordle dump --db wordle.db
```

## Architecture

- **Pattern matrix** — precomputed N×N table of Wordle response patterns (220M entries, ~210 MB in memory, built in ~0.5s using all CPU cores). Allows all partition and entropy computations to be pure memory accesses.
- **EntropySolver** — dynamic solver used during precomputation. At each node, picks the guess maximising weighted Shannon entropy over the remaining candidate set. Answer-list words are weighted 1000× to bias the tree toward better performance on likely answers.
- **Minimax optimizer** — for candidate sets of ≤15 words, switches from greedy entropy to alpha-beta minimax to minimise worst-case depth rather than expected depth. Seeded by the greedy result for aggressive early pruning; sub-calls restrict search to the candidate pool to keep cost O(K^depth).
- **build_db** — precomputation pipeline. Builds the full decision tree depth-first and writes it to SQLite in a single transaction. Root guess is found in parallel; all other nodes are single-threaded.
- **Database** — SQLite with FNV-1a checksum verified on every open. Nodes, edges, and metadata in three tables. 16,523 nodes for the full word list.
- **wordle CLI** — thin layer over the database. Each solve step is one SQL lookup.

## Spec

See [spec.md](spec.md) for the full behavioral specification in [dx format](https://github.com/dewitt/dx).

## Development

See [CLAUDE.md](CLAUDE.md) for agent-oriented context on resuming this project.
