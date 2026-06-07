# wordle-perfect

## Intent

- Precompute and store best-known Wordle solution paths so that, given any valid answer word, every step from first guess to solution is resolved in O(1) time with no runtime search.
- Iteratively hillclimb toward a decision tree with the lowest achievable worst-case solve depth, then lowest achievable average solve depth, evaluated against the full set of valid Wordle answer words. Global optimality is not claimed; the goal is to produce the best tree discoverable within practical compute constraints, with the architecture supporting continued improvement over time.
- Provide a command-line interface supporting three modes: solution mode (display the complete precomputed path for a given target word), solver mode (interactive: tool guesses, user provides responses), and batch evaluation mode (score the database against a supplied word list for hillclimbing and reporting).

## Invariants

### batch_mode_evaluation
Given a file containing one word per line (all valid answer words), the tool evaluates each word against the precomputed database and outputs per-word solve depths plus aggregate statistics: minimum depth, maximum depth, arithmetic mean depth, and a depth distribution showing the count of words solved at each depth level.

### completeness
Every word in the valid answer word list must be reachable via the precomputed decision tree. The database must contain a path from the starting guess to every valid answer word.

### consistency_enforcement
In solver mode, if a user-supplied response to a guess is logically inconsistent with any prior (guess, response) pair in the current session, the tool writes a descriptive error message to stderr and terminates the session with a non-zero exit status without issuing another guess.

### constant_time_lookup
Each step in solution mode or solver mode requires exactly one lookup operation against the database. Lookup cost is independent of the number of guesses already made and independent of the size of the word list.

### database_integrity
The database file includes a checksum that the tool verifies on startup. A corrupted or tampered file causes the tool to exit with a non-zero status and a checksum failure message on stderr before performing any solve operations.

### debug_dump_tool
If the database uses a proprietary binary format, the tool provides a sub-command that dumps the full database content in a human-readable text form (at minimum: one decision-tree node per line with its guess word, depth level, and the response-pattern-to-successor mappings), so that database contents can be inspected and verified without external tooling.

### database_metadata
Every database artifact records the word list source and retrieval date, the starting guess, the worst-case solve depth, the arithmetic mean solve depth, and the precomputation strategy name used to produce it. This metadata is human-readable and accessible via a dedicated sub-command, enabling comparison across hillclimbing iterations.

### response_encoding
Guess responses use a three-symbol alphabet: G (correct letter, correct position), Y (correct letter, wrong position), B (letter absent from the answer). Every response is exactly 5 symbols. Symbols are accepted case-insensitively from the user.

### solution_mode_output
In solution mode, given a valid answer word, the tool outputs the complete precomputed path: the ordered sequence of (guess, response) pairs from the first guess through the guess that equals the answer, followed by summary statistics including the number of guesses required and the mean depth of the database overall.

### worst_case_depth
No valid answer word requires more than 6 guesses to solve via the precomputed decision tree.

## Assumptions

### database_format_default
The database format is not mandated by this spec; the implementation may use SQLite, a flat binary file, or another format. SQLite is preferred for its built-in integrity tooling and inspectability; a proprietary binary format is acceptable if it materially reduces lookup latency or file size, provided the debug_dump_tool invariant is satisfied.

### guess_word_pool
The tool guesses only from the valid guess word list. The answer must be reachable via the decision tree but need not be on a restricted answer sublist; the decision tree is built to handle any word in the valid answer word list.

### past_answers_as_holdback
Publicly documented past Wordle answers serve as a holdback evaluation set used during optimization and hillclimbing. This set is an evaluation instrument only; it does not constrain or filter the word lists used to build the database.

### response_symbol_case
G, Y, and B are accepted in any case (upper or lower) from the user to reduce input friction.

### start_word_selection
The starting word is selected by the precomputation process as the root of the decision tree that yields the best metrics discovered so far. It is not chosen by hand. A mathematically provable optimal starting word may not be known; the process is free to try multiple candidates and retain the one producing the best (worst-case depth, mean depth) outcome.

