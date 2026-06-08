#!/usr/bin/env bash
# beam_sweep.sh — sweep --beam-width and report quality metrics + build time.
#
# Usage:
#   ./scripts/beam_sweep.sh [--full]
#
# With --full: builds against the full guess-word list (14,855 words as answers).
# Without:    builds against the curated answer list (2,355 words, default).
#
# Output (one line per beam width):
#   K=<N>  build=<Xs>  eval_sets=<S>  hits=<H>/<T>(<pct>%)  dag_reuse=<D>  worst=<W>  mean=<M>

set -euo pipefail

BUILD="./build/build_db"
WORDLE="./build/wordle"
ANSWERS="data/answers.txt"
FULL_FLAG=""
EVAL_TARGET="$ANSWERS"

if [[ "${1:-}" == "--full" ]]; then
    FULL_FLAG="--full"
    EVAL_TARGET="data/words.txt"
fi

# Beam widths to test.  Start narrow, go wide to find where quality plateaus.
WIDTHS=(1 2 3 5 10 20 50)

echo "beam_sweep${FULL_FLAG:+ (full)}"
echo "------------------------------------------------------------------------"
printf "%-6s  %-10s  %-12s  %-22s  %-10s  %-6s  %-8s\n" \
    "K" "build(s)" "eval sets" "cache hits" "dag reuse" "worst" "mean"
echo "------------------------------------------------------------------------"

for K in "${WIDTHS[@]}"; do
    OUTDB="/tmp/beam_sweep_k${K}.db"
    rm -f "$OUTDB"

    # Capture stdout; stderr goes to /dev/null (minimax noise)
    START=$(date +%s%N)
    OUTPUT=$(
        "$BUILD" \
            --beam-width "$K" \
            --output     "$OUTDB" \
            $FULL_FLAG   \
            2>/dev/null
    )
    END=$(date +%s%N)
    ELAPSED=$(echo "scale=1; ($END - $START) / 1000000000" | bc)

    # Extract beam-search-specific stats (only present for K > 1)
    EVAL_SETS=$(echo "$OUTPUT"  | grep -oE '[0-9]+ unique sets'      | awk '{print $1}' || echo "n/a")
    HITS_LINE=$(echo "$OUTPUT"  | grep "cache hits"                   || echo "")
    DAG_LINE=$(echo  "$OUTPUT"  | grep "paths reused"                 || echo "")

    if [[ -n "$HITS_LINE" ]]; then
        HITS=$(echo  "$HITS_LINE" | grep -oE '[0-9]+ cache hits'  | awk '{print $1}')
        TOTAL=$(echo "$HITS_LINE" | grep -oE '[0-9]+\.[0-9]+%' | sed 's/[^0-9.]//g' || echo "?")
        CACHE_STR="${HITS} (${TOTAL}%)"
        DAG_REUSE=$(echo "$DAG_LINE" | grep -oE '[0-9]+ paths' | awk '{print $1}' || echo "0")
    else
        EVAL_SETS="n/a"
        CACHE_STR="n/a (K=1)"
        DAG_REUSE="n/a"
    fi

    # Evaluate quality against answer list
    EVAL_OUT=$( "$WORDLE" --db "$OUTDB" eval 2>/dev/null || echo "" )
    WORST=$(echo "$EVAL_OUT" | grep -oE 'worst=[0-9]+' | head -1 | cut -d= -f2 || echo "?")
    MEAN=$(echo  "$EVAL_OUT" | grep -oE 'mean=[0-9]+\.[0-9]+'    | head -1 | cut -d= -f2 || echo "?")

    # Fallback: parse from build output if eval didn't work
    if [[ "$WORST" == "?" ]]; then
        WORST=$(echo "$OUTPUT" | grep "worst case" | grep -oE '[0-9]+' | head -1 || echo "?")
        MEAN=$(echo  "$OUTPUT" | grep "mean depth" | grep -oE '[0-9]+\.[0-9]+'   || echo "?")
    fi

    printf "%-6s  %-10s  %-12s  %-22s  %-10s  %-6s  %-8s\n" \
        "K=$K" "${ELAPSED}s" "$EVAL_SETS" "$CACHE_STR" "$DAG_REUSE" "$WORST" "$MEAN"

    rm -f "$OUTDB"
done

echo "------------------------------------------------------------------------"
echo "done."
