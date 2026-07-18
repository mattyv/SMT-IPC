// ============================================================================
// spsc_pipeline.cpp — sibling-vs-same-CCX placement crossover for a bounded
// producer/consumer handoff, as a function of consumer processing weight.
// ----------------------------------------------------------------------------
// smt_pingpong.cpp / sibling_noise.cpp measure two *already-spinning* threads
// handing a cache line back and forth, or one thread's throughput under a
// noisy sibling. Neither asks a placement question under REALISTIC pacing: a
// producer publishing a message, then a consumer popping and processing it,
// on either an SMT-sibling or a same-CCX peer of the consumer's core. This
// experiment asks: for a bounded producer/consumer handoff, does putting the
// producer on the consumer's SMT sibling (cheap message-detection latency,
// but the two threads share the physical core's execution ports) or on a
// same-CCX peer (its own physical core, but the handoff now crosses cores)
// win — and does the answer change as the consumer's per-message processing
// work grows?
//
// SHAPE OF THE EXPERIMENT
// ------------------------
// A producer thread manufactures fixed-layout messages (Msg, below), paced
// under MATCHED PACING (see PipeCfg::PROC_SWEEP_HEADROOM below — the producer
// stays a polite, non-hot tenant relative to the consumer's own processing
// cost at each level), and pushes them into a rigtorp::SPSCQueue<Msg>. A
// consumer thread — pinned to cpu0, waiting via a Pause spin (see "WAIT
// POLICY" below) — pops them, runs a port-bound processing kernel
// (process_msg_lanes) for a swept number of rounds, and records the
// end-to-end latency: consumer-completion TSC minus the publish TSC stamped
// into the message at the moment the producer first tried to publish it. The
// producer runs on either an SMT-sibling or a same-CCX peer of cpu0
// (topology discovery copied from smt_pingpong.cpp's auto-mode pair
// selection); cross-CCX is printed as a third reference line only.
//
// WAIT POLICY: Pause only. The consumer always waits via a tight re-poll +
// _mm_pause (see wait_pause below) — no BareSpin (which would port-starve an
// SMT-sibling producer, the same reasoning smt_pingpong.cpp applies to its
// own sibling pair) and no Blocking/futex (whose microsecond-scale park/wake
// round trip would swamp the nanosecond-scale processing-weight signal this
// sweep is trying to isolate).
//
// The headline: as the consumer's processing weight grows, does the
// SMT-sibling tier's lower message-detection latency keep winning, or does
// same-CCX's freedom from port contention take over? --proc-sweep sweeps a
// processing-weight ladder (PipeCfg::PROC_SWEEP_ROUNDS) and reports where (if
// anywhere) the sign of (sibling_p50 - sameccx_p50) flips.
//
// build: g++ -O3 -std=c++23 -pthread spsc_pipeline.cpp -o spsc_pipeline
//        -I <path-to-rigtorp-SPSCQueue-include>   (prefer the CMake build)
// run:   ./spsc_pipeline              (same as --proc-sweep, below)
//        ./spsc_pipeline --proc-sweep (sibling vs same-CCX placement across
//                                       consumer processing weight)
//        ./spsc_pipeline --test       (pure-logic self-check, no hardware)
// ============================================================================

// pp_core.hpp is included read-only, for rdtsc_now/calibrate/pin/pct/relax<>/
// the sysfs topology helpers/check_invariant_tsc. This file never uses
// bench<> (there is no ping-pong here), so — same discipline as
// sibling_noise.cpp — the resulting unused-function warnings for bench<> and
// friends are silenced locally rather than by touching the shared,
// already-reviewed header.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "pp_core.hpp"
#pragma GCC diagnostic pop

// rigtorp/SPSCQueue.h uses std::hardware_destructive_interference_size,
// which GCC warns is not ABI-stable across compiler versions/-mtune. That's
// a property of third-party code we don't own and must not edit (see the
// HARD CONSTRAINTS on pp_core.hpp/smt_pingpong.cpp/sibling_noise.cpp — the
// same "don't touch reviewed/vendored code, silence locally" discipline
// applies to this vendored header too), so the warning is silenced at the
// include site rather than globally.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winterference-size"
#include <rigtorp/SPSCQueue.h>
#pragma GCC diagnostic pop

#include <cmath>
#include <cstring>

// ===========================================================================
// rigtorp::SPSCQueue semantics, as read at the pinned commit
// (565a5149d54930463d58cb0f69b978d439555e66, tag v1.1) — recorded here per
// WP1 so the usage below doesn't depend on tribal memory of a header we don't
// own:
//   * SPSCQueue<T> q(capacity) allocates `capacity` USABLE slots — capacity()
//     reports back exactly what you passed in (the "+1 slack slot" the
//     constructor adds internally is not user-visible).
//   * front() returns T* to the next unread element, or nullptr if empty. It
//     does NOT remove the element — pop() does that separately. Calling
//     front() repeatedly without popping keeps returning the same element.
//   * try_emplace(...) constructs in place and returns false (no side effect,
//     no partial construction) if the queue is full; true on success.
//   * pop() asserts (debug builds only) that the queue is non-empty — it is
//     undefined behaviour to pop an empty queue, so every pop() call site
//     here is preceded by a successful front().
//   * Single-producer/single-consumer only: exactly one thread may call
//     try_emplace/emplace/push, exactly one (possibly different) thread may
//     call front/pop. No other concurrent access is safe.
// ===========================================================================
using Queue = rigtorp::SPSCQueue<struct Msg>;

// ---------------------------------------------------------------------------
// The message shape. Fixed-size, cache-line-aligned so one message occupies
// exactly one 64B line: no message ever straddles two lines, so publishing
// one message never causes coherence traffic on its neighbour's line.
// ---------------------------------------------------------------------------
struct alignas(64) Msg {
  uint64_t seq;     // strictly increasing per-message sequence number
  uint64_t pub_tsc; // rdtsc_now() at the producer's FIRST try_emplace attempt
  uint64_t
      payload[6]; // deterministic pseudo-random filler, see build_source_ring()
};
static_assert(sizeof(Msg) == 64, "Msg must occupy exactly one cache line");
static_assert(alignof(Msg) == 64, "Msg must be 64B-aligned");

// ---------------------------------------------------------------------------
// Tuning knobs. Same discipline as pp_core.hpp's Cfg / sibling_noise.cpp's
// NoiseCfg: every literal that shapes the measurement gets a name and a
// one-line reason here, nowhere else.
// ---------------------------------------------------------------------------
struct PipeCfg {
  // Distinct payload templates in the source ring. The producer cycles
  // through these (index n % SOURCE_RING_ELEMS) rather than generating fresh
  // random payloads per message, so payload generation cost never leaks into
  // the paced publish loop. 4096 is comfortably larger than any queue
  // capacity below, so a full queue's worth of in-flight messages never
  // repeats the same payload twice in a row.
  static constexpr int SOURCE_RING_ELEMS = 4096;

  // rigtorp::SPSCQueue capacity (usable slots). Large enough that normal
  // operation essentially never hits backpressure except at the most
  // saturated (gap=0) rows, where backpressure is expected and instrumented
  // (backpressure_count), not a bug.
  static constexpr size_t QUEUE_CAPACITY = 4096;

