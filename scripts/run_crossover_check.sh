#!/bin/sh
# run_crossover_check.sh — same-machine self-consistency gate for the static
# model against the measured crossover. Generates the predicted model curve with
# sibling_analyze --emit-model, then asserts (numerically, not by pixels) that
# it agrees with the measured CSV via plot_crossover.py --check.
#
# This check exercises TWO different parts of the model, and prints which:
#   1. the closed-form Δh/ε/duty constants — via the model curve vs measured
#      CSV (W* band overlap + per-point residuals);
#   2. the mca ENGINE itself (parsing + port-pressure math) — via a SELF-
#      overlay of the consumer region (--producer consumer --consumer
#      consumer) asserted against the measured busy-sibling multiplier. This
#      is the term that catches a broken/misparsing mca integration: without
#      it, the mca C contribution is small at every measured proc-sweep work
#      point in this repo's example, so (1) alone would pass even with a
#      garbage parser.
#
# WHEN <measured_overlap.csv> is non-empty, also generates the OVERLAP-regime
# model curve via --emit-model-overlap and passes it + <measured_overlap.csv>
# to plot_crossover.py --check's optional second pass (--model-overlap /
# --measured-overlap), which validates duty_overlap()'s W* band against the
# measured overlap sign-change bracket with a much looser tolerance
# (OVERLAP_REL_TOL in plot_crossover.py) — that regime only claims
# order-of-magnitude/shape agreement, not a tight fit (see README). This was
# hand-verified as a NEGATIVE control during development: feeding
# plot_crossover.py a copy of docs/crossover_model_overlap.csv with every
# w_ns value (and the wstar_lo/mid/hi header stamps) scaled ×4 produces a W*
# band of [1496,1856] ns against the real measured bracket [130,451] ns —
# DISJOINT, correctly FAILs. The real, unshifted CSV pair PASSes. That
# shifted CSV was throwaway (not committed); rerun the same ×4 scaling
# locally if you need to re-verify this check has teeth.
#
# IMPORTANT: this is only meaningful when the measured CSV was produced on the
# SAME machine as this run (sibling_analyze's llvm-mca model is uarch-specific).
# docs/crossover_data.csv ships this repo's Zen 5 numbers; regenerate it from
# spsc_pipeline --proc-sweep on your box before trusting a pass elsewhere.
#
# args: <sibling_analyze> <example.cpp> <profile> <measured.csv> <plot.py> \
#       <mca_bin> [tol_ns] [measured_overlap.csv]
set -e
SA="$1"; SRC="$2"; PROF="$3"; CSV="$4"; PLOT="$5"; MCA_BIN="$6"; TOL="${7:-130}"
CSV_OVERLAP="${8:-}"
MODEL="$(mktemp)"
MODEL_OVERLAP="$(mktemp)"
SELF_JSON="$(mktemp)"
STDERR_LOG="$(mktemp)"
# Build the --mca-bin flag as its own argv WORDS (via `set --`), not a single
# space-joined string: this is /bin/sh (no bash arrays), and an unquoted
# $MCA_FLAG below would word-split MCA_BIN on any spaces in the path (e.g.
# "/opt/my llvm/bin/llvm-mca"), silently passing the wrong argv to
# sibling_analyze. `set --` replaces $1.. (already captured into the named
# vars above, so safe to clobber) with exactly the flag's words, none if
# MCA_BIN is unset/empty.
if [ -n "$MCA_BIN" ]; then
  set -- --mca-bin "$MCA_BIN"
else
  set --
fi

# Keep sibling_analyze's stderr VISIBLE: it is the lint/perturbation/-mcpu
# honesty channel (LINT ERROR, marker-perturbation WARNING, the -mcpu
# resolution note). Discarding it would silence exactly the diagnostics that
# tell you the mca vector is untrustworthy. Capture to a file so it can still
# be printed cleanly (not interleaved mid-write with the model CSV going to
# stdout), and always show it on failure.
cleanup() {
  rm -f "$MODEL" "$MODEL_OVERLAP" "$SELF_JSON" "$STDERR_LOG"
}

if ! "$SA" "$SRC" --profile "$PROF" "$@" --emit-model \
    >"$MODEL" 2>"$STDERR_LOG"; then
  echo "sibling_analyze --emit-model failed; stderr:" >&2
  cat "$STDERR_LOG" >&2
  cleanup
  exit 1
fi
cat "$STDERR_LOG" >&2

# Self-overlay of the port-bound "consumer" region against itself: this
# reproduces the --calibrate shape (same region on both sides of overlay())
# so the check can validate the mca engine's own port-pressure math, not just
# the closed-form Δh/ε/duty constants above.
if ! "$SA" "$SRC" --profile "$PROF" "$@" --producer consumer \
    --consumer consumer --json >"$SELF_JSON" 2>"$STDERR_LOG"; then
  echo "sibling_analyze self-overlay (--json) failed; stderr:" >&2
  cat "$STDERR_LOG" >&2
  cleanup
  exit 1
fi
cat "$STDERR_LOG" >&2

# Optional overlap-regime pass (see module header): only run when a measured
# overlap CSV was given.
OVERLAP_FLAGS=""
if [ -n "$CSV_OVERLAP" ]; then
  if ! "$SA" "$SRC" --profile "$PROF" "$@" --emit-model-overlap \
      >"$MODEL_OVERLAP" 2>"$STDERR_LOG"; then
    echo "sibling_analyze --emit-model-overlap failed; stderr:" >&2
    cat "$STDERR_LOG" >&2
    cleanup
    exit 1
  fi
  cat "$STDERR_LOG" >&2
  OVERLAP_FLAGS="--model-overlap $MODEL_OVERLAP --measured-overlap $CSV_OVERLAP"
fi
rm -f "$STDERR_LOG"

# Disable -e around the check itself: a FAIL is a normal, expected exit code
# from plot_crossover.py (not a script-level error), and cleanup must still
# run either way. shellcheck disable=SC2086 (OVERLAP_FLAGS is intentionally
# word-split: 0 or 2 tokens, both fixed by this script, never user input).
set +e
python3 "$PLOT" --measured "$CSV" --model "$MODEL" --check --tol "$TOL" \
  --self-overlay-json "$SELF_JSON" $OVERLAP_FLAGS
STATUS=$?
set -e
cleanup
exit $STATUS
