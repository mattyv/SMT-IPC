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

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <climits> // INT_MAX
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <pthread.h>
#include <sched.h>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <x86intrin.h> // __rdtsc, _mm_pause, _mm_lfence

// ---------------------------------------------------------------------------
// Tuning knobs. Kept together so they're discoverable and adjustable in one
// place rather than sprinkled as magic numbers through the code.
// ---------------------------------------------------------------------------
struct Cfg {
  // A first, un-recorded pass. It pulls both cache lines into the right
  // coherence states, trains the branch predictors, and lets the CPU ramp to
  // its steady clock BEFORE we start recording — otherwise the first few
  // thousand samples are polluted by cold-start effects.
  static constexpr uint64_t WARM_ITERS = 200000;

  // Number of round trips actually recorded and fed into the percentiles.
  // 2M is enough to populate p99.99 (needs >~10k samples in the tail) while
  // still finishing in well under a second per pair.
  static constexpr uint64_t SAMPLE_ITERS = 2000000;

  // How long calibrate() busy-spins while comparing the TSC against a known
  // wall-clock. Longer = more accurate GHz estimate; 300ms is plenty to get
  // the frequency to ~4 significant figures.
  static constexpr int CALIB_MS = 300;
};

// ---------------------------------------------------------------------------
// The two shared flags that the threads ping-pong. Each lives on its own
// cache line so that a store to one never invalidates the other (that would
// be "false sharing" and would wreck the measurement).
//
// Why 128 bytes and not 64 (the actual line size)? Two reasons:
//   1. Intel's L2 adjacent-line prefetcher fetches 64B lines in aligned
//      128B PAIRS. If `a` and `b` were adjacent 64B lines, touching one could
//      speculatively drag in the other. 128B alignment + 128B size guarantees
//      each flag owns a full aligned 128B region, so the neighbouring line is
//      this slot's own dead padding, never the peer's flag.
//   2. Costs nothing on AMD, so we keep the same conservative discipline.
// ---------------------------------------------------------------------------
struct alignas(128) Slot {
  std::atomic<uint64_t> v{0};
  char pad[128 - sizeof(std::atomic<uint64_t>)]{}; // fill out the rest of 128B
};
static Slot a, b; // distinct lines, distinct 128B regions (verified in .bss)

// Cycles-per-nanosecond for the invariant TSC, filled in by calibrate().
// On AMD the TSC ticks at a constant base rate regardless of core boost, so
// this single value converts every rdtsc delta to ns for the whole run.
static double g_tsc_ghz = 0.0;

// ---------------------------------------------------------------------------
// Read the timestamp counter with lfences on both sides.
//
// Plain RDTSC can be reordered by the out-of-order engine, so a load from the
// timed region could drift outside the [t0, t1] window. The leading lfence
// stops earlier loads from leaking PAST the read; the trailing lfence stops
// the RDTSC itself from being hoisted before the work we want to measure.
// This costs a handful of cycles (~20-30ns of observer overhead per pair of
// reads) — a known, accepted tax on every sample.
// ---------------------------------------------------------------------------
static inline uint64_t rdtsc_now() {
  _mm_lfence();
  uint64_t t = __rdtsc();
  _mm_lfence();
  return t;
}

// ---------------------------------------------------------------------------
// The spin "relax" primitive, templated on whether to issue PAUSE.
//
// What PAUSE is: the x86 PAUSE instruction (emitted as `rep nop`). It's a hint
// to the CPU that says "I'm in a busy-wait loop." Two effects that matter here:
//   * On the core: it briefly stalls the pipeline so a tight spin doesn't
//     saturate the load unit and doesn't trigger a memory-order-violation
//     pipeline clear when the awaited value finally changes. On Zen 2+ this
//     park is ~64 cycles (~32ns @ 2GHz); on Zen 1 it was ~3 cycles.
//   * On an SMT core: it releases the shared execution ports to the OTHER
//     hardware thread — which is exactly why the sibling pair must use PAUSE.
// The cost: because PAUSE parks ~64 cycles, a peer store landing mid-PAUSE
// isn't seen until the PAUSE ends, adding ~up-to-one-PAUSE of detection
// latency to every sample. The Pause=false ("bare-spin") variant drops it to
// expose raw coherence cost — at the price of a tight re-load loop.
//
// Templating (rather than passing a runtime bool) means the compiler emits
// two specialized spin loops with the branch resolved at compile time — the
// hot loop never tests a flag per iteration. With Pause=false the body is
// empty and the loop becomes a raw re-load spin.
// ponytail: template so the pause branch is compiled away, not tested per-iter
// ---------------------------------------------------------------------------
template <bool Pause> static inline void relax() {
  if constexpr (Pause)
    _mm_pause();
}