  // Untimed messages run before the recorded window, per pass. Same
  // rationale as pp_core.hpp's Cfg::WARM_ITERS: warms the queue's cache
  // lines, trains branch prediction, and lets both cores ramp to steady
  // clock before anything is measured.
  static constexpr uint64_t WARM_MSGS = 20000;

  // Recorded messages per pass. 100k is enough to populate p99.99 (needs
  // order-1e4 samples in the tail) while keeping the full tier x policy x
  // gap sweep well under a few minutes.
  static constexpr uint64_t SAMPLE_MSGS = 100000;

  // Two-phase pace_until() switches from _mm_pause to a bare spin once
  // within this many TSC cycles of the deadline. 128 cycles (~40-65ns on a
  // 2-3GHz TSC) approximates one PAUSE's ~64-cycle park window (see
  // pp_core.hpp's relax<> comment) — close enough to the deadline that a
  // pause's own latency risks overshooting it, so the tail is bare-spun for
  // the tightest possible deadline detection.
  static constexpr uint64_t PACE_TAIL_CYCLES = 128;

  // Deterministic source-ring payload generation: shared init/seed and
  // multiplicative-mix constant, same style and same values as
  // sibling_noise.cpp's NoiseCfg (an odd 32-bit Knuth multiplicative-hash
  // constant defeats "output is a trivial function of input"; the seed just
  // needs to be a non-trivial non-zero starting state). Defined separately
  // here rather than shared with NoiseCfg because sibling_noise.cpp is
  // out-of-scope committed code this file must not touch or depend on.
  static constexpr uint64_t INIT_SEED = 0xdeadbeefULL;
  static constexpr uint64_t MIX_CONST = 2654435761ull;

  // --proc-sweep: processing-weight ladder, in process_msg_lanes rounds.
  // Geometric x4 growth from 0 to 8192. At ~1.5ns/round on this box (see the
  // calibrated-ns table --proc-sweep prints), this brackets {0, ~50, ~200,
  // ~800, ~3000, ~12000} ns of per-message processing — comfortably spanning
  // BOTH crossover bounds predicted by Finding 2: ~50ns (producer ran hot)
  // and ~1.3us (producer stays polite under matched pacing). A ladder that
  // stopped at the historical PROC_ROUNDS=24 (~tens of ns) would never reach
  // the polite-producer regime and would falsely read "no crossover".
  static constexpr int PROC_SWEEP_ROUNDS[] = {0, 32, 128, 512, 2048, 8192};

  // Matched-pacing headroom multiplier (see proc_sweep_gap_ns): the
  // producer's inter-arrival gap is this many times the calibrated
  // per-message processing cost. 1.5x keeps the consumer at ~65%
  // utilization (1/1.5), which does two things: keeps the queue near-empty
  // so a message's latency reflects handoff+processing rather than
  // Little's-law queueing delay, AND guarantees the producer's own
  // bare-spin pacing tail lands strictly after the consumer has finished
  // processing the PREVIOUS message — i.e. the producer stays a polite
  // (non-hot) tenant on a shared sibling core, which is the regime Finding
  // 2's ~1.3us crossover bound assumes.
  static constexpr double PROC_SWEEP_HEADROOM = 1.5;

  // Fixed ns folded into the matched-pacing gap on top of the calibrated
  // processing time, covering handoff-detection + pop overhead that isn't
  // part of process_msg_lanes itself (real, non-zero even at rounds=0, so
  // the gap doesn't under-provision at the cheap end of the ladder and
  // trigger spurious backpressure/queue buildup there).
  static constexpr double PROC_SWEEP_HANDOFF_ALLOWANCE_NS = 150.0;

  // Solo (single-thread, no contention) timed process_msg_lanes calls used
  // to calibrate each proc-sweep level's real ns/call cost. 4096 gives a
  // stable median without materially lengthening the sweep.
  static constexpr int PROC_SWEEP_CALIB_ITERS = 4096;

  // --proc-sweep validity-gate floor for waited-fraction (share of sampled
  // messages whose first front() poll found the queue empty). The reported
  // statistic is the p50, which is arithmetically immune to <50%
  // contamination, so a 0.90 floor (<=10% one-sided non-waited samples)
  // keeps the reported median within the clean distribution's ~p44-p56 band
  // (and in practice the decision cells run 0.97+, ~p51). It is not a
  // stricter (e.g. 0.99) floor because proc-sweep's matched pacing runs
  // ~300ns gaps, where 0.99 would demand near-perfect OS isolation across
  // 120k messages (i.e. it would test isolation, not saturation). 0.90 still
  // rejects genuinely contaminated cells (e.g. cross-CCX low-rounds rows run
  // 0.26-0.67 waited-fraction with sojourn-inflated medians).
  static constexpr double PROC_SWEEP_WAITED_FRACTION_MIN = 0.90;
};

// Per-pass metrics the consumer/producer accumulate. Plain struct, no
// synchronization needed: producer writes its own fields, consumer writes
// its own, and they're only read after both threads are joined.
struct PassMetrics {
  std::vector<uint32_t> latency_cycles; // t_done - pub_tsc, per sampled message
  std::vector<uint32_t>
      inter_arrival_cycles; // pub_tsc[n] - pub_tsc[n-1], sampled window only
  uint64_t waited_count =
      0; // sampled messages whose first front() poll was null
  uint64_t sampled_count = 0; // messages actually counted (n >= WARM_MSGS)
  uint64_t backpressure_count =
      0; // try_emplace failures (retried, not re-stamped)
  uint64_t late_publish_count =
      0; // deadline already passed when we reached publish

  // --proc-sweep: in-situ TSC cycles spent inside the processing call itself
  // (process_msg_lanes), per sampled message. Splits the end-to-end latency
  // delta between two placements into Δhandoff vs Δprocessing directly,
  // rather than inferring it indirectly.
  std::vector<uint32_t> proc_cycles;
};

// wait_pause: the consumer's per-message wait — tight re-poll + _mm_pause.
// This is the only wait strategy --proc-sweep uses (see the file-scope
// comment's "WAIT POLICY" section for why: BareSpin would port-starve an
// SMT-sibling producer, and Blocking's futex round trip would swamp the
// ns-scale processing-weight signal this sweep measures). `had_to_wait`
// receives true/false for whether this call's FIRST front() poll found the
// queue empty, so the caller can fold it into PassMetrics's waited-fraction
// only during the sampled window (warm-up messages don't pollute it).
static inline Msg *wait_pause(Queue &q, bool &had_to_wait) {
  Msg *m = q.front();
  had_to_wait = (m == nullptr);
  while (!m) {
    relax<true>(); // pp_core.hpp: Pause=true -> _mm_pause each spin
    m = q.front();
  }
  return m;
}

// ===========================================================================
// Source ring, processing, and the seq-gap detector (WP3).
// ===========================================================================

// Elision guard: the consumer folds every message's checksum in here so the
// compiler can never prove process_msg_lanes()'s work is dead and fold it
// away.
inline std::atomic<uint64_t> g_process_sink{0};