### tie_breaking_for_equal_paths
Where multiple candidate words yield identical worst-case and mean-depth outcomes at a given decision tree node, the lexicographically first word is chosen, ensuring the database is deterministic and reproducible across independent builds from the same word lists.

### word_list_versioning
The valid guess word list and valid answer word list are sourced from the NYT Wordle as of June 2026. Because the answer list has changed historically, any database artifact must document the exact source and retrieval date of the word lists used to build it. Rebuilding from a different word list snapshot may produce a different optimal tree.

## Contracts

### batch_mode_statistics

**Given** the database has been built and a file F contains one word per line, each word being a valid answer word

**When** the user runs the tool in batch evaluation mode with F as input

**Then** stdout contains one line per word showing the word and its solve depth, followed by aggregate statistics (worst-case depth, mean depth, depth distribution), and exit code is 0

### database_corruption_detection

**Given** the database file has been modified after it was built

**When** the tool is started in any mode

**Then** the tool exits with a non-zero status and writes a checksum failure message to stderr without performing any solve operations

### invalid_response_format

**Given** the tool has issued a guess in solver mode

**When** the user provides a response that is not exactly 5 symbols over {G,Y,B,g,y,b}

**Then** the tool writes an error message to stderr and re-prompts for the response without advancing the solver state

### solution_mode_invalid_word

**Given** the database has been built

**When** the user runs the tool in solution mode with a word that is not in the valid answer word list

**Then** the tool writes an error message to stderr and exits with a non-zero status

### solution_mode_known_answer

**Given** the database has been built and W is a valid answer word

**When** the user runs the tool in solution mode with W as the argument

**Then** stdout contains the complete ordered sequence of guesses and responses leading to W, the final guess equals W, every response is a valid 5-symbol string over {G,Y,B}, and the sequence length is between 1 and 6 inclusive

### solver_mode_correct_responses

**Given** the database has been built and the user starts the tool in solver mode

**When** the user provides correct G/Y/B responses for each guess made by the tool

**Then** the tool issues a guess equal to the hidden answer within 6 rounds and outputs a success message on stdout

### solver_mode_inconsistent_response

**Given** the tool has made at least one guess in solver mode and received a valid response

**When** the user provides a response to the next guess that is logically inconsistent with a prior (guess, response) pair in the session

**Then** the tool writes a descriptive error message to stderr identifying the inconsistency, exits with a non-zero status, and does not issue another guess

## Unconstrained

### cli_framework_and_ux
The choice of CLI framework, argument syntax, color output, progress indicators, and interactive prompt design are left to the implementation, provided all functional contracts are satisfied.

### database_internal_format
The on-disk layout of the precomputed database is unconstrained, provided O(1) per-step lookup is achievable and, if the format is binary and proprietary, the debug_dump_tool invariant is satisfied.

### implementation_language
Any programming language or combination of languages may be used. Implementations that exploit Apple Silicon M2 SIMD, NEON intrinsics, Grand Central Dispatch, or other hardware-level parallelism to reduce precomputation time are encouraged. Remote or distributed compute may be used for the precomputation phase.

### precomputation_strategy
The algorithm used to build the decision tree — minimax, entropy minimization, Monte Carlo tree search, branch-and-bound, beam search, or any hybrid — is unconstrained. Multiple strategies may be run and compared; the database artifact with the best (worst-case depth, then mean depth) metrics is retained. If a strategy can be shown to produce a provably optimal tree, that finding should be noted in the database metadata, but proof of optimality is not required.

### solver_intermediate_algorithm
The dynamic solver built as an intermediate development artifact (used for consistency checking in solver mode and as a stepping stone to full precomputation) may use any strategy. It need not be identical to the precomputed decision tree; it serves as a development and verification instrument.
