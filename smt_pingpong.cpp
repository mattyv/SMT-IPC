// ============================================================================
// smt_pingpong.cpp — cache-line ping-pong latency benchmark
// ----------------------------------------------------------------------------
// Measures the round-trip latency of handing one cache line back and forth
// between two CPUs, and reports the distribution (min / p50 / percentiles).
// The point is to compare three placements on a modern (AMD Zen) box:
//
//     * SMT sibling  — two hardware threads on the SAME physical core.
//                      They share L1/L2, so the line never leaves the core.
//     * same L3/CCX  — two DISTINCT cores that share an L3 slice (a "CCX").
//                      The line moves core->core but stays inside one L3.
//     * cross CCX    — two cores in different L3 domains. The line crosses
//                      the on-die interconnect: the expensive case.
//
// It also runs two spin-wait variants so you can separate the cost of the
// spin itself from the cost of the cache coherence traffic:
//
//     * "pause"     — spin loops issue _mm_pause (PAUSE / rep-nop). Realistic
//                     for a polite spinner, but PAUSE parks the core for
//                     ~64 cycles on Zen 2+, so it ADDS detection latency: if
//                     the peer's store lands mid-PAUSE you don't see it until
//                     the PAUSE finishes.
//     * "bare-spin" — spin loops just re-load in a tight loop. No PAUSE delay,
//                     so the number is closer to raw coherence latency, but
//                     the tight loop hammers the load unit and eats a branch
//                     mispredict on exit.
//
// The SMT-sibling pair is only ever run with "pause": a bare spin on one
// sibling starves the shared core's execution ports that the OTHER sibling
// needs to do its store, so it would measure self-inflicted starvation rather
// than handoff latency.
//
// The shared measurement core (Cfg, Slot, bench<>, calibration, sysfs
// topology helpers) lives in pp_core.hpp so a second experiment
// (sibling_noise.cpp) can reuse it without duplication — see that header for
// the primitives' own comments.
//
// build: g++ -O3 -std=c++23 -pthread smt_pingpong.cpp -o smt_pingpong
// run:   ./smt_pingpong          (auto-picks representative pairs from /sys)
//        ./smt_pingpong 0 12     (explicit cpu pair, both variants)
//
// Recommended environment for stable tails:
//   * SMT enabled (so a sibling pair even exists)
//   * turbo/boost OFF and governor=performance (so the clock doesn't drift
//     mid-run and smear the distribution)
//   * ideally isolcpus / nohz_full / IRQ affinity off the pair, else the big
//     p99.99/max outliers you see are just the OS scheduler preempting the
//     spin loop, not anything about the hardware.
// ============================================================================

#include "pp_core.hpp"

// Pure format/range check for a CPU-id command-line argument, split out of
// parse_cpu_arg() so it's unit-testable (parse_cpu_arg itself exits on
// failure, which run_self_tests() can't observe). Accepts only a bare
// sequence of ASCII digits — the leading isdigit check rejects "+5" and " 5",
// which strtol alone would silently accept via its own sign/whitespace
// skipping, inconsistent with is_all_digits()'s stricter token format.
//
// Critically, the v > INT_MAX check runs BEFORE any narrowing to int: strtol
// returns a 64-bit long here, so a value like 2^32 would pass a naive
// `v >= 0` check and then silently wrap to 0 on `int(v)`, turning
// "./smt_pingpong 4294967296 1" into a bogus-but-plausible (0, 1) run instead
// of a loud failure.
static bool is_valid_cpu_number(const char *s, long *out) {
  if (!std::isdigit(static_cast<unsigned char>(s[0])))
    return false;
  errno = 0;
  char *end = nullptr;
  long v = strtol(s, &end, 10);
  if (*end != '\0' || errno == ERANGE || v < 0 || v > INT_MAX)
    return false;
  *out = v;
  return true;
}

