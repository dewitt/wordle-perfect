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
- **Language**: Unconstrained. Prefer speed. Apple Silicon M2 (SIMD/NEON/GCD) is the local target; remote/distributed compute is allowed for precomputation.

## Current state

- [x] Behavioral spec written (`spec.md`, dx format)
- [ ] Word list acquisition
- [ ] Dynamic solver (entropy minimization or similar — intermediate artifact, needed for precomputation)
- [ ] Precomputation pipeline (builds the decision tree and database)
- [ ] CLI tool (solution, solver, batch modes)
- [ ] Database tooling (metadata sub-command, dump sub-command if binary)

## Files

| File | Purpose |
|------|---------|
| `spec.md` | Behavioral specification (dx format) — source of truth for what the system must do |
| `README.md` | Project overview |
| `CLAUDE.md` | This file |

## How to proceed

1. Read `spec.md` fully before writing any code.
2. Start with word list acquisition — fetch the current NYT Wordle valid guess list and answer list, document the source and date.
3. Build a dynamic solver first (entropy-based or minimax); this is the engine for precomputation.
4. Build the precomputation pipeline around the dynamic solver to generate the full decision tree.
5. Evaluate the tree using the past-answers holdback set and report metrics.
6. Hillclimb: try alternative start words, pruning strategies, or algorithms to improve worst-case then mean depth.
7. Build the CLI on top of the final database.

## Conventions

- Keep `spec.md` up to date if requirements change. Update `CLAUDE.md` whenever significant architectural decisions are made or implementation milestones are reached.
- Every database artifact should be self-describing (metadata sub-command) so results across hillclimbing runs can be compared.
- Commit incrementally with descriptive messages; this project uses its git history as a paper trail for optimization progress.