// Deterministically fill SOURCE_RING_ELEMS message payload templates. Pure
// function of PipeCfg constants, so it's identical (and testable) across
// runs — the point is realistic-looking, non-degenerate payload bytes, not
// actual randomness.
static std::vector<Msg> build_source_ring() {
  std::vector<Msg> ring(PipeCfg::SOURCE_RING_ELEMS);
  uint64_t state = PipeCfg::INIT_SEED;
  for (auto &m : ring) {
    m.seq = 0;     // overwritten per-publish
    m.pub_tsc = 0; // overwritten per-publish
    for (auto &p : m.payload) {
      state =
          state * PipeCfg::MIX_CONST + 1; // simple, deterministic LCG-ish mix
      p = state;
    }
  }
  return ring;
}

// ---------------------------------------------------------------------------
// process_msg_lanes (--proc-sweep, Finding 1): a PORT-BOUND processing
// kernel. A single dependent accumulator chain (each round's multiply-add
// depending on the previous round's result) would be LATENCY-bound: the core
// is mostly waiting on the multiply's own pipeline latency, not contending
// for execution ports, so it would be nearly immune to a noisy SMT sibling
// stealing port cycles — that shape would falsely show no sibling-vs-same-CCX
// crossover, because the thing being contended for (ports) is never the
// bottleneck.
//
// process_msg_lanes instead runs SIX INDEPENDENT accumulator lanes, one per
// payload word, with no cross-lane dependency: a superscalar core can issue
// several lanes' multiply-adds in the same cycle, so throughput is bounded
// by *port availability*, not by any single dependency chain's latency. That
// makes it sensitive to a port-hungry SMT sibling — which is the whole point
// of sweeping it.
//
// rounds=0 still depends on every payload word (each lane is seeded from its
// own word before the round loop even runs), so it's non-degenerate and
// distinguishable from rounds>=1 by construction, not by accident.
// ---------------------------------------------------------------------------
static inline uint64_t process_msg_lanes(const Msg &m, uint32_t rounds) {
  uint64_t lane[6];
  for (int k = 0; k < 6; k++)
    lane[k] = m.payload[k] ^ PipeCfg::INIT_SEED;
  for (uint32_t round = 0; round < rounds; round++) {
    for (int k = 0; k < 6; k++)
      lane[k] =
          lane[k] * PipeCfg::MIX_CONST + m.payload[k]; // no cross-lane dep
  }
  // Fold all six lanes into one return value — result always observably
  // used (the caller feeds this into g_process_sink), same elision-guard
  // discipline as sibling_noise.cpp's victim_chunk/tenant_step.
  uint64_t acc = 0;
  for (int k = 0; k < 6; k++)
    acc += lane[k];
  return acc;
}

// Fatal (not logged-and-continue) on a sequence gap: a gap means a message
// was lost, duplicated, or reordered, which would silently invalidate every
// latency sample in the pass. Same "invalid measurement is worse than no
// measurement" discipline as pp_core.hpp's pin-failure handling.
static inline void check_seq(uint64_t expected, uint64_t got) {
  if (expected != got) {
    fprintf(stderr,
            "fatal: sequence gap detected — expected seq %llu, got %llu. "
            "The queue lost, duplicated, or reordered a message; every "
            "latency sample in this pass is unreliable.\n",
            (unsigned long long)expected, (unsigned long long)got);
    exit(1);
  }
}

// ===========================================================================
// Pacing engine (WP4): absolute-deadline schedule, no drift accumulation.
// ===========================================================================

// ns -> TSC cycles using the calibrated g_tsc_ghz (pp_core.hpp). Pulled out
// as its own function so WP4's test can fix g_tsc_ghz = 1.0 (1 cycle == 1ns)
// and assert exact conversions without any hardware dependency.
static inline uint64_t ns_to_cycles(uint64_t ns) {
  return uint64_t(double(ns) * g_tsc_ghz);
}

// Deadline for message n, given the schedule starts at t_start and messages
// are paced gap_cycles apart. Absolute (not accumulated from the previous
// message's actual publish time) so a late message never drags every
// subsequent message's deadline later with it.
static inline uint64_t sched_deadline(uint64_t t_start, uint64_t n,
                                      uint64_t gap_cycles) {
  return t_start + n * gap_cycles;
}

// Two-phase busy-wait to an absolute TSC deadline. NEVER sleeps (a sleep's
// wakeup granularity would swamp the sub-microsecond gaps this sweep cares
// about). Phase 1 uses _mm_pause while comfortably far from the deadline (so
// a spinning producer isn't hammering the load unit or starving a sibling
// for no reason); phase 2 drops to a bare spin once within
// PACE_TAIL_CYCLES, trading core-friendliness for the tightest possible
// deadline detection right at the end.
// Returns true if the deadline had ALREADY passed on entry — i.e. the producer
// arrived late and there was nothing to wait for (it can't keep up with the
// schedule). Returns false if it actually paced (waited) to the deadline. This
// is the honest "late-publish" signal: comparing a post-pacing timestamp to the
// deadline would be trivially always-true, since pace_until only returns once
// now >= deadline. At gap=0 the deadline is fixed at t_start, so every message
// is legitimately "late" (saturated: no pacing) — that's correct, not a bug.
static inline bool pace_until(uint64_t deadline_tsc) {
  uint64_t now = rdtsc_now();
  bool late_on_entry = now > deadline_tsc; // strictly past = arrived behind
  while (now < deadline_tsc) {
    if (deadline_tsc - now > PipeCfg::PACE_TAIL_CYCLES)
      relax<true>(); // pp_core.hpp: _mm_pause
    // else: fall through to a bare re-check, no pause — tail-spin the last
    // stretch for minimum overshoot past the deadline.
    now = rdtsc_now();
  }
  return late_on_entry;
}

// ===========================================================================
// --proc-sweep pure helpers (WP1): matched-pacing gap arithmetic, the
// per-level gap-ns builder, and the crossover finder. All pure functions of
// their arguments — no hardware, no threads, no rdtsc — so they're exercised
// by run_self_tests() directly, hardware-free and CI-safe.
// ===========================================================================

// The matched-pacing gap for a proc-sweep level whose calibrated per-message
// processing cost is `proc_ns`. See PipeCfg::PROC_SWEEP_HEADROOM and
// PROC_SWEEP_HANDOFF_ALLOWANCE_NS for the rationale; this function is just
// the formula, kept separate and pure so it's independently testable
// (monotonicity, exactness at proc_ns=0) without running a real pass.
static inline double proc_sweep_gap_ns(double proc_ns) {
  return PipeCfg::PROC_SWEEP_HEADROOM *
         (proc_ns + PipeCfg::PROC_SWEEP_HANDOFF_ALLOWANCE_NS);
}

// Builds one gap_ns per calibrated-ns entry, via proc_sweep_gap_ns, rounded
// to the nearest ns (run_pass takes an integer ns gap).
static std::vector<uint64_t>
proc_sweep_cells(const std::vector<double> &calibrated_proc_ns) {
  std::vector<uint64_t> gaps;
  gaps.reserve(calibrated_proc_ns.size());
  for (double proc_ns : calibrated_proc_ns)
    gaps.push_back(uint64_t(std::lround(proc_sweep_gap_ns(proc_ns))));
  return gaps;
}

