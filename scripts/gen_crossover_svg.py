#!/usr/bin/env python3
"""Regenerate docs/crossover.svg — the three-regime placement×work figure (README Step 4).

Inputs, all committed so the headline image is auditable and regenerable:
  * docs/crossover_model.csv   — sibling_analyze's static prediction (dashed line).
  * the three measured series below, inline — each is the MEDIAN of 5
    `spsc_pipeline` runs on this repo's Zen 5 box (delta = sibling_p50 - sameccx_p50):
      polite  : ./spsc_pipeline --proc-sweep
      paced   : ./spsc_pipeline --both-busy           (gap widened to fit both -> disjoint windows)
      overlap : ./spsc_pipeline --both-busy-overlap   (consumer-only gap -> genuine overlap)
    x = calibrated consumer work ns (rounds 0/128/512/2048/8192). The overlap run's
    6.9µs rung SATURATES (queue-drift dominated, tens-hundreds of µs, flagged INVALID) and
    is dropped with an on-figure callout — dropping it is conservative (a valid rung with
    more pacing headroom would show the sibling ~+3.7µs WORSE, not better).

Run from the repo root:  python3 scripts/gen_crossover_svg.py
p50s wander ~10ns run-to-run (no core isolation); re-measure and update the medians
on your own box rather than trusting these absolute numbers.
"""
import csv
import math
import os

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MODEL_CSV = os.path.join(REPO, "docs", "crossover_model.csv")
OUT_SVG = os.path.join(REPO, "docs", "crossover.svg")

# --- measured, median of 5 runs each (see module docstring) ---
POLITE_SERIES = [(50, -50), (130, -50), (451, -50), (1743, -30), (6933, 50)]
PACED_SERIES = [(50, -50), (130, -40), (451, -40), (1743, 0), (6933, 170)]
OVERLAP_SERIES = [(50, -50), (130, -50), (451, 10), (1743, 781)]  # 6933 rung dropped: saturates/INVALID

# --- model W* band (from crossover_model.csv header) ---
WSTAR_LO, WSTAR_HI = 1100, 3315
OVERLAP_SAT_NOTE = "saturates (INVALID) by 7µs"

# theme-safe palette (legible on GitHub light #fff and dark #0d1117)
MUTED = "#8b949e"
GRID = "#8b949e"
MODEL = "#8b7ff0"       # prediction (dashed)
POLITE = "#14b8a6"      # polite producer
PACED = "#e0912f"       # both busy, paced apart (disjoint windows)
OVERLAP = "#e5484d"     # both busy, overlapping (danger)
BAND = "#8b7ff0"

# canvas + axes
W, H = 1020, 420
MARGIN_TOP, MARGIN_RIGHT, MARGIN_BOTTOM, MARGIN_LEFT = 34, 300, 54, 58
IW, IH = W - MARGIN_LEFT - MARGIN_RIGHT, H - MARGIN_TOP - MARGIN_BOTTOM
XMIN, XMAX = math.log10(35), math.log10(9000)          # log work-ns axis
YMIN, YMAX = -70, 190                                    # ns; paced +170 in-scale, overlap +781 clips
Y_GRID = (-50, 0, 50, 100, 150)
X_TICKS = (50, 100, 200, 500, 1000, 2000, 5000)
CLIP_ANNOT_AT = 1743   # x where the overlap "off-scale" callout is anchored


def load_model(path):
    pts = []
    with open(path) as f:
        for row in csv.reader(f):
            if not row or row[0].startswith("#") or row[0] == "w_ns":
                continue
            pts.append((float(row[0]), float(row[1])))
    return pts


def X(w):
    return MARGIN_LEFT + (math.log10(w) - XMIN) / (XMAX - XMIN) * IW


def Yv(d):
    return MARGIN_TOP + (YMAX - d) / (YMAX - YMIN) * IH


def clip(v, a, b):
    return max(a, min(b, v))


