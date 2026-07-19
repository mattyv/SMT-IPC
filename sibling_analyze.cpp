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

  double Dh() const { return handoff_sameccx_ns - handoff_sibling_ns; }
};

// One llvm-mca "Code Region": the fields we need from its summary plus the
// per-iteration pressure on every named processor resource.
struct RegionProfile {
  std::string name;           // region name, or "" for an unnamed region
  double cycles_per_iter = 0; // Block RThroughput
  double dispatch_width = 0;  // Dispatch Width
  double uops_per_cycle = 0;  // uOps Per Cycle
  long instructions = 0;      // total (all iterations)
  long iterations = 0;
  std::map<std::string, double>
      pressure; // resource name -> per-iteration cycles
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
// Parses the default `llvm-mca` textual report. We target the LLVM 18 format
// (pinned in CMake/CI); the fields we read — "Code Region", the summary block,
// the "Resources:" index->name table, and "Resource pressure per iteration:" —
// have been stable across many releases, but the version is pinned so a format
// drift is a loud build failure rather than a silent misparse.
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
      else if (std::smatch rm; std::regex_match(line, rm, res_decl))
        idx2name[rm[1].str()] = rm[2].str();
      else if (line.find("Resource pressure per iteration:") !=
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

// Per-resource utilisation for one region: pressure/cycles, plus the synthetic
// "dispatch" resource = uOpsPerCycle/DispatchWidth (front-end/retire width).
static std::map<std::string, double> utilisation(const RegionProfile &r) {
  std::map<std::string, double> u;
  double cyc = r.cycles_per_iter > 0 ? r.cycles_per_iter : 1.0;
  for (auto &[name, p] : r.pressure)
    u[name] = p / cyc;
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

// Producer busy-time per message, ns: its cycles-per-iteration converted to ns.
// (One region iteration == one message's worth of producer work, by the marking
// contract: the producer's marked loop body is the per-message publish path.)
static double producer_busy_ns(const RegionProfile &producer,
                               const Profile &prof) {
  return producer.cycles_per_iter / prof.freq_ghz;
}

// duty(W): fraction of the consumer's window during which the producer is hot.
// Fixed point: the message period is headroom*(contended consumer work +
// allowance); duty = producer_busy / period; the contended work depends on duty
// through C_eff. A handful of iterations converge (period is monotone in duty).
static double duty_of(double W_ns, const Profile &prof, double C,
                      double t_prod_ns) {
  double duty = 0.0;
  for (int it = 0; it < 32; it++) {
    double c_eff = 1.0 + prof.presence_tax + duty * (C - 1.0);
    double period = prof.pacing_headroom * (W_ns * c_eff + prof.allowance_ns);
    double next = period > 0 ? t_prod_ns / period : 0.0;
    if (next > 1.0)
      next = 1.0; // producer cannot be busy more than all the time
    if (std::fabs(next - duty) < 1e-9) {
      duty = next;
      break;
    }
    duty = next;
  }
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
  pw.presence_tax = prof.presence_tax * 1.5;
  double c_hi = 1.0 + 1.3 * (o.c - 1.0);
  b.lo = solve_wstar(pw, c_hi, t_prod_ns);
  // High budget (sibling looks better): weaker contention, smaller tax.
  Profile pb = prof;
  pb.presence_tax = prof.presence_tax * 0.5;
  double c_lo = 1.0 + 0.7 * (o.c - 1.0);
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
};

// `region_asm` is the raw asm text between a region's BEGIN and END markers.
static Lint lint_region(const std::string &name,
                        const std::string &region_asm) {
  Lint L;
  std::istringstream is(region_asm);
  int calls = 0, branches = 0, atomics = 0;
  for (std::string line; std::getline(is, line);) {
    std::string t = trim(line);
    if (t.empty() || t[0] == '.' || t[0] == '#')
      continue; // directive / comment / label
    if (t.back() == ':')
      continue; // label
    auto toks = tokens(t);
    if (toks.empty())
      continue;
    std::string op = toks[0];
    // lock-prefixed atomic: "lock" then the real op.
    bool locked = (op == "lock");
    if (locked && toks.size() > 1)
      op = toks[1];
    if (op.rfind("call", 0) == 0)
      calls++;
    else if (op.size() >= 2 && op[0] == 'j' && op != "jmp")
      branches++; // conditional jump (jne/je/jl/...)
    else if (locked || op.rfind("xchg", 0) == 0 || op.rfind("mfence", 0) == 0 ||
             op.rfind("lfence", 0) == 0 || op.rfind("sfence", 0) == 0)
      atomics++;
  }
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

// Run a command, capture stdout. Returns exit status; fills `out`.
static int run_capture(const std::string &cmd, std::string &out) {
  out.clear();
  FILE *p = popen(cmd.c_str(), "r");
  if (!p)
    return -1;
  char buf[4096];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), p)) > 0)
    out.append(buf, n);
  return pclose(p);
}

