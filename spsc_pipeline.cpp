// ============================================================================
// spsc_pipeline.cpp — latency-vs-core-utilization crossover for a bounded
// producer/consumer handoff, across topology placement and wait policy.
// ----------------------------------------------------------------------------
// smt_pingpong.cpp / sibling_noise.cpp measure two *already-spinning* threads
// handing a cache line back and forth, or one thread's throughput under a
// noisy sibling. Neither has a "core-idle" option: both keep every CPU
// involved fully busy the whole time. Real pipelines don't always have
// messages in flight — when the arrival rate is low, spinning to catch the
// next message wastes an entire core (or, on an SMT sibling, half a physical
// core) for nothing. This experiment asks: at what arrival rate does it stop
// being worth it to spin, and how much extra latency do you pay to let the
// consumer sleep instead?
//
// SHAPE OF THE EXPERIMENT
// ------------------------
// A producer thread manufactures fixed-layout messages (Msg, below) at a
// paced arrival rate and pushes them into a rigtorp::SPSCQueue<Msg>. A
// consumer thread pops them, does a small deterministic amount of processing,
// and records the end-to-end latency: consumer-completion TSC minus the
// publish TSC stamped into the message at the moment the producer first
// tried to publish it. Three axes are swept:
//
//   * PLACEMENT TIER   — consumer anchored on cpu0; producer on an SMT
//     sibling / same-CCX peer / cross-CCX peer of cpu0 (topology discovery
//     copied from smt_pingpong.cpp's auto-mode pair selection).
//   * WAIT POLICY       — how the consumer waits for the next message:
//     BareSpin (tight re-poll), Pause (re-poll + _mm_pause), or Blocking
//     (park on a futex when the queue is empty — see "THE FUTEX PROTOCOL"
//     below).
//   * ARRIVAL RATE       — the producer's inter-message gap, swept from 0
//     (saturated, back-to-back) to 20us (clearly sub-saturated).
//
// The headline: at gap=0 all three wait policies should converge (nothing is
// ever idle long enough for Blocking's park path to matter, and its
// wake-syscall count should be ~0 because the fast conditional-wake path
// never needs to call into the kernel). As the gap grows, BareSpin/Pause keep
// burning 100% of their core for a shrinking trickle of messages while
// Blocking's consumer core goes idle between messages — at the cost of the
// futex park/wake round trip added to each message's tail latency. Where
// that crossover sits, and how large the tax is, is what this binary reports.
//
// THE FUTEX PROTOCOL (the correctness-critical part of this file)
// ------------------------------------------------------------------
// Blocking's "sleep the consumer" mechanism is a hand-rolled eventcount built
// on one 32-bit word (`ParkWord::v`) and the raw futex(2) syscall — not
// std::condition_variable, because the whole point is to measure the actual
// syscall-level park/wake cost, not whatever a condvar's internal mutex adds
// on top. The correctness hazard is the classic missed-wakeup race: the
// producer must never publish a message, see "nobody is parked", and skip
// the wake — while the consumer is mid-way through announcing that it's
// about to park. See the fence comment on `publish_and_maybe_wake` below for
// the exact interleaving this would allow and why a pair of seq_cst
// std::atomic_thread_fence calls — one on each thread, between that thread's
// store and its subsequent load — closes it (the textbook Dekker/StoreLoad
// pattern; formally correct per the standard, not an x86-codegen accident).
// The MISSED-WAKE STRESS test in `--blocking-probe` exercises the park/wake
// protocol under randomized timing, but the race window here is a few
// nanoseconds wide and not reliably triggerable from userspace — it is
// protocol smoke coverage, not a regression test for the fence itself. The
// fence's correctness rests on the analytic Dekker argument below, not on
// any test catching its removal.
//
// WHAT THIS FILE DELIBERATELY DOES NOT DO
// ------------------------------------------
// No mwaitx/monitorx/CPUID/WAITPKG anywhere — that family of "hardware wait"
// instructions was measured non-viable for this kind of sub-microsecond
// handoff in an earlier phase of this project (~260ns p50 wake vs ~60ns for
// a spin, and a ~350-480ns timeout floor on this Zen 5 box); see the README's
// "SPSC pipeline" section ("The measured mwaitx detour") for the numbers.
// This file also does not spin-then-park hybrid before falling back to
// Blocking's futex path — Blocking here is a pure park-on-first-empty
// policy. A spin-then-park hybrid (spin briefly, then park if still empty)
// is very likely a better latency/utilization trade at the low end of the
// gap sweep and is noted as future work in the README's "Future work"
// subsection rather than implemented here, to keep this pass's three wait
// policies simple, orthogonal, and easy to reason about.
//
// build: g++ -O3 -std=c++23 -pthread spsc_pipeline.cpp -o spsc_pipeline
//        -I <path-to-rigtorp-SPSCQueue-include>   (prefer the CMake build)
// run:   ./spsc_pipeline                 (full tier x policy x gap sweep)
//        ./spsc_pipeline --test          (pure-logic self-check, no hardware)
//        ./spsc_pipeline --blocking-probe (real futex round-trip + missed-
//                                          wake stress test; no rdtsc/topology
//                                          dependency, safe to run in CI)
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

#include <cerrno>
#include <cstring>
#include <ctime>
#include <linux/futex.h>
#include <random>
#include <sys/syscall.h>
#include <unistd.h>

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
// WaitPolicy: how the consumer waits for the next message. Templated (not a
// runtime enum tested per-iteration) for the same reason pp_core.hpp's
// relax<bool Pause>() is templated — see its "ponytail" comment: the compiler
// specializes a dedicated wait loop per policy, so the hot loop never spends
// a branch on which policy it's running.
// ---------------------------------------------------------------------------
enum class WaitPolicy { BareSpin, Pause, Blocking };

static const char *policy_name(WaitPolicy p) {
  switch (p) {
  case WaitPolicy::BareSpin:
    return "BareSpin";
  case WaitPolicy::Pause:
    return "Pause";
  case WaitPolicy::Blocking:
    return "Blocking";
  }
  return "?";
}

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

  // Deterministic-mixing rounds the consumer performs per message in
  // process_msg(). Sized to land in the tens-of-ns range on a modern core —
  // enough to be a real, observable "processing" cost without dominating the
  // pass runtime at the busiest (gap=0) rows. Not derived from a specific
  // target latency, just picked and left alone once it looked right.
  static constexpr int PROC_ROUNDS = 24;

  // Producer inter-message gap sweep, nanoseconds. 0 = saturated baseline
  // (back-to-back publishes, no pacing wait at all — this is the row where
  // all three wait policies should converge and Blocking's wake-syscall
  // count should be ~0, because the queue is essentially never empty).
  // 1000ns sits near where this box's futex park/wake round trip starts to
  // matter (the crossover). 5000/20000ns are the clear sub-saturated regime,
  // where BareSpin/Pause visibly waste a whole core spinning for a trickle
  // of messages and Blocking's parked-time fraction should approach 1.
  static constexpr uint64_t GAP_NS[] = {0, 250, 1000, 5000, 20000};

  // --bursty sweep: burst lengths tested at a fixed idle/intra gap (below).
  // 1 is the even-schedule anchor (see sched_deadline's equivalence case,
  // asserted in run_self_tests) — a burst of length 1 IS the even sweep, so
  // this row is a live cross-check that the bursty codepath reduces to the
  // already-validated one. 64 is chosen to stay comfortably under
  // QUEUE_CAPACITY (4096) so a single burst never risks backpressure purely
  // from its own size, independent of how fast the consumer drains it.
  static constexpr uint64_t BURST_LENS[] = {1, 4, 16, 64};

  // --bursty sweep: gap between the last message of one burst and the first
  // message of the next, nanoseconds. Deliberately >> a futex park entry +
  // wake round trip (order-1us on this box, see the README's futex protocol
  // section) so Blocking reliably has time to actually park between bursts
  // rather than racing its own wake — the whole point of the bursty axis is
  // to observe a clean park-drain-park cycle, not a partially-amortized one.
  static constexpr uint64_t BURST_IDLE_GAP_NS = 20000;

  // --bursty sweep: gap between messages WITHIN a burst, nanoseconds. 0 =
  // back-to-back — the messages of one burst are modeled as already sitting
  // in a socket/ring buffer by the time the first one is observed, so there
  // is no pacing wait between them (this is also what makes every non-head
  // message's deadline equal the burst's start time — see the late-publish
  // note on the intra_gap=0 semantics in print_pass/README).
  static constexpr uint64_t BURST_INTRA_GAP_NS = 0;

  // Two-phase pace_until() switches from _mm_pause to a bare spin once
  // within this many TSC cycles of the deadline. 128 cycles (~40-65ns on a
  // 2-3GHz TSC) approximates one PAUSE's ~64-cycle park window (see
  // pp_core.hpp's relax<> comment) — close enough to the deadline that a
  // pause's own latency risks overshooting it, so the tail is bare-spun for
  // the tightest possible deadline detection.
  static constexpr uint64_t PACE_TAIL_CYCLES = 128;

  // Acceptable relative deviation of achieved p50 inter-arrival from the
  // target gap, as a fraction (0.05 = +/-5%). Used only as a reporting
  // threshold (flagging a row, not failing the run) since achieved pacing is
  // inherently environment-sensitive without core isolation.
  static constexpr double PACE_TOL = 0.05;

  // Minimum acceptable waited-fraction (share of messages whose first front()
  // poll found the queue empty) for a "clearly sub-saturated" gap row to be
  // considered a valid measurement of the wait path. Purely a reporting
  // threshold, not an assertion.
  static constexpr double WAITED_FRACTION_MIN = 0.99;

  // Backstop timeout on every futex_wait call: deadlock insurance only, not
  // part of the intended protocol. If the conditional-wake logic is correct
  // this should fire ~0 times per run; a nonzero count is a smoking gun for a
  // missed wake. ~100ms is far longer than any legitimate inter-message gap
  // in this sweep (max 20us) but short enough that a genuine bug still shows
  // up promptly rather than hanging the run.
  static constexpr int FUTEX_BACKSTOP_TIMEOUT_MS = 100;

  // Deterministic source-ring payload generation: shared init/seed and
  // multiplicative-mix constant, same style and same values as
  // sibling_noise.cpp's NoiseCfg (an odd 32-bit Knuth multiplicative-hash
  // constant defeats "output is a trivial function of input"; the seed just
  // needs to be a non-trivial non-zero starting state). Defined separately
  // here rather than shared with NoiseCfg because sibling_noise.cpp is
  // out-of-scope committed code this file must not touch or depend on.
  static constexpr uint64_t INIT_SEED = 0xdeadbeefULL;
  static constexpr uint64_t MIX_CONST = 2654435761ull;
};