// ---------------------------------------------------------------------------
// Establish the TSC frequency by pinning it against steady_clock.
//
// Read (tsc, wall) at the start, busy-spin CALIB_MS of real time, read
// (tsc, wall) again. The ratio of TSC-ticks to elapsed-ns is our GHz. We
// busy-spin instead of sleep() so we don't depend on timer/wakeup
// granularity, and the small asymmetry (tsc read just before each wall read)
// cancels out over a 300ms window.
// ---------------------------------------------------------------------------
static void calibrate() {
  uint64_t c0 = rdtsc_now();
  auto s0 = std::chrono::steady_clock::now();
  while (std::chrono::duration<double, std::milli>(
             std::chrono::steady_clock::now() - s0)
             .count() < Cfg::CALIB_MS) {
  }
  uint64_t c1 = rdtsc_now();
  auto s1 = std::chrono::steady_clock::now();
  double ns = std::chrono::duration<double, std::nano>(s1 - s0).count();
  g_tsc_ghz = double(c1 - c0) / ns; // cycles per nanosecond
}

// ---------------------------------------------------------------------------
// Pin the calling thread to a single logical CPU. This is what makes a
// "pair" mean anything: without it the scheduler could migrate the threads
// and we'd measure random placements. NOTE: affinity only steers OUR thread;
// it does not evict other work from that CPU — that needs isolcpus/cpuset.
// ---------------------------------------------------------------------------
static bool pin(int cpu) {
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu, &set);
  return pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;
}

// ---- /sys topology helpers ------------------------------------------------
// These read Linux's sysfs to discover the CPU cache/thread topology so the
// benchmark auto-selects a sibling / same-CCX / cross-CCX partner instead of
// making you look them up by hand.

// Read the first line of a sysfs file into a string.
static std::string slurp(const std::string &p) {
  std::ifstream f(p);
  std::string s;
  std::getline(f, s);
  return s;
}

// True iff `s` is non-empty and every character is an ASCII digit. Used to
// validate a token before handing it to stoi, so malformed sysfs content
// produces a clear fatal message instead of an uncaught std::invalid_argument.
static bool is_all_digits(const std::string &s) {
  return !s.empty() &&
         std::all_of(s.begin(), s.end(),
                      [](unsigned char c) { return std::isdigit(c) != 0; });
}

// Parse one CPU-number token, failing fast (loudly) if it isn't plain digits
// or doesn't fit in an int. This is the single place malformed sysfs content
// is caught, so a bad topology file becomes an explained exit(1) rather than
// an uncaught exception. Uses strtol + an explicit range check (not stoi):
// stoi throws std::out_of_range for a token like "99999999999", which would
// otherwise escape uncaught.
static int parse_uint_token(const std::string &tok) {
  if (!is_all_digits(tok)) {
    fprintf(stderr,
            "fatal: malformed CPU-list token '%s' (expected digits) — is "
            "sysfs topology data corrupt?\n",
            tok.c_str());
    exit(1);
  }
  errno = 0;
  char *end = nullptr;
  long v = strtol(tok.c_str(), &end, 10);
  if (errno == ERANGE || v > INT_MAX) {
    fprintf(stderr,
            "fatal: CPU-list token '%s' is out of range — is sysfs topology "
            "data corrupt?\n",
            tok.c_str());
    exit(1);
  }
  return int(v);
}

// Parse a Linux CPU list like "0,12" or "0-3,8-11" into a flat vector of ints.
// Handles both comma-separated singletons and dash-separated ranges.
static std::vector<int> parse_list(const std::string &s) {
  std::vector<int> out;
  std::stringstream ss(s);
  std::string tok;
  while (std::getline(ss, tok, ',')) { // split on commas
    auto dash = tok.find('-');
    if (dash == std::string::npos) { // singleton, e.g. "12"
      if (!tok.empty())
        out.push_back(parse_uint_token(tok));
    } else { // range, e.g. "0-3"
      int lo = parse_uint_token(tok.substr(0, dash)),
          hi = parse_uint_token(tok.substr(dash + 1));
      for (int i = lo; i <= hi; i++)
        out.push_back(i);
    }
  }
  return out;
}

