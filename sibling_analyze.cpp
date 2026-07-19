// ============================================================================
// sibling_analyze.cpp — a STATIC placement linter for two IPC threads.
// ----------------------------------------------------------------------------
// The other three tools in this repo MEASURE (they spin threads and time them).
// This one ANALYSES: you mark the hot loops of your real producer and consumer
// (see sibling_marks.hpp), point this tool at the source, and it predicts —
// without running anything — whether those two specific code regions will
// contend for a physical core's execution resources when placed on the two SMT
// siblings of that core, and roughly how much per-message work the consumer can
// carry before you should step out to a separate same-CCX core instead.
//
// It is a SCREENING LINTER, not an oracle. Read this before trusting a verdict:
//
//   * It sees EXECUTION PORTS + FRONT-END DISPATCH, and nothing else. Its
//     engine is llvm-mca, which models an idealised core: perfect caches,
//     infinite load/store buffers and miss-handling registers, a single
//     instruction stream. It cannot see cache-bandwidth contention, MSHR or
//     store-buffer pressure, L1 aliasing between siblings, or branch-predictor
//     pollution.
//   * Every one of those blind spots makes it UNDER-predict friction. So a
//     "COLLIDES" verdict is trustworthy (it found a real port/dispatch clash);
//     a "no port collision" verdict only means the compute ports are clear —
//     the memory system and front end are unchecked. The tool says so, loudly,
//     and never prints a bare "safe".
//   * The budget W* is an UPPER BOUND on the true sibling budget, for the same
//     reason. The tool errs toward recommending the sibling; the runtime
//     sibling_noise / spsc_pipeline remain ground truth for any load-bearing
//     "safe" call, especially for memory-flavoured regions.
//
// THE MODEL (all of it, so nothing is folklore):
//   Per region, llvm-mca gives the per-iteration pressure on each processor
//   resource r and the block's cycles-per-iteration. Utilisation u_s(r) =
//   pressure_s(r) / cycles_s is the fraction of cycles resource r is busy for
//   stream s running ALONE; the stream's bottleneck resource sits at u≈1.
//   Two siblings sharing the core place combined demand u_p(r)+u_c(r) on each
//   shared resource. If that exceeds 1, the resource cannot serve both at full
//   rate and (proportional-share) throughput scales down. The consumer's work
//   is therefore inflated by
//       C_raw = max over r of ( u_producer(r) + u_consumer(r) ),   floored at 1
//   where r ranges over every execution port AND a synthetic DISPATCH resource
//   (u_dispatch = uOpsPerCycle / DispatchWidth) — the dispatch row is what
//   catches two port-DISJOINT streams that still collide at shared rename/
//   retire bandwidth, the failure the pure port view misses.
//   A calibration scale (see --calibrate) maps C_raw to a measured multiplier:
//       C = 1 + scale * (C_raw - 1).
//
//   The placement budget then follows the crossover the README's graph shows.
//   End-to-end per message: sibling L_sib = h_sib + W * C_eff(W);
//   same-CCX L_ccx = h_ccx + W. Sibling wins while L_sib < L_ccx, i.e. while
//       W * ( eps + duty(W)*(C-1) )  <  Dh ,   Dh = h_ccx - h_sib.
//   eps is the presence tax (a merely-awake sibling still costs a few %,
//   measured by sibling_noise's pause-tenant row). duty(W) is the fraction of
//   the consumer's window during which the producer is actually executing;
//   under matched pacing the message period grows with W, so duty ∝ 1/W — it is
//   NOT a free constant, and the tool solves it as a fixed point from the
//   producer's own mca cycles-per-message. W* is the W where equality holds.
//   Note the awkward, honest consequence: with eps>0 a crossover always exists;
//   with eps≈0 the duty term saturates to a constant and the sibling may win at
//   every W — the tool reports that as "no crossover; sibling wins to <range>".
//
// The machine constants (h_sib, h_ccx, eps, pacing headroom, core GHz) are NOT
// hardcoded to one box — they come from a --profile file you generate from
// smt_pingpong / sibling_noise on YOUR target. example.profile ships the
// README's Zen 5 numbers, clearly labelled as an example to replace.
//
// build: g++ -O3 -std=c++23 sibling_analyze.cpp -o sibling_analyze
// run:   ./sibling_analyze mythreads.cpp                (auto
// producer/consumer)
//        ./sibling_analyze mythreads.cpp --profile my.profile
//        ./sibling_analyze --mca-file dump.txt          (skip compile; parse a
//                                                         saved llvm-mca dump)
//        ./sibling_analyze --calibrate                  (reproduce the measured
//                                                         busy-sibling
//                                                         multiplier through
//                                                         the static path)
//        ./sibling_analyze --test                       (pure-logic self-check,
//                                                         no compiler/mca
//                                                         needed)
// x86-64 only in spirit (SMT port contention); the parser/model are portable.
// ============================================================================

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <sys/wait.h> // WIFEXITED / WEXITSTATUS for pclose status decoding
#include <unistd.h>   // getpid, mkdtemp
#include <vector>

// ---------------------------------------------------------------------------
// Model / machine constants. Every literal that shapes a verdict is named here
// with a one-line reason, same discipline as pp_core.hpp's Cfg.
// ---------------------------------------------------------------------------
struct Profile {
  // Core clock during the hot loop, GHz. Converts mca CYCLES to ns. This is the
  // running core frequency, not the invariant TSC rate — set it from the boost
  // clock your hot core actually holds. Default is a placeholder.
  double freq_ghz = 3.0;
  // Handoff round-trip on an SMT sibling and on a same-CCX core, ns. From
  // smt_pingpong (its "SMT sibling" and "same L3 / CCX" median RTT rows). The
  // budget uses the DIFFERENCE; see Dh() below.
  double handoff_sibling_ns = 50.0;
  double handoff_sameccx_ns = 90.0;
  // Presence tax: fractional slowdown from a merely-awake (politely pausing)
  // sibling, independent of port contention — statically-partitioned back-end
  // structures. From sibling_noise's noop-tenant row (~+3%).
  double presence_tax = 0.03;
  // Matched-pacing headroom: message period = headroom * (consumer_work +
  // allowance). From spsc_pipeline's PROC_SWEEP_HEADROOM. Governs duty(W).
  double pacing_headroom = 1.5;
  // Fixed per-message overhead outside the swept work (pop + detect), ns. From
  // spsc_pipeline's PROC_BASE-style floor. Keeps duty(W) finite as W->0.
  double allowance_ns = 40.0;
  // Calibration scale mapping mca's C_raw-1 to a measured multiplier-1. 1.0
  // means "trust mca as-is"; --calibrate refines it against a measured busy-
  // sibling number for a known pair.
  double calib_scale = 1.0;
  // The -mcpu model llvm-mca should use. "native" asks LLVM to detect the host,
  // but LLVM < 19 has no znver5 model, so on a Zen 5 box native silently falls
  // back to a generic/znver4 model; set this to a known-good model explicitly
  // (e.g. "znver4") when native mis-resolves. Printed on every run.
  std::string mca_mcpu = "native";
  // Machine fingerprint (free-form, e.g. "zen5"): stamped into --emit-model
  // output and matched against the measured CSV so an overlay never silently
  // mixes two microarchitectures.
  std::string machine = "";

  double Dh() const { return handoff_sameccx_ns - handoff_sibling_ns; }
};

// Validate machine constants; returns an error string (empty == ok). A bad
// profile must fail loudly, not silently propagate inf/NaN/"W*=1ns" nonsense.
static std::string validate_profile(const Profile &p) {
  if (!(p.freq_ghz > 0))
    return "freq_ghz must be > 0";
  if (!(p.pacing_headroom > 0))
    return "pacing_headroom must be > 0";
  if (p.allowance_ns < 0)
    return "allowance_ns must be >= 0";
  if (p.presence_tax < 0)
    return "presence_tax must be >= 0";
  if (p.Dh() < 0)
    return "handoff_sameccx_ns < handoff_sibling_ns (Dh negative) — the "
           "sibling "
           "must have the SMALLER handoff; check the two values are not "
           "swapped";
  if (!(p.calib_scale > 0))
    return "calib_scale must be > 0";
  return "";
}

// One llvm-mca "Code Region": the fields we need from its summary plus the
// per-iteration pressure on every named processor resource.
struct RegionProfile {
  std::string name;           // region name, or "" for an unnamed region
  double cycles_per_iter = 0; // Block RThroughput — a THROUGHPUT LOWER BOUND
                              // only; ignores dependency-chain latency. Kept
                              // for display and as the defensive fallback in
                              // effective_cycles_per_iter() below.
  double total_cycles = 0;    // Total Cycles — the block's ACTUAL cycle count
                              // (accounts for latency chains); total_cycles /
                              // iterations is the real per-iteration cycle
                              // cost and is what utilisation()/
                              // producer_busy_ns() must divide by.
  double dispatch_width = 0;  // Dispatch Width
  double uops_per_cycle = 0;  // uOps Per Cycle
  long instructions = 0;      // total (all iterations)
  long iterations = 0;
  std::map<std::string, double>
      pressure; // resource name -> per-iteration cycles (summed over sub-units)
  std::map<std::string, int>
      units; // resource name -> number of parallel sub-units (its CAPACITY).
             // llvm-mca models a ProcResGroup (e.g. AMD Zn4LSU, Zn4FP45) as
             // several sub-indices [14.0]/[14.1]/[14.2] that all share ONE
             // name; that count is the resource's throughput, so per-unit
             // utilisation is pressure / (cycles * units), NOT pressure /
             // cycles. A plain single-ported resource has units == 1.
};

// Result of overlaying two regions.
struct Overlay {
  double c_raw = 1.0;           // max combined demand, before calibration
  double c = 1.0;               // 1 + scale*(c_raw-1)
  std::string bottleneck;       // resource where the max occurred
  double bottleneck_demand = 0; // combined u at the bottleneck
  bool collides = false;        // c_raw meaningfully over 1
  std::vector<std::pair<std::string, double>>
      top; // top few resources by demand
};

// ===========================================================================
// SECTION 1 — llvm-mca TEXT parser (pure).
// Parses the default `llvm-mca` textual report. The fields we read — "Code
// Region", the summary block ("Total Cycles", "Block RThroughput", "Dispatch
// Width", "uOps Per Cycle"), the "Resources:" index->name table, and
// "Resource pressure per iteration:" — have been validated against LLVM
// 18, 19, and 20's output and are stable across that range. NOTHING pins the
// llvm-mca version in CMake or CI, though: this parser does not detect a
// format drift on its own. The actual safety net is validate_region() at
// call time, which fails LOUDLY (a named error, not a silent misparse) if a
// future format leaves cycles_per_iter <= 0 or the pressure table empty — see
// its comment for exactly which fields it checks.
// ===========================================================================

// Trim ASCII whitespace from both ends.
static std::string trim(const std::string &s) {
  size_t a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos)
    return "";
  size_t b = s.find_last_not_of(" \t\r\n");
  return s.substr(a, b - a + 1);
}