// Extract the raw asm text of each named region from a compiled .s: everything
// between "# LLVM-MCA-BEGIN name" and the matching "# LLVM-MCA-END".
static std::map<std::string, std::string>
extract_regions_asm(const std::string &asm_text) {
  std::map<std::string, std::string> out;
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
      continue;
    }
    if (line.find("LLVM-MCA-END") != std::string::npos) {
      if (in)
        out[cur] += body;
      in = false;
      continue;
    }
    if (in)
      body += line + "\n";
  }
  return out;
}

// Histogram of instruction mnemonics over a whole .s (skips directives, labels,
// comments). Used for the marked-vs-unmarked perturbation diff.
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
    if (!toks.empty())
      h[toks[0]]++;
  }
  return h;
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
    if (eq == std::string::npos)
      continue;
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
  }
  return p;
}

// Compile a TU to asm (-S). `markers_off` selects the unmarked build. Returns
// the .s text, or empty + sets ok=false on compile failure.
static std::string compile_to_asm(const std::string &src,
                                  const std::string &extra_cflags,
                                  bool markers_off, bool &ok) {
  std::string sfile =
      "/tmp/sibling_analyze_" + std::string(markers_off ? "off" : "on") + ".s";
  std::string cmd = "g++ -O3 -std=c++23 -march=native -S " + extra_cflags +
                    (markers_off ? " -DSIBLING_MARKERS_OFF" : "") + " " + src +
                    " -o " + sfile + " 2>&1";
  std::string out;
  int rc = run_capture(cmd, out);
  if (rc != 0) {
    fprintf(stderr, "compile failed:\n%s\n", out.c_str());
    ok = false;
    return "";
  }
  ok = true;
  return slurp_file(sfile);
}

static const RegionProfile *find_region(const std::vector<RegionProfile> &rs,
                                        const std::string &name) {
  for (auto &r : rs)
    if (r.name == name)
      return &r;
  return nullptr;
}