// Parse a required CPU-id command-line argument. Fails fast (exit 1) with an
// explained message rather than silently misbehaving on garbage input: `atoi`
// would map "abc" or "-3" to a number and let the run proceed on a bogus pair.
static int parse_cpu_arg(const char *s, const std::vector<int> &online) {
  long v;
  if (!is_valid_cpu_number(s, &v)) {
    fprintf(stderr, "error: '%s' is not a valid non-negative CPU number\n", s);
    exit(1);
  }
  if (!contains(online, int(v))) {
    fprintf(stderr, "error: cpu%ld is not an online CPU\n", v);
    exit(1);
  }
  return int(v);
}

// ---------------------------------------------------------------------------
// Minimal, framework-free self-check: `./smt_pingpong --test`. Exercises the
// pure-logic helpers (parse_list, pct) in isolation — no threads, no /sys, no
// timing hardware needed, so it runs the same on any box / in CI.
// ---------------------------------------------------------------------------
static int run_self_tests() {
  auto expect_vec = [](const char *what, const std::vector<int> &got,
                       const std::vector<int> &want) {
    if (got != want) {
      fprintf(stderr, "FAIL %s\n", what);
      exit(1);
    }
  };
  // Exact rational arithmetic in every case exercised below (integer inputs,
  // halves), so bit-for-bit equality is the right check — no float slop.
  auto expect_eq = [](const char *what, double got, double want) {
    if (got != want) {
      fprintf(stderr, "FAIL %s: got %.6f want %.6f\n", what, got, want);
      exit(1);
    }
  };

  expect_vec("parse_list(\"0,12\")", parse_list("0,12"), {0, 12});
  expect_vec("parse_list(\"0-3,8-11\")", parse_list("0-3,8-11"),
             {0, 1, 2, 3, 8, 9, 10, 11});
  expect_vec("parse_list(\"\")", parse_list(""), {});
  expect_vec("parse_list(\"7\")", parse_list("7"), {7});

  auto expect_bool = [](const char *what, bool got, bool want) {
    if (got != want) {
      fprintf(stderr, "FAIL %s\n", what);
      exit(1);
    }
  };
  long v = -1;
  expect_bool("is_valid_cpu_number(\"5\")", is_valid_cpu_number("5", &v), true);
  expect_eq("is_valid_cpu_number(\"5\") value", double(v), 5.0);
  expect_bool("is_valid_cpu_number(\"0\")", is_valid_cpu_number("0", &v), true);
  // The bug this guards against: strtol(2^32) fits in a 64-bit long and would
  // pass a naive `v >= 0` check, then silently wrap to 0 when narrowed to
  // int — this must be rejected before narrowing, not after.
  expect_bool("is_valid_cpu_number(\"4294967296\") (2^32, overflows int)",
              is_valid_cpu_number("4294967296", &v), false);
  expect_bool("is_valid_cpu_number(\"+5\") (leading sign)",
              is_valid_cpu_number("+5", &v), false);
  expect_bool("is_valid_cpu_number(\" 5\") (leading space)",
              is_valid_cpu_number(" 5", &v), false);
  expect_bool("is_valid_cpu_number(\"-1\") (negative)",
              is_valid_cpu_number("-1", &v), false);
  expect_bool("is_valid_cpu_number(\"abc\") (non-numeric)",
              is_valid_cpu_number("abc", &v), false);
  expect_bool("is_valid_cpu_number(\"\") (empty)", is_valid_cpu_number("", &v),
              false);
  expect_bool("is_valid_cpu_number(\"12x\") (trailing junk)",
              is_valid_cpu_number("12x", &v), false);

  g_tsc_ghz = 1.0; // 1 cycle == 1 ns, so expected pct() values are exact
  std::vector<uint32_t> sorted{10, 20, 30, 40};
  expect_eq("pct(sorted, 0)", pct(sorted, 0), 10.0);
  expect_eq("pct(sorted, 100)", pct(sorted, 100), 40.0);
  expect_eq("pct(sorted, 50)", pct(sorted, 50), 25.0); // linear interpolation
  expect_eq("pct({42}, 50)", pct(std::vector<uint32_t>{42}, 50), 42.0);
  expect_eq("pct({}, 50)", pct(std::vector<uint32_t>{}, 50), 0.0);

  printf("all tests passed\n");
  return 0;
}

