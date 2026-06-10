# Builder performance benchmarks

Tracking the four hillclimbing targets, with the emphasis on **builder
performance** (#3), which directly unlocks lower **mean depth** (#2):

1. **Max depth** — done; 5 is provably optimal for the curated set (4 impossible).
2. **Mean depth** — open; currently 3.4870 (lookahead 1). The cheaper the build,
   the wider the lookahead/sweep we can afford → lower mean.
3. **Build time** — the active focus. Biggest lever.
4. **Runtime lookup** — already O(1) (mmap binary); not a current focus.

`build_db` prints a `WPMETRICS` line (stable `key=value` schema, `schema=1`) at
the end of every run. To record a new datapoint, run the build and paste the
`WPMETRICS` line into the log below with a one-line note on what changed.

Environment for the numbers below: Apple M2, 8 threads, Apple Clang, Release.
Default invocation unless noted: `./build/build_db --output wordle.db`
(top=50 openers, sweep_lookahead=1, emit_lookahead=1, worst≤5, 2,355 answers).

## Key observations from the baseline

- **The opener sweep dominates: ~95% of wall time** (52.7s of 55.4s) at only
  **~0.95 openers/sec**. Everything else (matrix 0.48s, emit 2.1s, eval/finalize/
  binary <0.1s) is noise by comparison.
- Feasibility memo hit-rate is only **69.6%** during the sweep because each of
  the 8 sweep workers keeps a *private* feasibility memo — there is no
  cross-opener/cross-thread sharing. A shared (or seeded) memo is the obvious
  first optimization to investigate.
- `feas_recur` (96.5K full searches) and `partitions` (825K) are the real work
  units; driving those down — or sharing their results — is the path to speedup.

## Log

| date       | wall_s | sweep_s | emit_s | sweep/s | feas_hit | mean   | note |
|------------|--------|---------|--------|---------|----------|--------|------|
| 2026-06-10 | 55.38  | 52.68   | 2.07   | 0.95    | 69.6%    | 3.4870 | **baseline** — instrumentation added |
| 2026-06-10 | 42.63  | 40.00   | 1.92   | 1.25    | 74.6%    | 3.4870 | shared feasibility cache across sweep workers |
| 2026-06-10 | 37.68  | 35.28   | 1.77   | 1.42    | 74.7%    | 3.4870 | partial_sort guess ranking (was full sort) |
| 2026-06-10 | 30.52  | 28.91   | 0.98   | 1.73    | 74.6%    | 3.4870 | reusable sparse-reset histograms; single-pass score_guess |
| 2026-06-10 | 29.38  | 27.79   | 0.99   | 1.80    | 82.2%    | 3.4870 | check feasibility buckets largest-first (fail-fast) |
| 2026-06-11 | 21.37  | 18.52   | 0.96   | 2.70    | —        | 3.4870 | **`--gpu`**: Metal-batched bucket ranking (branch gpu-frontier-feasibility) |

**Cumulative: 55.4s → 29.4s (−47%)**, mean unchanged (worst-5 / 3.4870). The
sweep is still ~95% of wall time; the floor is now `partition()` (≈707K calls)
and the per-node ranking that scans all ~15k guesses. Next levers: cut the
guess-pool scan (matrix-read locality / SIMD, #12), reduce FeasibilityCache lock
contention, or seed the sweep's shared cache from a quick first pass.

## GPU offload analysis (#28)

Q: can the M2 GPU take the build's dominant work (the per-node 15k-guess scan)?

Findings from instrumentation + benchmarks:
- The feasibility recursion is **~40k tiny candidate sets** (probe: <64 cand
  ≈39.7k nodes, 64–512 ≈250, **≥512: 0**) — each scans all ~15k guesses.
- **Per-set** GPU dispatch loses: ~0.5 ms launch latency × 40k = ~20 s, and tiny
  sets only get ~6.5× (vs 15× for big sets). The GPU is latency-bound here.
- **Batched** GPU wins big — `GpuScorer::score_sets` scores all guesses against
  many sets in one dispatch (benchmarked, exact parity):
  - 2000 sets × 16 cand: CPU 10.6 s → GPU 0.22 s (**49×**)
  - 2000 sets × 40 cand: CPU 17.2 s → GPU 0.30 s (**58×**)
  - 2000 sets × 128 cand: CPU 23.3 s → GPU 0.52 s (**45×**)
- Caveat: the GPU only pays off when the 220 MB pattern matrix is uploaded
  **once** and reused across many dispatches. One-shot uses (e.g. the single
  opener pre-ranking, ~50 ms on CPU) are net-negative (upload+transpose ≈100 ms).

**Integration (landed on branch `gpu-frontier-feasibility`, opt-in `--gpu`):**
in `is_feasible`, before recursing on a chosen guess's sibling buckets, batch-rank
all the multi-element buckets in one `score_sets` dispatch over a GPU-resident
matrix uploaded once and shared read-only across all sweep workers. The recursion
then consumes the cached rankings instead of CPU-scanning ~15k guesses per
bucket. Verified **byte-for-byte identical** trees (standard + full-coverage),
deterministic across runs. Result: wall **31.2s → 21.4s (−32%)**, sweep
29.3s → 18.5s.

Why not the full 45-60× the kernel shows: only the *ranking* is offloaded; the
recursion control flow, `partition`, and the GPU-result unpack stay on CPU, and
per-node batches are small (a node's few buckets) so dispatch latency isn't fully
amortised.

### Where is the wall? (ceiling experiment)

To find the limit, a probe stubbed out `best_guess_feasible`'s entropy ranking
(returning the feasible witness instead) on the `--gpu` build:

- full `--gpu` build: sweep ≈18 s
- with entropy ranking removed: **sweep ≈3.7 s, wall ≈6.0 s**

So with the max-bucket feasibility ranking already on the GPU, **the entire
remaining sweep cost is the CPU entropy ranking in `best_guess_feasible`.** If
that could be offloaded with parity, the build would approach ~6 s (≈5× vs the
30 s CPU baseline).

The blocker is **float vs double**: the GPU computes entropy in `float`, and the
tiny differences reorder near-entropy ties, changing the chosen guess and
producing a *different but equally-optimal* (worst-5 / 3.4870) tree. We require
byte-for-byte parity, so entropy ranking stays on the CPU in `double`.

Attempts to bound the CPU entropy scan to just the GPU-identified
progress-making guesses, or to GPU-presort then CPU-recompute a top pool, did
**not** reliably beat the current `--gpu` path (run-to-run thermal variance on
the M2 ≈±15% swamped the gains) and risked breaking parity. So the practical
ceiling for a *parity-preserving* GPU integration is roughly the current ~18 s
sweep; reaching ~6 s requires either accepting a different-but-optimal tree
(GPU float entropy) or a bit-exact GPU entropy (Metal `double`, slower) — both
deliberately not taken. Coarser whole-level batching could still trim the
GPU-side `score_sets`/unpack overhead.

### Raw WPMETRICS datapoints

```
# 2026-06-10 baseline (default build, M2/8t)
WPMETRICS schema=1 strategy=minimax-worst5-lookahead1 start=reast worst=5 mean=3.4870 nodes=2648 answers=2355 words=14855 jobs=8 top=50 sweep_lookahead=1 emit_lookahead=1 openers_swept=50 matrix_s=0.480 sweep_s=52.675 emit_s=2.068 eval_s=0.030 finalize_s=0.019 binary_s=0.001 build_s=54.843 wall_s=55.381 sweep_openers_per_s=0.95 emit_nodes_per_s=1280.3 feas_calls=1278074 feas_hits=890034 feas_recur=96538 feas_hit_rate=0.6964 partitions=825394 choice_calls=29186 choice_hits=1916 feas_memo=977 choice_memo=298

# 2026-06-10 after largest-bucket-first fail-fast (default build, M2/8t)
WPMETRICS schema=1 strategy=minimax-worst5-lookahead1 start=reast worst=5 mean=3.4870 nodes=2648 answers=2355 words=14855 jobs=8 top=50 sweep_lookahead=1 emit_lookahead=1 openers_swept=50 matrix_s=0.463 sweep_s=27.785 emit_s=0.989 eval_s=0.027 finalize_s=0.009 binary_s=0.001 build_s=28.872 wall_s=29.378 sweep_openers_per_s=1.80 emit_nodes_per_s=2676.9 feas_calls=843267 feas_hits=692810 feas_recur=57827 feas_hit_rate=0.8216 partitions=706680 choice_calls=29200 choice_hits=2050 feas_memo=980 choice_memo=298 shared_feas_cache=56772
```