// Whitespace-split into tokens.
static std::vector<std::string> tokens(const std::string &s) {
  std::vector<std::string> out;
  std::istringstream is(s);
  std::string t;
  while (is >> t)
    out.push_back(t);
  return out;
}

// Read a "Label: value" scalar from a line if it starts with `label`.
static bool scalar(const std::string &line, const char *label, double &out) {
  size_t n = std::strlen(label);
  if (line.compare(0, n, label) == 0) {
    out = std::atof(trim(line.substr(n + 1)).c_str()); // +1 skips the ':'
    return true;
  }
  return false;
}

// Parse a full llvm-mca text report into its code regions.
static std::vector<RegionProfile> parse_mca_text(const std::string &text) {
  std::vector<RegionProfile> regions;
  std::istringstream is(text);
  std::vector<std::string> lines;
  for (std::string l; std::getline(is, l);)
    lines.push_back(l);

  // A region begins at a line like "[0] Code Region - name" (name optional).
  static const std::regex region_hdr(
      R"(^\[\d+\]\s+Code Region(\s+-\s+(.*))?$)");
  // A resource declaration line: "[2]   - SKXPort0".
  static const std::regex res_decl(R"(^\[(\d+(?:\.\d+)?)\]\s+-\s+(\S+)\s*$)");

  int i = 0;
  const int N = (int)lines.size();
  while (i < N) {
    std::smatch m;
    if (!std::regex_match(lines[i], m, region_hdr)) {
      i++;
      continue;
    }
    RegionProfile r;
    r.name = m[2].matched ? trim(m[2].str()) : "";
    i++;

    std::map<std::string, std::string> idx2name; // resource index -> name
    // Walk until the next region header or EOF, filling fields as we see them.
    while (i < N && !std::regex_match(lines[i], region_hdr)) {
      const std::string &line = lines[i];
      double v;
      if (scalar(line, "Iterations:", v))
        r.iterations = (long)v;
      else if (scalar(line, "Instructions:", v))
        r.instructions = (long)v;
      else if (scalar(line, "Dispatch Width:", v))
        r.dispatch_width = v;
      else if (scalar(line, "uOps Per Cycle:", v))
        r.uops_per_cycle = v;
      else if (scalar(line, "Block RThroughput:", v))
        r.cycles_per_iter = v;
      else if (scalar(line, "Total Cycles:", v))
        r.total_cycles = v;
      else if (std::smatch rm; std::regex_match(line, rm, res_decl)) {
        idx2name[rm[1].str()] = rm[2].str();
        // Each sub-index declaration of a name adds one parallel unit of
        // capacity. "[3] - Zn4ALU0" => units 1; "[14.0/.1/.2] - Zn4LSU" => 3.
        r.units[rm[2].str()]++;
      } else if (line.find("Resource pressure per iteration:") !=
                 std::string::npos) {
        // Header line of column indices, then a values line, positionally
        // aligned. Zip them; skip '-' (zero pressure).
        if (i + 2 < N) {
          auto hdr = tokens(lines[i + 1]);
          auto val = tokens(lines[i + 2]);
          for (size_t k = 0; k < hdr.size() && k < val.size(); k++) {
            std::string idx = hdr[k];
            if (idx.size() >= 2 && idx.front() == '[' && idx.back() == ']')
              idx = idx.substr(1, idx.size() - 2);
            if (val[k] == "-")
              continue;
            auto it = idx2name.find(idx);
            if (it != idx2name.end())
              r.pressure[it->second] += std::atof(val[k].c_str());
          }
        }
        i += 2;
      }
      i++;
    }
    // Derive cycles_per_iter if mca omitted it (defensive): fall back to
    // TotalCycles/Iterations is unavailable here, so leave as parsed.
    regions.push_back(std::move(r));
  }
  return regions;
}

// ===========================================================================
// SECTION 2 — the overlay model (pure).
// ===========================================================================

// Real per-iteration cycle cost. llvm-mca's "Total Cycles" is the block's
// ACTUAL cycle count (it accounts for dependency-chain latency); "Block
// RThroughput" is a pure THROUGHPUT LOWER BOUND that ignores latency chains
// entirely. Dividing utilisation/duty math by RThroughput on a latency-bound
// region divides by a denominator far smaller than reality: utilisation is
// over-predicted (it can even exceed 1.0 for a SINGLE region alone, which is
// physically impossible — a region cannot be more than 100% busy on its own
// port). Prefer Total Cycles/Iterations; fall back to Block RThroughput only
// when Total Cycles is absent/degenerate (e.g. a hand-edited --mca-file dump
// from a format that omits it) — a defensive path, not the common case.
static double effective_cycles_per_iter(const RegionProfile &r) {
  if (r.total_cycles > 0 && r.iterations > 0)
    return r.total_cycles / (double)r.iterations;
  return r.cycles_per_iter > 0 ? r.cycles_per_iter : 1.0;
}

// Per-resource utilisation for one region: pressure/cycles, plus the synthetic
// "dispatch" resource = uOpsPerCycle/DispatchWidth (front-end/retire width).
static std::map<std::string, double> utilisation(const RegionProfile &r) {
  std::map<std::string, double> u;
  double cyc = effective_cycles_per_iter(r);
  for (auto &[name, p] : r.pressure) {
    // Divide the summed pressure by the resource's CAPACITY (cycles * units):
    // a 3-unit group at 3.0 cycles of pressure over a 1-cycle block is 100%
    // busy per unit (u = 3/(1*3) = 1.0), NOT 300% (u = 3/1). Without the unit
    // count this over-predicts grouped resources (AMD's load/store/FP groups) —
    // which would flip the tool's safety asymmetry into false COLLIDES verdicts
    // on exactly the platform the repo targets.
    int units = r.units.count(name) ? r.units.at(name) : 1;
    if (units < 1)
      units = 1;
    u[name] = p / (cyc * units);
  }
  if (r.dispatch_width > 0)
    u["dispatch(front-end)"] = r.uops_per_cycle / r.dispatch_width;
  return u;
}

// Overlay two regions: combined per-resource demand, max is the friction.
// `collide_margin` is how far over 1.0 the combined demand must be before we
// call it a collision (mca fractions carry noise; a hair over 1 is not a
// clash).
static Overlay overlay(const RegionProfile &producer,
                       const RegionProfile &consumer, const Profile &prof,
                       double collide_margin = 1.05) {
  auto up = utilisation(producer);
  auto uc = utilisation(consumer);
  std::map<std::string, double> demand;
  for (auto &[k, v] : up)
    demand[k] += v;
  for (auto &[k, v] : uc)
    demand[k] += v;

  Overlay o;
  for (auto &[k, d] : demand) {
    o.top.push_back({k, d});
    if (d > o.bottleneck_demand) {
      o.bottleneck_demand = d;
      o.bottleneck = k;
    }
  }
  std::sort(o.top.begin(), o.top.end(),
            [](auto &a, auto &b) { return a.second > b.second; });
  if (o.top.size() > 5)
    o.top.resize(5);

  o.c_raw = std::max(1.0, o.bottleneck_demand);
  o.c = 1.0 + prof.calib_scale * (o.c_raw - 1.0);
  o.collides = o.bottleneck_demand > collide_margin;
  return o;
}

// ===========================================================================
// SECTION 3 — duty(W) and the W* crossover (pure).
// ===========================================================================

// Producer busy-time per MESSAGE, ns: cycles-per-block * blocks-per-message,
// converted to ns. blocks_per_msg comes from --producer-iters-per-msg (default
// 1): one marked block need not be one message (mca's "iteration" is one repeat
// of the marked asm block, which may be a single loop iteration).
static double producer_busy_ns(const RegionProfile &producer,
                               const Profile &prof, double blocks_per_msg) {
  return effective_cycles_per_iter(producer) * blocks_per_msg / prof.freq_ghz;
}

// duty(W): fraction of the consumer's window during which the producer is hot.
// The message period is headroom*(contended consumer work + allowance) and
// duty = producer_busy / period, where the contended work depends on duty
// through c_eff = 1 + eps + duty*(C-1). Substituting gives a quadratic in duty:
//   a*duty^2 + b*duty - t = 0,  a = H*W*(C-1), b = H*(W*(1+eps)+A), t = t_prod.
// We take the positive root (exact — no iteration, no convergence caveat) and
// clamp to [0,1] (the producer cannot be busy more than all the time).
static double duty_of(double W_ns, const Profile &prof, double C,
                      double t_prod_ns) {
  double H = prof.pacing_headroom, A = prof.allowance_ns,
         eps = prof.presence_tax;
  double a = H * W_ns * (C - 1.0);
  double b = H * (W_ns * (1.0 + eps) + A);
  double duty;
  if (a <= 0.0) // C == 1 (or degenerate W): linear, duty = t / b
    duty = b > 0 ? t_prod_ns / b : 0.0;
  else
    duty = (-b + std::sqrt(b * b + 4.0 * a * t_prod_ns)) / (2.0 * a);
  if (duty < 0.0)
    duty = 0.0;
  if (duty > 1.0)
    duty = 1.0;
  return duty;
}

// g(W) = W*(eps + duty(W)*(C-1)) - Dh. Sibling wins where g<0; W* is g==0.
static double g_of(double W_ns, const Profile &prof, double C,
                   double t_prod_ns) {
  double duty = duty_of(W_ns, prof, C, t_prod_ns);
  return W_ns * (prof.presence_tax + duty * (C - 1.0)) - prof.Dh();
}

// Solve for W* by bisection over [lo, hi] ns. Returns nullopt if the sibling
// wins across the whole range (g(hi) < 0): with a small presence tax and a
// duty term that saturates, there may be no crossover at all.
static std::optional<double> solve_wstar(const Profile &prof, double C,
                                         double t_prod_ns, double lo = 1.0,
                                         double hi = 1.0e6) {
  double glo = g_of(lo, prof, C, t_prod_ns);
  double ghi = g_of(hi, prof, C, t_prod_ns);
  if (glo > 0)
    return lo; // even trivial work already past budget (large eps/C)
  if (ghi < 0)
    return std::nullopt; // sibling wins across the whole range
  for (int it = 0; it < 100; it++) {
    double mid = 0.5 * (lo + hi);
    double gm = g_of(mid, prof, C, t_prod_ns);
    if (gm < 0)
      lo = mid;
    else
      hi = mid;
  }
  return 0.5 * (lo + hi);
}

// Band sensitivity factors for solve_budget()'s W* range. The band is a
// deliberately coarse eps/C sensitivity sweep, NOT a statistical confidence
// interval: since g(W) is dominated by the presence_tax (eps) term (see the
// module header — eps sets the crossover's existence, C only steepens it),
// the printed "W* range" is really an EPS-SENSITIVITY band with a secondary
// C wobble layered on. Named here (not inline) so the sensitivity magnitude
// is discoverable/adjustable in one place.
struct BudgetBandCfg {
  // Low-budget (sibling looks worse) scenario: presence tax scaled up...
  static constexpr double kLoPresenceTaxMul = 1.5;
  // ...and C_raw's excess-over-1 scaled up (stronger port contention).
  static constexpr double kLoCExcessMul = 1.3;
  // High-budget (sibling looks better) scenario: the mirror-image weaker case.
  static constexpr double kHiPresenceTaxMul = 0.5;
  static constexpr double kHiCExcessMul = 0.7;
};

