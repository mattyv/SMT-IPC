// ============================================================================
// pp_core.hpp — shared primitives for the ping-pong benchmark family
// ----------------------------------------------------------------------------
// Extracted verbatim from smt_pingpong.cpp so a second experiment
// (sibling_noise.cpp) can reuse the reviewed, verified-correct measurement
// core (Slot layout, memory ordering, bench<> template, TSC calibration,
// sysfs topology helpers) without duplicating or re-deriving any of it.
//
// Nothing in this header changes the semantics of the original file — it is
// a mechanical move. See smt_pingpong.cpp's own header comment for the full
// rationale behind the design (SMT sibling / same-CCX / cross-CCX tiers,
// pause vs bare-spin, etc).
// ============================================================================
#pragma once

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
// inline gives the header a single ODR-safe definition even if some future TU
// includes it more than once. (Each program here is one TU in its own binary,
// so `static` would behave identically today — inline is just the safer default
// for a header-defined global.)
inline Slot a, b; // distinct lines, distinct 128B regions (verified in .bss)

// Cycles-per-nanosecond for the invariant TSC, filled in by calibrate().
// On AMD the TSC ticks at a constant base rate regardless of core boost, so
// this single value converts every rdtsc delta to ns for the whole run.
inline double g_tsc_ghz = 0.0;

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
  return !s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char c) {
    return std::isdigit(c) != 0;
  });
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
    if (line.rfind("flags", 0) ==
        0) { // first CPU's flags line is representative
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
  size_t lo = size_t(idx);                      // lower sample index
  double frac = idx - lo;                       // interpolation weight
  double v =
      sorted[lo] +
      (lo + 1 < sorted.size() ? frac * (sorted[lo + 1] - sorted[lo]) : 0);
  return v / g_tsc_ghz; // cycles -> ns
}