// ===========================================================================
// The futex eventcount. One 32-bit word on its own 128B-aligned region (same
// false-sharing discipline as pp_core.hpp's Slot): 0 = no consumer parked,
// 1 = consumer parked (or about to be).
// ===========================================================================
struct alignas(128) ParkWord {
  std::atomic<uint32_t> v{0};
  char pad[128 - sizeof(std::atomic<uint32_t>)]{};
};

// Count of FUTEX_WAKE syscalls actually issued (i.e. only on the conditional
// fast-path hit, never a syscall-per-message when the queue stays non-empty)
// and of FUTEX_WAIT timeouts hit (the backstop firing — see PipeCfg comment).
// Reset per pass by the caller diffing before/after snapshots.
inline std::atomic<uint64_t> g_futex_wake_syscalls{0};
inline std::atomic<uint64_t> g_futex_backstop_fires{0};

// Count of FUTEX_WAIT calls that returned EAGAIN (the kernel's value check
// found the word already changed and returned immediately without ever
// parking). This is a genuinely "cold" outcome for latency purposes — the
// consumer still paid for a syscall entry/exit — but it is NOT a park+wake
// round trip, so it's tracked separately from a backstop fire (a real park
// that then times out) and from a normal wake. Mirrors g_futex_backstop_fires:
// reset per pass by the caller diffing before/after snapshots (see
// run_policy_sweep).
inline std::atomic<uint64_t> g_futex_eagain{0};

// ---------------------------------------------------------------------------
// Raw futex(2) syscall wrapper. Using SYS_futex directly (not
// std::condition_variable) is the point of this experiment: we want to
// measure the actual kernel park/wake primitive, not whatever a condvar's
// internal mutex adds on top.
// ---------------------------------------------------------------------------
static long sys_futex(std::atomic<uint32_t> *addr, int op, uint32_t val,
                      const struct timespec *timeout) {
  // The futex(2) ABI operates on a plain 32-bit word at this address; the
  // reinterpret_cast below is only well-defined (and only matches what the
  // kernel actually reads/writes) if std::atomic<uint32_t> has no extra
  // bookkeeping and its ops never fall back to a lock-based emulation.
  static_assert(std::atomic<uint32_t>::is_always_lock_free &&
                    sizeof(std::atomic<uint32_t>) == sizeof(uint32_t),
                "futex(2) requires a bare, always-lock-free 32-bit word");
  return syscall(SYS_futex, reinterpret_cast<uint32_t *>(addr), op, val,
                 timeout, nullptr, 0);
}

using WakeFn = void (*)(std::atomic<uint32_t> *, int);
using WaitFn = void (*)(std::atomic<uint32_t> *, uint32_t,
                        const struct timespec *);

// Real FUTEX_WAKE_PRIVATE. Counted so a pass can report how many wake
// syscalls it actually issued (should be ~0 at gap=0, ~SAMPLE_MSGS at the
// most sub-saturated gap — proving the conditional fast path is doing its
// job rather than syscalling on every publish).
static void real_futex_wake(std::atomic<uint32_t> *addr, int n) {
  g_futex_wake_syscalls.fetch_add(1, std::memory_order_relaxed);
  sys_futex(addr, FUTEX_WAKE_PRIVATE, uint32_t(n), nullptr);
}

