# SMT-IPC — method & honesty notes

The [README](README.md) has the result and how to run the tools. This file is the rigor behind
`sibling_analyze` (Step 5): what its numbers actually predict, how the two dashed lines on the
crossover graph are computed, and where the whole thing stops being trustworthy. Read it if you
want to trust — or poke holes in — the static predictor.

## The budget number is mostly measured constants

For the paced case, `W*` is barely the llvm-mca model at all: `W* = Δh / (ε + duty·(C−1))`, and at
the crossover the `duty` term is tiny, so it collapses to **Δh/ε ≈ 50 ns / 3% ≈ 1.67 µs** — the
*measured* handoff edge (Step 1) over the *measured* presence tax (Step 2). The contention term `C`
nudges that by only **~0.8%** (13 ns), and `calib_scale` is *fit* to reproduce Step 2's 1.81×
anyway, so the magnitude agrees by construction. What the tool genuinely *predicts* is the
**verdict** — does a specific execution port oversubscribe when these two loops run at once, and
which one — read straight off llvm-mca on your compiled code. Trust the `COLLIDES`/`fit` call and
the named port; take `W*` as a ballpark.

## How close is `W*` to the measured crossover?

~1.65 µs predicted vs ~2.4–3.7 µs measured (it wanders run-to-run): same order of magnitude, within
a factor of ~2 — all a compile-time screen that runs *nothing* can honestly claim. The dashed
overlay on the crossover graph is `sibling_analyze --emit-model` against the measured curves;
`scripts/plot_crossover.py --check` asserts they agree numerically — but since the model's `C`
barely moves the curve, that check mostly confirms the Δh/ε constants line up, not that the port
model is right. It's same-machine only (the mca model is µarch-specific), so regenerate
`docs/crossover_data.csv` from `spsc_pipeline --proc-sweep` on your own box first.

## Two dashed lines, two different jobs

Purple is the PACED regime — `--emit-model`'s `duty(W)` assumes the producer politely waits for a
matched-pacing deadline, so the work windows stay disjoint. `--emit-model-overlap` draws the red
OVERLAP line (`spsc_pipeline --both-busy-overlap`), where the pacing gap is sized for the *consumer
alone* — `1.5·(W + 150 ns)`, the same `PROC_SWEEP_HEADROOM`/`PROC_SWEEP_HANDOFF_ALLOWANCE_NS`
constants as the paced gap, just without the producer's work folded in — so the two genuinely
overlap once `W` is big enough. Its `duty` is a first-order deadline-geometry closed form
(`duty_overlap`, no fixed point, no fitting): `duty_ov(W) = clamp((2−H) + (h_sib − H·A_gap)/W, 0,
1)`, H = pacing headroom, A_gap = 150, h_sib = the sibling handoff edge. Unlike the paced line,
where plugging in `C` moves the curve by under 1%, here `duty` *grows* with `W` — from 0 below a
~310 ns turn-on to a 0.5 asymptote — so `duty·(C−1)` dominates and the crossover is genuinely set by
the port-contention term `C`, not by the two measured constants alone. On this box: `C_ov ≈ 1.80`,
predicted `W* ≈ 405 ns` (range 374–464 ns), inside the coarse measured **rung bracket [130, 451]
ns** (the adjacent sweep rungs that straddle zero). The interpolated crossing wanders run-to-run —
~400 ns on the committed session, ~450–470 ns on repeats; the rising rung sits one ~10 ns `rdtsc`
quantum from zero, so the crossing is noise-limited and sometimes falls just outside the predicted
band. Not a bullseye — but the predicted sub-µs collapse is confirmed, and the location agrees to
within the measurement's resolution. That collapse is ~4× the paced `W*` (~1.65 µs), which is the
whole point: overlap turns a multi-µs budget into a sub-µs one.

## One honest catch on the overlap line

Its *shape* — the turn-on, the growth, the `2−H` ceiling — comes only from `H`, `A_gap`, and
`h_sib`, all fixed before the overlap experiment ever ran. Its *scale* doesn't: `C_ov`'s magnitude
rides on `calib_scale`, fit against `sibling_noise`'s busy-sibling ratio (1.81×) — a separate
experiment. So it's an out-of-sample test of the *mechanism* (does a duty-growing-with-`W` model
land the right order of magnitude and collapse direction?), not a from-scratch number. Which is
also why you should never re-run `--calibrate` against the overlap ladder — that'd just fit the
answer to the question. The `--check` tolerance is loose on purpose (`OVERLAP_REL_TOL`, a
factor-of-2 bar) for the same reason.

## Scope: sub-saturation only

Both dashed lines stop meaning anything once the queue itself saturates — the 6.9 µs rung on the
overlap measured series is EXCLUDED from `docs/crossover_data_overlap.csv` for that reason
(waited-fraction ~0.13, well outside the queue-free band): a pacing-headroom *methodology*
boundary, not a contention number. The model echoes it: as `W` grows, `duty_overlap`'s `2W` term
approaches its `H·(W+A_gap)` period and the clamp to 1 stops being a meaningful prediction — the
formula is first-order geometry, not a queueing model, and was never meant to reach into saturation.

## It's a screening linter, not an oracle

`llvm-mca` sees execution ports and front-end dispatch and *nothing else*: it assumes perfect
caches, perfect store buffers, a single instruction stream. So a `COLLIDES` verdict (the ports
genuinely oversubscribe) is trustworthy, while a "no collision" only clears the *compute* side — for
anything memory-heavy, confirm with `sibling_noise`/`spsc_pipeline`, which stay the ground truth.
The tool also diffs your code compiled with and without the markers, to catch a marker that changed
what ships (e.g. blocked vectorisation).