// W* as a range: propagate the dominant input uncertainties (calib_scale on C,
// and the handoff/presence terms) into a low/high budget. Deliberately coarse —
// the point of the band is to show that the placement decision is robust across
// wide model error, per the README's crossover being decades-wide.
struct Budget {
  std::optional<double> mid, lo, hi;
  double C = 1.0;
};
static Budget solve_budget(const Profile &prof, const Overlay &o,
                           double t_prod_ns) {
  Budget b;
  b.C = o.c;
  b.mid = solve_wstar(prof, o.c, t_prod_ns);
  // Low budget (sibling looks worse): stronger contention, bigger presence tax.
  Profile pw = prof;
  pw.presence_tax = prof.presence_tax * BudgetBandCfg::kLoPresenceTaxMul;
  double c_hi = 1.0 + BudgetBandCfg::kLoCExcessMul * (o.c - 1.0);
  b.lo = solve_wstar(pw, c_hi, t_prod_ns);
  // High budget (sibling looks better): weaker contention, smaller tax.
  Profile pb = prof;
  pb.presence_tax = prof.presence_tax * BudgetBandCfg::kHiPresenceTaxMul;
  double c_lo = 1.0 + BudgetBandCfg::kHiCExcessMul * (o.c - 1.0);
  b.hi = solve_wstar(pb, c_lo, t_prod_ns);
  return b;
}

// ===========================================================================
// SECTION 4 — region asm lint (pure). Scans the instruction lines of one marked
// region for the shapes that make the mca vector a lie (see marking contract).
// ===========================================================================
struct Lint {
  std::vector<std::string> errors;   // fatal: vector is untrustworthy
  std::vector<std::string> warnings; // caveat: verdict weaker
  int compute = 0;                   // count of real port-bound instructions
};

// `region_asm` is the raw asm text between a region's BEGIN and END markers.
static Lint lint_region(const std::string &name,
                        const std::string &region_asm) {
  Lint L;
  std::istringstream is(region_asm);
  int calls = 0, branches = 0, atomics = 0, compute = 0;
  for (std::string line; std::getline(is, line);) {
    std::string t = trim(line);
    if (t.empty() || t[0] == '.' || t[0] == '#')
      continue; // directive / comment
    auto toks = tokens(t);
    if (toks.empty())
      continue;
    std::string op = toks[0];
    // A label may share a line with an instruction ("foo: nop"): drop a leading
    // "label:" token and re-read the mnemonic, rather than losing the whole
    // line. A bare label line ("foo:") then has no further token and is
    // skipped.
    if (!op.empty() && op.back() == ':') {
      if (toks.size() < 2)
        continue;
      op = toks[1];
    }
    // lock-prefixed atomic: "lock" then the real op.
    bool locked = (op == "lock");
    if (locked && toks.size() > 1)
      op = toks[1];
    if (op.rfind("call", 0) == 0)
      calls++;
    else if (op.rfind("jmp", 0) == 0)
      continue; // unconditional jump: not a mispredict/branch-model concern
    else if (op.size() >= 2 && op[0] == 'j')
      branches++; // conditional jump (jne/je/jl/...)
    else if (locked || op.rfind("xchg", 0) == 0 || op.rfind("mfence", 0) == 0 ||
             op.rfind("lfence", 0) == 0 || op.rfind("sfence", 0) == 0)
      atomics++;
    else
      compute++; // a real instruction that lands on some port
  }
  L.compute = compute;
  if (calls)
    L.errors.push_back(name + ": contains " + std::to_string(calls) +
                       " call(s) — the callee's demand is invisible to mca, so "
                       "this region's port vector is silently incomplete. "
                       "Inline the callee or shrink the region.");
  if (atomics)
    L.warnings.push_back(
        name + ": contains " + std::to_string(atomics) +
        " atomic/fence op(s) — that is handoff/coherence cost, which belongs "
        "to "
        "the Dh term (smt_pingpong), not to W. Move the marker off the queue "
        "push/pop or it double-counts.");
  if (branches)
    L.warnings.push_back(
        name + ": contains " + std::to_string(branches) +
        " conditional branch(es) — mca assumes the whole span executes every "
        "iteration and models no mispredict. A cold path inside the region "
        "inflates its demand; shrink the region to the true steady-state "
        "body.");
  if (compute == 0)
    L.errors.push_back(
        name +
        ": marked region contains ZERO compute instructions — the "
        "compiler hoisted the register-only work out across the marker (a "
        "\"memory\" clobber orders memory ops, not register dataflow). The "
        "region's port vector is empty and any verdict from it is a lie. Wrap "
        "a "
        "loop whose body touches memory, or restructure so the work stays "
        "put.");
  return L;
}

// ===========================================================================
// SECTION 5 — driver plumbing (impure: shells out to the compiler and mca).
// Kept below the pure core so --test never touches any of it.
// ===========================================================================

static std::string slurp_file(const std::string &path) {
  std::ifstream f(path);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

// Single-quote a token for safe shell interpolation: wrap in '...' and escape
// any embedded single quote as '\''. Defeats command injection and word-
// splitting from source paths / --cflags (an ordinary space in a path, or a
// malicious '; rm ...', both become one inert argument).
static std::string shell_quote(const std::string &s) {
  std::string out = "'";
  for (char c : s) {
    if (c == '\'')
      out += "'\\''";
    else
      out += c;
  }
  out += "'";
  return out;
}

// Run a command; capture stdout into `out` and stderr into `err` (kept
// SEPARATE, so a tool's warnings are surfaced to the user rather than fed into
// the parser). Returns the child's exit code (0 = success), or -1 to spawn /
// 128+signal on abnormal termination. stderr is routed through a unique temp
// file so nothing is interleaved into stdout.
static int run_capture(const std::string &cmd, std::string &out,
                       std::string &err, const std::string &tmpdir) {
  out.clear();
  err.clear();
  std::string errfile = tmpdir + "/stderr." + std::to_string(getpid());
  std::string full = cmd + " 2>" + shell_quote(errfile);
  FILE *p = popen(full.c_str(), "r");
  if (!p)
    return -1;
  char buf[4096];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), p)) > 0)
    out.append(buf, n);
  int st = pclose(p);
  err = slurp_file(errfile);
  ::remove(errfile.c_str());
  if (st == -1)
    return -1;
  if (WIFSIGNALED(st))
    return 128 + WTERMSIG(st);
  return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

// Extract the raw asm text of each named region from a compiled .s (everything
// between "# LLVM-MCA-BEGIN name" and "# LLVM-MCA-END"), and count how many
// times each name is opened. A name opened more than once means the marked
// function was inlined at several call sites (or duplicate markers) — llvm-mca
// then emits several same-named regions and find_region() would silently pick
// one, so the driver hard-errors on a count > 1.
struct RegionAsm {
  std::map<std::string, std::string> body;
  std::map<std::string, int> begins;
};
static RegionAsm extract_regions_asm(const std::string &asm_text) {
  RegionAsm out;
  std::istringstream is(asm_text);
  std::string cur;
  std::string body;
  bool in = false;
  for (std::string line; std::getline(is, line);) {
    if (line.find("LLVM-MCA-BEGIN") != std::string::npos) {
      auto toks = tokens(line);
      cur = toks.empty() ? "" : toks.back();
      body.clear();
      in = true;
      out.begins[cur]++;
      continue;
    }
    if (line.find("LLVM-MCA-END") != std::string::npos) {
      if (in)
        out.body[cur] += body;
      in = false;
      continue;
    }
    if (in)
      body += line + "\n";
  }
  return out;
}

// Is this mnemonic pure data movement / stack shuffling (as opposed to work
// that lands on an execution port)? The perturbation diff ignores these: a
// "memory"-clobber marker legitimately forces a few boundary spills/reloads and
// register repacks, and flagging those would cry wolf on every real region. The
// perturbation we DO care about — blocked vectorisation or hoisting of the
// actual arithmetic — changes the COMPUTE mnemonics (scalar imul vs packed
// vpmuludq, etc.), which are kept.
static bool is_data_movement(const std::string &op) {
  // popcnt/lzcnt/tzcnt start with letters that would trip the "pop"/"..."
  // prefixes below but are real ALU ops — exclude them explicitly.
  if (op.rfind("popcnt", 0) == 0)
    return false;
  static const char *prefixes[] = {
      "mov",        "vmov",    "cmov",    "kmov",     "lea",    "push",
      "pop",        "vzero",   "vinsert", "vextract", "vpinsr", "vpextr",
      "vperm",      "vpunpck", "punpck",  "vshuf",    "shuf",   "vpbroadcast",
      "vbroadcast", "vpblend", "vblend",  "pblend",   "vpack",  "pack",
      "vpalignr",   "palignr"};
  for (const char *p : prefixes)
    if (op.rfind(p, 0) == 0)
      return true;
  return op == "nop" || op == "endbr64" || op == "ret" || op == "retq" ||
         op == "vzeroupper";
}

// Fraction of COMPUTE instructions that changed between two builds' histograms.
// 0 = identical compute; ~1 = wholly different. A vectorisation flip or a
// hoist moves many arithmetic mnemonics and scores high; a ±1 loop-induction
// wobble from a boundary spill scores near zero. The driver warns above a
// threshold so the perturbation check fires on real changes, not noise.
static double perturbation_ratio(const std::map<std::string, int> &on,
                                 const std::map<std::string, int> &off) {
  std::map<std::string, int> keys;
  for (auto &[k, v] : on)
    keys[k] = 0;
  for (auto &[k, v] : off)
    keys[k] = 0;
  long diff = 0, base = 0;
  for (auto &[k, _] : keys) {
    int a = on.count(k) ? on.at(k) : 0;
    int b = off.count(k) ? off.at(k) : 0;
    diff += std::labs(a - b);
    base += b;
  }
  if (base == 0)
    return diff > 0 ? 1.0 : 0.0;
  return (double)diff / (double)base;
}

// Histogram of COMPUTE instruction mnemonics over a whole .s (skips directives,
// labels, comments, and data-movement per is_data_movement). Used for the
// marked-vs-unmarked perturbation diff, which should fire on a changed
// computation, not on benign boundary spills.
static std::map<std::string, int>
mnemonic_histogram(const std::string &asm_text) {
  std::map<std::string, int> h;
  std::istringstream is(asm_text);
  for (std::string line; std::getline(is, line);) {
    std::string t = trim(line);
    if (t.empty() || t[0] == '.' || t[0] == '#')
      continue;
    if (t.back() == ':')
      continue;
    auto toks = tokens(t);
    if (toks.empty())
      continue;
    if (is_data_movement(toks[0]))
      continue;
    h[toks[0]]++;
  }
  return h;
}