// find_crossover: pure bracket search over (proc_ns, delta_ns) points,
// SORTED ascending by proc_ns, where delta_ns = sibling_p50 - sameccx_p50 at
// that processing level. Cases:
//   * fewer than 2 points -> not found (nothing to bracket).
//   * any point with delta exactly 0 -> that point IS the crossover
//     (bracket collapses to itself, interpolated_proc_ns = that point's
//     proc_ns).
//   * the first adjacent pair with delta going negative -> positive (sibling
//     faster, then same-CCX faster) -> bracketed, linearly interpolated for
//     delta=0 between them.
//   * no such pair (all-negative, all-positive, or any other non-crossing
//     shape) -> not found; the caller reports "no crossover in range" with
//     whichever side won throughout.
// ---------------------------------------------------------------------------
struct CrossoverResult {
  bool found = false;
  double lo_proc_ns = 0, lo_delta = 0;
  double hi_proc_ns = 0, hi_delta = 0;
  double interpolated_proc_ns = 0;
};

static CrossoverResult
find_crossover(const std::vector<std::pair<double, double>> &points) {
  CrossoverResult r;
  if (points.size() < 2)
    return r;

  for (const auto &p : points) {
    if (p.second == 0.0) {
      r.found = true;
      r.lo_proc_ns = r.hi_proc_ns = r.interpolated_proc_ns = p.first;
      r.lo_delta = r.hi_delta = 0.0;
      return r;
    }
  }

  for (size_t i = 0; i + 1 < points.size(); i++) {
    double d0 = points[i].second, d1 = points[i + 1].second;
    if (d0 < 0.0 && d1 > 0.0) {
      r.found = true;
      r.lo_proc_ns = points[i].first;
      r.lo_delta = d0;
      r.hi_proc_ns = points[i + 1].first;
      r.hi_delta = d1;
      double frac = -d0 / (d1 - d0); // where between lo and hi delta hits 0
      r.interpolated_proc_ns =
          r.lo_proc_ns + frac * (r.hi_proc_ns - r.lo_proc_ns);
      return r;
    }
  }
  return r; // no negative->positive crossing found
}

// ===========================================================================
// Topology / placement (D5): partner selection copied from
// smt_pingpong.cpp's auto-mode pair discovery (~lines 267-297) — same
// sysfs-driven sibling/same-L3/cross-L3 selection, reused read-only via
// pp_core.hpp's helpers. Unlike smt_pingpong, the roles are swapped: the
// CONSUMER (latency-critical side — it's what we're measuring) is pinned to
// the anchor cpu0, and the PRODUCER runs on the tier partner.
// ===========================================================================
struct Topology {
  int base = -1;     // consumer's cpu (anchor)
  int sibling = -1;  // producer cpu for the SMT-sibling tier, or -1
  int same_l3 = -1;  // producer cpu for the same-CCX tier, or -1
  int other_l3 = -1; // producer cpu for the cross-CCX tier, or -1
};

static Topology discover_topology() {
  Topology t;
  auto cpus = online_cpus();
  if (cpus.size() < 2) {
    fprintf(stderr, "fatal: need >=2 online cpus for a producer/consumer "
                    "pipeline\n");
    exit(1);
  }
  t.base = cpus[0];
  auto sibs = siblings_of(t.base);
  auto l3 = l3_peers_of(t.base);

  for (int c : sibs)
    if (c != t.base) {
      t.sibling = c;
      break;
    }
  for (int c : l3)
    if (!contains(sibs, c)) {
      t.same_l3 = c;
      break;
    }
  if (!l3.empty())
    for (int c : cpus)
      if (!contains(l3, c)) {
        t.other_l3 = c;
        break;
      }
  // else: l3 empty (sysfs unreadable) -> leave other_l3 at -1 rather than
  // risk picking cpus[0] == base itself, same reasoning as smt_pingpong.cpp.
  return t;
}

// ===========================================================================
// One pass: WARM_MSGS + SAMPLE_MSGS messages, one producer/consumer thread
// pair, one (tier, gap) cell of the --proc-sweep ladder. Pause-only (see the
// file-scope comment's "WAIT POLICY" section): BareSpin would port-starve an
// SMT-sibling producer, and Blocking's futex round trip would swamp the
// ns-scale processing-weight signal this sweep measures.
//
// The producer publish loop below deliberately does NOT go through a
// fence+conditional-wake path (that machinery existed only for the deleted
// Blocking wait policy). Both tiers being compared see the plain
// try_emplace-and-retry loop identically, so removing that fence cannot bias
// the sibling-vs-same-CCX delta this sweep reports — see the acceptance-gate
// note in the carve's task description for the measurement this was checked
// against.
// ===========================================================================
static PassMetrics run_pass(int consumer_cpu, int producer_cpu, uint64_t gap_ns,
                            int proc_rounds, const std::vector<Msg> &ring) {
  Queue q(PipeCfg::QUEUE_CAPACITY);
  const uint64_t total_msgs = PipeCfg::WARM_MSGS + PipeCfg::SAMPLE_MSGS;

  PassMetrics metrics;
  metrics.latency_cycles.reserve(PipeCfg::SAMPLE_MSGS);
  metrics.inter_arrival_cycles.reserve(PipeCfg::SAMPLE_MSGS);
  metrics.proc_cycles.reserve(PipeCfg::SAMPLE_MSGS);

  std::atomic<bool> consumer_ready{false};

  std::thread consumer([&] {
    if (!pin(consumer_cpu)) {
      fprintf(stderr, "  ! pin cpu%d failed (consumer)\n", consumer_cpu);
      exit(1);
    }
    consumer_ready.store(true, std::memory_order_release);
    uint64_t expected_seq = 0;
    for (uint64_t n = 0; n < total_msgs; n++) {
      bool had_to_wait = false;
      Msg *m = wait_pause(q, had_to_wait);

      check_seq(expected_seq, m->seq);
      expected_seq++;

      // In-situ timing: an rdtsc pair bracketing ONLY the processing call
      // itself. This splits Δend-to-end directly into Δhandoff (everything
      // before proc_t0) vs Δprocessing (proc_t1 - proc_t0) for a placement
      // comparison — the constant per-message observer tax of the extra
      // rdtsc pair is itself placement-independent, so it cancels out of
      // any delta between tiers.
      uint64_t proc_t0 = rdtsc_now();
      uint64_t sink = process_msg_lanes(*m, uint32_t(proc_rounds));
      uint64_t proc_t1 = rdtsc_now();
      uint64_t t_done = rdtsc_now();
      uint64_t pub_tsc = m->pub_tsc;

      if (n >= PipeCfg::WARM_MSGS) {
        metrics.sampled_count++;
        uint32_t sample_latency = uint32_t(t_done - pub_tsc);
        metrics.latency_cycles.push_back(sample_latency);
        metrics.proc_cycles.push_back(uint32_t(proc_t1 - proc_t0));
        if (had_to_wait)
          metrics.waited_count++;
        g_process_sink.fetch_add(sink, std::memory_order_relaxed);
      }
      q.pop();
    }
  });

  if (!pin(producer_cpu)) {
    fprintf(stderr, "  ! pin cpu%d failed (producer)\n", producer_cpu);
    exit(1);
  }
  while (!consumer_ready.load(std::memory_order_acquire))
    relax<true>();

  const uint64_t gap_cycles = ns_to_cycles(gap_ns);

  uint64_t t_start = rdtsc_now();
  uint64_t prev_pub_tsc = 0;
  for (uint64_t n = 0; n < total_msgs; n++) {
    uint64_t deadline = sched_deadline(t_start, n, gap_cycles);
    bool late = pace_until(deadline); // true iff we arrived past the deadline

    Msg msg = ring[n % PipeCfg::SOURCE_RING_ELEMS];
    msg.seq = n;
    msg.pub_tsc = rdtsc_now(); // stamped ONCE, before the first try_emplace

    if (n >= PipeCfg::WARM_MSGS && late)
      metrics.late_publish_count++;

    while (!q.try_emplace(msg)) {
      if (n >= PipeCfg::WARM_MSGS)
        metrics.backpressure_count++;
      // retry WITHOUT re-stamping seq/pub_tsc — the message keeps its
      // original "first offered" timestamp regardless of how many retries
      // backpressure costs it.
      relax<true>(); // pp_core.hpp: _mm_pause — a bare-spin retry here would
                     // port-starve the consumer on the sibling tier at
                     // gap=0, where backpressure is expected and frequent.
    }

    if (n >= PipeCfg::WARM_MSGS) {
      if (prev_pub_tsc != 0)
        metrics.inter_arrival_cycles.push_back(
            uint32_t(msg.pub_tsc - prev_pub_tsc));
      prev_pub_tsc = msg.pub_tsc;
    }
  }

  consumer.join();
  return metrics;
}

