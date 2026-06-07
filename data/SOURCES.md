# Word List Sources

## words.txt — valid guess universe

- **Count**: 14,855 five-letter words
- **Source**: SukkaW's NYT Wordle word list gist (direct extraction from NYT JavaScript bundle)
  - URL: https://gist.github.com/SukkaW/92ff13af03a0117e5bafec6c7f7d6dce
  - Commit: `a7412e4a551bf5863060610e1edbad576fd678a6`
- **Retrieved**: 2026-06-07
- **Format**: Plain text, one word per line, lowercase, alphabetically sorted
- **Notes**: This list grew substantially from the original ~12,970-word Wardle/early-NYT list. The increase reflects NYT's additions to the valid guesses dictionary since acquisition (January 2022).

## answers.txt — curated answer list (evaluation holdback set)

- **Count**: 2,314 five-letter words
- **Source**: cfreshman's Wordle answers gist (original Josh Wardle answer list)
  - URL: https://gist.github.com/cfreshman/a03ef2cba789d8cf00c08f767e0fad7b
- **Retrieved**: 2026-06-07
- **Format**: Plain text, one word per line, lowercase, alphabetically sorted
- **Notes**: This is the original Wardle answer list (pre-NYT). NYT has since removed ~7 words (SLAVE, FETUS, FIBRE, etc.) and added ~40 new words. The original list remains the best widely-validated baseline for evaluation. NYT began recycling answers on 2026-02-02, making the curated list a moving target. `answers.txt` is used here **only as an evaluation/hillclimbing holdback set**, not as the solution space — `words.txt` defines the full solution space.

## Design rationale

The solver builds decision tree paths to **every word in words.txt** (~14,855). This makes the database robust to future changes in NYT's curated answer list. `answers.txt` is used to evaluate and hillclimb the tree: a good solver should solve every word in `answers.txt` in ≤ 5 guesses on average, since those are the words NYT has historically chosen.