// ---------------------------------------------------------------------------
// Run one labelled pair through warm-up + recorded pass, then print the
// distribution. Templated on Pause so the caller picks the spin variant.
// ---------------------------------------------------------------------------
template <bool Pause> static void run(const char *label, int cpu_a, int cpu_b) {
  if (cpu_a < 0 || cpu_b < 0) { // topology didn't yield such a partner
    printf("%-30s (no such pair)\n", label);
    return;
  }
  if (cpu_a == cpu_b) { // backstop for an auto-mode tier that degenerated to
                        // the anchor CPU (explicit mode is already rejected
                        // in main() before this point, so this should only
                        // ever fire from auto-mode topology discovery)
    fprintf(stderr,
            "%-30s error: cpu%d == cpu%d, refusing to run a same-CPU pair\n",
            label, cpu_a, cpu_b);
    return;
  }
  std::vector<uint32_t> s(Cfg::SAMPLE_ITERS); // pre-sized, pre-touched
  bench<Pause>(cpu_a, cpu_b, Cfg::WARM_ITERS, nullptr); // discard
  bench<Pause>(cpu_a, cpu_b, Cfg::SAMPLE_ITERS, &s);    // record

  std::sort(s.begin(), s.end()); // percentiles need sorted input
  // Mean in double is exact here: 2M samples * ~few-thousand cycles stays well
  // under 2^53, so no precision loss accumulating the sum.
  double mean = 0;
  for (uint32_t c : s)
    mean += c;
  mean /= s.size();
  mean /= g_tsc_ghz; // cycles -> ns

  printf("%-30s cpu%-3d<->cpu%-3d\n", label, cpu_a, cpu_b);
  printf("    min %6.1f  p50 %6.1f  mean %6.1f  p90 %6.1f  p99 %6.1f  p99.9 "
         "%7.1f  p99.99 %8.1f  max %9.1f  (ns RTT)\n",
         pct(s, 0), pct(s, 50), mean, pct(s, 90), pct(s, 99), pct(s, 99.9),
         pct(s, 99.99), pct(s, 100));
}

