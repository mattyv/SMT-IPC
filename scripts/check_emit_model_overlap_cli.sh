#!/bin/sh
# check_emit_model_overlap_cli.sh — CLI-parse regression probe for
# --emit-model-overlap (WP3 of the overlap-regime plan).
#
# This is a shell-level INTEGRATION probe, not a --test unit check: the flag
# parsing/mutual-exclusion logic lives inline in main()'s argv loop, which
# --test's pure SECTION 6 never touches (see sibling_analyze.cpp's module
# header — --test is deliberately compiler/mca/hardware-free). Uses
# --mca-file against a canned dump (no compiler/llvm-mca needed) so this
# probe runs unconditionally alongside the other --test-adjacent checks, the
# same reasoning as check_f1_residual.sh.
#
# Asserts, by EXIT CODE and a light header-line grep only (never full stdout
# text, per the plan):
#   1. --emit-model-overlap alone succeeds and stamps "# regime=overlap".
#   2. --emit-model --emit-model-overlap together is REJECTED (nonzero exit)
#      — they are two different regimes' curves, not composable.
#   3. --emit-model alone still succeeds (the flag addition didn't break the
#      existing path) and does NOT stamp "# regime=overlap" (paced output is
#      unchanged — see WP2's byte-identical regression guard).
#
# args: <sibling_analyze binary> <mca dump file> <profile file>
set -e
SA="$1"
MCA="$2"
PROF="$3"

fail() {
  echo "FAIL check_emit_model_overlap_cli: $1" >&2
  exit 1
}

# 1. --emit-model-overlap alone: must succeed and stamp the overlap marker.
OUT="$("$SA" --mca-file "$MCA" --profile "$PROF" --emit-model-overlap 2>/dev/null)" \
  || fail "--emit-model-overlap alone exited nonzero"
printf '%s\n' "$OUT" | grep -q '^# regime=overlap$' \
  || fail "--emit-model-overlap output missing '# regime=overlap' header"

# 2. Combined flags: must be REJECTED.
if "$SA" --mca-file "$MCA" --profile "$PROF" --emit-model --emit-model-overlap \
    >/dev/null 2>/dev/null; then
  fail "--emit-model + --emit-model-overlap together exited 0 (should be rejected)"
fi

# 3. --emit-model alone: still succeeds, and does NOT carry the overlap stamp.
OUT2="$("$SA" --mca-file "$MCA" --profile "$PROF" --emit-model 2>/dev/null)" \
  || fail "--emit-model alone exited nonzero"
if printf '%s\n' "$OUT2" | grep -q '^# regime=overlap$'; then
  fail "--emit-model (paced) output unexpectedly carries '# regime=overlap'"
fi

echo "OK check_emit_model_overlap_cli"