// All online logical CPUs.
static std::vector<int> online_cpus() {
  return parse_list(slurp("/sys/devices/system/cpu/online"));
}

// The hardware threads that share a physical core with `cpu` (includes `cpu`
// itself). On an SMT-on box this is the sibling pair; on SMT-off it's just
// `cpu`. This is how we find the SMT-sibling partner.
static std::vector<int> siblings_of(int cpu) {
  return parse_list(slurp("/sys/devices/system/cpu/cpu" + std::to_string(cpu) +
                          "/topology/thread_siblings_list"));
}

// The CPUs that share an L3 cache with `cpu`. On AMD an L3 slice == a CCX, so
// "shares L3" == "same CCX". We scan the cache/index* entries from the highest
// index down looking for the level-3 cache and return its shared_cpu_list.
static std::vector<int> l3_peers_of(int cpu) {
  for (int idx = 3; idx >= 0; idx--) {
    auto s = slurp("/sys/devices/system/cpu/cpu" + std::to_string(cpu) +
                   "/cache/index" + std::to_string(idx) + "/level");
    if (s == "3")
      return parse_list(slurp("/sys/devices/system/cpu/cpu" +
                              std::to_string(cpu) + "/cache/index" +
                              std::to_string(idx) + "/shared_cpu_list"));
  }
  return {};
}

// Small membership helper for the pair-selection logic below.
static bool contains(const std::vector<int> &v, int x) {
  return std::find(v.begin(), v.end(), x) != v.end();
}

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

// Read /proc/cpuinfo and warn (loudly, non-fatally) if the invariant-TSC
// flags aren't present. calibrate() assumes a constant-rate TSC regardless of
// core boost/idle state; without it every ns figure this run prints is
// unreliable, but the topology ORDERING result is still meaningful, so we
// warn rather than abort.
static void check_invariant_tsc() {
  std::ifstream f("/proc/cpuinfo");
  std::string line;
  bool found_flags = false, constant = false, nonstop = false;
  while (std::getline(f, line)) {
    if (line.rfind("flags", 0) == 0) { // first CPU's flags line is representative
      found_flags = true;
      constant = line.find("constant_tsc") != std::string::npos;
      nonstop = line.find("nonstop_tsc") != std::string::npos;
      break;
    }
  }
  if (!found_flags || !constant || !nonstop) {
    fprintf(stderr,
            "\n*** WARNING: constant_tsc/nonstop_tsc not found in "
            "/proc/cpuinfo flags. ***\n"
            "*** All ns figures below assume an invariant TSC and may be "
            "unreliable. ***\n\n");
  }
}