int main(int argc, char **argv) {
  // Self-check mode: pure-logic tests only, no hardware/timing dependency.
  if (argc == 2 && std::strcmp(argv[1], "--test") == 0)
    return run_self_tests();

  // Anything else must be either "no args" (auto mode) or "cpu_a cpu_b"
  // (explicit mode) — argc == 2 (one CPU) or argc > 3 is a usage error, not a
  // silent fall-through into auto mode.
  if (argc != 1 && argc != 3) {
    fprintf(stderr, "usage: %s [cpu_a cpu_b]\n", argv[0]);
    return 1;
  }

  // Validate explicit-mode CPU args BEFORE calibrating/measuring anything, so
  // a bad argument fails immediately rather than after paying for calibration.
  int x = -1, y = -1;
  if (argc == 3) {
    auto online = online_cpus();
    x = parse_cpu_arg(argv[1], online);
    y = parse_cpu_arg(argv[2], online);
    // Same-CPU pair is not a pair at all. Checked here (before calibrate())
    // rather than left solely to run()'s guard: run()'s guard only warns and
    // returns 0, which would print a nonsense "SMT siblings" line (siblings_of
    // trivially contains the CPU itself) and exit 0 despite the README's
    // promise of non-zero on bad input.
    if (x == y) {
      fprintf(stderr,
              "error: cpu%d == cpu%d, refusing to run a same-CPU pair\n", x, y);
      return 1;
    }
  }

  check_invariant_tsc(); // warn (non-fatally) before trusting any ns figure
  calibrate();           // establish g_tsc_ghz before any measurement
  printf("TSC: %.4f GHz\n\n", g_tsc_ghz);

  // Explicit pair mode: user names two CPUs, we show both spin variants so the
  // PAUSE contribution is visible directly.
  if (argc == 3) {
    run<true>("explicit pair (pause)", x, y);

    // Bare spin on an SMT sibling starves the shared core's execution ports
    // that the OTHER sibling needs to store its reply (see file header) — an
    // invalid measurement, so skip it rather than run and mislabel it.
    auto x_sibs = siblings_of(x);
    if (x_sibs.empty()) {
      // sysfs was unreadable, so we genuinely don't know whether x/y are
      // siblings. Fail OPEN (warn, still run) rather than silently skipping a
      // legitimate measurement just because /sys couldn't be read.
      fprintf(stderr,
              "warning: could not determine SMT sibling status for cpu%d "
              "(sysfs unreadable) — if cpu%d/cpu%d are actually siblings, "
              "the bare-spin variant below is invalid\n",
              x, x, y);
      run<false>("explicit pair (bare-spin)", x, y);
    } else if (contains(x_sibs, y)) {
      printf("explicit pair (bare-spin)     skipped: cpu%d/cpu%d are SMT "
             "siblings — bare spin would starve the shared core's ports\n",
             x, y);
    } else {
      run<false>("explicit pair (bare-spin)", x, y);
    }
    return 0;
  }

  // Auto mode: discover a representative partner at each topology distance.
  auto cpus = online_cpus();
  if (cpus.size() < 2) {
    fprintf(stderr, "need >=2 cpus\n");
    return 1;
  }
  int base = cpus[0];            // anchor everything on the first online CPU
  auto sibs = siblings_of(base); // its SMT sibling(s)
  auto l3 = l3_peers_of(base);   // its L3/CCX peers

  // Pick one partner per distance class:
  int sibling = -1, same_l3 = -1, other_l3 = -1;
  for (int c : sibs) // an SMT sibling that isn't `base` itself
    if (c != base) {
      sibling = c;
      break;
    }
  for (int c : l3) // shares L3 but NOT the same physical core -> distinct core
    if (!contains(sibs, c)) {
      same_l3 = c;
      break;
    }
  if (!l3.empty())     // only search if we actually know base's L3 peers —
    for (int c : cpus) // outside base's L3 entirely -> cross-CCX
      if (!contains(l3, c)) {
        other_l3 = c;
        break;
      }
  // else: l3 is empty (sysfs didn't yield peers), so every cpu trivially
  // satisfies "!contains(l3, c)" and the loop would pick cpus[0] == base
  // itself — leave other_l3 at -1 instead of running a same-CPU "pair".

  printf("base cpu%d | SMT: %s | L3 peers: %zu\n\n", base,
         sibs.size() > 1 ? "on" : "off", l3.size());

  // SMT sibling: pause only. A bare spin on one sibling starves the shared
  // core's execution ports that the OTHER sibling needs to store, so it would
  // measure self-starvation, not handoff latency.
  run<true>("SMT sibling (pause)", base, sibling);

  // Cross-core pairs: run BOTH variants. The (pause - bare-spin) gap is the
  // PAUSE detection latency; the bare-spin figure approximates raw coherence
  // cost. Cross-CCX especially shows a phase-locked, inflated median under
  // pause because the return store keeps landing early in a PAUSE window.
  run<true>("same L3 / CCX (pause)", base, same_l3);
  run<false>("same L3 / CCX (bare-spin)", base, same_l3);
  run<true>("cross CCX (pause)", base, other_l3);
  run<false>("cross CCX (bare-spin)", base, other_l3);

  printf("\nnote: for stable tails, run with boost off + governor=performance "
         "(this box is not locked).\n"
         "      bare-spin figures still carry ~20-30ns rdtsc observer "
         "overhead.\n"
         "      big p99.99/max outliers are OS jitter (no core isolation).\n");
  return 0;
}
