#!/bin/sh
# check_f1_residual.sh — Finding-1 REGRESSION PROBE for sibling_analyze.
#
# This is an INTEGRATION test, not a unit test: it runs the actual BUILT
# BINARY end-to-end (parse --mca-file -> overlay -> print_json), the same
# code path a real user hits, so it catches drift at the print_report()/
# print_json() call sites themselves — not just in a helper function those
# call sites could silently stop calling.
#
# Background: the consumer-side per-message work estimate (print_report's
# W_block/W_msg, print_json's consumer_ns_per_block/consumer_ns_per_msg) must
# be computed from EFFECTIVE cycles (Total Cycles/Iterations), NOT Block
# RThroughput (a throughput lower bound blind to dependency-chain latency).
# sibling_analyze --test's own part-(d) self-check recomputes this value
# INLINE via effective_cycles_per_iter() and never calls print_report() or
# print_json() — so a revert at either print site (reintroducing
# `cons.cycles_per_iter`) would still pass `--test` untouched. This script
# closes that gap by asserting the value the BINARY ACTUALLY PRINTS.
#
# examples/f1_residual_latency_bound.mca is a canned llvm-mca dump (same
# latency-bound consumer region as kCannedMcaLatencyBound in
# sibling_analyze.cpp, llvm-mca-20's REAL report for
# examples/spsc_marked.cpp's serial-chain producer) where:
#   Block RThroughput   = 6.0   cyc/block   (WRONG denominator if used)
#   Total Cycles/Iters   = 24.05 cyc/block  (CORRECT — effective)
# examples/f1_residual_latency_bound.profile pins freq_ghz=1.0 so
# consumer_ns_per_block == effective cyc/block exactly: 24.05 ns.
#
# args: <sibling_analyze binary> <mca dump file> <profile file> [tol_ns]
set -e
SA="$1"
MCA="$2"
PROF="$3"
TOL="${4:-0.1}" # ns tolerance around the expected 24.05, named not magic

EXPECTED_EFFECTIVE=24.05
# What the OLD (buggy) print-site code would have emitted instead: the
# RThroughput-derived value (6.0 cyc / 1.0 GHz = 6.0 ns). Asserting the
# printed value is far from THIS, not just close to the correct one, is what
# gives the probe teeth against a revert.
WRONG_RTHROUGHPUT=6.0

OUT="$("$SA" --mca-file "$MCA" --profile "$PROF" --json 2>/dev/null)"

# Extract consumer_ns_per_block from the JSON (a small fixed-shape emitter,
# not general JSON — a plain sed/awk pull matches how the rest of this
# repo's shell probes read sibling_analyze's --json output).
GOT="$(printf '%s\n' "$OUT" | sed -n 's/.*"consumer_ns_per_block": \([0-9.]*\).*/\1/p')"

if [ -z "$GOT" ]; then
  echo "FAIL check_f1_residual: could not find consumer_ns_per_block in --json output:" >&2
  printf '%s\n' "$OUT" >&2
  exit 1
fi

awk -v got="$GOT" -v expected="$EXPECTED_EFFECTIVE" -v wrong="$WRONG_RTHROUGHPUT" \
    -v tol="$TOL" 'BEGIN {
  diff_expected = got - expected; if (diff_expected < 0) diff_expected = -diff_expected;
  diff_wrong = got - wrong; if (diff_wrong < 0) diff_wrong = -diff_wrong;
  if (diff_expected > tol) {
    printf "FAIL check_f1_residual: consumer_ns_per_block=%.4f, expected ~%.4f (effective Total Cycles/Iterations), NOT the RThroughput-derived value\n", got, expected > "/dev/stderr";
    exit 1;
  }
  if (diff_wrong < tol) {
    printf "FAIL check_f1_residual: consumer_ns_per_block=%.4f matches the WRONG RThroughput-derived value (%.4f) — the F1 residual fix has regressed\n", got, wrong > "/dev/stderr";
    exit 1;
  }
  printf "OK check_f1_residual: consumer_ns_per_block=%.4f matches effective (Total Cycles/Iterations), not RThroughput\n", got;
}'