// Split a .s into (function-symbol -> body) chunks. A function starts at a
// column-0 label that is not a local ".L" label; everything up to the next such
// label is its body. Used to scope the perturbation diff to only the functions
// that actually contain markers, so a regional vectorisation loss isn't diluted
// by unrelated code elsewhere in the TU.
static std::map<std::string, std::string>
split_functions(const std::string &asm_text) {
  std::map<std::string, std::string> out;
  std::istringstream is(asm_text);
  std::string cur, body;
  for (std::string line; std::getline(is, line);) {
    if (!line.empty() && line[0] != ' ' && line[0] != '\t' &&
        line.back() == ':' && line.rfind(".L", 0) != 0 && line[0] != '.') {
      if (!cur.empty())
        out[cur] += body;
      cur = line.substr(0, line.size() - 1);
      body.clear();
    } else if (!cur.empty()) {
      body += line + "\n";
    }
  }
  if (!cur.empty())
    out[cur] += body;
  return out;
}

// Perturbation ratio scoped to the functions that contain markers: compare each
// marked function's compute histogram against the same function in the unmarked
// build, aggregated. Returns 0 if no marked function is found in both.
static double marked_function_perturbation(const std::string &asm_on,
                                           const std::string &asm_off) {
  auto fon = split_functions(asm_on);
  auto foff = split_functions(asm_off);
  std::string on_marked, off_marked;
  for (auto &[name, body] : fon) {
    if (body.find("LLVM-MCA-BEGIN") == std::string::npos)
      continue;
    on_marked += body;
    auto it = foff.find(name);
    if (it != foff.end())
      off_marked += it->second;
  }
  if (on_marked.empty() || off_marked.empty())
    return 0.0; // nothing comparable — no false alarm
  return perturbation_ratio(mnemonic_histogram(on_marked),
                            mnemonic_histogram(off_marked));
}

// Load a key=value profile file over the defaults.
static Profile load_profile(const std::string &path) {
  Profile p;
  std::ifstream f(path);
  if (!f) {
    fprintf(stderr,
            "warning: profile '%s' not readable — using placeholder "
            "defaults (NOT your machine).\n",
            path.c_str());
    return p;
  }
  for (std::string line; std::getline(f, line);) {
    line = trim(line);
    if (line.empty() || line[0] == '#')
      continue;
    auto eq = line.find('=');
    if (eq == std::string::npos) {
      // A non-empty, non-comment line with no '=' is malformed (e.g. a missed
      // separator, `presence_tax 0.03`) — same typo class as an unknown key, so
      // warn rather than silently skip.
      fprintf(stderr,
              "warning: profile '%s': ignoring malformed line (no '='): '%s'\n",
              path.c_str(), line.c_str());
      continue;
    }
    std::string k = trim(line.substr(0, eq));
    double v = std::atof(trim(line.substr(eq + 1)).c_str());
    if (k == "freq_ghz")
      p.freq_ghz = v;
    else if (k == "handoff_sibling_ns")
      p.handoff_sibling_ns = v;
    else if (k == "handoff_sameccx_ns")
      p.handoff_sameccx_ns = v;
    else if (k == "presence_tax")
      p.presence_tax = v;
    else if (k == "pacing_headroom")
      p.pacing_headroom = v;
    else if (k == "allowance_ns")
      p.allowance_ns = v;
    else if (k == "calib_scale")
      p.calib_scale = v;
    else if (k == "mca_mcpu")
      p.mca_mcpu = trim(line.substr(eq + 1));
    else if (k == "machine")
      p.machine = trim(line.substr(eq + 1));
    else
      // An unrecognized key is almost always a typo (e.g. `presence_taax`),
      // which would otherwise silently retain the DEFAULT for the real key and
      // emit a confident-but-wrong budget. Surface it loudly instead of
      // dropping it on the floor.
      fprintf(stderr,
              "warning: profile '%s': ignoring unrecognized key '%s' "
              "(typo? — the intended field keeps its default value)\n",
              path.c_str(), k.c_str());
  }
  return p;
}

// Compile a TU to asm (-S). `markers_off` selects the unmarked build. Every
// interpolated token (source path, each cflag) is shell-quoted, and outputs go
// to unique per-run paths inside `tmpdir`. Returns the .s text; sets ok=false
// and prints the compiler's stderr on failure.
// `cxx` is the compiler driver (default "g++", override via --cxx or $CXX).
// Base flags default to the SAME flags this repo's other binaries are
// measured with (-O3 -std=c++23, no -march=native — see CMakeLists.txt: no
// -march is set there, so the measured crossover was produced by a scalar,
// non-AVX-512-vectorised build). Hardcoding -march=native here used to
// analyse a DIFFERENT, more-vectorised port profile than the one that
// produced the measured numbers; pass `--cflags -march=native` explicitly if
// you want that (and re-measure to match).
static std::string compile_to_asm(const std::string &src,
                                  const std::vector<std::string> &cflags,
                                  bool markers_off, const std::string &tmpdir,
                                  const std::string &cxx, bool &ok) {
  std::string sfile = tmpdir + "/" + (markers_off ? "off" : "on") + ".s";
  std::string cmd = shell_quote(cxx) + " -O3 -std=c++23 -S";
  for (auto &f : cflags)
    cmd += " " + shell_quote(f);
  if (markers_off)
    cmd += " -DSIBLING_MARKERS_OFF";
  cmd += " " + shell_quote(src) + " -o " + shell_quote(sfile);
  std::string out, err;
  int rc = run_capture(cmd, out, err, tmpdir);
  if (rc != 0) {
    fprintf(stderr, "compile failed:\n%s%s\n", out.c_str(), err.c_str());
    ok = false;
    return "";
  }
  ok = true;
  return slurp_file(sfile);
}

// Create a unique per-run temp directory (mkdtemp). Returns "" on failure.
static std::string make_tmpdir() {
  char tmpl[] = "/tmp/sibling_analyze.XXXXXX";
  char *d = mkdtemp(tmpl);
  return d ? std::string(d) : std::string();
}

// Best-effort recursive removal of a temp dir we created (only our own files).
static void remove_tmpdir(const std::string &dir) {
  if (dir.empty())
    return;
  std::string cmd = "rm -rf " + shell_quote(dir);
  std::string o, e;
  run_capture(cmd, o, e, "/tmp");
}

// Run llvm-mca on a .s file with an explicit -mcpu. stdout -> `out`, stderr ->
// `err` (surfaced, since a wrong/unrecognised -mcpu warning must reach the user
// rather than be parsed as report text). Returns exit code.
// `mca_bin` is the llvm-mca binary to invoke (default "llvm-mca"; override via
// --mca-bin or $SIBLING_ANALYZE_MCA — bare "llvm-mca" is often the WRONG
// binary: distros ship versioned names like llvm-mca-20 with no unversioned
// symlink, so a bare invocation can silently pick up a stale/absent tool
// instead of the one CMake actually discovered).
static int run_mca(const std::string &sfile, const std::string &mcpu,
                   const std::string &mca_bin, const std::string &tmpdir,
                   std::string &out, std::string &err) {
  std::string cmd = shell_quote(mca_bin) + " -mcpu=" + shell_quote(mcpu) + " " +
                    shell_quote(sfile);
  return run_capture(cmd, out, err, tmpdir);
}

static const RegionProfile *find_region(const std::vector<RegionProfile> &rs,
                                        const std::string &name) {
  for (auto &r : rs)
    if (r.name == name)
      return &r;
  return nullptr;
}

// Post-parse sanity: a region is only usable if mca actually populated it.
// Returns an error string naming the missing field (empty == ok). This closes
// the fail-UNSAFE paths where a format drift or an empty region silently yields
// cycles=0 (→ utilisation divides by a 1.0 fallback) or empty pressure (→
// c_raw floors to 1.0, a confident false "no collision").
static std::string validate_region(const RegionProfile &r) {
  if (!(r.cycles_per_iter > 0))
    return "'" + r.name +
           "': Block RThroughput not parsed (cycles_per_iter <= 0) — llvm-mca "
           "output format may have drifted, or the region is degenerate";
  if (r.pressure.empty())
    return "'" + r.name +
           "': no resource pressure parsed — the region is empty or the "
           "'Resource pressure per iteration' table was not found";
  if (!(r.dispatch_width > 0))
    return "'" + r.name + "': Dispatch Width not parsed (<= 0)";
  if (r.instructions <= 0)
    return "'" + r.name + "': Instructions not parsed (<= 0)";
  return "";
}

// ---------------------------------------------------------------------------
// Human-readable report + machine-readable JSON.
// ---------------------------------------------------------------------------
// Per-message multipliers. mca's "iteration" is one repeat of the marked ASM
// BLOCK, which is NOT necessarily one message (it is usually one loop
// iteration). To turn per-block cycles into per-message work you must say how
// many blocks make a message. Defaults are 1 with `explicit_msg=false`, in
// which case the tool prints the budget but withholds the bottom-line
// placement recommendation (it cannot know the consumer's real per-message
// work) rather than printing a near-unconditional "sibling wins".
struct Iters {
  double cons = 1.0, prod = 1.0;
  bool explicit_msg = false;  // --consumer-iters-per-msg given
  bool prod_explicit = false; // --producer-iters-per-msg given
};

// ---------------------------------------------------------------------------
// Human-friendly names for the resource groups llvm-mca reports, so the
// headline verdict can say "load/store unit" instead of "Zn4LSU" to a reader
// who doesn't know AMD's internal port naming. Single named table (not
// scattered literals) so new mappings are added in one place. `is_prefix`
// entries match any resource name STARTING WITH `raw` (e.g. all of
// Zn4ALU0/Zn4ALU1/... collapse to one human name); exact entries must match
// in full. An unmapped resource falls back to printing its raw mca name
// rather than hiding it.
// ---------------------------------------------------------------------------
struct ResourceNameEntry {
  const char *raw;   // exact name, or a prefix when is_prefix is true
  const char *human; // plain-English name for the headline
  bool is_prefix;
};
static const ResourceNameEntry kResourceHumanNames[] = {
    {"Zn4LSU", "load/store unit", false},
    {"Zn4FP45", "FP/vector units", false},
    {"Zn4ALU", "integer ALU", true},
    {"dispatch(front-end)", "front-end dispatch width", false},
};

static std::string human_resource_name(const std::string &raw) {
  for (const auto &e : kResourceHumanNames) {
    if (e.is_prefix) {
      if (raw.rfind(e.raw, 0) == 0) // raw starts with e.raw
        return e.human;
    } else if (raw == e.raw) {
      return e.human;
    }
  }
  return raw; // unmapped: fall back to the raw mca name, don't hide it
}

