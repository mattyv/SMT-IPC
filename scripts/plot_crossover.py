#!/usr/bin/env python3
"""plot_crossover.py — overlay sibling_analyze's PREDICTED crossover on the
MEASURED sibling-vs-same-CCX curve, and check that they agree numerically.

This is the second-pass companion to sibling_analyze (README Step 5). It does
NOT re-derive any model math — it reads a model curve CSV emitted by
`sibling_analyze --emit-model` (the C++ tool is the single source of the model)
and a measured CSV (from spsc_pipeline --proc-sweep), and:

  * renders a comparison SVG: measured delta points, the dashed predicted model
    curve, a shaded W* band (predicted budget with its uncertainty), the
    measured sign-change bracket, and a zero line;
  * with --check, asserts the two AGREE by numbers, never pixels: the predicted
    W* band must overlap the measured sign-change bracket, and the model's delta
    at each measured work point must be within tolerance of the measured delta.
    Exits non-zero on failure, so CI can gate on it.

HONESTY / MACHINE-MISMATCH: the measured CSV and the model CSV MUST come from
the SAME machine and session. sibling_analyze's llvm-mca model is uarch-specific
(it reads the -mcpu=native scheduler model); overlaying a model built on box A
onto box B's measured curve looks like validation but is apples-to-oranges. The
stamp line (pass --stamp) records machine/commit/LLVM so a stale or mismatched
overlay is visible. The model curve is always drawn DASHED and labelled "model"
so it can never be misread as a second measurement.

Pure stdlib (no matplotlib) so it runs anywhere; it writes SVG text directly.

usage:
  # 1. generate the model on YOUR box (same one that produced the measured CSV):
  ./sibling_analyze your_threads.cpp --profile your.profile --emit-model > model.csv
  # 2. render + check:
  python3 scripts/plot_crossover.py --measured docs/crossover_data.csv \
      --model model.csv --out docs/crossover_compare.svg --stamp "zen5 abc1234 llvm18"
  python3 scripts/plot_crossover.py --measured docs/crossover_data.csv \
      --model model.csv --check --tol 80
"""
import argparse
import json
import math
import sys

# Default measured busy-sibling multiplier (sibling_noise 'hot'/'idle' ratio),
# README's Zen 5 figure. Mirrors sibling_analyze.cpp's own `--calibrate`
# default (search main() for `calib_measured = 1.81`) — the two constants are
# NOT programmatically shared (different languages/binaries) but are the same
# ground-truth number by construction, so keep them in sync by hand if either
# changes.
DEFAULT_MEASURED_MULTIPLIER = 1.81

# Default relative-error tolerance for the self-overlay C-term check (--check
# with a --self-overlay-json report): how far the calibrated mca port-
# contention term C is allowed to drift from the measured busy-sibling
# multiplier before run_check() fails it. 20% is loose on purpose — this is
# the SAME loose-tolerance discipline as --tol/--rel-tol for the delta_ns
# residuals above (the model is a coarse upper-bound tool, not a tight fit).
# Named once here (was previously duplicated as a bare 0.20 literal in both
# run_check()'s default arg and the --c-rel-tol argparse default) so the two
# can never drift apart.
DEFAULT_C_REL_TOL = 0.20


def read_measured(path):
    """CSV: work_ns,sibling_p50_ns,sameccx_p50_ns -> ([(work_ns, delta_ns)],
    machine). The machine fingerprint is read from a `# machine=NAME` comment."""
    pts = []
    machine = None
    with open(path) as f:
        for line in f:
            line = line.strip()
            if line.startswith("#"):
                for tok in line.lstrip("# ").split():
                    if tok.startswith("machine="):
                        machine = tok.split("=", 1)[1]
                continue
            if not line or line.startswith("work_ns"):
                continue
            work, sib, ccx = (float(x) for x in line.split(",")[:3])
            pts.append((work, sib - ccx))
    pts.sort()
    return pts, machine