// ---------------------------------------------------------------------------
// Human-readable report + machine-readable JSON.
// ---------------------------------------------------------------------------
static void print_report(const RegionProfile &prod, const RegionProfile &cons,
                         const Profile &prof, const Overlay &o,
                         const Budget &b) {
  auto ns = [&](double cyc) { return cyc / prof.freq_ghz; };
  double W_cons = ns(cons.cycles_per_iter);
  double t_prod = producer_busy_ns(prod, prof);

  printf("\n=== sibling_analyze — STATIC, ports+dispatch only ===\n");
  printf("producer '%s': %.1f cyc/msg (%.1f ns @ %.2f GHz)\n",
         prod.name.c_str(), prod.cycles_per_iter, t_prod, prof.freq_ghz);
  printf("consumer '%s': %.1f cyc/msg (%.1f ns), bottleneck resource below\n",
         cons.name.c_str(), cons.cycles_per_iter, W_cons);

  printf("\ncombined demand (producer+consumer utilisation, >1.0 = "
         "oversubscribed):\n");
  for (auto &[k, d] : o.top)
    printf("    %-22s %.2f%s\n", k.c_str(), d,
           k == o.bottleneck ? "   <== bottleneck" : "");

  printf("\nfriction: C_raw=%.2f  -> C=%.2f (calib_scale=%.2f)\n", o.c_raw, o.c,
         prof.calib_scale);
  if (o.collides)
    printf("VERDICT: COLLIDES on '%s' (combined demand %.2f). The two threads "
           "fight for this resource on a shared core.\n",
           o.bottleneck.c_str(), o.bottleneck_demand);
  else
    printf(
        "VERDICT: no PORT/DISPATCH collision (max demand %.2f). NOTE: this "
        "checks execution ports + front end ONLY — cache bandwidth, MSHRs, "
        "store buffer and L1 aliasing are unchecked. Confirm with "
        "sibling_noise/spsc_pipeline if either region touches >L1 data or is "
        "store-heavy.\n",
        o.bottleneck_demand);

  printf("\nplacement budget W* (consumer ns/msg below which the sibling "
         "wins):\n");
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

  if (b.mid) {
    if (W_cons < *b.mid)
      printf(
          "\n=> your consumer does ~%.0f ns/msg < W*~%.0f ns: SIBLING is the "
          "faster placement (subject to the memory caveat above).\n",
          W_cons, *b.mid);
    else
      printf(
          "\n=> your consumer does ~%.0f ns/msg >= W*~%.0f ns: step OUT to a "
          "same-CCX core; on-core contention now outweighs the handoff edge.\n",
          W_cons, *b.mid);
  } else {
    printf("\n=> no crossover in range: for this pair the sibling's handoff "
           "edge is not overtaken by contention up to 1 ms/msg of work.\n");
  }
}

static void print_json(const RegionProfile &prod, const RegionProfile &cons,
                       const Profile &prof, const Overlay &o, const Budget &b) {
  auto jopt = [](const std::optional<double> &w) {
    return w ? std::to_string((long)llround(*w)) : std::string("null");
  };
  printf("{\n");
  printf("  \"producer\": \"%s\", \"consumer\": \"%s\",\n", prod.name.c_str(),
         cons.name.c_str());
  printf("  \"producer_ns_per_msg\": %.1f,\n", producer_busy_ns(prod, prof));
  printf("  \"consumer_ns_per_msg\": %.1f,\n",
         cons.cycles_per_iter / prof.freq_ghz);
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
Total Cycles:      205
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
Total Cycles:      808
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
    check(o.bottleneck_demand >= 1.0, "some resource at/over 1.0");
    // The dispatch row: consumer 0.99/6 + producer 1.95/6 = ~0.49, no
    // collision.
    check(!o.collides || o.bottleneck != "SKXPort1" ||
              o.bottleneck_demand < 1.1,
          "disjoint-bottleneck pair not flagged as a hard Port1 collision");

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
    Lint lk = lint_region("r", "  lock incq (%rdi)\n");
    check(bad.errors.size() && lk.warnings.size(), "lock flagged as warning");
    Lint ok = lint_region("r", "  imulq %rax, %rdx\n  addq (%rdi), %rdx\n");
    check(ok.errors.empty() && ok.warnings.empty(), "clean region is clean");
  }

  // Region asm extraction from a tiny .s.
  {
    std::string s = "foo:\n  # LLVM-MCA-BEGIN consumer\n  imulq %rax, %rdx\n"
                    "  # LLVM-MCA-END consumer\n  ret\n";
    auto m = extract_regions_asm(s);
    check(m.count("consumer") == 1, "extracted consumer region");
    check(m["consumer"].find("imulq") != std::string::npos,
          "region body captured");
  }

  if (fails == 0)
    printf("all tests passed\n");
  return fails ? 1 : 0;
}