static void print_report(const RegionProfile &prod, const RegionProfile &cons,
                         const Profile &prof, const Overlay &o, const Budget &b,
                         const Iters &it,
                         const std::vector<std::string> &lint_warnings = {}) {
  auto ns = [&](double cyc) { return cyc / prof.freq_ghz; };
  // The consumer's per-message WORK estimate — what the bottom-line
  // recommendation compares against W* — must use the same real-cycle
  // denominator as utilisation()/producer_busy_ns() (Total Cycles/Iterations),
  // NOT Block RThroughput. RThroughput is a throughput lower bound that is
  // blind to dependency-chain latency; on a latency-bound consumer it
  // understates W_msg (this repo's own real numbers: RThroughput=6.0 cyc vs
  // effective=24.05 cyc, a 4x gap), which would silently push the "sibling
  // wins" recommendation past where it actually holds. See
  // effective_cycles_per_iter()'s comment for the full rationale.
  double W_block = ns(effective_cycles_per_iter(cons)); // one marked block
  double W_msg = W_block * it.cons; // per message (if known)
  double t_prod = producer_busy_ns(prod, prof, it.prod);

  printf("\n=== sibling_analyze — STATIC, ports+dispatch only ===\n");
  // Two DISTINCT cyc/block figures, labelled, same as the consumer line
  // below: RThroughput (the mca-reported throughput lower bound) vs
  // effective (Total Cycles/Iterations). t_prod (the ns/msg figure) is
  // derived from EFFECTIVE cycles (producer_busy_ns() above), so it must sit
  // next to the effective cyc/block, not RThroughput — printing RThroughput
  // cyc unlabeled next to an effective-derived ns figure reads as internally
  // inconsistent on a latency-bound producer (e.g. "6.0 cyc/block (4.8
  // ns/msg @ 4.98 GHz)" looks like 6.0 cyc == 4.8 ns, which at 4.98 GHz would
  // be ~1.2 ns — the two numbers actually come from different denominators).
  printf("producer '%s': %.1f cyc/block RThroughput, %.1f cyc/block effective "
         "(%.1f ns/msg effective @ %.2f GHz, %gx blocks/msg)\n",
         prod.name.c_str(), prod.cycles_per_iter,
         effective_cycles_per_iter(prod), t_prod, prof.freq_ghz, it.prod);
  printf("consumer '%s': %.1f cyc/block RThroughput, %.1f cyc/block effective "
         "(%.1f ns/block effective), bottleneck below\n",
         cons.name.c_str(), cons.cycles_per_iter,
         effective_cycles_per_iter(cons), W_block);

  // -------------------------------------------------------------------------
  // RESULT: plain English a non-expert can act on, LEADING the report. The
  // precise numbers (combined demand table, C_raw/C, W* mid/range) are kept
  // below under "detail:" for the reader who wants them — this headline is
  // the one-line takeaway, folding in what used to be a separate bare
  // "VERDICT:" line plus a separate bottom "=>" recommendation.
  //
  // A port/dispatch COLLISION is a static, message-rate-independent fact
  // (the two blocks alone already oversubscribe a shared resource), so its
  // action (separate cores) is unconditional and does not depend on the
  // duty-cycle/W* budget below — that budget only answers "how much work
  // before same-CCX beats sibling", a question that presupposes no
  // collision in the first place.
  // -------------------------------------------------------------------------
  if (o.collides) {
    printf("\nRESULT: these two loops will contend on the %s (together they "
           "demand %.2fx what one core supplies) -> place them on SEPARATE "
           "cores.\n",
           human_resource_name(o.bottleneck).c_str(), o.bottleneck_demand);
  } else if (!it.explicit_msg) {
    // The bottom-line recommendation compares the consumer's PER-MESSAGE work
    // to W*. That requires knowing blocks-per-message; without
    // --consumer-iters-per-msg we refuse to guess (a marked loop body is
    // ~one iteration, so assuming 1 would make the answer
    // near-unconditionally "sibling").
    printf("\nRESULT: no port collision. Per-message work is UNKNOWN, so "
           "there's no placement recommendation yet -> pass "
           "--consumer-iters-per-msg N (blocks per message); the consumer's "
           "marked block alone is ~%.1f ns.\n",
           W_block);
  } else if (b.mid) {
    if (W_msg < *b.mid)
      printf("\nRESULT: no port collision. Placement budget ~%.0f ns of "
             "work per message; your consumer does ~%.0f ns/msg -> the SMT "
             "SIBLING is the faster placement (with ~%.0f ns to spare).\n",
             *b.mid, W_msg, *b.mid - W_msg);
    else
      printf("\nRESULT: no port collision, but your consumer's ~%.0f ns/msg "
             "exceeds the ~%.0f ns placement budget -> place on a SAME-CCX "
             "core instead; on-core contention now outweighs the sibling's "
             "handoff edge.\n",
             W_msg, *b.mid);
  } else {
    printf("\nRESULT: no port collision, and no contention crossover found "
           "up to a millisecond of per-message work -> the SMT SIBLING is "
           "the faster placement across the modelled range.\n");
  }

  // Lint caveats travel WITH the verdict (not buried earlier in build output)
  // so the reader never separates a warning like "shrink region to the true
  // steady-state body" from the number it qualifies.
  for (const std::string &w : lint_warnings)
    printf("  caveat: %s\n", w.c_str());

  // ---- detail: the precise numbers behind the RESULT headline above. ----
  printf("\ndetail: combined demand (producer+consumer utilisation, >1.0 = "
         "oversubscribed):\n");
  for (auto &[k, d] : o.top)
    printf("    %-22s %.2f%s\n", k.c_str(), d,
           k == o.bottleneck ? "   <== bottleneck" : "");
  printf("  friction: C_raw=%.2f -> C=%.2f (calib_scale=%.2f); bottleneck "
         "'%s' (%s)\n",
         o.c_raw, o.c, prof.calib_scale, o.bottleneck.c_str(),
         human_resource_name(o.bottleneck).c_str());
  if (!o.collides)
    printf("  NOTE: this checks execution ports + front end ONLY — cache "
           "bandwidth, MSHRs, store buffer and L1 aliasing are unchecked. "
           "Confirm with sibling_noise/spsc_pipeline if either region "
           "touches >L1 data or is store-heavy.\n");

  printf("\ndetail: placement budget W* (consumer ns of WORK PER MESSAGE "
         "below which the sibling wins):\n");
  auto show = [](const char *tag, const std::optional<double> &w) {
    if (w)
      printf("    %-8s W* = %.0f ns\n", tag, *w);
    else
      printf("    %-8s no crossover — sibling wins across the swept range "
             "(C~1 and/or tiny presence tax)\n",
             tag);
  };
  show("mid", b.mid);
  show("range", b.lo);
  show("      ", b.hi);
  printf("  (W* is an UPPER BOUND: every unmodelled effect — caches, MSHRs, "
         "front-end sharing — only lowers it.)\n");
  if (!it.prod_explicit)
    printf("  (producer assumed 1 block/msg — if its marked span is one loop "
           "iteration, pass --producer-iters-per-msg N; too low under-counts "
           "duty and INFLATES W*.)\n");
}

static void print_json(const RegionProfile &prod, const RegionProfile &cons,
                       const Profile &prof, const Overlay &o, const Budget &b,
                       const Iters &it) {
  auto jopt = [](const std::optional<double> &w) {
    return w ? std::to_string((long)llround(*w)) : std::string("null");
  };
  printf("{\n");
  printf("  \"producer\": \"%s\", \"consumer\": \"%s\",\n", prod.name.c_str(),
         cons.name.c_str());
  printf("  \"producer_ns_per_msg\": %.1f, \"iters_per_msg_explicit\": %s,\n",
         producer_busy_ns(prod, prof, it.prod),
         it.explicit_msg ? "true" : "false");
  // Same F1 fix as print_report(): effective cycles (Total Cycles/Iterations),
  // NOT Block RThroughput — see effective_cycles_per_iter()'s comment. Field
  // NAMES/shape are unchanged (scripts depend on them); only the previously-
  // wrong VALUES are fixed.
  printf("  \"consumer_ns_per_block\": %.1f, \"consumer_ns_per_msg\": %.1f,\n",
         effective_cycles_per_iter(cons) / prof.freq_ghz,
         effective_cycles_per_iter(cons) * it.cons / prof.freq_ghz);
  printf("  \"c_raw\": %.3f, \"c\": %.3f, \"calib_scale\": %.3f,\n", o.c_raw,
         o.c, prof.calib_scale);
  printf("  \"bottleneck\": \"%s\", \"bottleneck_demand\": %.3f, \"collides\": "
         "%s,\n",
         o.bottleneck.c_str(), o.bottleneck_demand,
         o.collides ? "true" : "false");
  printf("  \"wstar_ns_mid\": %s, \"wstar_ns_lo\": %s, \"wstar_ns_hi\": %s,\n",
         jopt(b.mid).c_str(), jopt(b.lo).c_str(), jopt(b.hi).c_str());
  printf("  \"dh_ns\": %.1f, \"presence_tax\": %.3f, \"note\": \"static "
         "ports+dispatch only; W* is an upper bound\"\n",
         prof.Dh(), prof.presence_tax);
  printf("}\n");
}

// Emit the predicted model curve as CSV on stdout: for a log-spaced sweep of
// consumer work W, the predicted end-to-end delta (sibling - same-CCX) in ns,
// i.e. g_of(W). This is the SINGLE SOURCE of the model shape (it reuses the
// same g_of()/duty_of() the W* solve uses); the plot script only draws it, so
// the picture can never drift from the tool's own math. Header comment lines
// carry C, the W* band, and Dh so the plotter can render the band + zero line
// without re-deriving anything.
static void emit_model(const RegionProfile &prod, const Profile &prof,
                       const Overlay &o, const Budget &b, const Iters &it) {
  double t_prod = producer_busy_ns(prod, prof, it.prod);
  // "none" (not "nan"): Python's float("nan") parses successfully, so a "nan"
  // sentinel would silently pass an isinstance(float) guard and poison the
  // downstream check. "none" stays a string the plotter can test explicitly.
  auto opt = [](const std::optional<double> &w) {
    return w ? std::to_string((long)llround(*w)) : std::string("none");
  };
  printf("# sibling_analyze model curve (predicted sibling - same-CCX, ns)\n");
  printf("# machine=%s mca_mcpu=%s\n",
         prof.machine.empty() ? "unspecified" : prof.machine.c_str(),
         prof.mca_mcpu.c_str());
  printf("# C=%.4f calib_scale=%.4f dh_ns=%.2f presence_tax=%.4f "
         "producer_ns=%.2f freq_ghz=%.3f\n",
         o.c, prof.calib_scale, prof.Dh(), prof.presence_tax, t_prod,
         prof.freq_ghz);
  printf("# wstar_mid=%s wstar_lo=%s wstar_hi=%s\n", opt(b.mid).c_str(),
         opt(b.lo).c_str(), opt(b.hi).c_str());
  printf("w_ns,delta_ns\n");
  // Log sweep 10 ns .. 100 us, 80 points.
  const int N = 80;
  double loW = 10.0, hiW = 100000.0;
  for (int i = 0; i < N; i++) {
    double f = (double)i / (N - 1);
    double W = loW * std::pow(hiW / loW, f);
    printf("%.3f,%.4f\n", W, g_of(W, prof, o.c, t_prod));
  }
}

// ===========================================================================
// SECTION 6 — self-tests (pure; no compiler / mca / hardware needed).
// ===========================================================================