// Real FUTEX_WAIT_PRIVATE with the backstop timeout. ETIMEDOUT is the only
// "unexpected" outcome we count — EAGAIN (value changed before the kernel
// actually parked us) and a normal wake are both the intended protocol
// working, not something to flag.
static void real_futex_wait(std::atomic<uint32_t> *addr, uint32_t expected,
                            const struct timespec *timeout) {
  long rc = sys_futex(addr, FUTEX_WAIT_PRIVATE, expected, timeout);
  if (rc == -1 && errno == ETIMEDOUT)
    g_futex_backstop_fires.fetch_add(1, std::memory_order_relaxed);
  else if (rc == -1 && errno == EAGAIN)
    g_futex_eagain.fetch_add(1, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// maybe_wake: the conditional fast path. One relaxed load; only on a hit do
// we clear the word and pay for a syscall. Clearing BEFORE calling wake_fn
// matters: if a consumer is mid-FUTEX_WAIT right now, clearing first means
// the kernel's value check inside futex_wait sees 0 and returns EAGAIN
// immediately (no sleep, no missed wake) rather than racing the wake.
//
// `wake_fn` is a parameter (not a hardwired call) so WP2's pure-logic test
// can inject a counting stub and assert the two required behaviours
// directly: not called when parked==0; called exactly once, after the clear,
// when parked==1.
// ---------------------------------------------------------------------------
static inline bool maybe_wake(ParkWord &pw, WakeFn wake_fn = real_futex_wake) {
  if (pw.v.load(std::memory_order_relaxed) == 1) {
    pw.v.store(0, std::memory_order_relaxed); // clear first — see comment above
    wake_fn(&pw.v, 1);
    return true;
  }
  return false;
}

// ---------------------------------------------------------------------------
// publish_and_maybe_wake: the SINGLE publish path used by every WaitPolicy,
// not just Blocking. This is deliberate: it keeps the fence+load cost
// constant across all three policies' rows, so a BareSpin vs Blocking
// comparison at the same gap is comparing wait-side behaviour only, not
// paying a fence tax on some rows and not others. (~One seq_cst fence per
// published message is the tax every row pays for Blocking's correctness;
// see the README's "SPSC pipeline" section, "The futex conditional-wake
// protocol", for its measured cost.)
//
// Returns false (no side effect — try_emplace either fully succeeds or does
// nothing) if the queue is full; the caller retries WITHOUT re-stamping
// seq/pub_tsc, so a backpressure retry never lies about when the message was
// first offered.
//
// THE FENCE — WHY IT IS REQUIRED, NOT DEFENSIVE PROGRAMMING:
// try_emplace() release-publishes writeIdx_ (a plain `mov` on x86-TSO — see
// pp_core.hpp's "why atomics" discussion for why a release store costs
// nothing extra there). x86-TSO does NOT guarantee that a later load (here,
// parked.load()) waits for an earlier store (writeIdx_'s release) to become
// globally visible before the load executes — stores can sit in the store
// buffer while later loads proceed. Without an intervening seq_cst fence,
// this interleaving is possible:
//
//   consumer: front() sees empty -> parked.store(1, relaxed) -> re-checks
//             front(), STILL sees empty (producer's writeIdx_ store hasn't
//             drained from its store buffer yet) -> calls futex_wait
//   producer: try_emplace() succeeds (writeIdx_ store enters store buffer)
//             -> parked.load() executes EARLY, before writeIdx_ drains,
//             and observes parked==0 -> skips the wake
//
// Result: the message is fully published, the consumer is genuinely parked,
// and nobody wakes it — a missed wake, masked only by the backstop timeout.
//
// THE FIX — a textbook Dekker/StoreLoad fence pair, not a seq_cst store:
// a seq_cst store on one thread paired with a seq_cst fence + relaxed load
// on another is NOT formally guaranteed to give StoreLoad ordering — that
// pairing only "works" on x86 because GCC happens to compile a seq_cst
// store to `xchg`, itself a full barrier. It is x86-codegen-dependent, not
// standard-guaranteed. The formally correct construction is two seq_cst
// FENCES, one on each thread, each sitting between that thread's own store
// and its own subsequent load:
//
//   producer: writeIdx_ store (try_emplace)
//             atomic_thread_fence(seq_cst)   <-- forces the store to drain
//             parked.load(relaxed)
//   consumer: parked.store(1, relaxed)
//             atomic_thread_fence(seq_cst)   <-- forces the store to drain
//             front() re-check (== a load of writeIdx_)
//
// Two seq_cst fences on different threads are totally ordered against each
// other (that ordering is exactly what memory_order_seq_cst guarantees for
// fences, independent of any store's memory order). Whichever fence is
// second in that total order is guaranteed to see everything the other
// thread's store did before its own fence — so at least one side of the
// race above always sees the other's write, closing the missed wake. This
// holds by the standard, not by x86 store-buffer-drain behaviour, and it
// compiles to the same instruction on x86 either way (a fence and a seq_cst
// store both lower to a full barrier here) — so this is a correctness fix
// with no measured cost change. Removing either fence reopens the race; see
// `--blocking-probe`'s MISSED-WAKE STRESS test for protocol smoke coverage
// (NOT a regression test for this fence — the race window is a few ns wide,
// far below what randomized userspace jitter can reliably straddle; see
// that test's comment below for why).
// ---------------------------------------------------------------------------
template <typename Q, typename T>
static inline bool publish_and_maybe_wake(Q &q, const T &msg, ParkWord &pw,
                                          WakeFn wake_fn = real_futex_wake) {
  if (!q.try_emplace(msg))
    return false;
  std::atomic_thread_fence(std::memory_order_seq_cst); // REQUIRED — see above
  maybe_wake(pw, wake_fn);
  return true;
}

// ---------------------------------------------------------------------------
// wait_blocking_impl: the park loop, factored to take a `front` callable
// instead of a concrete Queue so WP2's pure-logic test can drive it with a
// scripted sequence of return values (no real queue, no real futex, no
// threads) and assert the re-check race is closed correctly.
//
//   while (!(m = front())) {
//     parked.store(1, relaxed);          // announce BEFORE the re-check
//     atomic_thread_fence(seq_cst);      // Dekker pair with the producer's
//                                        // fence — see fence comment above
//     if ((m = front())) {               // re-check closes the announce/park
//       parked.store(0, relaxed);        // race: if the message arrived
//       break;                           // between the outer check and here,
//     }                                  // we must not sleep on it
//     wait_fn(&parked, 1, backstop);     // otherwise, actually park
//   }
//
// `parked_out` (WP-A): set to true immediately before the FIRST call to
// wait_fn for this invocation — i.e. the point where a real syscall entry is
// actually paid, whether that call ultimately parks-and-wakes or returns
// EAGAIN (see real_futex_wait's g_futex_eagain counter for that split). It is
// NOT the same as "the message was found on a later poll": if the re-check
// closes the race (front() sees the message on the second poll), wait_fn is
// never called and parked_out stays false — that path is cheap (no syscall)
// even though the outer had_to_wait signal was true. Left false by default so
// callers that don't care (the existing --blocking-probe helpers) can ignore
// it.
// ---------------------------------------------------------------------------
template <typename FrontFn>
static inline auto
wait_blocking_impl(FrontFn front, ParkWord &pw, WaitFn wait_fn,
                   const struct timespec &backstop,
                   bool *parked_out = nullptr) -> decltype(front()) {
  decltype(front()) m;
  while (!(m = front())) {
    pw.v.store(1,
               std::memory_order_relaxed); // announce — see fence comment above
    std::atomic_thread_fence(
        std::memory_order_seq_cst); // REQUIRED — Dekker pair
    if ((m = front())) {
      pw.v.store(0,
                 std::memory_order_relaxed); // race closed, undo the announce
      break;
    }
    if (parked_out)
      *parked_out =
          true; // about to pay for a syscall entry — see comment above
    wait_fn(&pw.v, 1, &backstop);
  }
  return m;
}

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
      0;                      // deadline already passed when we reached publish
  uint64_t parked_cycles = 0; // TSC cycles spent inside wait_fn (Blocking only)

  // Consumer-side TSC stamps bracketing just the sampled (post-WARM_MSGS)
  // window, taken at the top of the n==WARM_MSGS iteration and at the
  // bottom of the final iteration. parked_fraction (see print_pass) divides
  // parked_cycles by this window rather than by the caller's own wall-clock
  // straddling thread pin/spin-up + WARM_MSGS: that wider window inflates
  // the denominator, so a fully-parked pass would top out around 0.83
  // instead of approaching 1.0.
  uint64_t sampled_wall_start_tsc = 0;
  uint64_t sampled_wall_end_tsc = 0;

  // Global futex counter snapshots taken at the SAME WARM->SAMPLE boundary
  // as sampled_wall_start_tsc above (n == WARM_MSGS, consumer side). The
  // caller (run_policy_sweep) diffs its own post-pass reading of the
  // corresponding g_futex_* counter against these instead of a pre-run_pass
  // snapshot, so every per-pass instrument -- wake/eagain/backstop counts,
  // parked_fraction, and the cold/warm counts -- is measured over the exact
  // same sample-only window. Before this, wake/eagain/backstop were diffed
  // over the WHOLE pass (WARM_MSGS+SAMPLE_MSGS) while n_bursts and the
  // cold/warm counts were sample-only, inflating wakes-per-burst by exactly
  // (WARM_MSGS+SAMPLE_MSGS)/SAMPLE_MSGS -- see the README's MUST-FIX 1 note.
  uint64_t wake_syscalls_at_sample_start = 0;
  uint64_t backstop_fires_at_sample_start = 0;
  uint64_t eagain_at_sample_start = 0;

  // WP-A: cold/warm latency split, consumer-observed — see cold_tag's
  // comment for the exact (and deliberately asymmetric) definition per wait
  // policy. `latency_cycles` above is left untouched (same values, same
  // order) so the existing even-sweep output/consumers stay comparable;
  // these are an additional, parallel breakdown of the same samples.
  std::vector<uint32_t> cold_latency_cycles;
  std::vector<uint32_t> warm_latency_cycles;

  // 4-way cross-tab: cold/warm crossed with burst-head/non-head (WP-B/C;
  // burst_len=1 in the even sweep makes every message a head, so
  // cold_nonhead/warm_nonhead are always 0 there — see is_burst_head).
  uint64_t cold_head = 0;
  uint64_t cold_nonhead = 0;
  uint64_t warm_head = 0;
  uint64_t warm_nonhead = 0;
};

// wait_for_message<P>: the consumer's per-message wait, specialized per
// WaitPolicy at compile time (see the WaitPolicy comment above for why).
// `had_to_wait` receives true/false for whether this call's FIRST front()
// poll found the queue empty, so the caller can fold it into PassMetrics
// only during the sampled window (warm-up messages don't pollute
// waited-fraction). `parked` (WP-A) is Blocking-specific: set true iff
// wait_blocking_impl actually invoked wait_fn (a real syscall entry) for
// this message; BareSpin/Pause never touch it, since a spin policy's "cold"
// classification only ever needs had_to_wait (see cold_tag below).
template <WaitPolicy P>
static inline Msg *wait_for_message(Queue &q, ParkWord &pw, bool &had_to_wait,
                                    bool &parked);

template <>
inline Msg *wait_for_message<WaitPolicy::BareSpin>(Queue &q, ParkWord &,
                                                   bool &had_to_wait, bool &) {
  Msg *m = q.front();
  had_to_wait = (m == nullptr);
  while (!m) {
    relax<false>(); // pp_core.hpp: Pause=false -> tight re-load, no PAUSE
    m = q.front();
  }
  return m;
}

template <>
inline Msg *wait_for_message<WaitPolicy::Pause>(Queue &q, ParkWord &,
                                                bool &had_to_wait, bool &) {
  Msg *m = q.front();
  had_to_wait = (m == nullptr);
  while (!m) {
    relax<true>(); // pp_core.hpp: Pause=true -> _mm_pause each spin
    m = q.front();
  }
  return m;
}

template <>
inline Msg *wait_for_message<WaitPolicy::Blocking>(Queue &q, ParkWord &pw,
                                                   bool &had_to_wait,
                                                   bool &parked) {
  had_to_wait = (q.front() == nullptr);
  parked = false;
  struct timespec backstop {};
  backstop.tv_sec = PipeCfg::FUTEX_BACKSTOP_TIMEOUT_MS / 1000;
  backstop.tv_nsec = (PipeCfg::FUTEX_BACKSTOP_TIMEOUT_MS % 1000) * 1000000L;
  return wait_blocking_impl([&] { return q.front(); }, pw, real_futex_wait,
                            backstop, &parked);
}

// ---------------------------------------------------------------------------
// cold_tag (WP-A): pure classifier for whether a sampled message's latency
// belongs in the "cold" or "warm" bucket, consumer-observed. Deliberately
// asymmetric between spin policies and Blocking — see the WP-A plan this
// implements:
//   * BareSpin/Pause: cold iff the first front() poll returned null
//     (had_to_wait). There is no cheaper/more-expensive path inside a spin
//     loop — every extra iteration costs the same tight re-poll — so
//     had_to_wait is the only signal available or needed.
//   * Blocking: cold iff wait_blocking_impl actually invoked wait_fn for
//     this message (`parked`), NOT merely had_to_wait. had_to_wait alone
//     would misclassify the "re-check closes the race" case as cold even
//     though no syscall was paid there — parked is the precise signal for
//     "this message's tail paid for a syscall entry" (EAGAIN or a genuine
//     park+wake, both cold; see real_futex_wait's g_futex_eagain split for
//     which).
// A message can have had_to_wait==true (queue observed empty) yet still be
// WARM under Blocking, if the re-check closed the race before any syscall.
// ---------------------------------------------------------------------------
static inline bool cold_tag(bool had_to_wait, bool parked, bool is_blocking) {
  if (is_blocking)
    return parked;
  return had_to_wait;
}

// is_burst_head (WP-B/WP-A): pure predicate — is sequence number `seq` the
// first message of its burst, given a burst length `burst_len`? burst_len=1
// makes every message a "head" (the even-sweep equivalence case: there are
// no non-head messages when every burst is exactly one message long).
static inline bool is_burst_head(uint64_t seq, uint64_t burst_len) {
  return (seq % burst_len) == 0;
}

// ===========================================================================
// Source ring, processing, and the seq-gap detector (WP3).
// ===========================================================================

// Elision guard: the consumer folds every message's checksum in here so the
// compiler can never prove process_msg()'s work is dead and fold it away.
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

// Deterministic, state-mutating "processing": PROC_ROUNDS multiplicative-mix
// passes over the message's 6 payload words. Returns the final checksum
// (folded into g_process_sink by the caller) so this function's result is
// always observably used, same elision-guard discipline as
// sibling_noise.cpp's victim_chunk/tenant_step.
static inline uint64_t process_msg(const Msg &m) {
  uint64_t acc = m.seq ^ PipeCfg::INIT_SEED;
  for (int round = 0; round < PipeCfg::PROC_ROUNDS; round++) {
    for (uint64_t p : m.payload) {
      acc = acc * PipeCfg::MIX_CONST + p;
    }
  }
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

// ---------------------------------------------------------------------------
// ArrivalSpec / sched_deadline (WP-B): a pure closed-form generalization of
// the even sweep's `t_start + n*gap_cycles` schedule to bursty arrival. A
// burst is `burst_len` messages published `intra_gap_cycles` apart, followed
// by `idle_gap_cycles` before the next burst's first message. burst_len=1
// collapses this back to the plain even schedule (see the equivalence
// anchor in run_self_tests): with only one message per "burst", intra_gap
// never applies and every message is idle_gap_cycles after the last, i.e.
// bit-identical to the pre-WP-B `t_start + n*gap_cycles` line.
// ---------------------------------------------------------------------------
struct ArrivalSpec {
  uint64_t burst_len;        // messages per burst; 1 = even-schedule anchor
  uint64_t intra_gap_cycles; // gap between messages WITHIN a burst
  uint64_t idle_gap_cycles;  // gap between the last message of one burst and
                             // the first message of the next
};

// Deadline for message n, given the schedule starts at t_start. Absolute
// (not accumulated from the previous message's actual publish time) for the
// same no-drift-accumulation reason as the even sweep's schedule: a late
// burst never drags every subsequent burst's deadline later with it.
static inline uint64_t sched_deadline(uint64_t t_start, uint64_t n,
                                      const ArrivalSpec &s) {
  uint64_t pos = n % s.burst_len; // this message's position within its burst
  uint64_t burst_idx = n / s.burst_len; // which burst this message belongs to
  // One full burst's wall-clock span: (burst_len-1) intra-gaps to publish
  // the burst's own messages, plus the idle gap before the NEXT burst starts.
  uint64_t period = (s.burst_len - 1) * s.intra_gap_cycles + s.idle_gap_cycles;
  return t_start + burst_idx * period + pos * s.intra_gap_cycles;
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

// Pure predicate (WP-testable without any hardware): should this
// (policy, is_sibling_tier) combination be skipped? BareSpin on the SMT
// sibling tier means the producer and consumer are two hardware threads of
// the SAME physical core — a bare spin on one thread saturates the shared
// core's execution ports the other thread needs to actually issue its
// store/load, starving the peer rather than measuring handoff latency (the
// same reasoning smt_pingpong.cpp ~258-260 applies to its own sibling pair).
// Every other combination — including Blocking on the sibling tier, the
// marquee row: a parked consumer frees the ENTIRE logical CPU (and eases
// port pressure on the physical core) rather than starving its sibling —
// is allowed.
static inline bool should_skip(WaitPolicy policy, bool is_sibling_tier) {
  return policy == WaitPolicy::BareSpin && is_sibling_tier;
}

// ===========================================================================
// One pass: WARM_MSGS + SAMPLE_MSGS messages, one producer/consumer thread
// pair, one (tier, policy, gap) cell of the sweep.
// ===========================================================================
// `burst_len`/`intra_gap_ns`/`idle_gap_ns` (WP-A/B/C): the producer's arrival
// schedule, in ns (converted to an ArrivalSpec in cycles below), plus the
// value that drives the consumer's cold/warm cross-tab head vs. non-head
// split via is_burst_head. Defaults reproduce the pre-WP-B even sweep
// exactly: burst_len=1 makes every message a head and intra_gap_ns is never
// actually used by sched_deadline in that case (see its comment), so only
// idle_gap_ns matters — callers pass the same `gap_ns` for both intra and
// idle to get the historical single-gap sweep. WP-C's bursty sweep passes a
// real burst_len with intra_gap_ns=PipeCfg::BURST_INTRA_GAP_NS and
// idle_gap_ns=PipeCfg::BURST_IDLE_GAP_NS instead.
template <WaitPolicy P>
static PassMetrics run_pass(int consumer_cpu, int producer_cpu,
                            uint64_t intra_gap_ns, uint64_t idle_gap_ns,
                            const std::vector<Msg> &ring,
                            uint64_t burst_len = 1) {
  Queue q(PipeCfg::QUEUE_CAPACITY);
  ParkWord pw;
  const uint64_t total_msgs = PipeCfg::WARM_MSGS + PipeCfg::SAMPLE_MSGS;

  PassMetrics metrics;
  metrics.latency_cycles.reserve(PipeCfg::SAMPLE_MSGS);
  metrics.inter_arrival_cycles.reserve(PipeCfg::SAMPLE_MSGS);
  metrics.cold_latency_cycles.reserve(PipeCfg::SAMPLE_MSGS);
  metrics.warm_latency_cycles.reserve(PipeCfg::SAMPLE_MSGS);

  std::atomic<bool> consumer_ready{false};

  std::thread consumer([&] {
    if (!pin(consumer_cpu)) {
      fprintf(stderr, "  ! pin cpu%d failed (consumer)\n", consumer_cpu);
      exit(1);
    }
    consumer_ready.store(true, std::memory_order_release);
    uint64_t expected_seq = 0;
    for (uint64_t n = 0; n < total_msgs; n++) {
      // Mark the start of the sampled (post-warm-up) window on the consumer
      // side itself, rather than reusing the caller's wall-clock straddling
      // thread spin-up + WARM_MSGS — see parked_fraction's denominator
      // comment on PassMetrics below for why that distinction matters.
      if (n == PipeCfg::WARM_MSGS) {
        metrics.sampled_wall_start_tsc = rdtsc_now();
        // See PassMetrics's wake_syscalls_at_sample_start comment: taken at
        // the exact same boundary as sampled_wall_start_tsc, so the caller
        // can diff its post-pass reading against these instead of a
        // pre-run_pass (whole-pass) snapshot.
        metrics.wake_syscalls_at_sample_start =
            g_futex_wake_syscalls.load(std::memory_order_relaxed);
        metrics.backstop_fires_at_sample_start =
            g_futex_backstop_fires.load(std::memory_order_relaxed);
        metrics.eagain_at_sample_start =
            g_futex_eagain.load(std::memory_order_relaxed);
      }

      bool had_to_wait = false;
      bool parked = false; // WP-A: Blocking-only, see wait_for_message/cold_tag
      uint64_t wait_t0 = 0;
      if constexpr (P == WaitPolicy::Blocking)
        wait_t0 = rdtsc_now();
      Msg *m = wait_for_message<P>(q, pw, had_to_wait, parked);
      uint64_t wait_t1 = 0;
      if constexpr (P == WaitPolicy::Blocking)
        wait_t1 = rdtsc_now();

      check_seq(expected_seq, m->seq);
      expected_seq++;

      uint64_t sink = process_msg(*m);
      uint64_t t_done = rdtsc_now();
      uint64_t pub_tsc = m->pub_tsc;

      if (n >= PipeCfg::WARM_MSGS) {
        metrics.sampled_count++;
        uint32_t sample_latency = uint32_t(t_done - pub_tsc);
        metrics.latency_cycles.push_back(sample_latency);
        if (had_to_wait)
          metrics.waited_count++;
        if constexpr (P == WaitPolicy::Blocking) {
          // Only count the time actually spent waiting when the queue was
          // empty; this slightly over-counts (it includes the loop's own
          // front()-recheck work, not purely the syscall), but is a cheap,
          // honest upper bound without instrumenting wait_fn itself.
          if (had_to_wait)
            metrics.parked_cycles += (wait_t1 - wait_t0);
        }

        // WP-A cold/warm tagging — same latency value (t_done - pub_tsc), no
        // new timestamp: pub_tsc is stamped after pace_until returns, so any
        // idle gap before publish is never inside the sample (see
        // pace_until's comment). cold_tag is deliberately asymmetric between
        // spin policies (had_to_wait only) and Blocking (parked, i.e. an
        // actual wait_fn/syscall invocation) — see its comment.
        bool is_blocking = (P == WaitPolicy::Blocking);
        bool cold = cold_tag(had_to_wait, parked, is_blocking);
        bool head = is_burst_head(m->seq, burst_len);
        if (cold) {
          metrics.cold_latency_cycles.push_back(sample_latency);
          if (head)
            metrics.cold_head++;
          else
            metrics.cold_nonhead++;
        } else {
          metrics.warm_latency_cycles.push_back(sample_latency);
          if (head)
            metrics.warm_head++;
          else
            metrics.warm_nonhead++;
        }

        g_process_sink.fetch_add(sink, std::memory_order_relaxed);
      }
      q.pop();

      if (n == total_msgs - 1)
        metrics.sampled_wall_end_tsc = t_done;
    }
  });

  if (!pin(producer_cpu)) {
    fprintf(stderr, "  ! pin cpu%d failed (producer)\n", producer_cpu);
    exit(1);
  }
  while (!consumer_ready.load(std::memory_order_acquire))
    relax<true>();

  // Even sweep constructs {1, gap_cycles, gap_cycles}: with burst_len=1,
  // sched_deadline's pos is always 0 and burst_idx==n, so this reduces
  // exactly to the pre-WP-B `t_start + n*gap_cycles` — the equivalence
  // anchor asserted in run_self_tests. WP-C's bursty sweep passes a spec
  // with intra_gap_cycles != idle_gap_cycles instead.
  const ArrivalSpec spec{burst_len, ns_to_cycles(intra_gap_ns),
                         ns_to_cycles(idle_gap_ns)};

  uint64_t t_start = rdtsc_now();
  uint64_t prev_pub_tsc = 0;
  for (uint64_t n = 0; n < total_msgs; n++) {
    uint64_t deadline = sched_deadline(t_start, n, spec);
    bool late = pace_until(deadline); // true iff we arrived past the deadline

    Msg msg = ring[n % PipeCfg::SOURCE_RING_ELEMS];
    msg.seq = n;
    msg.pub_tsc = rdtsc_now(); // stamped ONCE, before the first try_emplace

    if (n >= PipeCfg::WARM_MSGS && late)
      metrics.late_publish_count++;

    while (!publish_and_maybe_wake(q, msg, pw)) {
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

  // The final (end-of-pass) futex wake/backstop/eagain counter reading is
  // NOT taken here: they're global counters shared across every pass, and
  // run_policy_sweep takes that reading itself right after this call
  // returns, diffing it against metrics.wake_syscalls_at_sample_start etc.
  // (the WARM->SAMPLE-boundary snapshot taken above, on the consumer
  // thread) rather than against a pre-run_pass snapshot — see PassMetrics's
  // comment and the README's MUST-FIX 1 note for why the window has to
  // match n_bursts's SAMPLE_MSGS-only denominator.
  return metrics;
}

// wakes_per_burst: pure helper for the Blocking amortization headline
// (n_bursts, wakes-per-burst — see print_pass). `total_msgs_in_window` MUST
// be the exact same message count `wakes` was diffed over; the caller
// controls that via the argument rather than this function reaching into
// PipeCfg itself, so a caller that (re-)introduces a whole-pass window
// (WARM_MSGS+SAMPLE_MSGS) instead of the sample-only window (SAMPLE_MSGS)
// gets a silently wrong ratio again — see the README's MUST-FIX 1 note for
// the exact bug this was pulled out of run_policy_sweep to make testable.
static double wakes_per_burst(uint64_t wakes, uint64_t total_msgs_in_window,
                              uint64_t burst_len) {
  uint64_t n_bursts = burst_len ? total_msgs_in_window / burst_len : 0;
  return n_bursts ? double(wakes) / double(n_bursts) : 0.0;
}

// `gap_ns` is the idle/steady-state gap (the even sweep's single gap, or
// WP-C's BURST_IDLE_GAP_NS between bursts). `burst_len` defaults to 1 (the
// even sweep — no per-burst line printed); WP-C's bursty sweep passes the
// real burst length to get the amortization headline (n-bursts,
// wakes-per-burst) and the intra_gap=0 late-publish explanatory note.
static void print_pass(const char *tier_label, WaitPolicy policy,
                       uint64_t gap_ns, const PassMetrics &m,
                       uint64_t wake_syscalls, uint64_t backstop_fires,
                       uint64_t eagain_count, uint64_t burst_len = 1) {
  std::vector<uint32_t> lat = m.latency_cycles;
  std::sort(lat.begin(), lat.end());
  std::vector<uint32_t> ia = m.inter_arrival_cycles;
  std::sort(ia.begin(), ia.end());
  std::vector<uint32_t> cold_lat = m.cold_latency_cycles;
  std::sort(cold_lat.begin(), cold_lat.end());
  std::vector<uint32_t> warm_lat = m.warm_latency_cycles;
  std::sort(warm_lat.begin(), warm_lat.end());

  double waited_fraction =
      m.sampled_count ? double(m.waited_count) / double(m.sampled_count) : 0.0;
  // Denominator is the SAMPLED window only (consumer-stamped, see
  // PassMetrics comment) — not the pass's full wall time, which would also
  // count thread pin/spin-up and WARM_MSGS and understate parked_fraction.
  uint64_t sampled_wall_cycles =
      m.sampled_wall_end_tsc - m.sampled_wall_start_tsc;
  double parked_fraction = sampled_wall_cycles ? double(m.parked_cycles) /
                                                     double(sampled_wall_cycles)
                                               : 0.0;

  if (burst_len > 1)
    printf("  %-14s %-9s burst_len=%-4llu idle-gap=%6lluns\n", tier_label,
           policy_name(policy), (unsigned long long)burst_len,
           (unsigned long long)gap_ns);
  else
    printf("  %-14s %-9s gap=%6lluns\n", tier_label, policy_name(policy),
           (unsigned long long)gap_ns);
  printf("    latency(ns)   min %8.1f  p50 %8.1f  p90 %8.1f  p99 %8.1f  "
         "p99.9 %9.1f  p99.99 %10.1f  max %10.1f\n",
         pct(lat, 0), pct(lat, 50), pct(lat, 90), pct(lat, 99), pct(lat, 99.9),
         pct(lat, 99.99), pct(lat, 100));
  if (!ia.empty())
    printf("    inter-arrival(ns) target %6llu  achieved p50 %8.1f  p99 "
           "%8.1f\n",
           (unsigned long long)gap_ns, pct(ia, 50), pct(ia, 99));
  printf("    waited-fraction %.4f  backpressure %llu  late-publish %llu  "
         "futex-wake-syscalls %llu  futex-backstop-fires %llu  "
         "parked-fraction %.4f\n",
         waited_fraction, (unsigned long long)m.backpressure_count,
         (unsigned long long)m.late_publish_count,
         (unsigned long long)wake_syscalls, (unsigned long long)backstop_fires,
         parked_fraction);
  // WP-A: cold/warm latency split (see cold_tag) plus the 4-way cross-tab
  // against burst-head/non-head (is_burst_head; always all-head in the even
  // sweep, since run_pass's burst_len defaults to 1 there) and the EAGAIN
  // count (a "cold but no genuine park+wake" sub-case of Blocking, split out
  // from a real backstop-timeout park — see real_futex_wait).
  printf("    cold n=%zu p50 %8.1f p99 %8.1f  |  warm n=%zu p50 %8.1f p99 "
         "%8.1f  |  eagain %llu  |  xtab c/h=%llu c/nh=%llu w/h=%llu "
         "w/nh=%llu\n",
         cold_lat.size(), pct(cold_lat, 50), pct(cold_lat, 99), warm_lat.size(),
         pct(warm_lat, 50), pct(warm_lat, 99), (unsigned long long)eagain_count,
         (unsigned long long)m.cold_head, (unsigned long long)m.cold_nonhead,
         (unsigned long long)m.warm_head, (unsigned long long)m.warm_nonhead);
  if (gap_ns == 0)
    printf("    note: at gap=0 the queue is saturated, so latency here is "
           "sojourn time through queue depth (Little's law), not wake\n"
           "          latency — see the README's \"SPSC pipeline\" section, "
           "\"The crossover: latency vs. core utilization\", for why.\n");
  if (burst_len > 1) {
    // WP-C: the amortization headline — how many bursts this pass covered,
    // and how many FUTEX_WAKE syscalls it paid PER BURST (as opposed to per
    // message). If Blocking's conditional-wake fast path is amortizing
    // correctly, this should approach 1 as burst_len grows (one wake per
    // burst, on the burst's first message, rather than one per message).
    // wake_syscalls here is the SAMPLE-window delta (run_policy_sweep diffs
    // against wake_syscalls_at_sample_start), so SAMPLE_MSGS is the matching
    // window — same-window discipline the helper's comment insists on.
    uint64_t n_bursts = PipeCfg::SAMPLE_MSGS / burst_len;
    // wakes-per-burst is only meaningful for Blocking (the only policy that
    // issues FUTEX_WAKE); for spin policies it is trivially 0 and would just
    // be noise, so print n/a there.
    if (policy == WaitPolicy::Blocking)
      printf("    n-bursts %llu  wakes-per-burst %.4f\n",
             (unsigned long long)n_bursts,
             wakes_per_burst(wake_syscalls, PipeCfg::SAMPLE_MSGS, burst_len));
    else
      printf("    n-bursts %llu  wakes-per-burst n/a (spin policy)\n",
             (unsigned long long)n_bursts);
    // Explanatory note for the intra_gap=0 late-publish reading — same
    // "saturated, not a bug" logic as the gap=0 note above, just applied
    // per-burst instead of per-pass: with BURST_INTRA_GAP_NS=0, every
    // non-head message in a burst shares its burst-head's deadline (see
    // sched_deadline: pos*intra_gap_cycles == 0 for every pos), so
    // pace_until finds it already "late" by construction as soon as the
    // head has published. Expect late-publish to land near
    // SAMPLE_MSGS*(burst_len-1)/burst_len — the non-head share of the
    // sample — not near 0.
    printf("    note: intra-burst gap is 0, so every non-head message's "
           "deadline equals its burst's start time and reads \"late\" by\n"
           "          construction (expect late-publish "
           "~= SAMPLE_MSGS*(burst_len-1)/burst_len) — same logic as the "
           "gap=0 note, applied per burst.\n");
  }
}

// ===========================================================================
// WP2 test (1): pure-logic tests, no threads/no real futex/no hardware.
// ===========================================================================
namespace test_futex_logic {

struct WakeCall {
  int count = 0;
  uint32_t observed_parked_value_at_call = 0xffffffffu;
};
inline WakeCall g_wake_call;

static void stub_wake_recording(std::atomic<uint32_t> *addr, int n) {
  g_wake_call.count++;
  g_wake_call.observed_parked_value_at_call =
      addr->load(std::memory_order_relaxed);
  (void)n;
}
static void stub_wake_should_not_be_called(std::atomic<uint32_t> *, int) {
  fprintf(stderr, "FAIL: wake_fn called when it should not have been\n");
  exit(1);
}

struct WaitCall {
  int count = 0;
};
inline WaitCall g_wait_call;
static void stub_wait_recording(std::atomic<uint32_t> *, uint32_t,
                                const struct timespec *) {
  g_wait_call.count++;
}
static void stub_wait_should_not_be_called(std::atomic<uint32_t> *, uint32_t,
                                           const struct timespec *) {
  fprintf(stderr, "FAIL: wait_fn called when it should not have been\n");
  exit(1);
}

static void run() {
  // maybe_wake: parked==0 -> wake_fn must not be called.
  {
    ParkWord pw;
    pw.v.store(0, std::memory_order_relaxed);
    bool woke = maybe_wake(pw, stub_wake_should_not_be_called);
    if (woke) {
      fprintf(stderr, "FAIL maybe_wake(parked=0) reported woke=true\n");
      exit(1);
    }
  }
  // maybe_wake: parked==1 -> wake_fn called exactly once, AFTER the clear
  // (the stub observes parked==0 at call time).
  {
    ParkWord pw;
    pw.v.store(1, std::memory_order_relaxed);
    g_wake_call = WakeCall{};
    bool woke = maybe_wake(pw, stub_wake_recording);
    if (!woke || g_wake_call.count != 1) {
      fprintf(stderr,
              "FAIL maybe_wake(parked=1): woke=%d count=%d (want 1,1)\n", woke,
              g_wake_call.count);
      exit(1);
    }
    if (g_wake_call.observed_parked_value_at_call != 0) {
      fprintf(stderr,
              "FAIL maybe_wake: parked was not cleared before wake_fn was "
              "called (observed %u, want 0)\n",
              g_wake_call.observed_parked_value_at_call);
      exit(1);
    }
    if (pw.v.load(std::memory_order_relaxed) != 0) {
      fprintf(stderr, "FAIL maybe_wake: parked left at %u after wake, want 0\n",
              pw.v.load(std::memory_order_relaxed));
      exit(1);
    }
  }

  // wait_blocking_impl: message already visible on the OUTER check ->
  // wait_fn must never be called at all. WP-A: parked_out must stay false —
  // no syscall was ever entered for this message (cold_tag's WARM case).
  {
    ParkWord pw;
    int call = 0;
    auto front = [&]() -> int * {
      call++;
      static int dummy = 42;
      return &dummy; // visible immediately
    };
    struct timespec ts {};
    bool parked_out = false;
    int *m = wait_blocking_impl(front, pw, stub_wait_should_not_be_called, ts,
                                &parked_out);
    if (m == nullptr || *m != 42 || call != 1) {
      fprintf(stderr, "FAIL wait_blocking_impl immediate-visible path\n");
      exit(1);
    }
    if (parked_out) {
      fprintf(stderr, "FAIL wait_blocking_impl immediate-visible: "
                      "parked_out=true, want false (no syscall entered)\n");
      exit(1);
    }
  }

  // wait_blocking_impl: empty on the outer check, but visible by the
  // RE-CHECK (the announce/park race window) -> wait_fn must be skipped,
  // and parked must be left at 0 (the announce is undone). WP-A: parked_out
  // must stay false — the re-check closed the race before any syscall was
  // paid, so this message is WARM under cold_tag despite had_to_wait==true.
  {
    ParkWord pw;
    int call = 0;
    static int dummy = 7;
    auto front = [&]() -> int * {
      call++;
      // call 1 = outer check (empty), call 2 = re-check (now visible)
      return call >= 2 ? &dummy : nullptr;
    };
    struct timespec ts {};
    bool parked_out = false;
    int *m = wait_blocking_impl(front, pw, stub_wait_should_not_be_called, ts,
                                &parked_out);
    if (m == nullptr || *m != 7 || call != 2) {
      fprintf(stderr,
              "FAIL wait_blocking_impl re-check-closes-race path: "
              "call=%d\n",
              call);
      exit(1);
    }
    if (pw.v.load(std::memory_order_relaxed) != 0) {
      fprintf(stderr, "FAIL wait_blocking_impl: parked left set after "
                      "re-check closed the race\n");
      exit(1);
    }
    if (parked_out) {
      fprintf(stderr, "FAIL wait_blocking_impl re-check-closes-race: "
                      "parked_out=true, want false (no syscall entered)\n");
      exit(1);
    }
  }

  // wait_blocking_impl: empty on both outer check and re-check -> wait_fn
  // called exactly once, then the outer while loop re-polls and finds the
  // message. WP-A: parked_out must be true — a real syscall entry happened
  // (cold_tag's cold case for Blocking).
  {
    ParkWord pw;
    int call = 0;
    static int dummy = 99;
    g_wait_call = WaitCall{};
    auto front = [&]() -> int * {
      call++;
      // call 1 = outer (empty), call 2 = re-check (still empty, so we park),
      // call 3 = outer loop's next front() after the (stubbed) wait returns.
      return call >= 3 ? &dummy : nullptr;
    };
    struct timespec ts {};
    bool parked_out = false;
    int *m =
        wait_blocking_impl(front, pw, stub_wait_recording, ts, &parked_out);
    if (m == nullptr || *m != 99 || g_wait_call.count != 1) {
      fprintf(stderr,
              "FAIL wait_blocking_impl genuine-park path: call=%d "
              "wait_count=%d\n",
              call, g_wait_call.count);
      exit(1);
    }
    if (!parked_out) {
      fprintf(stderr, "FAIL wait_blocking_impl genuine-park: parked_out=false, "
                      "want true (a syscall was entered)\n");
      exit(1);
    }
  }

  // wait_blocking_impl: front() stays null across TWO full rounds of the
  // outer while loop (outer-empty, re-check-empty, park+wake-stubbed-return,
  // outer-empty AGAIN, re-check-empty AGAIN, park a second time, then
  // finally visible) -> wait_fn called exactly twice, parked_out still true.
  // This is the "mid-burst re-park" shape WP-C's cross-tab relies on (cold ^
  // nonhead messages inside a burst): parked_out only needs to answer "was
  // wait_fn ever invoked for THIS message", not "how many times".
  {
    ParkWord pw;
    int call = 0;
    static int dummy = 123;
    g_wait_call = WaitCall{};
    auto front = [&]() -> int * {
      call++;
      // Round 1: call 1 = outer (empty), call 2 = re-check (empty) -> park.
      // Round 2: call 3 = outer (empty), call 4 = re-check (empty) -> park.
      // Round 3: call 5 = outer loop's next front(), now visible.
      return call >= 5 ? &dummy : nullptr;
    };
    struct timespec ts {};
    bool parked_out = false;
    int *m =
        wait_blocking_impl(front, pw, stub_wait_recording, ts, &parked_out);
    if (m == nullptr || *m != 123 || g_wait_call.count != 2) {
      fprintf(stderr,
              "FAIL wait_blocking_impl two-round-park path: call=%d "
              "wait_count=%d (want 5,2)\n",
              call, g_wait_call.count);
      exit(1);
    }
    if (!parked_out) {
      fprintf(stderr, "FAIL wait_blocking_impl two-round-park: "
                      "parked_out=false, want true\n");
      exit(1);
    }
  }

  printf("  futex logic tests: OK\n");
}

} // namespace test_futex_logic

// ===========================================================================
// WP3/WP4 pure-logic self-tests.
// ===========================================================================
static void run_self_tests() {
  test_futex_logic::run();

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

  // process_msg: deterministic given the same Msg, and depends on payload
  // (changing a payload word changes the result) — guards against the
  // compiler or a future edit accidentally eliding the payload loop.
  {
    Msg m{};
    m.seq = 5;
    for (int k = 0; k < 6; k++)
      m.payload[k] = uint64_t(k) * 12345;
    uint64_t r1 = process_msg(m);
    uint64_t r2 = process_msg(m);
    if (r1 != r2) {
      fprintf(stderr, "FAIL process_msg: not deterministic\n");
      exit(1);
    }
    m.payload[0] += 1;
    uint64_t r3 = process_msg(m);
    if (r3 == r1) {
      fprintf(stderr, "FAIL process_msg: result independent of payload\n");
      exit(1);
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
    // any earlier message actually published.
    uint64_t t_start = 1'000'000;
    uint64_t gap_cycles = ns_to_cycles(1000);
    for (uint64_t n = 0; n < 5; n++) {
      uint64_t deadline = t_start + n * gap_cycles;
      uint64_t want = t_start + n * 1000;
      if (deadline != want) {
        fprintf(stderr,
                "FAIL deadline schedule at n=%llu: got %llu want %llu\n",
                (unsigned long long)n, (unsigned long long)deadline,
                (unsigned long long)want);
        exit(1);
      }
    }
    // sched_deadline (WP-B): exactness for a real burst schedule,
    // burst_len=4, intra_gap=0, idle_gap=20000. period = 3*0 + 20000 =
    // 20000. Messages 0-3 are burst 0 (all deadline t_start, since
    // intra_gap=0 means no separation within a burst); messages 4-7 are
    // burst 1, all at t_start+20000.
    {
      ArrivalSpec spec{/*burst_len=*/4, /*intra_gap_cycles=*/0,
                       /*idle_gap_cycles=*/20000};
      uint64_t want[8] = {t_start,         t_start,         t_start,
                          t_start,         t_start + 20000, t_start + 20000,
                          t_start + 20000, t_start + 20000};
      for (uint64_t n = 0; n < 8; n++) {
        uint64_t got = sched_deadline(t_start, n, spec);
        if (got != want[n]) {
          fprintf(stderr,
                  "FAIL sched_deadline burst_len=4,intra=0 at n=%llu: got "
                  "%llu want %llu\n",
                  (unsigned long long)n, (unsigned long long)got,
                  (unsigned long long)want[n]);
          exit(1);
        }
      }
    }

    // sched_deadline: nonzero intra-gap, burst_len=3, intra=250, idle=5000.
    // period = 2*250 + 5000 = 5500. Burst 0: n=0,1,2 at t_start, t_start+250,
    // t_start+500. Burst 1 starts at t_start+5500: n=3,4,5 at t_start+5500,
    // t_start+5750, t_start+6000.
    {
      ArrivalSpec spec{/*burst_len=*/3, /*intra_gap_cycles=*/250,
                       /*idle_gap_cycles=*/5000};
      uint64_t want[6] = {t_start,        t_start + 250,  t_start + 500,
                          t_start + 5500, t_start + 5750, t_start + 6000};
      for (uint64_t n = 0; n < 6; n++) {
        uint64_t got = sched_deadline(t_start, n, spec);
        if (got != want[n]) {
          fprintf(stderr,
                  "FAIL sched_deadline burst_len=3,intra=250 at n=%llu: got "
                  "%llu want %llu\n",
                  (unsigned long long)n, (unsigned long long)got,
                  (unsigned long long)want[n]);
          exit(1);
        }
      }
    }

    // sched_deadline EQUIVALENCE ANCHOR: burst_len=1 must reproduce
    // t_start + n*G bit-for-bit for every gap in the real GAP_NS sweep — the
    // guarantee that refactoring the even sweep's producer loop onto
    // sched_deadline changed nothing observable.
    for (uint64_t gap_ns : PipeCfg::GAP_NS) {
      uint64_t g_cycles = ns_to_cycles(gap_ns);
      ArrivalSpec spec{/*burst_len=*/1, /*intra_gap_cycles=*/g_cycles,
                       /*idle_gap_cycles=*/g_cycles};
      for (uint64_t n = 0; n < 5; n++) {
        uint64_t got = sched_deadline(t_start, n, spec);
        uint64_t want = t_start + n * g_cycles;
        if (got != want) {
          fprintf(stderr,
                  "FAIL sched_deadline equivalence anchor: gap_ns=%llu "
                  "n=%llu got %llu want %llu\n",
                  (unsigned long long)gap_ns, (unsigned long long)n,
                  (unsigned long long)got, (unsigned long long)want);
          exit(1);
        }
      }
    }

    // sched_deadline: monotonic non-decreasing across a burst boundary
    // (n=burst_len-1 -> n=burst_len), the point where period jumps in.
    {
      ArrivalSpec spec{/*burst_len=*/4, /*intra_gap_cycles=*/100,
                       /*idle_gap_cycles=*/1000};
      uint64_t d_last_in_burst = sched_deadline(t_start, 3, spec);
      uint64_t d_first_next_burst = sched_deadline(t_start, 4, spec);
      if (d_first_next_burst < d_last_in_burst) {
        fprintf(stderr,
                "FAIL sched_deadline: deadline went backwards across burst "
                "boundary (n=3 -> %llu, n=4 -> %llu)\n",
                (unsigned long long)d_last_in_burst,
                (unsigned long long)d_first_next_burst);
        exit(1);
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

  // should_skip: the one hard placement rule.
  if (!should_skip(WaitPolicy::BareSpin, /*is_sibling_tier=*/true)) {
    fprintf(stderr,
            "FAIL should_skip: BareSpin must be skipped on sibling tier\n");
    exit(1);
  }
  if (should_skip(WaitPolicy::BareSpin, /*is_sibling_tier=*/false) ||
      should_skip(WaitPolicy::Pause, true) ||
      should_skip(WaitPolicy::Pause, false) ||
      should_skip(WaitPolicy::Blocking, true) ||
      should_skip(WaitPolicy::Blocking, false)) {
    fprintf(stderr, "FAIL should_skip: an allowed combination was skipped\n");
    exit(1);
  }

  // cold_tag (WP-A): the deliberate spin-vs-Blocking asymmetry.
  {
    // Spin policies (is_blocking=false): had_to_wait is the only signal —
    // parked is irrelevant and ignored either way.
    if (cold_tag(/*had_to_wait=*/true, /*parked=*/false,
                 /*is_blocking=*/false) != true ||
        cold_tag(true, true, false) != true) {
      fprintf(stderr, "FAIL cold_tag: spin had_to_wait=true must be cold "
                      "regardless of parked\n");
      exit(1);
    }
    if (cold_tag(/*had_to_wait=*/false, /*parked=*/false,
                 /*is_blocking=*/false) != false ||
        cold_tag(false, true, false) != false) {
      fprintf(stderr, "FAIL cold_tag: spin had_to_wait=false must be warm "
                      "regardless of parked\n");
      exit(1);
    }
    // Blocking (is_blocking=true): parked is the only signal. had_to_wait
    // alone (re-check closed the race, no syscall) must classify WARM.
    if (cold_tag(/*had_to_wait=*/true, /*parked=*/false,
                 /*is_blocking=*/true) != false) {
      fprintf(stderr, "FAIL cold_tag: blocking had_to_wait=true, parked=false "
                      "(re-check closed the race) must be WARM, not cold\n");
      exit(1);
    }
    if (cold_tag(/*had_to_wait=*/true, /*parked=*/true, /*is_blocking=*/true) !=
        true) {
      fprintf(stderr, "FAIL cold_tag: blocking parked=true must be cold\n");
      exit(1);
    }
    // had_to_wait=false, parked=true is not a reachable combination in
    // practice (wait_for_message<Blocking> only sets parked inside the wait
    // loop, which only runs when the initial front() was already null) but
    // cold_tag is a pure function of its inputs and must still honor
    // "parked wins" for is_blocking=true.
    if (cold_tag(/*had_to_wait=*/false, /*parked=*/true,
                 /*is_blocking=*/true) != true) {
      fprintf(stderr, "FAIL cold_tag: blocking parked=true must be cold even "
                      "if had_to_wait=false\n");
      exit(1);
    }
  }

  // is_burst_head (WP-B/WP-A): pure predicate on seq % burst_len.
  {
    // burst_len=1 -> every message is a head (even-sweep equivalence case).
    for (uint64_t seq = 0; seq < 5; seq++)
      if (!is_burst_head(seq, /*burst_len=*/1)) {
        fprintf(stderr,
                "FAIL is_burst_head: burst_len=1 must make seq=%llu "
                "a head\n",
                (unsigned long long)seq);
        exit(1);
      }
    // burst_len=4 -> heads at 0,4,8; non-heads at 1,2,3,5,6,7.
    bool want_head[8] = {true, false, false, false, true, false, false, false};
    for (uint64_t seq = 0; seq < 8; seq++)
      if (is_burst_head(seq, /*burst_len=*/4) != want_head[seq]) {
        fprintf(stderr,
                "FAIL is_burst_head: burst_len=4 seq=%llu got %d want %d\n",
                (unsigned long long)seq, is_burst_head(seq, 4), want_head[seq]);
        exit(1);
      }
  }

  // wakes_per_burst (MUST-FIX 1): the ratio must be computed over the SAME
  // message window the wake count was diffed over. These cases pin both the
  // arithmetic and, critically, that the intended denominator is the
  // sample-only window (SAMPLE_MSGS), not the whole pass — the exact bug this
  // helper was extracted to prevent (whole-pass window inflates the ratio by
  // (WARM+SAMPLE)/SAMPLE).
  {
    // Perfect amortization: one wake per burst. 100000 msgs / burst_len 4 =
    // 25000 bursts; 25000 wakes -> exactly 1.0.
    if (wakes_per_burst(25000, 100000, 4) != 1.0) {
      fprintf(stderr, "FAIL wakes_per_burst: perfect-amortization case\n");
      exit(1);
    }
    // Same wakes but the WRONG (whole-pass) window must NOT read 1.0 — it
    // reads 25000 / (120000/4) = 25000/30000 ~= 0.833, proving the ratio is
    // sensitive to the window mismatch this bug was about.
    if (wakes_per_burst(25000, 120000, 4) >= 1.0) {
      fprintf(stderr, "FAIL wakes_per_burst: whole-pass window must differ\n");
      exit(1);
    }
    // Degenerate guards: burst_len 0 and a window smaller than a burst.
    if (wakes_per_burst(5, 0, 0) != 0.0 || wakes_per_burst(5, 2, 4) != 0.0) {
      fprintf(stderr, "FAIL wakes_per_burst: degenerate guards\n");
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
// WP2 test (2): --blocking-probe. Real threads, real futex syscalls. No
// rdtsc/topology dependency (deliberately — this must run on any CI runner,
// pinned or not, with no notion of "cpu0's sibling").
// ===========================================================================
// Tuning knobs for --blocking-probe, at file scope (a local class cannot
// have static data members in standard C++, only as a non-conforming GCC
// extension under -fpermissive — which the house -Wall -Wextra build does
// not use).
struct ProbeCfg {
  // Round-trip smoke count: enough iterations to make a systematic
  // ordering bug near-certain to show up, small enough to run instantly.
  static constexpr int ROUNDTRIP_ROUNDS = 200;
  // Missed-wake stress round count: protocol smoke coverage for the park/
  // wake path under randomized timing (park/wake round trip, EAGAIN,
  // spurious-wake, no-deadlock, seq-integrity under stress) — NOT a
  // regression test for the seq_cst fence pair in publish_and_maybe_wake.
  // The actual race window the fence closes is a few nanoseconds wide;
  // this probe's timing granularity (nanosleep-based jitter, see
  // MAX_JITTER_US below, and nanosleep is itself a barrier) cannot reliably
  // straddle a window that narrow, so a red run here proves nothing about
  // the fence either way. The fence's correctness rests on the analytic
  // Dekker argument in publish_and_maybe_wake's comment, not on this test.
  // 10k rounds x randomized microdelays still gives good coverage of the
  // ordinary protocol paths (announce/re-check, EAGAIN, genuine park).
  static constexpr int STRESS_ROUNDS = 10000;
  // Randomized delay window (microseconds) the producer sleeps before
  // publishing each round, deliberately straddling the few-hundred-ns
  // scale of the announce/re-check/park window so some fraction of rounds
  // land the publish WHILE the consumer is between its announce and its
  // park call — exactly the race the fence closes.
  static constexpr int MAX_JITTER_US = 200;
  // Fixed seed: the stress test's timing is randomized but reproducible
  // across runs/CI, so a failure is debuggable rather than a one-off.
  static constexpr uint32_t SEED = 0x5eed5eedu;
  // Backstop timeout used by this probe's futex_wait calls: generous
  // relative to MAX_JITTER_US so it never legitimately fires, making any
  // nonzero backstop-fire count during the stress test a genuine signal.
  static constexpr int BACKSTOP_TIMEOUT_MS = 200;
};

static int run_blocking_probe() {
  // (a) Basic round-trip: producer publishes N small ints through a real
  // SPSCQueue<int>, consumer waits via the REAL wait_blocking_impl + real
  // futex, both using the actual production maybe_wake/publish path (via a
  // thin int-specialized publish helper below) rather than a reimplemented
  // stand-in — this exercises the exact functions the pipeline uses.
  rigtorp::SPSCQueue<int> q(4);
  ParkWord pw;

  auto consume_one = [&](int expected) -> bool {
    struct timespec ts {};
    ts.tv_sec = ProbeCfg::BACKSTOP_TIMEOUT_MS / 1000;
    ts.tv_nsec = (ProbeCfg::BACKSTOP_TIMEOUT_MS % 1000) * 1000000L;
    int *m =
        wait_blocking_impl([&] { return q.front(); }, pw, real_futex_wait, ts);
    bool ok = (*m == expected);
    q.pop();
    return ok;
  };
  auto publish_one = [&](int v) {
    while (!publish_and_maybe_wake(q, v, pw)) {
    }
  };

  printf("--blocking-probe: (a) round-trip smoke (%d rounds)\n",
         ProbeCfg::ROUNDTRIP_ROUNDS);
  {
    uint64_t backstop_before =
        g_futex_backstop_fires.load(std::memory_order_relaxed);
    bool all_ok = true;
    std::thread consumer([&] {
      for (int i = 0; i < ProbeCfg::ROUNDTRIP_ROUNDS; i++)
        if (!consume_one(i))
          all_ok = false;
    });
    for (int i = 0; i < ProbeCfg::ROUNDTRIP_ROUNDS; i++)
      publish_one(i);
    consumer.join();
    uint64_t backstop_after =
        g_futex_backstop_fires.load(std::memory_order_relaxed);
    if (!all_ok) {
      fprintf(stderr, "FAIL round-trip smoke: value mismatch\n");
      return 1;
    }
    printf("  OK: %d/%d round trips correct, backstop fires %llu\n",
           ProbeCfg::ROUNDTRIP_ROUNDS, ProbeCfg::ROUNDTRIP_ROUNDS,
           (unsigned long long)(backstop_after - backstop_before));
  }

  printf("--blocking-probe: (b) MISSED-WAKE STRESS (%d rounds, jitter up to "
         "%dus)\n",
         ProbeCfg::STRESS_ROUNDS, ProbeCfg::MAX_JITTER_US);
  {
    uint64_t backstop_before =
        g_futex_backstop_fires.load(std::memory_order_relaxed);
    std::atomic<int> consumed{0};
    bool all_ok = true;
    std::thread consumer([&] {
      for (int i = 0; i < ProbeCfg::STRESS_ROUNDS; i++) {
        if (!consume_one(i))
          all_ok = false;
        consumed.fetch_add(1, std::memory_order_relaxed);
      }
    });
    std::mt19937 rng(ProbeCfg::SEED);
    std::uniform_int_distribution<int> jitter_us(0, ProbeCfg::MAX_JITTER_US);
    for (int i = 0; i < ProbeCfg::STRESS_ROUNDS; i++) {
      int us = jitter_us(rng);
      if (us > 0) {
        struct timespec sleep_ts {};
        sleep_ts.tv_sec = 0;
        sleep_ts.tv_nsec = us * 1000L;
        nanosleep(&sleep_ts, nullptr);
      }
      publish_one(i);
    }
    consumer.join();
    uint64_t backstop_after =
        g_futex_backstop_fires.load(std::memory_order_relaxed);
    uint64_t fires = backstop_after - backstop_before;
    if (!all_ok || consumed.load() != ProbeCfg::STRESS_ROUNDS) {
      fprintf(stderr, "FAIL missed-wake stress: consumed=%d want=%d ok=%d\n",
              consumed.load(), ProbeCfg::STRESS_ROUNDS, all_ok);
      return 1;
    }
    printf("  OK: %d/%d rounds consumed, backstop fires %llu (want 0)\n",
           ProbeCfg::STRESS_ROUNDS, ProbeCfg::STRESS_ROUNDS,
           (unsigned long long)fires);
    if (fires != 0) {
      fprintf(stderr,
              "FAIL missed-wake stress: backstop fired %llu times — this is "
              "the deadlock-insurance timeout catching what would otherwise "
              "be a missed wake. Expected exactly 0.\n",
              (unsigned long long)fires);
      return 1;
    }
  }

  printf("--blocking-probe: (c) EAGAIN path (no-deadlock)\n");
  {
    // Manually put the word in the "not actually parked" state (0) and call
    // real_futex_wait with expected=1: the kernel's value check sees 0 !=
    // 1 and returns EAGAIN immediately without ever sleeping. Bound the
    // wall-clock time to prove it — if this ever blocked, it would hang for
    // the full backstop timeout instead of returning near-instantly.
    ParkWord solo;
    solo.v.store(0, std::memory_order_relaxed);
    struct timespec ts {};
    ts.tv_sec = 5; // deliberately long — if EAGAIN doesn't fire, this test
                   // would hang for 5s, which is itself the failure signal
    ts.tv_nsec = 0;
    auto t0 = std::chrono::steady_clock::now();
    real_futex_wait(&solo.v, 1, &ts);
    auto t1 = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    // Generous bound: a genuine EAGAIN return is a syscall round trip, so
    // this should complete in well under a millisecond; 500ms leaves ample
    // headroom for a loaded CI runner while still being far short of the 5s
    // timeout, so a hang is unambiguous.
    if (ms > 500.0) {
      fprintf(stderr,
              "FAIL EAGAIN path: real_futex_wait(expected=1) took %.1fms — "
              "looks like it actually slept instead of returning EAGAIN\n",
              ms);
      return 1;
    }
    printf("  OK: EAGAIN returned in %.3fms (did not sleep)\n", ms);
  }

  printf("--blocking-probe: all checks passed\n");
  return 0;
}

// ===========================================================================
// WP5/WP-C: the full sweep — 3 tiers x 3 policies (minus skips) x N cells.
// A SweepCell is one (burst_len, intra_gap_ns, idle_gap_ns) row; the default
// even sweep and the --bursty sweep are just two different cell lists fed
// through the same counter-diff/run_pass/print_pass scaffolding, rather than
// duplicated loops — see even_sweep_cells/bursty_sweep_cells below.
// ===========================================================================
struct SweepCell {
  uint64_t burst_len;
  uint64_t intra_gap_ns;
  uint64_t idle_gap_ns; // also the value printed as "gap="/"idle-gap="
};

// The historical even sweep: one cell per PipeCfg::GAP_NS entry, burst_len=1
// (every message is its own "burst" of length one — see sched_deadline's
// equivalence case). intra_gap_ns is set equal to idle_gap_ns so
// sched_deadline's period math reduces to the plain `t_start + n*gap`
// schedule regardless of which of the two fields it happens to read.
static std::vector<SweepCell> even_sweep_cells() {
  std::vector<SweepCell> cells;
  cells.reserve(std::size(PipeCfg::GAP_NS));
  for (uint64_t gap_ns : PipeCfg::GAP_NS)
    cells.push_back({/*burst_len=*/1, /*intra_gap_ns=*/gap_ns,
                     /*idle_gap_ns=*/gap_ns});
  return cells;
}

// --bursty sweep: one cell per PipeCfg::BURST_LENS entry, at the fixed
// BURST_INTRA_GAP_NS/BURST_IDLE_GAP_NS from PipeCfg.
static std::vector<SweepCell> bursty_sweep_cells() {
  std::vector<SweepCell> cells;
  cells.reserve(std::size(PipeCfg::BURST_LENS));
  for (uint64_t burst_len : PipeCfg::BURST_LENS)
    cells.push_back(
        {burst_len, PipeCfg::BURST_INTRA_GAP_NS, PipeCfg::BURST_IDLE_GAP_NS});
  return cells;
}

static void run_policy_sweep(WaitPolicy policy, const char *tier_label,
                             int consumer_cpu, int producer_cpu,
                             bool is_sibling_tier, const std::vector<Msg> &ring,
                             const std::vector<SweepCell> &cells) {
  if (consumer_cpu < 0 || producer_cpu < 0) {
    printf("  %-14s %-9s (no such pair)\n", tier_label, policy_name(policy));
    return;
  }
  if (should_skip(policy, is_sibling_tier)) {
    printf("  %-14s %-9s skipped: bare spin on an SMT sibling would starve "
           "the shared core's ports the peer thread needs (see "
           "smt_pingpong.cpp)\n",
           tier_label, policy_name(policy));
    return;
  }
  for (const SweepCell &cell : cells) {
    PassMetrics m;
    switch (policy) {
    case WaitPolicy::BareSpin:
      m = run_pass<WaitPolicy::BareSpin>(consumer_cpu, producer_cpu,
                                         cell.intra_gap_ns, cell.idle_gap_ns,
                                         ring, cell.burst_len);
      break;
    case WaitPolicy::Pause:
      m = run_pass<WaitPolicy::Pause>(consumer_cpu, producer_cpu,
                                      cell.intra_gap_ns, cell.idle_gap_ns, ring,
                                      cell.burst_len);
      break;
    case WaitPolicy::Blocking:
      m = run_pass<WaitPolicy::Blocking>(consumer_cpu, producer_cpu,
                                         cell.intra_gap_ns, cell.idle_gap_ns,
                                         ring, cell.burst_len);
      break;
    }
    // Diffed against the sample-window snapshot run_pass took at n ==
    // WARM_MSGS (metrics.wake_syscalls_at_sample_start etc.), NOT a
    // before-this-call snapshot -- so every count below is scoped to the
    // same SAMPLE_MSGS-only window as n_bursts and the cold/warm counts
    // (see PassMetrics's comment and the README's MUST-FIX 1 note).
    uint64_t wake_after = g_futex_wake_syscalls.load(std::memory_order_relaxed);
    uint64_t backstop_after =
        g_futex_backstop_fires.load(std::memory_order_relaxed);
    uint64_t eagain_after = g_futex_eagain.load(std::memory_order_relaxed);

    print_pass(tier_label, policy, cell.idle_gap_ns, m,
               wake_after - m.wake_syscalls_at_sample_start,
               backstop_after - m.backstop_fires_at_sample_start,
               eagain_after - m.eagain_at_sample_start, cell.burst_len);
  }
}

int main(int argc, char **argv) {
  if (argc == 2 && std::strcmp(argv[1], "--test") == 0) {
    run_self_tests();
    return 0;
  }
  if (argc == 2 && std::strcmp(argv[1], "--blocking-probe") == 0) {
    return run_blocking_probe();
  }
  bool bursty = false;
  if (argc == 2 && std::strcmp(argv[1], "--bursty") == 0) {
    bursty = true;
  } else if (argc != 1) {
    fprintf(stderr, "usage: %s [--test | --blocking-probe | --bursty]\n",
            argv[0]);
    return 1;
  }

  check_invariant_tsc();
  calibrate();
  printf("TSC: %.4f GHz\n\n", g_tsc_ghz);

  Topology topo = discover_topology();
  printf("consumer anchored on cpu%d | sibling=%d same_l3=%d other_l3=%d\n\n",
         topo.base, topo.sibling, topo.same_l3, topo.other_l3);

  auto ring = build_source_ring();

  struct Tier {
    const char *label;
    int producer_cpu;
    bool is_sibling;
  };
  Tier tiers[] = {
      {"SMT sibling", topo.sibling, true},
      {"same L3/CCX", topo.same_l3, false},
      {"cross CCX", topo.other_l3, false},
  };
  WaitPolicy policies[] = {WaitPolicy::BareSpin, WaitPolicy::Pause,
                           WaitPolicy::Blocking};

  // WP-C: --bursty swaps the cell list (burst lengths at a fixed idle gap)
  // in place of the default even sweep (gaps at burst_len=1); everything
  // else — tiers, policies, should_skip, the counter-diff scaffolding —
  // is unchanged and shared between the two modes.
  const std::vector<SweepCell> cells =
      bursty ? bursty_sweep_cells() : even_sweep_cells();

  if (bursty) {
    printf("--bursty: burst_len in {");
    for (size_t i = 0; i < std::size(PipeCfg::BURST_LENS); i++)
      printf("%s%llu", i ? "," : "",
             (unsigned long long)PipeCfg::BURST_LENS[i]);
    printf("}, intra-gap=%lluns, idle-gap=%lluns\n\n",
           (unsigned long long)PipeCfg::BURST_INTRA_GAP_NS,
           (unsigned long long)PipeCfg::BURST_IDLE_GAP_NS);
  }

  for (const Tier &tier : tiers) {
    printf("=== tier: %s (cpu%d <-> cpu%d) ===\n", tier.label, topo.base,
           tier.producer_cpu);
    for (WaitPolicy policy : policies)
      run_policy_sweep(policy, tier.label, topo.base, tier.producer_cpu,
                       tier.is_sibling, ring, cells);
    printf("\n");
  }

  printf("note: no core isolation — absolute numbers are OS-jitter-sensitive;\n"
         "      compare rows within this run only. futex-wake-syscalls near 0\n"
         "      at gap=0 and near sample-count at the largest gap is the\n"
         "      conditional-wake fast path proving itself out.\n");
  return 0;
}