def read_model(path):
    """Model CSV from --emit-model. Returns (curve, meta) where curve is
    [(w_ns, delta_ns)] and meta carries C and the W* band from the header."""
    curve = []
    meta = {}
    with open(path) as f:
        for line in f:
            s = line.strip()
            if s.startswith("#"):
                for tok in s.lstrip("# ").split():
                    if "=" in tok:
                        k, v = tok.split("=", 1)
                        try:
                            meta[k] = float(v)
                        except ValueError:
                            meta[k] = v
                continue
            if s.startswith("w_ns") or not s:
                continue
            w, d = (float(x) for x in s.split(",")[:2])
            curve.append((w, d))
    curve.sort()
    return curve, meta


def sign_change_bracket(pts):
    """First adjacent pair whose delta crosses zero, in EITHER direction
    (neg->pos: sibling gets worse than same-CCX as work grows — the usual
    contention-dominated shape; or pos->neg: same-CCX starts out ahead but the
    sibling's handoff edge overtakes it as work grows — happens on boxes where
    Dh is small/negative-leaning relative to eps*W at low W). Returns (lo_w,
    hi_w, direction) or None. `direction` is "rising" (neg->pos) or "falling"
    (pos->neg) so callers can report which shape was measured rather than
    silently treating either the same as "no crossover"."""
    for (w0, d0), (w1, d1) in zip(pts, pts[1:]):
        if d0 <= 0 <= d1 and d0 != d1:
            return (w0, w1, "rising")
        if d0 >= 0 >= d1 and d0 != d1:
            return (w0, w1, "falling")
    return None


def interp_delta(curve, w):
    """Linear-interpolate the model delta at work w (curve sorted by w)."""
    if w <= curve[0][0]:
        return curve[0][1]
    if w >= curve[-1][0]:
        return curve[-1][1]
    for (w0, d0), (w1, d1) in zip(curve, curve[1:]):
        if w0 <= w <= w1:
            t = (w - w0) / (w1 - w0)
            return d0 + t * (d1 - d0)
    return curve[-1][1]


def _finite(x):
    """True iff x is a finite number (the 'none' W* sentinel is a str, and
    float('nan') would slip past a bare isinstance(float) — guard both)."""
    return isinstance(x, (int, float)) and not isinstance(x, bool) \
        and x == x and abs(x) != float("inf")


def read_self_overlay_c(path):
    """Read a sibling_analyze --json report (self-overlay: --producer NAME
    --consumer NAME with the SAME region on both sides) and return its
    calibrated "c" field. This is the mca port-contention prediction alone —
    the same shape --calibrate exercises — so asserting it against a measured
    busy-sibling multiplier validates the mca ENGINE (parsing + pressure
    math), not just the closed-form Δh/ε/duty constants the rest of --check
    exercises. Returns None if the field is missing (caller then skips the
    C-term assertion but still reports it as skipped, not silently)."""
    with open(path) as f:
        data = json.load(f)
    return data.get("c")