def build_svg(model):
    s = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W} {H}" '
         f'font-family="ui-monospace,SFMono-Regular,Menlo,monospace" role="img" '
         f'aria-label="Sibling minus same-CCX placement latency vs consumer work, for a polite '
         f'producer, a paced both-busy producer, and an overlapping both-busy producer; the '
         f'overlapping case makes the sibling lose far earlier.">']

    top, bot = MARGIN_TOP, MARGIN_TOP + IH
    y0 = Yv(0)
    s.append(f'<rect x="{MARGIN_LEFT}" y="{top:.1f}" width="{IW}" height="{y0-top:.1f}" fill="{OVERLAP}" opacity="0.05"/>')
    s.append(f'<rect x="{MARGIN_LEFT}" y="{y0:.1f}" width="{IW}" height="{bot-y0:.1f}" fill="{POLITE}" opacity="0.06"/>')
    xlo, xhi = X(WSTAR_LO), X(WSTAR_HI)
    s.append(f'<rect x="{xlo:.1f}" y="{top}" width="{xhi-xlo:.1f}" height="{IH}" fill="{BAND}" opacity="0.13"/>')
    s.append(f'<text x="{(xlo+xhi)/2:.1f}" y="{top-8}" text-anchor="middle" font-size="10.5" font-weight="700" fill="{MODEL}">model W* band</text>')

    for v in Y_GRID:
        y = Yv(v)
        zero = (v == 0)
        dash = "" if zero else 'stroke-dasharray="2 4" opacity="0.35"'
        s.append(f'<line x1="{MARGIN_LEFT}" y1="{y:.1f}" x2="{MARGIN_LEFT+IW}" y2="{y:.1f}" stroke="{GRID}" stroke-width="{1.4 if zero else 1}" {dash}/>')
        s.append(f'<text x="{MARGIN_LEFT-9}" y="{y+3.5:.1f}" text-anchor="end" font-size="10.5" fill="{MUTED}">{"+" if v>0 else ""}{v}</text>')
    for w in X_TICKS:
        x = X(w)
        s.append(f'<line x1="{x:.1f}" y1="{top}" x2="{x:.1f}" y2="{bot}" stroke="{GRID}" stroke-width="1" stroke-dasharray="2 4" opacity="0.35"/>')
        s.append(f'<text x="{x:.1f}" y="{bot+17}" text-anchor="middle" font-size="10.5" fill="{MUTED}">{str(w//1000)+"µs" if w>=1000 else str(w)+"ns"}</text>')
    s.append(f'<text x="{MARGIN_LEFT+IW/2:.1f}" y="{H-6}" text-anchor="middle" font-size="11" fill="{MUTED}">consumer work per message</text>')
    s.append(f'<text transform="translate(14,{top+IH/2:.1f}) rotate(-90)" text-anchor="middle" font-size="11" fill="{MUTED}">sibling &#8722; same-CCX (ns)</text>')
    s.append(f'<text x="{MARGIN_LEFT+6}" y="{bot-7}" font-size="10.5" font-weight="700" fill="{POLITE}">sibling faster &#8595;</text>')
    s.append(f'<text x="{MARGIN_LEFT+6}" y="{top+13}" font-size="10.5" font-weight="700" fill="{OVERLAP}">same-CCX faster &#8593;</text>')

    pts = [f'{X(w):.1f} {clip(Yv(d),top,bot):.1f}' for w, d in model if 35 <= w <= 9000]
    s.append(f'<polyline points="{" ".join(pts)}" fill="none" stroke="{MODEL}" stroke-width="2" stroke-dasharray="6 4" opacity="0.9"/>')

    def series(data, color):
        path = " ".join(f'{"M" if i==0 else "L"}{X(w):.1f} {clip(Yv(d),top,bot):.1f}' for i, (w, d) in enumerate(data))
        s.append(f'<path d="{path}" fill="none" stroke="{color}" stroke-width="2.2"/>')
        for w, d in data:
            s.append(f'<circle cx="{X(w):.1f}" cy="{clip(Yv(d),top,bot):.1f}" r="4.3" fill="{color}" stroke="#0d1117" stroke-width="0.6"/>')

    series(POLITE_SERIES, POLITE)
    series(PACED_SERIES, PACED)
    series(OVERLAP_SERIES, OVERLAP)

    ox, oy = X(CLIP_ANNOT_AT), Yv(YMAX)
    s.append(f'<text x="{ox+6:.1f}" y="{oy+22:.1f}" font-size="10" font-weight="700" fill="{OVERLAP}">&#8593; +781 ns, {OVERLAP_SAT_NOTE}</text>')

    lx, ly = MARGIN_LEFT + IW + 14, top + 8
    legend = [("prediction (llvm-mca)", MODEL, True),
              ("measured: polite producer", POLITE, False),
              ("measured: both busy, paced apart", PACED, False),
              ("measured: both busy, OVERLAPPING", OVERLAP, False)]
    for i, (lbl, col, dash) in enumerate(legend):
        yy = ly + i * 21
        if dash:
            s.append(f'<line x1="{lx}" y1="{yy}" x2="{lx+20}" y2="{yy}" stroke="{col}" stroke-width="2.2" stroke-dasharray="5 3"/>')
        else:
            s.append(f'<line x1="{lx}" y1="{yy}" x2="{lx+20}" y2="{yy}" stroke="{col}" stroke-width="2.2"/><circle cx="{lx+10}" cy="{yy}" r="4" fill="{col}"/>')
        s.append(f'<text x="{lx+26}" y="{yy+3.5}" font-size="10.5" fill="{MUTED}">{lbl}</text>')
    cy0 = ly + 4 * 21 + 12
    s.append(f'<text x="{lx}" y="{cy0}" font-size="10" fill="{MUTED}">crossover (sibling stops winning):</text>')
    s.append(f'<text x="{lx}" y="{cy0+16}" font-size="10" fill="{POLITE}">polite / paced: ~2&#8211;3.7µs</text>')
    s.append(f'<text x="{lx}" y="{cy0+31}" font-size="10" fill="{OVERLAP}">overlapping: below ~0.5µs (then off scale)</text>')
    s.append(f'<text x="{lx}" y="{cy0+46}" font-size="10" fill="{MODEL}">model W*: ~1.7µs</text>')

    s.append('</svg>')
    return "\n".join(s) + "\n"


if __name__ == "__main__":
    open(OUT_SVG, "w").write(build_svg(load_model(MODEL_CSV)))
    print(f"wrote {OUT_SVG}")