// ===========================================================================
// SECTION 7 — --calibrate: reproduce a measured busy-sibling multiplier through
// the static path, and print the scale factor to put in your profile.
// ===========================================================================
static int run_calibrate(double measured_multiplier) {
  // Emit a tiny TU whose single marked region is the sibling_noise victim: 8
  // independent imul-accumulate lanes. Overlaying it with itself is the
  // static analogue of "victim on one thread, identical busy tenant on its
  // sibling" — the experiment that measured ~1.8x.
  const char *src = R"SRC(
#include <cstdint>
#include "sibling_marks.hpp"
uint64_t k(const uint64_t* p, int n){
  uint64_t a=1,b=2,c=3,d=4,e=5,f=6,g=7,h=8;
  for(int i=0;i<n;i+=8){ SIBLING_REGION_BEGIN("victim");
    a=a*2654435761u+p[i]; b=b*2654435761u+p[i+1]; c=c*2654435761u+p[i+2]; d=d*2654435761u+p[i+3];
    e=e*2654435761u+p[i+4]; f=f*2654435761u+p[i+5]; g=g*2654435761u+p[i+6]; h=h*2654435761u+p[i+7];
    SIBLING_REGION_END("victim"); }
  return a+b+c+d+e+f+g+h;
}
)SRC";
  std::ofstream("/tmp/sibling_calib.cpp") << src;
  bool ok = false;
  std::string asm_on =
      compile_to_asm("/tmp/sibling_calib.cpp", "-I.", false, ok);
  if (!ok)
    return 1;
  std::ofstream("/tmp/sibling_calib.s") << asm_on;
  std::string mca;
  if (run_capture("llvm-mca -mcpu=native /tmp/sibling_calib.s 2>&1", mca) !=
      0) {
    fprintf(stderr, "llvm-mca failed:\n%s\n", mca.c_str());
    return 1;
  }
  auto regions = parse_mca_text(mca);
  const RegionProfile *v = find_region(regions, "victim");
  if (!v) {
    fprintf(stderr, "calibrate: victim region not found in mca output\n");
    return 1;
  }
  Profile prof;
  Overlay self = overlay(*v, *v, prof);
  double scale =
      self.c_raw > 1.0 ? (measured_multiplier - 1.0) / (self.c_raw - 1.0) : 1.0;
  printf("calibrate: victim bottleneck '%s', C_raw=%.3f\n",
         self.bottleneck.c_str(), self.c_raw);
  printf("measured busy-sibling multiplier: %.3f\n", measured_multiplier);
  printf("=> calib_scale = %.3f   (put `calib_scale=%.3f` in your profile)\n",
         scale, scale);
  printf("note: measured multiplier defaults to the README's Zen 5 figure "
         "(1.81); pass your own machine's sibling_noise 'hot' median / 'idle' "
         "median as the argument to --calibrate.\n");
  return 0;
}

// ===========================================================================
// main
// ===========================================================================
static void usage(const char *a0) {
  fprintf(stderr,
          "usage:\n"
          "  %s <source.cpp> [--producer NAME] [--consumer NAME] "
          "[--profile FILE] [--cflags \"...\"] [--json]\n"
          "  %s --mca-file <llvm-mca-dump.txt> [--producer NAME] "
          "[--consumer NAME] [--profile FILE] [--json]\n"
          "  %s --calibrate [MEASURED_MULTIPLIER]\n"
          "  %s --test\n",
          a0, a0, a0, a0);
}