// ---------------------------------------------------------------------------
// calibrate_proc_level_ns (--proc-sweep): the real x-axis value for one
// proc-sweep level. Runs SOLO (no producer thread, no queue, no contention)
// on the pinned consumer CPU: PROC_SWEEP_CALIB_ITERS timed process_msg_lanes
// calls over the source ring, median (via pct(...,50), consistent with every
// other percentile this file reports). This is deliberately NOT the same as
// the in-situ proc_cycles collected during a real pass (those run under
// producer/sibling contention, which is exactly the effect --proc-sweep is
// trying to measure) — this calibration number is the CONTENTION-FREE
// baseline used only to size the matched-pacing gap (proc_sweep_gap_ns), so
// the gap doesn't itself depend on whatever contention the sweep is about to
// go measure.
// ---------------------------------------------------------------------------
static double calibrate_proc_level_ns(int consumer_cpu, uint32_t rounds,
                                      const std::vector<Msg> &ring) {
  if (!pin(consumer_cpu)) {
    fprintf(stderr, "fatal: pin cpu%d failed (--proc-sweep calibration)\n",
            consumer_cpu);
    exit(1);
  }
  std::vector<uint32_t> samples;
  samples.reserve(PipeCfg::PROC_SWEEP_CALIB_ITERS);
  uint64_t sink = 0; // elision guard, folded into g_process_sink below
  for (int i = 0; i < PipeCfg::PROC_SWEEP_CALIB_ITERS; i++) {
    const Msg &m = ring[i % PipeCfg::SOURCE_RING_ELEMS];
    uint64_t t0 = rdtsc_now();
    sink += process_msg_lanes(m, rounds);
    uint64_t t1 = rdtsc_now();
    samples.push_back(uint32_t(t1 - t0));
  }
  g_process_sink.fetch_add(sink, std::memory_order_relaxed);
  std::sort(samples.begin(), samples.end());
  return pct(samples, 50);
}

// Forward declaration: proc_sweep_cell_valid is defined alongside the rest
// of the --proc-sweep machinery (below), but its validity-gate logic is
// pure and self-contained enough to unit-test directly from
// run_self_tests() here. PassMetrics itself is already fully defined above.
static bool proc_sweep_cell_valid(const PassMetrics &m);

