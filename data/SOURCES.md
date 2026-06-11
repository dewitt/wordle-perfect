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

- **Count**: 2,355 five-letter words
- **Source**: cfreshman's Wordle answers gist (original Josh Wardle answer list,
  2,315 words) + 40 post-acquisition NYT additions
  - URL: https://gist.github.com/cfreshman/a03ef2cba789d8cf00c08f767e0fad7b
- **Retrieved**: 2026-06-07
- **Format**: Plain text, one word per line, lowercase, alphabetically sorted
- **Provenance arithmetic**: cfreshman base = 2,315 words; we added exactly **40**
  post-acquisition NYT answers (0 removals) → **2,355**. For reference, relative
  to Alex Selby's `wordlist_nyt20220830_hidden` (2,309 words, a strict subset of
  ours) we have **+46** words (2,355 = 2,309 + 46).
- **Notes**: This builds on the original Wardle answer list (pre-NYT). NYT has
  added new answers since acquisition; we apply the 40 additions but no removals,
  keeping the list a superset for evaluation. NYT began recycling answers on
  2026-02-02, making the curated list a moving target. `answers.txt` is used here
  **only as an evaluation/hillclimbing holdback set**, not as the solution space —
  `words.txt` defines the full solution space.

## Design rationale

The solver builds decision tree paths to **every word in words.txt** (~14,855). This makes the database robust to future changes in NYT's curated answer list. `answers.txt` is used to evaluate and hillclimb the tree: a good solver should solve every word in `answers.txt` in ≤ 5 guesses on average, since those are the words NYT has historically chosen.
