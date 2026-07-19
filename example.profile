# example.profile — machine constants for sibling_analyze.
# ---------------------------------------------------------------------------
# THESE ARE EXAMPLE NUMBERS (this repo's AMD Zen 5 box, from the README). They
# are here to show the format and let the tool run out of the box — they are
# NOT your machine. Regenerate every value on your target before trusting a
# budget, and pass this file with --profile.
#
# How to obtain each value on your box:

# Core clock the HOT loop actually holds, GHz. Converts mca cycles -> ns. Read
# the boost clock your pinned hot core sustains (not the invariant TSC rate) —
# this box (governor=performance) sustains ~4.98 GHz under sibling load.
freq_ghz = 4.98

# One-way handoff latency, ns, on an SMT sibling vs a same-CCX core. ONLY THE
# DIFFERENCE enters the model (the sibling's handoff advantage on the critical
# path). Take it from spsc_pipeline's LIGHTEST-work end-to-end p50 row — the
# sibling vs same-CCX delta there is the honest one-way handoff edge (~40-50 ns
# on this box). Do NOT use smt_pingpong's raw RTT directly: it is a round trip,
# roughly twice the one-way cost.
handoff_sibling_ns = 70
handoff_sameccx_ns = 120

# Presence tax: fractional slowdown from a merely-awake (politely pausing)
# sibling, independent of port contention. From sibling_noise's noop-tenant
# median / idle median (~+3% here). This term is what guarantees a finite
# crossover; do not set it to 0 unless you have measured it there.
presence_tax = 0.03

# Matched-pacing headroom: message period = headroom * (consumer_work +
# allowance). From spsc_pipeline's PROC_SWEEP_HEADROOM (1.5 = ~65% utilisation).
pacing_headroom = 1.5

# Fixed per-message overhead outside the swept processing work (queue pop +
# handoff detection), ns. Keeps duty(W) finite as W -> 0. Only the PACED
# regime's duty(W) uses this one.
allowance_ns = 40

# Fixed ns folded into spsc_pipeline --both-busy-overlap's consumer-only
# pacing gap (its PROC_SWEEP_HANDOFF_ALLOWANCE_NS constant — see
# spsc_pipeline.cpp's PipeCfg). Only the OVERLAP regime's duty_overlap(W)
# uses this one; it is NOT the same knob as allowance_ns above.
gap_allowance_ns = 150

# Calibration scale: maps mca's raw combined-demand C_raw onto a measured
# busy-sibling multiplier. Run `sibling_analyze --calibrate <your measured
# hot/idle ratio>` and paste the value it prints. 1.0 means "trust mca as-is".
calib_scale = 0.820

# The -mcpu model llvm-mca uses. "native" auto-detects the host, BUT LLVM < 19
# has no znver5 model, so on a Zen 5 box native silently falls back to a
# generic/znver4 model — set this explicitly (e.g. znver4) if native
# mis-resolves, or upgrade to LLVM >= 19 for a true znver5 model.
mca_mcpu = native

# Machine fingerprint, stamped into --emit-model output and matched against the
# measured CSV so an overlay never silently mixes two microarchitectures.
machine = zen5