// ===========================================================================
// WP3/WP4 pure-logic self-tests.
// ===========================================================================
static void run_self_tests() {
  // Msg layout.
  static_assert(sizeof(Msg) == 64);
  static_assert(alignof(Msg) == 64);

  // build_source_ring: deterministic, non-degenerate, and every payload word
  // actually gets touched (guards against an off-by-one that leaves a slot
  // all-zero).
  {
    auto ring = build_source_ring();
    if (ring.size() != PipeCfg::SOURCE_RING_ELEMS) {
      fprintf(stderr, "FAIL build_source_ring: wrong size\n");
      exit(1);
    }
    auto ring2 = build_source_ring();
    for (size_t i = 0; i < ring.size(); i++)
      for (int k = 0; k < 6; k++)
        if (ring[i].payload[k] != ring2[i].payload[k]) {
          fprintf(stderr, "FAIL build_source_ring: not deterministic\n");
          exit(1);
        }
    bool any_nonzero = false;
    for (uint64_t p : ring[0].payload)
      if (p != 0)
        any_nonzero = true;
    if (!any_nonzero) {
      fprintf(stderr, "FAIL build_source_ring: degenerate all-zero slot\n");
      exit(1);
    }
  }

  // process_msg_lanes (--proc-sweep, Finding 1): deterministic; depends on
  // EACH of the 6 payload words individually (not just some of them, which
  // would indicate a lane wired to the wrong word); rounds=0 != rounds=1;
  // different round counts give different results.
  {
    Msg m{};
    m.seq = 5;
    for (int k = 0; k < 6; k++)
      m.payload[k] = uint64_t(k + 1) * 999331;

    uint64_t r1 = process_msg_lanes(m, 3);
    uint64_t r2 = process_msg_lanes(m, 3);
    if (r1 != r2) {
      fprintf(stderr, "FAIL process_msg_lanes: not deterministic\n");
      exit(1);
    }

    for (int k = 0; k < 6; k++) {
      Msg mk = m;
      mk.payload[k] += 1;
      uint64_t rk = process_msg_lanes(mk, 3);
      if (rk == r1) {
        fprintf(stderr,
                "FAIL process_msg_lanes: result independent of payload[%d]\n",
                k);
        exit(1);
      }
    }

    uint64_t r_rounds0 = process_msg_lanes(m, 0);
    uint64_t r_rounds1 = process_msg_lanes(m, 1);
    uint64_t r_rounds2 = process_msg_lanes(m, 2);
    if (r_rounds0 == r_rounds1 || r_rounds1 == r_rounds2 ||
        r_rounds0 == r_rounds2) {
      fprintf(stderr,
              "FAIL process_msg_lanes: rounds=0/1/2 must all differ (got "
              "%llu, %llu, %llu)\n",
              (unsigned long long)r_rounds0, (unsigned long long)r_rounds1,
              (unsigned long long)r_rounds2);
      exit(1);
    }
  }

  // proc_sweep_gap_ns (--proc-sweep, WP1): exact formula at known inputs,
  // and monotonic non-decreasing in proc_ns.
  {
    // proc_ns=0 -> HEADROOM * HANDOFF_ALLOWANCE_NS exactly.
    double want0 =
        PipeCfg::PROC_SWEEP_HEADROOM * PipeCfg::PROC_SWEEP_HANDOFF_ALLOWANCE_NS;
    if (proc_sweep_gap_ns(0.0) != want0) {
      fprintf(stderr, "FAIL proc_sweep_gap_ns(0): got %.4f want %.4f\n",
              proc_sweep_gap_ns(0.0), want0);
      exit(1);
    }
    // proc_ns=1000 -> exact formula.
    double want1000 = PipeCfg::PROC_SWEEP_HEADROOM *
                      (1000.0 + PipeCfg::PROC_SWEEP_HANDOFF_ALLOWANCE_NS);
    if (proc_sweep_gap_ns(1000.0) != want1000) {
      fprintf(stderr, "FAIL proc_sweep_gap_ns(1000): got %.4f want %.4f\n",
              proc_sweep_gap_ns(1000.0), want1000);
      exit(1);
    }
    // Monotonic non-decreasing across the real ladder's expected ns range.
    double prev = proc_sweep_gap_ns(0.0);
    double sample_ns[] = {10, 50, 200, 800, 3000, 12000};
    for (double ns : sample_ns) {
      double g = proc_sweep_gap_ns(ns);
      if (g < prev) {
        fprintf(stderr,
                "FAIL proc_sweep_gap_ns: not monotonic at ns=%.1f (got %.1f "
                "< prev %.1f)\n",
                ns, g, prev);
        exit(1);
      }
      prev = g;
    }
  }

  // proc_sweep_cells (--proc-sweep, WP1): each gap_ns ==
  // round(proc_sweep_gap_ns(calibrated_ns)).
  {
    std::vector<double> calibrated = {0.0, 50.0, 1300.0};
    std::vector<uint64_t> gaps = proc_sweep_cells(calibrated);
    if (gaps.size() != calibrated.size()) {
      fprintf(stderr, "FAIL proc_sweep_cells: wrong cell count\n");
      exit(1);
    }
    for (size_t i = 0; i < gaps.size(); i++) {
      uint64_t want = uint64_t(std::lround(proc_sweep_gap_ns(calibrated[i])));
      if (gaps[i] != want) {
        fprintf(stderr,
                "FAIL proc_sweep_cells at i=%zu: gap=%llu want gap=%llu\n", i,
                (unsigned long long)gaps[i], (unsigned long long)want);
        exit(1);
      }
    }
  }

  // proc_sweep_cell_valid (--proc-sweep validity gate): backpressure!=0
  // invalidates regardless of waited-fraction; waited-fraction just below
  // PROC_SWEEP_WAITED_FRACTION_MIN is invalid, just above (with
  // backpressure==0) is valid.
  {
    const double eps = 0.001;

    // backpressure!=0 -> invalid even with a perfect waited-fraction.
    {
      PassMetrics m;
      m.sampled_count = 1000;
      m.waited_count = 1000; // waited_fraction == 1.0
      m.backpressure_count = 1;
      if (proc_sweep_cell_valid(m)) {
        fprintf(stderr, "FAIL proc_sweep_cell_valid: backpressure!=0 must be "
                        "invalid regardless of waited-fraction\n");
        exit(1);
      }
    }

    // waited-fraction just below the floor, backpressure==0 -> invalid.
    {
      PassMetrics m;
      m.sampled_count = 1000;
      m.waited_count =
          uint64_t((PipeCfg::PROC_SWEEP_WAITED_FRACTION_MIN - eps) * 1000.0);
      m.backpressure_count = 0;
      if (proc_sweep_cell_valid(m)) {
        fprintf(stderr,
                "FAIL proc_sweep_cell_valid: waited-fraction just below "
                "PROC_SWEEP_WAITED_FRACTION_MIN must be invalid\n");
        exit(1);
      }
    }

    // waited-fraction just above the floor, backpressure==0 -> valid.
    {
      PassMetrics m;
      m.sampled_count = 1000;
      m.waited_count =
          uint64_t((PipeCfg::PROC_SWEEP_WAITED_FRACTION_MIN + eps) * 1000.0);
      m.backpressure_count = 0;
      if (!proc_sweep_cell_valid(m)) {
        fprintf(stderr,
                "FAIL proc_sweep_cell_valid: waited-fraction just above "
                "PROC_SWEEP_WAITED_FRACTION_MIN (backpressure==0) must be "
                "valid\n");
        exit(1);
      }
    }
  }

  // find_crossover (--proc-sweep, WP1): the four required cases.
  {
    // <2 points -> not found.
    if (find_crossover({}).found || find_crossover({{100.0, -5.0}}).found) {
      fprintf(stderr, "FAIL find_crossover: <2 points must not be found\n");
      exit(1);
    }
    // all-negative (sibling wins throughout) -> not found.
    {
      CrossoverResult r =
          find_crossover({{0.0, -10.0}, {100.0, -5.0}, {1000.0, -1.0}});
      if (r.found) {
        fprintf(stderr,
                "FAIL find_crossover: all-negative must not be found\n");
        exit(1);
      }
    }
    // clean neg->pos -> bracketed and linearly interpolated.
    {
      CrossoverResult r =
          find_crossover({{0.0, -20.0}, {100.0, -10.0}, {200.0, 10.0}});
      if (!r.found || r.lo_proc_ns != 100.0 || r.hi_proc_ns != 200.0) {
        fprintf(stderr,
                "FAIL find_crossover: neg->pos not bracketed correctly "
                "(found=%d lo=%.1f hi=%.1f)\n",
                r.found, r.lo_proc_ns, r.hi_proc_ns);
        exit(1);
      }
      // delta goes -10 -> +10 linearly over [100,200] -> zero at 150.
      if (std::fabs(r.interpolated_proc_ns - 150.0) > 1e-9) {
        fprintf(stderr,
                "FAIL find_crossover: interpolated_proc_ns=%.4f want 150.0\n",
                r.interpolated_proc_ns);
        exit(1);
      }
    }
    // exact-zero at a sample point -> that point IS the crossover.
    {
      CrossoverResult r =
          find_crossover({{0.0, -5.0}, {500.0, 0.0}, {1000.0, 5.0}});
      if (!r.found || r.interpolated_proc_ns != 500.0) {
        fprintf(stderr,
                "FAIL find_crossover: exact-zero case not handled (found=%d "
                "interp=%.4f)\n",
                r.found, r.interpolated_proc_ns);
        exit(1);
      }
    }
  }

  // check_seq: matching seq is a silent no-op (doesn't exit); mismatch is
  // tested via a subprocess-style manual check isn't feasible here without a
  // framework, so this only exercises the success path directly — the
  // failure path (fatal exit) is exercised implicitly by every real pass
  // never triggering it.
  check_seq(5, 5);

  // ns_to_cycles / pace deadline arithmetic, with g_tsc_ghz fixed to 1.0 (1
  // cycle == 1 ns) so expected values are exact integers, no hardware
  // dependency.
  {
    g_tsc_ghz = 1.0;
    if (ns_to_cycles(0) != 0 || ns_to_cycles(1000) != 1000 ||
        ns_to_cycles(20000) != 20000) {
      fprintf(stderr, "FAIL ns_to_cycles at g_tsc_ghz=1.0\n");
      exit(1);
    }
    // Absolute-deadline schedule exactness: message n's deadline is
    // t_start + n*gap_cycles, no drift accumulation regardless of how late
    // any earlier message actually published. Exercised via the real
    // sched_deadline() function (not a hand-duplicated formula) at a few
    // literal gap values.
    uint64_t t_start = 1'000'000;
    for (uint64_t gap_ns : {0ull, 1000ull, 20000ull}) {
      uint64_t gap_cycles = ns_to_cycles(gap_ns);
      for (uint64_t n = 0; n < 5; n++) {
        uint64_t got = sched_deadline(t_start, n, gap_cycles);
        uint64_t want = t_start + n * gap_cycles;
        if (got != want) {
          fprintf(stderr,
                  "FAIL sched_deadline at gap_ns=%llu n=%llu: got %llu want "
                  "%llu\n",
                  (unsigned long long)gap_ns, (unsigned long long)n,
                  (unsigned long long)got, (unsigned long long)want);
          exit(1);
        }
      }
    }

    // Late-publish classification: exercises the REAL pace_until(), not a
    // hand-rolled `now > deadline` comparison duplicating its logic — a
    // prior version of this test compared two plain ints and never called
    // pace_until at all, so it could not have caught a bug in the function
    // it claimed to test.
    //
    // A deadline of 0 is unconditionally in the past (rdtsc_now() is a
    // hardware cycle counter, never 0 by the time this runs): pace_until
    // must report late_on_entry=true and return immediately, no spinning.
    if (pace_until(0) != true) {
      fprintf(stderr, "FAIL pace_until(0): want late=true (deadline already "
                      "past)\n");
      exit(1);
    }
    // A deadline comfortably in the future must report late_on_entry=false
    // and actually pace up to it. FUTURE_DEADLINE_OFFSET_CYCLES just needs
    // to clear the few-dozen-cycle gap between computing the deadline here
    // and pace_until's own entry rdtsc_now() call, with margin — the test
    // still completes in well under a microsecond of real spinning.
    constexpr uint64_t FUTURE_DEADLINE_OFFSET_CYCLES = 100000;
    if (pace_until(rdtsc_now() + FUTURE_DEADLINE_OFFSET_CYCLES) != false) {
      fprintf(stderr, "FAIL pace_until(future): want late=false (deadline "
                      "not yet reached)\n");
      exit(1);
    }
  }

  // pct() sanity — same values smt_pingpong.cpp's own self-test already
  // established, just confirming the shared header linked in correctly.
  g_tsc_ghz = 1.0;
  std::vector<uint32_t> sorted{10, 20, 30, 40};
  if (pct(sorted, 50) != 25.0) {
    fprintf(stderr, "FAIL pct sanity check\n");
    exit(1);
  }

  printf("all tests passed\n");
}

