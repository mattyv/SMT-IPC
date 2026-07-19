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
import math
import sys


def read_measured(path):
    """CSV: work_ns,sibling_p50_ns,sameccx_p50_ns -> [(work_ns, delta_ns)]."""
    pts = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#") or line.startswith("work_ns"):
                continue
            work, sib, ccx = (float(x) for x in line.split(",")[:3])
            pts.append((work, sib - ccx))
    pts.sort()
    return pts


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
    """First adjacent pair whose delta crosses zero (neg->pos). Returns
    (lo_w, hi_w) or None. This is the honest 'measured crossover interval'."""
    for (w0, d0), (w1, d1) in zip(pts, pts[1:]):
        if d0 <= 0 <= d1 and d0 != d1:
            return (w0, w1)
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


def run_check(measured, curve, meta, tol_ns):
    """Numeric agreement check. Returns (ok, lines)."""
    lines = []
    ok = True

    lo, hi = meta.get("wstar_lo"), meta.get("wstar_hi")
    bracket = sign_change_bracket(measured)
    if bracket is None:
        lines.append("measured: no sign change found — cannot bracket crossover")
        ok = False
    elif not (isinstance(lo, float) and isinstance(hi, float)):
        lines.append("model: no finite W* band — nothing to compare")
        ok = False
    else:
        # Bands overlap iff each starts before the other ends.
        overlap = (lo <= bracket[1]) and (bracket[0] <= hi)
        lines.append(
            f"W* band [{lo:.0f},{hi:.0f}] ns vs measured bracket "
            f"[{bracket[0]:.0f},{bracket[1]:.0f}] ns: "
            f"{'OVERLAP ok' if overlap else 'DISJOINT — FAIL'}"
        )
        ok = ok and overlap

    worst = 0.0
    for w, dm in measured:
        dpred = interp_delta(curve, w)
        r = abs(dpred - dm)
        worst = max(worst, r)
        lines.append(
            f"  work={w:8.0f} ns  measured Δ={dm:7.1f}  model Δ={dpred:7.1f}  "
            f"|resid|={r:6.1f} ns"
        )
    lines.append(f"max residual {worst:.1f} ns (tol {tol_ns:.0f} ns): "
                 f"{'ok' if worst <= tol_ns else 'FAIL'}")
    ok = ok and (worst <= tol_ns)
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
    ap.add_argument("--tol", type=float, default=80.0,
                    help="max |model-measured| delta residual, ns")
    a = ap.parse_args()

    measured = read_measured(a.measured)
    curve, meta = read_model(a.model)
    if len(measured) < 2 or len(curve) < 2:
        print("need >=2 measured and >=2 model points", file=sys.stderr)
        return 2

    if a.out:
        with open(a.out, "w") as f:
            f.write(render_svg(measured, curve, meta, a.stamp))
        print(f"wrote {a.out}")

    if a.check:
        ok, lines = run_check(measured, curve, meta, a.tol)
        print("\n".join(lines))
        print("CHECK:", "PASS" if ok else "FAIL")
        return 0 if ok else 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
