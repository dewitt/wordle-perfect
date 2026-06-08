#!/usr/bin/env bash
# memo_sweep.sh — sweep --memo-limit hyperparameter and report build time + hit rate.
#
# Usage:
#   ./scripts/memo_sweep.sh [--full]
#
# With --full: sweeps against the full guess-word list (all 14,855 words as answers).
# Without:    sweeps against the curated answer list (2,355 words, default).
#
# Output (one line per limit value):
#   memo-limit=<N>  build=<Xs>  hits=<H>/<T> (<pct>%)  sets=<S>  worst=<W>  mean=<M>
#
# Limits tested: 0 (disabled), 30, 60, 100, 200, 500, unbounded (99999).

set -euo pipefail

BUILD="./build/build_db"
TMPDIR_PREFIX="/tmp/memo_sweep"
FULL_FLAG=""
LABEL_SUFFIX=""

if [[ "${1:-}" == "--full" ]]; then
    FULL_FLAG="--full"
    LABEL_SUFFIX=" (full)"
fi

LIMITS=(0 30 60 100 200 500 99999)

echo "memo_sweep: sweeping --memo-limit${LABEL_SUFFIX}"
echo "--------------------------------------------------------------"
printf "%-18s  %-12s  %-25s  %-10s  %-8s  %-8s\n" \
    "memo-limit" "build(s)" "cache hits" "sets cached" "worst" "mean"
echo "--------------------------------------------------------------"

for N in "${LIMITS[@]}"; do
    if (( N >= 99999 )); then
        LABEL="unbounded"
    else
        LABEL="$N"
    fi

    OUTDB="${TMPDIR_PREFIX}_${LABEL}.db"
    rm -f "$OUTDB"

    # Run build_db and capture stdout; time it
    START=$(date +%s%N)
    OUTPUT=$(
        "$BUILD" \
            --memo-limit "$N" \
            --output     "$OUTDB" \
            $FULL_FLAG   \
            2>/dev/null
    )
    END=$(date +%s%N)
    ELAPSED=$(echo "scale=2; ($END - $START) / 1000000000" | bc)

    # Extract metrics from output
    MEMO_LINE=$(echo "$OUTPUT" | grep "^memo cache:" || echo "")
    STATS_LINE=$(echo "$OUTPUT" | grep "^  worst case" || echo "")
    MEAN_LINE=$(echo "$OUTPUT"  | grep "^  mean depth"  || echo "")

    if [[ -n "$MEMO_LINE" ]]; then
        HITS=$(echo "$MEMO_LINE"  | grep -oE '[0-9]+ hits'    | awk '{print $1}')
        TOTAL=$(echo "$MEMO_LINE" | grep -oE '[0-9]+ lookups' | awk '{print $1}')
        PCT=$(echo   "$MEMO_LINE" | grep -oE '\([0-9.]+%\)'   | tr -d '()')
        SETS=$(echo  "$MEMO_LINE" | grep -oE '[0-9]+ unique'  | awk '{print $1}')
        CACHE_STR="${HITS}/${TOTAL} (${PCT})"
    else
        CACHE_STR="disabled"
        SETS="0"
    fi

    WORST=$(echo "$STATS_LINE" | grep -oE '[0-9]+' | head -1 || echo "?")
    MEAN=$(echo  "$MEAN_LINE"  | grep -oE '[0-9]+\.[0-9]+'    || echo "?")

    printf "%-18s  %-12s  %-25s  %-10s  %-8s  %-8s\n" \
        "$LABEL" "${ELAPSED}s" "$CACHE_STR" "$SETS" "$WORST" "$MEAN"

    rm -f "$OUTDB"
done

echo "--------------------------------------------------------------"
echo "done."