int main(int argc, char **argv) {
  if (argc >= 2 && std::strcmp(argv[1], "--test") == 0)
    return run_self_tests();
  if (argc >= 2 && std::strcmp(argv[1], "--calibrate") == 0) {
    double m = (argc >= 3) ? std::atof(argv[2]) : 1.81;
    return run_calibrate(m);
  }
  if (argc < 2) {
    usage(argv[0]);
    return 1;
  }

  std::string source, mca_file, profile_path, cflags = "-I.";
  std::string producer = "producer", consumer = "consumer";
  bool as_json = false;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    auto next = [&](const char *what) -> std::string {
      if (i + 1 >= argc) {
        fprintf(stderr, "missing value after %s\n", what);
        exit(1);
      }
      return argv[++i];
    };
    if (a == "--producer")
      producer = next("--producer");
    else if (a == "--consumer")
      consumer = next("--consumer");
    else if (a == "--profile")
      profile_path = next("--profile");
    else if (a == "--cflags")
      cflags += " " + next("--cflags");
    else if (a == "--mca-file")
      mca_file = next("--mca-file");
    else if (a == "--json")
      as_json = true;
    else if (a[0] == '-') {
      fprintf(stderr, "unknown flag %s\n", a.c_str());
      usage(argv[0]);
      return 1;
    } else
      source = a;
  }

  Profile prof = profile_path.empty() ? Profile() : load_profile(profile_path);
  if (profile_path.empty())
    fprintf(stderr,
            "note: no --profile given; using PLACEHOLDER machine "
            "constants (Dh=%.0f ns, eps=%.2f). Generate a real profile "
            "from smt_pingpong/sibling_noise on your target.\n",
            prof.Dh(), prof.presence_tax);

  std::string mca_text;
  if (!mca_file.empty()) {
    mca_text = slurp_file(mca_file);
  } else if (!source.empty()) {
    bool ok = false;
    std::string asm_on = compile_to_asm(source, cflags, false, ok);
    if (!ok)
      return 1;
    std::ofstream("/tmp/sibling_analyze.s") << asm_on;

    // Perturbation diff: unmarked build vs marked, whole-file mnemonic mix.
    std::string asm_off = compile_to_asm(source, cflags, true, ok);
    if (ok) {
      auto h_on = mnemonic_histogram(asm_on);
      auto h_off = mnemonic_histogram(asm_off);
      if (h_on != h_off)
        fprintf(stderr,
                "\n*** WARNING: marked and unmarked builds have different "
                "instruction mixes — the markers perturbed codegen (likely "
                "blocked vectorisation or hoisting). The analysed code may not "
                "be what ships. Move markers to wrap the loop, not its body. "
                "***\n");
    }

    // Region lint.
    auto region_asm = extract_regions_asm(asm_on);
    bool fatal = false;
    for (auto &nm : {producer, consumer}) {
      auto it = region_asm.find(nm);
      if (it == region_asm.end()) {
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
      for (auto &w : L.warnings)
        fprintf(stderr, "lint warning: %s\n", w.c_str());
    }
    if (fatal) {
      fprintf(stderr, "aborting: a region's port vector is untrustworthy (see "
                      "LINT ERROR above).\n");
      return 1;
    }

    std::string rc;
    if (run_capture("llvm-mca -mcpu=native /tmp/sibling_analyze.s 2>&1", rc) !=
        0) {
      fprintf(stderr, "llvm-mca failed:\n%s\n", rc.c_str());
      return 1;
    }
    mca_text = rc;
  } else {
    usage(argv[0]);
    return 1;
  }

  auto regions = parse_mca_text(mca_text);
  const RegionProfile *prod = find_region(regions, producer);
  const RegionProfile *cons = find_region(regions, consumer);
  if (!prod || !cons) {
    fprintf(stderr,
            "error: could not find both regions '%s' and '%s' in the mca "
            "output (found %zu region(s)). Check your marker names.\n",
            producer.c_str(), consumer.c_str(), regions.size());
    return 1;
  }

  Overlay o = overlay(*prod, *cons, prof);
  Budget b = solve_budget(prof, o, producer_busy_ns(*prod, prof));

  if (as_json)
    print_json(*prod, *cons, prof, o, b);
  else
    print_report(*prod, *cons, prof, o, b);
  return 0;
}