// A canned llvm-mca report with two regions: an imul-bound "consumer" (8 imul
// on one port, saturated) and a light "producer". Mirrors the real LLVM 18
// format this repo pins.
static const char *kCannedMca = R"MCA(
[0] Code Region - producer

Iterations:        100
Instructions:      400
Total Cycles:      200
Total uOps:        400
Dispatch Width:    6
uOps Per Cycle:    1.95
IPC:               1.95
Block RThroughput: 2.0

Resources:
[0]   - SKXDivider
[1]   - SKXFPDivider
[2]   - SKXPort0
[3]   - SKXPort1
[4]   - SKXPort2
[5]   - SKXPort3
[6]   - SKXPort4
[7]   - SKXPort5
[8]   - SKXPort6
[9]   - SKXPort7

Resource pressure per iteration:
[0]    [1]    [2]    [3]    [4]    [5]    [6]    [7]    [8]    [9]
 -      -     1.00    -      -      -      -     1.00    -      -

[1] Code Region - consumer

Iterations:        100
Instructions:      800
Total Cycles:      800
Total uOps:        800
Dispatch Width:    6
uOps Per Cycle:    0.99
IPC:               0.99
Block RThroughput: 8.0

Resources:
[0]   - SKXDivider
[1]   - SKXFPDivider
[2]   - SKXPort0
[3]   - SKXPort1
[4]   - SKXPort2
[5]   - SKXPort3
[6]   - SKXPort4
[7]   - SKXPort5
[8]   - SKXPort6
[9]   - SKXPort7

Resource pressure per iteration:
[0]    [1]    [2]    [3]    [4]    [5]    [6]    [7]    [8]    [9]
 -      -      -     8.00    -      -      -      -      -      -
)MCA";

// A canned llvm-mca report for a LATENCY-BOUND region: Total Cycles (2405) is
// far above Block RThroughput * Iterations (6.0*100=600) because the six
// output words are filled by a SERIAL dependency chain (s = s*MIX+1, each word
// depending on the previous), not independent lanes — mca's Block RThroughput
// only sees the throughput ceiling of the busiest port and is blind to that
// chain. These are the REAL numbers llvm-mca-20 (znver5) reports for
// examples/spsc_marked.cpp's "producer" region — not invented — so this is the
// exact shape that let F1 ship: with the old cycles_per_iter-only denominator,
// u = 6.00/6.0 = 1.00 (single region reads as 100% saturated); the correct
// denominator (Total Cycles/Iterations = 24.05) gives u = 6.00/24.05 ~= 0.25,
// matching the real occupancy.
static const char *kCannedMcaLatencyBound = R"MCA(
[0] Code Region - latency_producer

Iterations:        100
Instructions:      1900
Total Cycles:      2405
Total uOps:        1900
Dispatch Width:    6
uOps Per Cycle:    0.79
IPC:               0.79
Block RThroughput: 6.0

Resources:
[0]   - Zn4ALU0
[1]   - Zn4ALU1

Resource pressure per iteration:
[0]    [1]
 -     6.00
)MCA";

static int run_self_tests() {
  int fails = 0;
  auto check = [&](bool ok, const char *msg) {
    if (!ok) {
      fprintf(stderr, "FAIL: %s\n", msg);
      fails++;
    }
  };

  // Parse the canned report.
  auto regions = parse_mca_text(kCannedMca);
  check(regions.size() == 2, "expected 2 regions");
  const RegionProfile *prod = find_region(regions, "producer");
  const RegionProfile *cons = find_region(regions, "consumer");
  check(prod && cons, "both named regions found");
  if (prod && cons) {
    check(std::fabs(cons->cycles_per_iter - 8.0) < 1e-6,
          "consumer RThroughput");
    check(std::fabs(cons->pressure.at("SKXPort1") - 8.0) < 1e-6,
          "consumer Port1 pressure parsed");
    check(std::fabs(prod->dispatch_width - 6.0) < 1e-6,
          "producer dispatch width");

    // Overlay: consumer alone saturates Port1 (u=1.0); producer adds nothing on
    // Port1, so the combined Port1 demand is ~1.0 — NOT a collision. This is
    // the asymmetric case (disjoint bottlenecks) and is the important negative
    // test.
    Profile prof;
    Overlay o = overlay(*prod, *cons, prof);
    // Consumer alone saturates Port1 (8.0/8.0 = 1.0); the producer puts nothing
    // on Port1, so combined Port1 == exactly 1.0 and nothing else is higher —
    // NOT a collision. Assert the concrete values, not a trivially-true OR.
    check(o.bottleneck == "SKXPort1", "disjoint pair bottleneck is Port1");
    check(std::fabs(o.bottleneck_demand - 1.0) < 1e-6,
          "disjoint pair: combined Port1 == 1.0");
    check(!o.collides, "disjoint pair does NOT collide (demand 1.0 < margin)");

    // Now the golden shape: consumer overlaid with ITSELF (imul vs imul) must
    // land Port1 at 2.0 — the measured busy-sibling ~1.8x case.
    Overlay self = overlay(*cons, *cons, prof);
    check(self.bottleneck == "SKXPort1", "imul-vs-imul bottleneck is Port1");
    check(std::fabs(self.bottleneck_demand - 2.0) < 1e-6,
          "imul-vs-imul combined Port1 demand == 2.0");
    check(std::fabs(self.c_raw - 2.0) < 1e-6,
          "C_raw == 2.0 for the golden pair");

    // Calibration mapping: with scale = (1.8-1)/(2.0-1) = 0.8, C -> 1.8.
    Profile cal = prof;
    cal.calib_scale = 0.8;
    Overlay selfc = overlay(*cons, *cons, cal);
    check(std::fabs(selfc.c - 1.8) < 1e-6, "calibrated C == measured 1.8");
  }

  // ProcResGroup (dot-index) normalization — the regression test for the bug
  // that shipped: a 3-unit AMD-style Zn4LSU group carrying 3.0 cycles of
  // pressure over a 1-cycle block is 100% busy per unit, u == 1.0, NOT 3.0. A
  // single light region must therefore NOT self-report as oversubscribed.
  {
    static const char *kGrouped = R"MCA(
[0] Code Region - grp

Iterations:        100
Instructions:      300
Total Cycles:      100
Total uOps:        300
Dispatch Width:    6
uOps Per Cycle:    3.0
IPC:               3.0
Block RThroughput: 1.0

Resources:
[0]   - Zn4ALU0
[14.0] - Zn4LSU
[14.1] - Zn4LSU
[14.2] - Zn4LSU

Resource pressure per iteration:
[0]    [14.0] [14.1] [14.2]
 -      1.00   1.00   1.00
)MCA";
    auto gr = parse_mca_text(kGrouped);
    check(gr.size() == 1 && gr[0].name == "grp", "grouped region parsed");
    if (gr.size() == 1) {
      check(gr[0].units["Zn4LSU"] == 3, "Zn4LSU counted as 3 parallel units");
      check(std::fabs(gr[0].pressure["Zn4LSU"] - 3.0) < 1e-6,
            "Zn4LSU summed pressure == 3.0");
      auto u = utilisation(gr[0]);
      check(std::fabs(u["Zn4LSU"] - 1.0) < 1e-6,
            "Zn4LSU per-unit utilisation == 1.0 (NOT 3.0)");
      // Overlaying this light region with a disjoint one must not collide.
      RegionProfile light;
      light.name = "light";
      light.cycles_per_iter = 1.0;
      light.dispatch_width = 6.0;
      light.uops_per_cycle = 1.0;
      light.instructions = 1;
      light.pressure["Zn4ALU0"] = 0.2;
      light.units["Zn4ALU0"] = 1;
      Profile prof;
      Overlay og = overlay(light, gr[0], prof);
      check(!og.collides, "light+grouped region does NOT false-collide");
    }
  }

  // F1 REGRESSION — Total Cycles must be the utilisation/duty denominator, NOT
  // Block RThroughput (a throughput lower bound that is blind to dependency
  // chains). kCannedMcaLatencyBound is llvm-mca-20's REAL report for
  // examples/spsc_marked.cpp's serial-chain "producer" region: Block
  // RThroughput=6.0 but Total Cycles=2405 over 100 iterations (24.05 cyc/iter)
  // because the chain is latency-, not throughput-, bound. None of the OTHER
  // canned regions above has Total Cycles != Block RThroughput*Iterations, so
  // this is the exact blind spot that let the bug ship.
  {
    auto lat = parse_mca_text(kCannedMcaLatencyBound);
    check(lat.size() == 1 && lat[0].name == "latency_producer",
          "latency-bound region parsed");
    if (lat.size() == 1) {
      const RegionProfile &r = lat[0];
      check(std::fabs(r.total_cycles - 2405.0) < 1e-6,
            "Total Cycles parsed (2405, distinct from RThroughput*Iters=600)");

      // (a) u <= 1.0 for a single region: with the buggy RThroughput-only
      // denominator, u = 6.00/6.0 = 1.00 exactly (already borderline); the
      // real bug manifests when RThroughput is smaller than pressure alone
      // would need, or — as caught by the exact-value assert below — the
      // computed utilisation must match Total Cycles math, not RThroughput
      // math, and no resource utilisation may exceed 1.0 for one region alone
      // (a region cannot be >100% busy on its own port).
      auto u = utilisation(r);
      double u_alu1 = u.count("Zn4ALU1") ? u.at("Zn4ALU1") : -1.0;
      check(u_alu1 > 0.0 && u_alu1 <= 1.0 + 1e-9,
            "single latency-bound region: u(Zn4ALU1) in (0,1]");
      // Exact value: 6.00 / (2405/100) = 6.00/24.05 ~= 0.2495 — the ~0.25
      // real-occupancy figure the fable review measured. The OLD (buggy) code
      // would have computed 6.00/6.0 = 1.0, i.e. 4x too high.
      check(std::fabs(u_alu1 - (6.00 / 24.05)) < 1e-6,
            "utilisation uses Total Cycles/Iterations, not Block RThroughput");

      // (b) a serial-chain (latency-bound) pair does NOT collide: overlaying
      // this light-occupancy (~0.25) region with itself combines to ~0.50,
      // well under the 1.05 collide_margin. Under the OLD bug (u=1.0 alone)
      // this overlay would combine to ~2.0 and falsely COLLIDE.
      Profile prof;
      Overlay self_lat = overlay(r, r, prof);
      check(!self_lat.collides,
            "serial-chain pair does NOT false-collide (real occupancy ~0.25 "
            "each)");
      check(self_lat.bottleneck_demand < 1.0,
            "serial-chain pair combined demand stays under 1.0");

      // (c) producer_busy_ns must reflect ACTUAL cycles, not RThroughput: at
      // 1 GHz and 1 block/msg, busy ns == effective cycles/iter == 24.05, NOT
      // the RThroughput-derived 6.0.
      Profile ghz1;
      ghz1.freq_ghz = 1.0;
      double busy = producer_busy_ns(r, ghz1, 1.0);
      check(std::fabs(busy - 24.05) < 1e-6,
            "producer_busy_ns uses Total Cycles/Iterations (24.05 ns @ 1GHz), "
            "not Block RThroughput (would be 6.0 ns)");

      // (d) F1 RESIDUAL — the CONSUMER-side per-message work estimate (what
      // print_report's W_block/W_msg and print_json's consumer_ns_per_block/
      // consumer_ns_per_msg compute, and what the final placement
      // recommendation compares against W*) must ALSO use effective cycles
      // (Total Cycles/Iterations), not Block RThroughput. The producer side
      // was fixed first ((c) above); this mirrors it for the consumer side,
      // which the review found the original fix missed. At 1 GHz,
      // consumer_ns_per_block == effective cycles/iter == 24.05, NOT the
      // RThroughput-derived 6.0.
      double consumer_ns_per_block =
          effective_cycles_per_iter(r) / ghz1.freq_ghz;
      check(std::fabs(consumer_ns_per_block - 24.05) < 1e-6,
            "consumer_ns_per_block uses Total Cycles/Iterations (24.05 ns "
            "@ 1GHz), not Block RThroughput (would be 6.0 ns)");
      check(std::fabs(consumer_ns_per_block -
                      r.cycles_per_iter / ghz1.freq_ghz) > 1.0,
            "sanity: effective and RThroughput-derived denominators actually "
            "differ for this canned latency-bound region");
    }
  }

  // duty(W) monotonicity: duty must fall as W grows (period grows).
  {
    Profile prof;
    double d_small = duty_of(50.0, prof, 1.8, 20.0);
    double d_big = duty_of(5000.0, prof, 1.8, 20.0);
    check(d_small > d_big, "duty(W) decreases with W");
    check(d_small <= 1.0 && d_big >= 0.0, "duty in [0,1]");
  }

  // W* crossover: with a real presence tax and C>1 there is a finite crossover,
  // and it must sit in the microseconds for the README's polite-producer regime
  // (sanity band, not an exact value).
  {
    Profile prof; // Dh = 40, eps = 0.03
    auto w = solve_wstar(prof, 1.8, 20.0);
    check(w.has_value(), "finite W* exists with eps>0");
    if (w)
      check(*w > 200.0 && *w < 20000.0, "W* lands in the ~sub-10us band");
  }

  // W* with C==1 and zero presence tax: sibling always wins -> no crossover.
  {
    Profile prof;
    prof.presence_tax = 0.0;
    auto w = solve_wstar(prof, 1.0, 20.0);
    check(!w.has_value(), "no crossover when C==1 and eps==0");
  }

  // Lint: a call is a hard error; a lock op is a warning; clean asm is clean.
  {
    Lint bad = lint_region("r", "  imulq %rax, %rdx\n  call foo\n");
    check(!bad.errors.empty(), "call flagged as a lint error");
    // A lock op WARNS (not errors), given the region also has real compute so
    // the zero-compute hard error doesn't fire. Assert lk's own state, not
    // bad's (the prior sloppy re-check of bad.errors is what this replaces).
    Lint lk = lint_region("r", "  imulq %rax, %rdx\n  lock incq (%rdi)\n");
    check(!lk.warnings.empty() && lk.errors.empty(),
          "lock flagged as warning, not error");
    Lint ok = lint_region("r", "  imulq %rax, %rdx\n  addq (%rdi), %rdx\n");
    check(ok.errors.empty() && ok.warnings.empty(), "clean region is clean");
  }

  // Perturbation ratio: a ±1 induction wobble is ~0; a vectorisation flip is
  // large. is_data_movement keeps movq/vmov out of the score.
  {
    std::map<std::string, int> a{{"imulq", 8}, {"addq", 8}, {"incq", 5}};
    std::map<std::string, int> b{{"imulq", 8}, {"addq", 8}, {"incq", 6}};
    check(perturbation_ratio(a, b) < 0.10, "small induction wobble is benign");
    std::map<std::string, int> vec{{"vpmuludq", 2}, {"vpaddq", 2}};
    std::map<std::string, int> scal{{"imulq", 8}, {"addq", 8}};
    check(perturbation_ratio(vec, scal) > 0.5, "vectorisation flip is flagged");
    check(is_data_movement("movq") && is_data_movement("vmovdqa") &&
              !is_data_movement("imulq") && !is_data_movement("vpmuludq"),
          "data-movement classification");
  }

  // Region asm extraction + duplicate detection from a tiny .s.
  {
    std::string s = "foo:\n  # LLVM-MCA-BEGIN consumer\n  imulq %rax, %rdx\n"
                    "  # LLVM-MCA-END consumer\n  ret\n";
    auto m = extract_regions_asm(s);
    check(m.body.count("consumer") == 1, "extracted consumer region");
    check(m.begins["consumer"] == 1, "consumer opened once");
    check(m.body["consumer"].find("imulq") != std::string::npos,
          "region body captured");
    // Two openings of the same name (inlined twice) must be counted as 2.
    std::string dup = s +
                      "bar:\n  # LLVM-MCA-BEGIN consumer\n  addq %rax, %rbx\n"
                      "  # LLVM-MCA-END consumer\n  ret\n";
    check(extract_regions_asm(dup).begins["consumer"] == 2,
          "duplicate region opening counted");
  }

  // lint_region: empty region is a hard error; a real one is clean.
  {
    check(!lint_region("r", "  # comment only\n").errors.empty(),
          "zero-compute region flagged as error");
    Lint good = lint_region("r", "  imulq %rax, %rdx\n  addq (%rdi), %rdx\n");
    check(good.errors.empty() && good.compute == 2, "clean region: 2 compute");
    // a (non-directive) label sharing a line with an instruction is not dropped
    check(lint_region("r", "foo: imulq %rax, %rdx\n").compute == 1,
          "label+insn on one line still counts the insn");
  }

  // validate_region: a populated region passes; missing fields fail by name.
  {
    RegionProfile r;
    r.name = "x";
    r.cycles_per_iter = 8;
    r.dispatch_width = 6;
    r.instructions = 8;
    r.pressure["P1"] = 8;
    check(validate_region(r).empty(), "populated region validates");
    RegionProfile bad = r;
    bad.cycles_per_iter = 0;
    check(!validate_region(bad).empty(), "cycles=0 region rejected");
    RegionProfile empty = r;
    empty.pressure.clear();
    check(!validate_region(empty).empty(), "empty-pressure region rejected");
  }

  // validate_profile: good passes; swapped handoff / bad freq rejected.
  {
    Profile p;
    check(validate_profile(p).empty(), "default profile valid");
    Profile sw = p;
    sw.handoff_sibling_ns = 90;
    sw.handoff_sameccx_ns = 50; // Dh negative
    check(!validate_profile(sw).empty(), "negative Dh rejected");
    Profile f0 = p;
    f0.freq_ghz = 0;
    check(!validate_profile(f0).empty(), "freq_ghz=0 rejected");
  }

  // Budget band ordering: lo <= mid <= hi whenever all three are finite.
  {
    Profile prof;
    Overlay ov;
    ov.c = 1.8;
    Budget bud = solve_budget(prof, ov, 20.0);
    if (bud.lo && bud.mid && bud.hi)
      check(*bud.lo <= *bud.mid + 1e-6 && *bud.mid <= *bud.hi + 1e-6,
            "W* band ordered lo <= mid <= hi");
  }

  // duty_of exact root: at C==1 duty is the linear t/b; must be in [0,1] and
  // match a direct evaluation.
  {
    Profile prof;
    double d = duty_of(1000.0, prof, 1.0, 50.0);
    double b = prof.pacing_headroom *
               (1000.0 * (1.0 + prof.presence_tax) + prof.allowance_ns);
    check(std::fabs(d - 50.0 / b) < 1e-9, "duty exact at C==1");
    check(duty_of(1000.0, prof, 3.0, 50.0) >= 0 &&
              duty_of(1000.0, prof, 3.0, 50.0) <= 1.0,
          "duty in [0,1] at C=3");
  }

  if (fails == 0)
    printf("all tests passed\n");
  return fails ? 1 : 0;
}