// ===========================================================================
// --proc-sweep (WP3): measure whether SMT-sibling or same-CCX placement is
// faster end-to-end vs. consumer per-message processing weight, and find
// the crossover — see the file-scope comment's "The crux — matched pacing"
// section and Findings 1/2 for the design this implements.
// ===========================================================================

// Per-(tier, level) result: the raw pass metrics plus the validity gate
// verdict (backpressure_count==0 AND
// waited_fraction>=PROC_SWEEP_WAITED_FRACTION_MIN — a proc-sweep-specific
// threshold; see PipeCfg::PROC_SWEEP_WAITED_FRACTION_MIN for why it must
// NOT reuse the even sweep's WAITED_FRACTION_MIN).
struct ProcSweepCellResult {
  int rounds;
  double calibrated_proc_ns;
  uint64_t gap_ns;
  PassMetrics metrics;
  bool valid;
};

static double proc_insitu_p50_ns(const PassMetrics &m) {
  std::vector<uint32_t> pc = m.proc_cycles;
  std::sort(pc.begin(), pc.end());
  return pct(pc, 50);
}

static double waited_fraction_of(const PassMetrics &m) {
  return m.sampled_count ? double(m.waited_count) / double(m.sampled_count)
                         : 0.0;
}

static bool proc_sweep_cell_valid(const PassMetrics &m) {
  return m.backpressure_count == 0 &&
         waited_fraction_of(m) >= PipeCfg::PROC_SWEEP_WAITED_FRACTION_MIN;
}

static ProcSweepCellResult run_proc_sweep_cell(int rounds, double calibrated_ns,
                                               uint64_t gap_ns,
                                               int consumer_cpu,
                                               int producer_cpu,
                                               const std::vector<Msg> &ring) {
  // Pause only (see the file-scope "WAIT POLICY" note): a bare spin would
  // port-starve an SMT-sibling producer, and a futex-based block/wake's
  // microsecond-scale round trip would swamp the ns-scale processing-weight
  // signal this sweep is trying to isolate.
  PassMetrics m = run_pass(consumer_cpu, producer_cpu, gap_ns, rounds, ring);
  return {rounds, calibrated_ns, gap_ns, std::move(m),
          proc_sweep_cell_valid(m)};
}

static void print_proc_sweep_cell(const ProcSweepCellResult &r) {
  std::vector<uint32_t> lat = r.metrics.latency_cycles;
  std::sort(lat.begin(), lat.end());
  std::vector<uint32_t> ia = r.metrics.inter_arrival_cycles;
  std::sort(ia.begin(), ia.end());
  printf("  rounds=%-6d calib=%8.1fns gap=%7lluns  e2e p50 %9.1f  p99 "
         "%9.1f  proc-insitu p50 %8.1f  waited-frac %.4f  backpressure "
         "%llu  achieved-ia p50 %8.1f%s\n",
         r.rounds, r.calibrated_proc_ns, (unsigned long long)r.gap_ns,
         pct(lat, 50), pct(lat, 99), proc_insitu_p50_ns(r.metrics),
         waited_fraction_of(r.metrics),
         (unsigned long long)r.metrics.backpressure_count, pct(ia, 50),
         r.valid ? "" : "  INVALID (wait-path contaminated)");
}

