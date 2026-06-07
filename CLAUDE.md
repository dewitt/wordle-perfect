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
- [x] Word list acquisition (`data/words.txt` 14,855 words; `data/answers.txt` 2,315 words)
- [x] Pattern matrix precomputation (220M entries, ~210 MB, built in 0.5s on M2)
- [x] Dynamic solver — entropy-greedy (`EntropySolver`, `solver.cpp`)
- [x] Precomputation pipeline — `build_db` tool; builds SQLite decision tree
- [x] CLI tool — `wordle` binary; `solve`, `play`, `eval`, `info`, `dump` modes
- [x] Database integrity — FNV-1a checksum on every open

**Latest database results (entropy-greedy-v1, start word: tares):**
- Worst case: 6 guesses (all 2,315 answers solved)
- Mean depth: 4.0199 guesses
- Distribution: 0×1, 9×2, 434×3, 1401×4, 444×5, 27×6

**Next: hillclimbing** — 27 words still need 6 guesses; try answer-weighted entropy, minimax, alternative start words.

## Spec format

`spec.md` uses the [dx specification format](https://github.com/dewitt/dx). dx specs are structured Markdown with five section types: Intent (goals), Invariants (non-negotiable observable properties), Assumptions (documented decisions), Contracts (Given/When/Then black-box tests), and Unconstrained (explicit implementation freedoms). Read the dx README before editing `spec.md`.

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