// ===========================================================================
// SECTION 7 — --calibrate: reproduce a measured busy-sibling multiplier through
// the static path, and print the scale factor to put in your profile.
// ===========================================================================
static int run_calibrate(double measured_multiplier, const std::string &mcpu,
                         const std::string &mca_bin, const std::string &cxx) {
  // Emit a tiny TU whose single marked region is the sibling_noise victim: 8
  // independent imul-accumulate lanes, PURE REGISTER (no memory loads, so it
  // exercises only the multiply port and does not drag in grouped Load/LSU
  // resources whose mis-costing would contaminate the scale). Markers wrap the
  // LOOP per the tool's own contract; the marker macros are inlined so the
  // calibration doesn't depend on cwd / -I.
  const char *src = R"SRC(
#include <cstdint>
#define SIBLING_REGION_BEGIN(n) __asm__ volatile("# LLVM-MCA-BEGIN " n ::: "memory")
#define SIBLING_REGION_END(n)   __asm__ volatile("# LLVM-MCA-END " n ::: "memory")
uint64_t k(uint64_t n){
  uint64_t a=1,b=2,c=3,d=4,e=5,f=6,g=7,h=8;
  SIBLING_REGION_BEGIN("victim");
  for(uint64_t i=0;i<n;i++){
    a=a*2654435761u+i; b=b*2654435761u+i; c=c*2654435761u+i; d=d*2654435761u+i;
    e=e*2654435761u+i; f=f*2654435761u+i; g=g*2654435761u+i; h=h*2654435761u+i;
  }
  SIBLING_REGION_END("victim");
  return a+b+c+d+e+f+g+h;
}
)SRC";
  std::string tmpdir = make_tmpdir();
  if (tmpdir.empty()) {
    fprintf(stderr, "calibrate: could not create temp dir\n");
    return 1;
  }
  std::string cppfile = tmpdir + "/calib.cpp";
  std::ofstream(cppfile) << src;
  bool ok = false;
  std::string asm_on = compile_to_asm(cppfile, {}, false, tmpdir, cxx, ok);
  if (!ok) {
    remove_tmpdir(tmpdir);
    return 1;
  }
  std::string sfile = tmpdir + "/calib.s";
  std::ofstream(sfile) << asm_on;
  std::string mca, err;
  int rc = run_mca(sfile, mcpu, mca_bin, tmpdir, mca, err);
  if (!err.empty())
    fprintf(stderr, "llvm-mca (mcpu=%s) messages:\n%s\n", mcpu.c_str(),
            err.c_str());
  if (rc != 0) {
    fprintf(stderr, "llvm-mca failed (exit %d)\n", rc);
    remove_tmpdir(tmpdir);
    return 1;
  }
  auto regions = parse_mca_text(mca);
  const RegionProfile *v = find_region(regions, "victim");
  if (!v || !validate_region(*v).empty()) {
    fprintf(stderr, "calibrate: victim region not usable: %s\n",
            v ? validate_region(*v).c_str() : "not found");
    remove_tmpdir(tmpdir);
    return 1;
  }
  Profile prof;
  Overlay self = overlay(*v, *v, prof);
  double scale =
      self.c_raw > 1.0 ? (measured_multiplier - 1.0) / (self.c_raw - 1.0) : 1.0;
  printf("calibrate (mcpu=%s): victim bottleneck '%s', C_raw=%.3f\n",
         mcpu.c_str(), self.bottleneck.c_str(), self.c_raw);
  printf("measured busy-sibling multiplier: %.3f\n", measured_multiplier);
  printf("=> calib_scale = %.3f   (put `calib_scale=%.3f` in your profile)\n",
         scale, scale);
  printf("note: measured multiplier defaults to the README's Zen 5 figure "
         "(1.81); pass your own machine's sibling_noise 'hot' median / 'idle' "
         "median as the argument to --calibrate. Assumes a multiply/port-bound "
         "consumer like the victim; a memory-bound consumer needs its own "
         "measured scale.\n");
  remove_tmpdir(tmpdir);
  return 0;
}

