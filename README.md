# wordle-perfect

A perfect Wordle solver built on precomputed, O(1)-lookup decision trees.

The system hillclimbs toward the best-known solution tree — minimizing worst-case solve depth first, then average solve depth — across the full set of valid Wordle answer words. Precomputed paths are stored in a database that allows each guess in a solve to be looked up in constant time with no runtime search.

## Status

Early development. Specification complete; implementation not yet started.

## Spec

See [spec.md](spec.md) for the full behavioral specification in [dx format](https://github.com/dewitt/dx).

## Modes

- **Solution mode** — given a target word, display its complete precomputed solve path
- **Solver mode** — interactive: tool guesses, user supplies G/Y/B responses
- **Batch mode** — evaluate a word list against the database and report statistics

## Development

See [CLAUDE.md](CLAUDE.md) for agent-oriented context on resuming this project.
