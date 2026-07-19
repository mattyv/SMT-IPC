#!/bin/sh
# run_crossover_check.sh — same-machine self-consistency gate for the static
# model against the measured crossover. Generates the predicted model curve with
# sibling_analyze --emit-model, then asserts (numerically, not by pixels) that
# it agrees with the measured CSV via plot_crossover.py --check.
#
# IMPORTANT: this is only meaningful when the measured CSV was produced on the
# SAME machine as this run (sibling_analyze's llvm-mca model is uarch-specific).
# docs/crossover_data.csv ships this repo's Zen 5 numbers; regenerate it from
# spsc_pipeline --proc-sweep on your box before trusting a pass elsewhere.
#
# args: <sibling_analyze> <example.cpp> <profile> <measured.csv> <plot.py> [tol_ns]
set -e
SA="$1"; SRC="$2"; PROF="$3"; CSV="$4"; PLOT="$5"; TOL="${6:-130}"
MODEL="$(mktemp)"
"$SA" "$SRC" --profile "$PROF" --emit-model >"$MODEL" 2>/dev/null
python3 "$PLOT" --measured "$CSV" --model "$MODEL" --check --tol "$TOL"
rm -f "$MODEL"