// ===========================================================================
// main
// ===========================================================================
static void usage(const char *a0) {
  fprintf(stderr,
          "usage:\n"
          "  %s <source.cpp> [--producer NAME] [--consumer NAME] "
          "[--profile FILE] [--cflags TOK]... [--consumer-iters-per-msg N] "
          "[--producer-iters-per-msg N] [--mca-bin PATH] [--cxx PATH] "
          "[--json|--emit-model]\n"
          "  %s --mca-file <llvm-mca-dump.txt> [--producer NAME] "
          "[--consumer NAME] [--profile FILE] [--json]\n"
          "  %s --calibrate [MEASURED_MULTIPLIER] [--profile FILE] "
          "[--mca-bin PATH] [--cxx PATH]\n"
          "  %s --test\n"
          "\n"
          "  --mca-bin PATH   llvm-mca binary to invoke (default: "
          "$SIBLING_ANALYZE_MCA if set, else \"llvm-mca\"). Distros often ship "
          "only a versioned name (llvm-mca-20) with no bare symlink.\n"
          "  --cxx PATH       C++ compiler driver (default: $CXX if set, else "
          "\"g++\").\n",
          a0, a0, a0, a0);
}

int main(int argc, char **argv) {
  if (argc >= 2 && std::strcmp(argv[1], "--test") == 0)
    return run_self_tests();

  // Shared arg parse (also used by --calibrate for --profile).
  std::string source, mca_file, profile_path;
  std::string producer = "producer", consumer = "consumer";
  std::vector<std::string> cflags{"-I."};
  bool as_json = false, emit_model_csv = false, calibrate = false;
  double calib_measured = 1.81;
  Iters iters;
  // Defaults for the mca binary / compiler driver: an explicit --flag wins
  // over the environment, which wins over the bare-name fallback.
  std::string mca_bin = "llvm-mca";
  if (const char *e = std::getenv("SIBLING_ANALYZE_MCA"); e && *e)
    mca_bin = e;
  std::string cxx = "g++";
  if (const char *e = std::getenv("CXX"); e && *e)
    cxx = e;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    auto next = [&](const char *what) -> std::string {
      if (i + 1 >= argc) {
        fprintf(stderr, "missing value after %s\n", what);
        exit(1);
      }
      return argv[++i];
    };
    if (a == "--calibrate") {
      calibrate = true;
      if (i + 1 < argc && argv[i + 1][0] != '-')
        calib_measured = std::atof(argv[++i]);
    } else if (a == "--producer")
      producer = next("--producer");
    else if (a == "--consumer")
      consumer = next("--consumer");
    else if (a == "--profile")
      profile_path = next("--profile");
    else if (a == "--cflags")
      cflags.push_back(next("--cflags"));
    else if (a == "--mca-file")
      mca_file = next("--mca-file");
    else if (a == "--mca-bin")
      mca_bin = next("--mca-bin");
    else if (a == "--cxx")
      cxx = next("--cxx");
    else if (a == "--consumer-iters-per-msg") {
      iters.cons = std::atof(next("--consumer-iters-per-msg").c_str());
      iters.explicit_msg = true;
    } else if (a == "--producer-iters-per-msg") {
      iters.prod = std::atof(next("--producer-iters-per-msg").c_str());
      iters.prod_explicit = true;
    } else if (a == "--json")
      as_json = true;
    else if (a == "--emit-model")
      emit_model_csv = true;
    else if (a[0] == '-') {
      fprintf(stderr, "unknown flag %s\n", a.c_str());
      usage(argv[0]);
      return 1;
    } else
      source = a;
  }

  Profile prof = profile_path.empty() ? Profile() : load_profile(profile_path);
  if (std::string e = validate_profile(prof); !e.empty()) {
    fprintf(stderr, "fatal: invalid profile: %s\n", e.c_str());
    return 1;
  }
  if (iters.cons <= 0 || iters.prod <= 0) {
    fprintf(stderr, "fatal: --*-iters-per-msg must be > 0\n");
    return 1;
  }

  if (calibrate) {
    // A busy-sibling multiplier is >1 by construction (contention slows you
    // down). A non-numeric optional arg (e.g. a stray filename) atof's to 0 and
    // would print a nonsensical negative scale — reject it.
    if (!(calib_measured > 1.0)) {
      fprintf(stderr,
              "fatal: --calibrate multiplier must be > 1.0 (measured busy / "
              "idle ratio); got %g\n",
              calib_measured);
      return 1;
    }
    return run_calibrate(calib_measured, prof.mca_mcpu, mca_bin, cxx);
  }

  if (source.empty() && mca_file.empty()) {
    usage(argv[0]);
    return 1;
  }
  if (profile_path.empty())
    fprintf(stderr,
            "note: no --profile given; using PLACEHOLDER machine "
            "constants (Dh=%.0f ns, eps=%.2f). Generate a real profile "
            "from smt_pingpong/sibling_noise on your target.\n",
            prof.Dh(), prof.presence_tax);

  std::string mca_text, tmpdir;
  // Collected region-lint warnings (e.g. "shrink region to the true
  // steady-state body"), re-surfaced next to the final verdict in
  // print_report() so the caveat travels with the number instead of being
  // lost earlier in the build/lint output above. Empty on the --mca-file
  // path, which skips the asm lint entirely (see the note printed below).
  std::vector<std::string> lint_warnings;
  if (!mca_file.empty()) {
    // A pre-saved dump has no source asm, so the region lint (call/atomic/
    // empty-region checks) and the perturbation diff cannot run — only the
    // post-parse validate_region below applies. Note it so the weaker guarantee
    // is explicit.
    fprintf(stderr, "note: --mca-file skips the asm lint and perturbation "
                    "check (no source); only region validation applies.\n");
    mca_text = slurp_file(mca_file);
  } else {
    tmpdir = make_tmpdir();
    if (tmpdir.empty()) {
      fprintf(stderr, "fatal: could not create temp dir\n");
      return 1;
    }
    bool ok = false;
    std::string asm_on = compile_to_asm(source, cflags, false, tmpdir, cxx, ok);
    if (!ok) {
      remove_tmpdir(tmpdir);
      return 1;
    }
    std::string sfile = tmpdir + "/on.s";
    std::ofstream(sfile) << asm_on;

    // Perturbation diff, scoped to the marked functions only (so a regional
    // vectorisation loss isn't diluted by the rest of the TU).
    std::string asm_off = compile_to_asm(source, cflags, true, tmpdir, cxx, ok);
    if (ok) {
      double pr = marked_function_perturbation(asm_on, asm_off);
      if (pr > 0.15)
        fprintf(stderr,
                "\n*** WARNING: markers perturbed codegen — %.0f%% of compute "
                "instructions differ (in the marked functions) between the "
                "marked and unmarked builds (likely blocked vectorisation or "
                "hoisting). The analysed code may not be what ships; move "
                "markers to wrap the loop, not its body. ***\n",
                pr * 100.0);
    }

    // Region lint + duplicate-region detection.
    auto region_asm = extract_regions_asm(asm_on);
    bool fatal = false;
    for (auto &nm : {producer, consumer}) {
      if (region_asm.begins.count(nm) && region_asm.begins.at(nm) > 1) {
        fprintf(stderr,
                "LINT ERROR: region '%s' is opened %d times (inlined at "
                "several sites, or duplicate markers) — mca emits several "
                "same-named regions and the analysis cannot tell which is "
                "'the' region. Give each an unambiguous name.\n",
                nm.c_str(), region_asm.begins.at(nm));
        fatal = true;
        continue;
      }
      auto it = region_asm.body.find(nm);
      if (it == region_asm.body.end()) {
        fprintf(stderr,
                "warning: region '%s' not found in the compiled asm — "
                "did you mark it?\n",
                nm.c_str());
        continue;
      }
      Lint L = lint_region(nm, it->second);
      for (auto &e : L.errors) {
        fprintf(stderr, "LINT ERROR: %s\n", e.c_str());
        fatal = true;
      }
      for (auto &w : L.warnings) {
        fprintf(stderr, "lint warning: %s\n", w.c_str());
        lint_warnings.push_back(w);
      }
    }
    if (fatal) {
      fprintf(stderr, "aborting: a region's port vector is untrustworthy (see "
                      "LINT ERROR above).\n");
      remove_tmpdir(tmpdir);
      return 1;
    }

    std::string err;
    int rc = run_mca(sfile, prof.mca_mcpu, mca_bin, tmpdir, mca_text, err);
    fprintf(stderr, "llvm-mca (%s): -mcpu=%s", mca_bin.c_str(),
            prof.mca_mcpu.c_str());
    if (!err.empty())
      fprintf(stderr, " (messages: %s)", trim(err).c_str());
    fprintf(stderr, "\n");
    if (rc != 0) {
      fprintf(stderr, "llvm-mca failed (exit %d)\n", rc);
      remove_tmpdir(tmpdir);
      return 1;
    }
  }

  auto regions = parse_mca_text(mca_text);
  const RegionProfile *prod = find_region(regions, producer);
  const RegionProfile *cons = find_region(regions, consumer);
  if (!prod || !cons) {
    fprintf(stderr,
            "error: could not find both regions '%s' and '%s' in the mca "
            "output (found %zu region(s)). Check your marker names.\n",
            producer.c_str(), consumer.c_str(), regions.size());
    remove_tmpdir(tmpdir);
    return 1;
  }
  // Post-parse validation: refuse to emit a confident verdict from a region mca
  // didn't actually populate (closes the fail-unsafe paths).
  for (const RegionProfile *r : {prod, cons}) {
    if (std::string e = validate_region(*r); !e.empty()) {
      fprintf(stderr, "error: region %s\n", e.c_str());
      remove_tmpdir(tmpdir);
      return 1;
    }
  }

  Overlay o = overlay(*prod, *cons, prof);
  Budget b = solve_budget(prof, o, producer_busy_ns(*prod, prof, iters.prod));

  if (emit_model_csv)
    emit_model(*prod, prof, o, b, iters);
  else if (as_json)
    print_json(*prod, *cons, prof, o, b, iters);
  else
    print_report(*prod, *cons, prof, o, b, iters, lint_warnings);
  remove_tmpdir(tmpdir);
  return 0;
}