static void run_proc_sweep(const Topology &topo, const std::vector<Msg> &ring) {
  // Fail-fast: both a sibling and a same-CCX peer are required, or the
  // headline question ("where does sibling vs same-CCX cross over?") has
  // nothing to compare and is unanswerable on this box.
  if (topo.sibling < 0 || topo.same_l3 < 0) {
    fprintf(stderr,
            "fatal: --proc-sweep requires BOTH an SMT-sibling AND a "
            "same-CCX peer of cpu%d to be present; missing: %s%s%s"
            "the question \"where does sibling-vs-same-CCX placement cross "
            "over as processing weight grows?\" has nothing to compare "
            "against without both tiers, so it is unanswerable on this "
            "box.\n",
            topo.base, topo.sibling < 0 ? "sibling " : "",
            topo.same_l3 < 0 ? "same-CCX " : "",
            (topo.sibling < 0) == (topo.same_l3 < 0) ? "" : "");
    exit(1);
  }

  const size_t n_levels = std::size(PipeCfg::PROC_SWEEP_ROUNDS);

  printf("=== --proc-sweep: crossover between processing weight and "
         "placement ===\n\n");
  // Both predicted bounds (Finding 2): ~50ns is where the crossover would
  // sit if the producer ran hot (unpaced, ~81% sibling-contention tax, same
  // regime sibling_noise.cpp measures: crossover ~= 40ns/0.81); ~1.3us is
  // where it sits if the producer stays polite under this sweep's matched
  // pacing (PAUSE-waiting through the consumer's processing window costs
  // only ~3%, so crossover ~= 40ns/0.03). This sweep's ladder (see
  // PipeCfg::PROC_SWEEP_ROUNDS) brackets both.
  printf("prediction: crossover between ~50ns (producer ran HOT: ~81%% "
         "sibling-contention tax, crossover ~= 40ns/0.81) and ~1.3us "
         "(producer stays POLITE under this sweep's matched pacing: ~3%% "
         "tax, crossover ~= 40ns/0.03). See the README's \"SPSC pipeline\" "
         "section for the derivation.\n\n");

  // Calibration: solo, contention-free, on the pinned consumer CPU.
  printf("rounds -> calibrated processing ns (solo, on cpu%d):\n", topo.base);
  std::vector<double> calibrated;
  calibrated.reserve(n_levels);
  for (int rounds : PipeCfg::PROC_SWEEP_ROUNDS) {
    double ns = calibrate_proc_level_ns(topo.base, uint32_t(rounds), ring);
    calibrated.push_back(ns);
    printf("  rounds=%-6d  %9.1f ns\n", rounds, ns);
  }
  printf("\n");

  const std::vector<uint64_t> gaps = proc_sweep_cells(calibrated);

  struct TierRun {
    const char *label;
    int producer_cpu;
  };
  std::vector<TierRun> tiers = {{"SMT sibling", topo.sibling},
                                {"same L3/CCX", topo.same_l3}};
  // Cross-CCX is a 3rd REFERENCE line only (not part of the crossover
  // computation below), and only if this box actually has one.
  if (topo.other_l3 >= 0)
    tiers.push_back({"cross CCX", topo.other_l3});

  std::vector<std::vector<ProcSweepCellResult>> results; // [tier][level]
  for (const TierRun &tier : tiers) {
    printf("=== tier: %s (cpu%d <-> cpu%d), WaitPolicy::Pause ===\n",
           tier.label, topo.base, tier.producer_cpu);
    std::vector<ProcSweepCellResult> tier_results;
    tier_results.reserve(n_levels);
    for (size_t i = 0; i < n_levels; i++) {
      ProcSweepCellResult r =
          run_proc_sweep_cell(PipeCfg::PROC_SWEEP_ROUNDS[i], calibrated[i],
                              gaps[i], topo.base, tier.producer_cpu, ring);
      print_proc_sweep_cell(r);
      tier_results.push_back(std::move(r));
    }
    results.push_back(std::move(tier_results));
    printf("\n");
  }

  // SUMMARY: sibling vs same-CCX only (tiers[0]/tiers[1] by construction
  // above; cross-CCX, if present, is tiers[2] and is a reference line only,
  // not part of this comparison or the crossover search).
  printf("SUMMARY (sibling vs same-CCX; delta_ns = sibling_p50 - "
         "sameccx_p50; proc_insitu_ratio = sibling/sameccx in-situ "
         "processing p50):\n");
  printf("  %-8s %14s %14s %12s %12s\n", "rounds", "sibling_p50", "sameccx_p50",
         "delta_ns", "proc_ratio");
  std::vector<std::pair<double, double>> crossover_points; // (proc_ns, delta)
  bool any_invalid = false;
  for (size_t i = 0; i < n_levels; i++) {
    const ProcSweepCellResult &sib = results[0][i];
    const ProcSweepCellResult &sc = results[1][i];

    std::vector<uint32_t> sib_lat = sib.metrics.latency_cycles;
    std::sort(sib_lat.begin(), sib_lat.end());
    std::vector<uint32_t> sc_lat = sc.metrics.latency_cycles;
    std::sort(sc_lat.begin(), sc_lat.end());
    double sib_p50 = pct(sib_lat, 50), sc_p50 = pct(sc_lat, 50);
    double delta = sib_p50 - sc_p50;

    double sib_proc = proc_insitu_p50_ns(sib.metrics);
    double sc_proc = proc_insitu_p50_ns(sc.metrics);
    double ratio = sc_proc != 0.0 ? sib_proc / sc_proc : 0.0;

    bool row_valid = sib.valid && sc.valid;
    printf("  rounds=%-6d %14.1f %14.1f %12.1f %12.4f%s\n", sib.rounds, sib_p50,
           sc_p50, delta, ratio, row_valid ? "" : "  (excluded: INVALID)");
    if (row_valid)
      crossover_points.push_back({sib.calibrated_proc_ns, delta});
    else
      any_invalid = true;
  }

  if (any_invalid)
    printf("\nnote: at least one rounds level had an INVALID (wait-path "
           "contaminated) cell in sibling or same-CCX and was excluded from "
           "the crossover computation below.\n");

  if (crossover_points.size() < 2) {
    printf("\ncrossover: not determinable — fewer than 2 valid (rounds, "
           "delta) points to bracket a sign change.\n");
  } else {
    CrossoverResult cr = find_crossover(crossover_points);
    if (cr.found) {
      printf("\ncrossover: bracketed between proc_ns=%.1f (delta=%.1f) and "
             "proc_ns=%.1f (delta=%.1f); linear-interpolated crossover at "
             "proc_ns ~= %.1f\n",
             cr.lo_proc_ns, cr.lo_delta, cr.hi_proc_ns, cr.hi_delta,
             cr.interpolated_proc_ns);
    } else {
      bool all_neg = std::all_of(
          crossover_points.begin(), crossover_points.end(),
          [](const std::pair<double, double> &p) { return p.second < 0.0; });
      bool all_pos = std::all_of(
          crossover_points.begin(), crossover_points.end(),
          [](const std::pair<double, double> &p) { return p.second > 0.0; });
      const char *who = all_neg   ? "sibling"
                        : all_pos ? "same-CCX"
                                  : "neither consistently";
      printf("\ncrossover: none found through proc_ns up to %.1f — %s wins "
             "throughout the measured range (no sign change in delta_ns).\n",
             crossover_points.back().first, who);
    }
  }

  printf("\nSCOPE NOTE: the message source here is emulated in-memory by "
         "design (build_source_ring + rigtorp::SPSCQueue) so this sweep "
         "isolates placement and processing weight from I/O — a real socket's "
         "microsecond-scale recv() would dominate end-to-end latency and put "
         "this question in a different regime entirely; this result answers "
         "\"given a message is already in hand, does placement or processing "
         "weight decide who's faster\", not \"is this fast enough for a real "
         "feed\". Also: no core isolation — absolute numbers are "
         "OS-jitter-sensitive, so compare tiers within this run only.\n");
}

int main(int argc, char **argv) {
  if (argc == 2 && std::strcmp(argv[1], "--test") == 0) {
    run_self_tests();
    return 0;
  }
  // No-arg and --proc-sweep are aliases: this binary now runs exactly one
  // experiment (see the file-scope comment).
  if (argc == 2 && std::strcmp(argv[1], "--proc-sweep") != 0) {
    fprintf(stderr, "usage: %s [--test | --proc-sweep]\n", argv[0]);
    return 1;
  }
  if (argc > 2) {
    fprintf(stderr, "usage: %s [--test | --proc-sweep]\n", argv[0]);
    return 1;
  }

  check_invariant_tsc();
  calibrate();
  printf("TSC: %.4f GHz\n\n", g_tsc_ghz);

  Topology topo = discover_topology();
  printf("consumer anchored on cpu%d | sibling=%d same_l3=%d other_l3=%d\n\n",
         topo.base, topo.sibling, topo.same_l3, topo.other_l3);

  auto ring = build_source_ring();

  run_proc_sweep(topo, ring);
  return 0;
}