// ===========================================================================
// The measurement itself.
//
// The protocol is a strict, serialized handoff of a monotonically increasing
// sequence number. For iteration i:
//
//   INITIATOR (cpu_a, the timed thread)      RESPONDER (cpu_b)
//   -------------------------------------    ----------------------------
//   t0 = rdtsc
//   store a = i   ---- line `a` migrates -->  spin until a == i
//                                             store b = i
//   spin until b == i  <-- line `b` back ---
//   t1 = rdtsc
//   sample = t1 - t0   (one full round trip)
//
// Each store forces the OTHER core to give up the line (a coherence
// invalidate / RFO), so exactly one line moves each way. Both threads are
// already hot-spinning, so no thread-wakeup cost is included — this is the
// best-case handoff between two live spinners, the standard ping-pong metric.
//
// Using a strictly increasing `i` (rather than toggling a bool) means a stale
// cached value can never accidentally satisfy the wait: the spin only exits
// when it genuinely observes THIS iteration's value, ruling out ABA effects.
//
// `samples == nullptr` runs the same protocol without recording — that's the
// warm-up pass.
// ===========================================================================
template <bool Pause>
static void bench(int cpu_a, int cpu_b, uint64_t iters,
                  std::vector<uint32_t> *samples) {
  // Reset both flags to 0 so iteration 1 (value 1) is a clean edge.
  a.v.store(0, std::memory_order_relaxed);
  b.v.store(0, std::memory_order_relaxed);

  // Handshake flag so the initiator doesn't start timing before the responder
  // has actually pinned itself and entered its spin loop.
  std::atomic<bool> ready{false};

  std::thread responder([&] {
    if (!pin(cpu_b)) {
      // Fatal, not just logged: an unpinned thread can migrate mid-run, so
      // the "pair" no longer means anything and the README says such runs
      // are invalid. Abrupt exit is fine for a benchmark tool.
      fprintf(stderr, "  ! pin cpu%d failed\n", cpu_b);
      exit(1);
    }
    ready.store(true, std::memory_order_release); // "I'm in position"
    for (uint64_t i = 1; i <= iters; i++) {
      // Wait to observe the initiator's store of `i` into `a`...
      while (a.v.load(std::memory_order_acquire) != i)
        relax<Pause>();
      // ...then bounce `i` straight back via `b`.
      b.v.store(i, std::memory_order_release);
    }
  });

  if (!pin(cpu_a)) {
    // Same reasoning as the responder's pin failure above: fatal, not logged.
    fprintf(stderr, "  ! pin cpu%d failed\n", cpu_a);
    exit(1);
  }
  while (!ready.load(std::memory_order_acquire)) // wait for responder ready
    relax<Pause>();

  if (samples) {
    // Recorded pass: time every single round trip.
    for (uint64_t i = 1; i <= iters; i++) {
      uint64_t t0 = rdtsc_now();
      a.v.store(i, std::memory_order_release); // kick: send `i` to responder
      while (b.v.load(std::memory_order_acquire) != i) // wait for it to return
        relax<Pause>();
      uint64_t t1 = rdtsc_now();
      // uint32 delta: only wraps if a single round trip exceeds 2^32 cycles
      // (~2s), i.e. a multi-second stall. Fine here; keeps the sample array
      // half the size and cache-friendlier.
      (*samples)[i - 1] = uint32_t(t1 - t0);
    }
  } else {
    // Warm-up pass: same traffic, no timing, results discarded.
    for (uint64_t i = 1; i <= iters; i++) {
      a.v.store(i, std::memory_order_release);
      while (b.v.load(std::memory_order_acquire) != i)
        relax<Pause>();
    }
  }
  responder.join();
}

// ---------------------------------------------------------------------------
// Linear-interpolated percentile over a SORTED sample array, returned in ns.
//
//   idx = p% * (n-1) gives a fractional rank; we blend the two neighbouring
//   samples by the fractional part. p=0 -> sorted[0] (min), p=100 -> the last
//   element (max). The (lo+1 < size) guard keeps the max case in-bounds. The
//   final divide converts TSC cycles -> ns using the calibrated frequency.
// ---------------------------------------------------------------------------
static double pct(const std::vector<uint32_t> &sorted, double p) {
  if (sorted.empty())
    return 0;
  double idx = p / 100.0 * (sorted.size() - 1); // fractional rank
  size_t lo = size_t(idx);                       // lower sample index
  double frac = idx - lo;                         // interpolation weight
  double v =
      sorted[lo] +
      (lo + 1 < sorted.size() ? frac * (sorted[lo + 1] - sorted[lo]) : 0);
  return v / g_tsc_ghz; // cycles -> ns
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
  expect_bool("is_valid_cpu_number(\"5\")", is_valid_cpu_number("5", &v),
              true);
  expect_eq("is_valid_cpu_number(\"5\") value", double(v), 5.0);
  expect_bool("is_valid_cpu_number(\"0\")", is_valid_cpu_number("0", &v),
              true);
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
              "error: cpu%d == cpu%d, refusing to run a same-CPU pair\n", x,
              y);
      return 1;
    }
  }

  check_invariant_tsc(); // warn (non-fatally) before trusting any ns figure
  calibrate();            // establish g_tsc_ghz before any measurement
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
  int base = cpus[0];              // anchor everything on the first online CPU
  auto sibs = siblings_of(base);   // its SMT sibling(s)
  auto l3 = l3_peers_of(base);     // its L3/CCX peers

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
  if (!l3.empty()) // only search if we actually know base's L3 peers —
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

  printf("\nnote: SMT on, boost off, governor=performance for stable tails.\n"
         "      bare-spin figures still carry ~20-30ns rdtsc observer "
         "overhead.\n"
         "      big p99.99/max outliers are OS jitter (no core isolation).\n");
  return 0;
}