def run_check(measured, curve, m_machine, meta, tol_ns, rel_tol, force,
              self_overlay_c=None, measured_multiplier=DEFAULT_MEASURED_MULTIPLIER,
              c_rel_tol=DEFAULT_C_REL_TOL):
    """Numeric agreement check. Returns (ok, lines). Compares machine
    fingerprints, the W* band vs the measured sign-change bracket (with a
    both-sides-no-crossover case that PASSES), per-point residuals with a
    scale-aware tolerance max(tol_ns, rel_tol*|measured|), and — if
    self_overlay_c is given — the calibrated mca port-contention term C
    against a measured busy-sibling multiplier. Without that last term, the
    numeric check only exercises Δh/ε/duty: the mca C contribution is small at
    every measured proc-sweep work point in this repo's example, so a check
    without it would pass even with a garbage/misparsing mca engine."""
    lines = []
    ok = True

    terms = ["Δh + ε (handoff/presence-tax closed-form constants, via the W* "
             "band vs measured bracket)",
             "per-point residual (model curve vs measured, scale-aware "
             "tolerance)"]
    terms.append("mca C (port-contention engine, via self-overlay vs measured "
                 "busy-sibling multiplier)" if self_overlay_c is not None else
                 "mca C — SKIPPED (no --self-overlay-json given): this run "
                 "does NOT validate the mca engine itself, only the "
                 "closed-form constants above")
    lines.append("checking: " + "; ".join(terms))

    # Machine fingerprint: an mca model is uarch-specific, so refuse to validate
    # a cross-machine overlay unless explicitly forced.
    mod_machine = meta.get("machine")
    if isinstance(mod_machine, (int, float)):
        mod_machine = str(mod_machine)
    if m_machine and mod_machine and m_machine != mod_machine:
        msg = (f"machine mismatch: measured='{m_machine}' model='{mod_machine}' "
               f"— an mca model is uarch-specific; this overlay is not a "
               f"validation")
        if force:
            lines.append(msg + " (forced, continuing)")
        else:
            lines.append(msg + " — FAIL (pass --force to override)")
            ok = False
    elif not (m_machine and mod_machine):
        lines.append("note: machine fingerprint missing on one side — cannot "
                     "confirm same-machine; treat result with care")

    lo, hi = meta.get("wstar_lo"), meta.get("wstar_hi")
    model_has_band = _finite(lo) and _finite(hi)
    bracket = sign_change_bracket(measured)
    if bracket is None and not model_has_band:
        # Perfect agreement: neither side crosses zero in range (sibling always
        # wins). This is a PASS, not a failure.
        lines.append("both measured and model predict NO crossover in range "
                     "(sibling wins throughout): agree")
    elif bracket is None:
        lines.append("mismatch: measured shows NO crossover but model predicts "
                     f"W* in [{lo:.0f},{hi:.0f}] — FAIL")
        ok = False
    elif not model_has_band:
        lines.append("mismatch: model predicts NO crossover but measured "
                     f"crosses in [{bracket[0]:.0f},{bracket[1]:.0f}] "
                     f"({bracket[2]}) — FAIL")
        ok = False
    else:
        direction = bracket[2]
        overlap = (lo <= bracket[1]) and (bracket[0] <= hi)
        dir_note = ("" if direction == "rising" else
                     " [pos->neg: same-CCX starts ahead, sibling's handoff "
                     "edge overtakes it as work grows — not a 'no crossover' "
                     "case]")
        lines.append(
            f"W* band [{lo:.0f},{hi:.0f}] ns vs measured bracket "
            f"[{bracket[0]:.0f},{bracket[1]:.0f}] ns ({direction}){dir_note}: "
            f"{'OVERLAP ok' if overlap else 'DISJOINT — FAIL'}"
        )
        ok = ok and overlap

    worst_ratio = 0.0
    for w, dm in measured:
        dpred = interp_delta(curve, w)
        r = abs(dpred - dm)
        tol = max(tol_ns, rel_tol * abs(dm))  # scale-aware
        worst_ratio = max(worst_ratio, r / tol)
        lines.append(
            f"  work={w:8.0f} ns  measured Δ={dm:7.1f}  model Δ={dpred:7.1f}  "
            f"|resid|={r:6.1f} ns  (tol {tol:.0f})"
        )
    lines.append(f"residual/tol worst {worst_ratio:.2f}: "
                 f"{'ok' if worst_ratio <= 1.0 else 'FAIL'}")
    ok = ok and (worst_ratio <= 1.0)

    if self_overlay_c is not None:
        c_rel_err = abs(self_overlay_c - measured_multiplier) / measured_multiplier
        c_ok = c_rel_err <= c_rel_tol
        lines.append(
            f"mca C (self-overlay, calibrated) = {self_overlay_c:.3f} vs "
            f"measured busy-sibling multiplier {measured_multiplier:.3f}: "
            f"rel err {c_rel_err:.1%} (tol {c_rel_tol:.0%}): "
            f"{'ok' if c_ok else 'FAIL'}"
        )
        ok = ok and c_ok

    return ok, lines


# --- minimal SVG rendering (log-x, linear-y) -------------------------------
def render_svg(measured, curve, meta, stamp):
    W, H = 720, 440
    L, R, T, B = 70, 30, 40, 60
    pw, ph = W - L - R, H - T - B

    xs = [w for w, _ in curve] + [w for w, _ in measured]
    x_lo, x_hi = min(xs), max(xs)
    lx_lo, lx_hi = math.log10(x_lo), math.log10(x_hi)
    ys = [d for _, d in curve] + [d for _, d in measured]
    y_lo, y_hi = min(ys + [0.0]), max(ys + [0.0])
    pad = 0.1 * (y_hi - y_lo or 1)
    y_lo -= pad
    y_hi += pad

    def X(w):
        return L + pw * (math.log10(w) - lx_lo) / (lx_hi - lx_lo)

    def Y(d):
        return T + ph * (y_hi - d) / (y_hi - y_lo)

    s = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" '
         f'font-family="sans-serif" font-size="12">']
    s.append(f'<rect width="{W}" height="{H}" fill="white"/>')

    # W* band (predicted budget, shaded) and measured bracket.
    lo, hi = meta.get("wstar_lo"), meta.get("wstar_hi")
    if isinstance(lo, float) and isinstance(hi, float):
        x0, x1 = X(max(lo, x_lo)), X(min(hi, x_hi))
        s.append(f'<rect x="{x0:.1f}" y="{T}" width="{max(0,x1-x0):.1f}" '
                 f'height="{ph}" fill="#1f6feb" opacity="0.12"/>')
    br = sign_change_bracket(measured)
    if br:
        x0, x1 = X(br[0]), X(br[1])
        s.append(f'<rect x="{x0:.1f}" y="{T}" width="{max(0,x1-x0):.1f}" '
                 f'height="{ph}" fill="#2ea043" opacity="0.12"/>')

    # zero line
    s.append(f'<line x1="{L}" y1="{Y(0):.1f}" x2="{W-R}" y2="{Y(0):.1f}" '
             f'stroke="#888" stroke-dasharray="2,2"/>')
    # axes
    s.append(f'<line x1="{L}" y1="{T}" x2="{L}" y2="{H-B}" stroke="#333"/>')
    s.append(f'<line x1="{L}" y1="{H-B}" x2="{W-R}" y2="{H-B}" stroke="#333"/>')
    # x ticks at decades
    d0, d1 = math.ceil(lx_lo), math.floor(lx_hi)
    for e in range(int(d0), int(d1) + 1):
        w = 10 ** e
        xx = X(w)
        lbl = f"{w:.0f}ns" if w < 1000 else f"{w/1000:.0f}µs"
        s.append(f'<line x1="{xx:.1f}" y1="{H-B}" x2="{xx:.1f}" y2="{H-B+4}" '
                 f'stroke="#333"/>')
        s.append(f'<text x="{xx:.1f}" y="{H-B+18}" text-anchor="middle">{lbl}'
                 f'</text>')
    # y label + sign guide
    s.append(f'<text x="16" y="{T+ph/2:.0f}" transform="rotate(-90 16 '
             f'{T+ph/2:.0f})" text-anchor="middle">sibling − same-CCX (ns)</text>')
    s.append(f'<text x="{L+4}" y="{Y(0)-4:.1f}" fill="#888">↓ sibling faster '
             f'/ ↑ same-CCX faster</text>')
    s.append(f'<text x="{L+pw/2:.0f}" y="{H-8}" text-anchor="middle">consumer '
             f'processing per message</text>')

    # model curve (DASHED — never looks like data)
    pts = " ".join(f"{X(w):.1f},{Y(d):.1f}" for w, d in curve)
    s.append(f'<polyline points="{pts}" fill="none" stroke="#1f6feb" '
             f'stroke-width="2" stroke-dasharray="6,4"/>')
    # measured points + connecting line (solid, markers)
    mp = " ".join(f"{X(w):.1f},{Y(d):.1f}" for w, d in measured)
    s.append(f'<polyline points="{mp}" fill="none" stroke="#2ea043" '
             f'stroke-width="1.5"/>')
    for w, d in measured:
        s.append(f'<circle cx="{X(w):.1f}" cy="{Y(d):.1f}" r="3.5" '
                 f'fill="#2ea043"/>')

    # legend
    cval = meta.get("C")
    lx, ly = W - R - 210, T + 6
    s.append(f'<rect x="{lx}" y="{ly}" width="200" height="58" fill="white" '
             f'stroke="#ccc"/>')
    s.append(f'<line x1="{lx+8}" y1="{ly+16}" x2="{lx+34}" y2="{ly+16}" '
             f'stroke="#1f6feb" stroke-width="2" stroke-dasharray="6,4"/>')
    cstr = f"{cval:.2f}" if isinstance(cval, float) else "?"
    s.append(f'<text x="{lx+40}" y="{ly+20}">model (static, C={cstr})</text>')
    s.append(f'<line x1="{lx+8}" y1="{ly+34}" x2="{lx+34}" y2="{ly+34}" '
             f'stroke="#2ea043" stroke-width="1.5"/>')
    s.append(f'<circle cx="{lx+21}" cy="{ly+34}" r="3.5" fill="#2ea043"/>')
    s.append(f'<text x="{lx+40}" y="{ly+38}">measured (runtime)</text>')
    s.append(f'<text x="{lx+8}" y="{ly+52}" fill="#888" font-size="10">'
             f'blue band = W*; green band = measured</text>')

    if stamp:
        s.append(f'<text x="{L}" y="{T-14}" fill="#888" font-size="10">'
                 f'{stamp}  (model is an UPPER-BOUND prediction; ports+dispatch '
                 f'only)</text>')
    s.append("</svg>")
    return "\n".join(s)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--measured", required=True)
    ap.add_argument("--model", required=True)
    ap.add_argument("--out")
    ap.add_argument("--stamp", default="")
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--tol", type=float, default=130.0,
                    help="absolute floor for the delta residual tolerance, ns")
    ap.add_argument("--rel-tol", type=float, default=0.15,
                    help="relative residual tolerance (fraction of |measured|)")
    ap.add_argument("--force", action="store_true",
                    help="proceed even if machine fingerprints mismatch")
    ap.add_argument("--self-overlay-json",
                    help="sibling_analyze --json output from a SELF-overlay "
                    "(--producer NAME --consumer NAME, same region both "
                    "sides) of the port-bound region. When given, --check "
                    "ALSO asserts its calibrated 'c' against "
                    "--measured-multiplier — this is what validates the mca "
                    "engine itself, not just the closed-form Δh/ε constants.")
    ap.add_argument("--measured-multiplier", type=float,
                    default=DEFAULT_MEASURED_MULTIPLIER,
                    help="measured busy-sibling multiplier (sibling_noise "
                    "hot/idle ratio) to compare --self-overlay-json's "
                    "calibrated C against")
    ap.add_argument("--c-rel-tol", type=float, default=DEFAULT_C_REL_TOL,
                    help="relative tolerance for the self-overlay C "
                    "assertion — generous by default because the self-"
                    "overlaid region need not be the exact kernel shape "
                    "--calibrate was run against")
    a = ap.parse_args()

    measured, m_machine = read_measured(a.measured)
    curve, meta = read_model(a.model)
    if len(measured) < 2 or len(curve) < 2:
        print("need >=2 measured and >=2 model points", file=sys.stderr)
        return 2

    if a.out:
        with open(a.out, "w") as f:
            f.write(render_svg(measured, curve, meta, a.stamp))
        print(f"wrote {a.out}")

    if a.check:
        self_overlay_c = (read_self_overlay_c(a.self_overlay_json)
                          if a.self_overlay_json else None)
        ok, lines = run_check(measured, curve, m_machine, meta, a.tol,
                              a.rel_tol, a.force, self_overlay_c,
                              a.measured_multiplier, a.c_rel_tol)
        print("\n".join(lines))
        print("CHECK:", "PASS" if ok else "FAIL")
        return 0 if ok else 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
