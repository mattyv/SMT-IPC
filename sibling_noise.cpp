// ============================================================================
// sibling_noise.cpp — does a busy SMT sibling slow down a port-bound victim?
// ----------------------------------------------------------------------------
// REVISION NOTE (this file was rewritten from an earlier ping-pong-based
// design): a review proved the original experiment structurally insensitive.
// It ran the same-CCX ping-pong from smt_pingpong.cpp as the "victim" and put
// a noisy tenant on the victim initiator's SMT sibling. But that ping-pong's
// round trip is dominated by the CROSS-CORE coherence path (line migrating to
// the responder core and back) — only a single-digit-nanosecond sliver of
// each sample actually executes ON the contended core. A busy sibling can
// only ever perturb that sliver, so the predicted signal sat below the
// OS-jitter floor by construction. The prediction failed at every percentile
// in that design, and it was not a tuning problem — the workload just wasn't
// shaped to be sensitive to SMT port contention. See git history for the
// retired design/data if you want the old numbers.
//
// THE CORRECTED EXPERIMENT
// -------------------------
// Only two logical CPUs matter here: `cpu_hot` (the anchor) and its SMT
// sibling `cpu_sib`. There is no ping-pong, no responder, no second data core
// — 100% of the timed victim work executes ON the physical core shared with
// the tenant, because that is the only core whose contention we're trying to
// observe.
//
//   VICTIM (runs on cpu_hot): a THROUGHPUT-BOUND, HIGH-ILP block, not a
//   dependent chain. A dependent/latency-bound workload (pointer chase,
//   single accumulator) is exactly what SMT coexists with well — the two
//   hardware threads interleave into idle issue slots, so a busy sibling
//   barely affects it and the experiment would show no signal (this is
//   effectively what went wrong with the original ping-pong-based design,
//   for a different underlying reason). Instead the victim runs N_LANES
//   independent accumulator lanes over a small L1-resident buffer — many
//   independent operations per iteration with no cross-iteration dependency
//   bottleneck, so the lanes issue in parallel and press the core's issue
//   ports. When a busy sibling competes for those same ports, victim
//   throughput is expected to drop ~1.3-2x, and — unlike a latency-bound
//   design — that shows cleanly in the MEDIAN, not just the tail.
//
//   TENANT (runs on cpu_tenant — cpu_sib, cpu_hot's SMT sibling, by default;
//   or a same-CCX non-sibling core under --same-ccx, see below), three
//   states, each its own pass:
//     (a) idle — no tenant thread spawned at all.
//     (b) hot  — a genuinely port-bound independent-ops loop (multiple
//                independent chains, the same "many independent lanes"
//                shape as the victim) that competes hard for shared issue
//                ports. This replaces the old tenant's weak IPC~2 mul/add
//                chain, which wasn't port-hungry enough to matter.
//     (c) noop — a `_mm_pause` spin loop: present on the core, but PAUSE
//                yields the shared ports back every iteration.
//
// METHODOLOGY (fixes the n=1 / thermal-drift problems the review raised):
// passes are INTERLEAVED round-robin across R repeats — idle, noop, hot,
// idle, noop, hot, ... — rather than run as three long sequential blocks.
// This spreads thermal ramp, governor transitions, and background scheduler
// noise evenly across all three states instead of letting a drifting laptop
// bias whichever state happens to run last. Samples are pooled across all R
// repeats per state for the percentile report; the per-repeat medians are
// ALSO reported separately (min/median/max of the R medians) so the reader
// can see whether the gap between states exceeds the run-to-run spread —
// that is the honest significance check, not just eyeballing one number.
//
// PREDICTION: in the MEDIAN, idle ~= noop << hot — a busy sibling steals
// issue ports (~1.3-2x slowdown expected); a polite pause sibling yields
// ports so it should look ~= idle; mere tenant presence (vs. genuine port
// contention) adds only minor static-partition overhead. The median is the
// robust signal here; tails remain OS-jitter-sensitive without core
// isolation (same caveat as smt_pingpong.cpp).
//
// CONTROL MODE: --same-ccx pins the tenant to a same-CCX, DIFFERENT physical
// core instead of cpu_hot's SMT sibling (shares L3 only, not ports/L1/L2/
// store buffer). Everything else — victim workload, 3 tenant states,
// interleaved repeats, reporting — is identical. Since the L1-resident,
// port-bound victim never touches L3 traffic, the control's prediction is
// idle ~= noop ~= hot: no shared on-core resource for the tenant to steal, so
// clustering here (vs. the default run's idle~=noop<<hot) is what isolates
// the sibling-mode slowdown to on-core contention rather than "a busy
// neighbor somewhere on the chip."
//
// build: g++ -O3 -std=c++23 -pthread sibling_noise.cpp -o sibling_noise
// run:   ./sibling_noise             (auto-selects cpu_hot / SMT sibling)
//        ./sibling_noise --same-ccx  (control: cpu_hot / same-CCX non-sibling)
//        ./sibling_noise --test      (pure-logic self-check, no timing hw)
// ============================================================================

// pp_core.hpp declares l3_peers_of()/contains() (static, internal linkage),
// used below to select the --same-ccx control's tenant CPU: the same "same
// L3, not a sibling" selection smt_pingpong.cpp's auto mode uses for its
// "same L3 / CCX" tier.
#include "pp_core.hpp"

// ---------------------------------------------------------------------------
// Tuning knobs specific to this experiment. Kept together, same discipline as
// pp_core.hpp's Cfg: every literal that shapes the measurement gets a name
// and a one-line reason here, nowhere else.
// ---------------------------------------------------------------------------
struct NoiseCfg {
  // Shared init/seed value for both the victim's and the tenant's
  // accumulators. Defined exactly once here (the retired design had this
  // literal, 0xdeadbeef, duplicated across functions) — any odd-ish non-zero
  // 64-bit constant works, its only job is to give the mix a non-trivial
  // starting state so the first few iterations aren't degenerate.
  static constexpr uint64_t INIT_SEED = 0xdeadbeefULL;

  // Multiplicative mixing constant shared by the victim's and tenant's hot
  // loops (Knuth's multiplicative-hash constant: an odd 32-bit value with no
  // small factors). Any odd constant defeats the "output is a trivial
  // function of input" case the compiler could otherwise exploit; this one
  // is a well-known, unremarkable choice.
  static constexpr uint64_t MIX_CONST = 2654435761ull;

  // Number of independent accumulator lanes in the victim's hot loop. Each
  // lane has no dependency on any other lane within an iteration, so the
  // core's out-of-order engine can issue all N_LANES chains' operations in
  // parallel instead of stalling on a single dependency chain — this is what
  // makes the victim throughput-bound (port-pressure-sensitive) rather than
  // latency-bound (dependency-chain-sensitive, and therefore SMT-insensitive,
  // which is what doomed the retired ping-pong design). 8 independent chains
  // fully saturate the core's integer-multiply pipes: 8 lanes / ~4-cycle imul
  // latency ≈ 2 imul/cycle, which is exactly the mul-pipe width on a modern
  // core (e.g. Zen 5) — so the loop is throughput-bound on a resource the two
  // SMT threads must share, while still fitting comfortably in registers.
  static constexpr int N_LANES = 8;

  // Element count of the victim's scratch buffer (uint64_t elements).
  // 512 * 8B = 4KB, comfortably inside a private L1D (typically 32-48KB on
  // this class of core), so the victim's traffic never spills to L2/L3 —
  // contention stays confined to what a busy sibling can actually contend
  // for (issue ports, L1, store buffer), not memory latency.
  static constexpr int BUF_ELEMS = 512;

  // Number of full passes over the buffer per timed "chunk" (one rdtsc
  // sample). A single pass over 512 elements / 8 lanes is only ~64 loop
  // iterations — too short relative to the ~20-30ns rdtsc observer tax
  // (pp_core.hpp) to give a clean signal. Repeating the pass CHUNK_PASSES
  // times brings one timed sample up to a few hundred ns, which is long
  // enough to amortize the rdtsc tax to a small fraction of the sample while
  // still giving a tight, fast-to-collect distribution. Tune this constant
  // (not the buffer or lane count) if the measured ns/chunk printed at
  // startup drifts far outside the target band on a different CPU.
  static constexpr int CHUNK_PASSES = 6;

  // Number of untimed warm-up chunks run before the timed pass. Same
  // rationale as Cfg::WARM_ITERS in pp_core.hpp: pulls the buffer into L1,
  // trains branch prediction (there is none here, but this also lets the
  // core ramp to steady clock) and lets the tenant reach steady state before
  // any timed sample is taken.
  static constexpr uint64_t WARM_ITERS = 2000;

  // Number of timed chunks recorded per pass (per state, per repeat). Pooled
  // across R repeats this gives R * SAMPLE_ITERS samples per state, enough
  // to resolve p99.9 (needs order-1e3 samples in the tail) while keeping the
  // whole run well under a minute.
  static constexpr uint64_t SAMPLE_ITERS = 5000;

  // Number of interleaved repeats of {idle, noop, hot}. Running the three
  // states round-robin R times (rather than as one long sequential block
  // each) spreads thermal ramp / governor / scheduler drift evenly across
  // all three, so a slow drift over the run's lifetime doesn't bias whichever
  // state happens to run last. R=8 is enough to compute a meaningful
  // min/median/max of per-repeat medians (the run-to-run stability check)
  // without dominating total run time.
  static constexpr int R = 8;

  // Element count of the tenant's "hot" scratch buffer, same L1-residency
  // reasoning as BUF_ELEMS above.
  static constexpr int TENANT_BUF_ELEMS = 512;

  // Number of independent accumulator lanes in the tenant's hot loop. Mirrors
  // N_LANES: the retired design's tenant used a single dependent mul/add
  // chain (effectively IPC~2, not genuinely port-bound), which wasn't hungry
  // enough to contend meaningfully for shared issue ports. Multiple
  // independent lanes make the tenant a real port-pressure source, matching
  // the victim's own shape.
  static constexpr int TENANT_LANES = 8;

  // The tenant's hot loop checks the stop flag only every (mask+1)
  // iterations, not every iteration: checking every iteration would add an
  // extra atomic load into the tight loop we're deliberately trying to make
  // port/cache hungry. 4096 outer iterations of the small inner loop is still
  // well under a millisecond, so shutdown latency stays negligible.
  static constexpr uint64_t HOT_STOP_CHECK_MASK = 0xFFFull;
};

// Elision guards: publish each hot loop's final accumulator here so the
// compiler can never prove the arithmetic is dead and fold it away. Nothing
// else reads these values — they exist purely as observable sinks.
static std::atomic<uint64_t> g_victim_sink{0};
static std::atomic<uint64_t> g_tenant_sink{0};

// ---------------------------------------------------------------------------
// Tenant CPU placement, selected by --same-ccx. Factored into small pure
// functions (rather than inlined in main()) so run_self_tests() can exercise
// the selection logic directly, without /sys or process args.
// ---------------------------------------------------------------------------

// Default placement: the first CPU in `sibs` (cpu_hot's SMT-sibling list,
// from siblings_of()) that isn't cpu_hot itself. Returns -1 if cpu_hot has no
// distinct sibling (SMT off, or unreadable topology).
static int choose_sibling_cpu(int cpu_hot, const std::vector<int> &sibs) {
  for (int c : sibs)
    if (c != cpu_hot)
      return c;
  return -1;
}

// --same-ccx placement: the first CPU that shares cpu_hot's L3 (`l3_peers`,
// from l3_peers_of()) but is NOT one of cpu_hot's SMT siblings (`sibs`) — i.e.
// same CCX, distinct physical core. This mirrors smt_pingpong.cpp auto mode's
// "same L3 / CCX" tier selection (its `same_l3` loop) exactly, so both tools
// agree on what "same-CCX, non-sibling" means. Returns -1 if no such CPU
// exists (e.g. a single-core-per-CCX topology, or unreadable sysfs).
static int choose_same_ccx_cpu(const std::vector<int> &l3_peers,
                               const std::vector<int> &sibs) {
  for (int c : l3_peers)
    if (!contains(sibs, c))
      return c;
  return -1;
}

// ---------------------------------------------------------------------------
// One timed "chunk" of the victim's throughput-bound workload: CHUNK_PASSES
// full passes over `buf`, each pass updating N_LANES independent
// accumulators with no cross-lane or cross-iteration dependency. Factored
// out of the timing loop so run_self_tests() can call it directly and assert
// it actually mutates state, without spinning a thread or touching rdtsc.
// ---------------------------------------------------------------------------
static inline void victim_chunk(uint64_t *buf,
                                uint64_t acc[NoiseCfg::N_LANES]) {
  for (int pass = 0; pass < NoiseCfg::CHUNK_PASSES; pass++) {
    for (int i = 0; i < NoiseCfg::BUF_ELEMS; i += NoiseCfg::N_LANES) {
      for (int k = 0; k < NoiseCfg::N_LANES; k++) {
        // Independent per-lane update: acc[k] depends only on acc[k] and
        // buf[i+k] from THIS iteration, never on any other lane, so the
        // N_LANES chains can issue and execute in parallel.
        acc[k] = acc[k] * NoiseCfg::MIX_CONST + buf[i + k];
      }
    }
  }
}

// One "step" of the tenant's hot loop, same independent-lanes shape as
// victim_chunk. Factored out so run_self_tests() can assert it mutates state
// without spinning a thread.
static inline void tenant_step(uint64_t *buf,
                               uint64_t acc[NoiseCfg::TENANT_LANES]) {
  for (int i = 0; i < NoiseCfg::TENANT_BUF_ELEMS; i += NoiseCfg::TENANT_LANES) {
    for (int k = 0; k < NoiseCfg::TENANT_LANES; k++) {
      acc[k] = acc[k] * NoiseCfg::MIX_CONST + buf[i + k];
    }
  }
}

// "Bad tenant": genuinely port-bound independent-lanes loop pinned to
// cpu_tenant. Runs until `stop` is observed, then publishes its accumulators
// to the sink so the compiler can't prove the whole loop is dead.
static void tenant_hot(int cpu, std::atomic<bool> &ready,
                       std::atomic<bool> &stop) {
  if (!pin(cpu)) {
    fprintf(stderr, "  ! pin cpu%d failed (tenant)\n", cpu);
    exit(1);
  }
  std::vector<uint64_t> buf(NoiseCfg::TENANT_BUF_ELEMS, 1);
  uint64_t acc[NoiseCfg::TENANT_LANES];
  for (int k = 0; k < NoiseCfg::TENANT_LANES; k++)
    acc[k] = NoiseCfg::INIT_SEED + uint64_t(k); // distinct per-lane seed
  ready.store(true, std::memory_order_release);
  uint64_t iter = 0;
  for (;;) {
    tenant_step(buf.data(), acc);
    if ((++iter & NoiseCfg::HOT_STOP_CHECK_MASK) == 0 &&
        stop.load(std::memory_order_relaxed))
      break;
  }
  uint64_t sink = 0;
  for (int k = 0; k < NoiseCfg::TENANT_LANES; k++)
    sink ^= acc[k];
  g_tenant_sink.store(sink, std::memory_order_relaxed);
}

// "Polite tenant": present on cpu_tenant, but PAUSE yields the shared
// execution ports back to the other hardware thread every iteration (this
// only matters under the default SMT-sibling placement — under --same-ccx
// the two cores don't share ports at all, so PAUSE has nothing to yield).
static void tenant_noop(int cpu, std::atomic<bool> &ready,
                        std::atomic<bool> &stop) {
  if (!pin(cpu)) {
    fprintf(stderr, "  ! pin cpu%d failed (tenant)\n", cpu);
    exit(1);
  }
  ready.store(true, std::memory_order_release);
  while (!stop.load(std::memory_order_relaxed))
    _mm_pause();
}

enum class TenantMode { Idle, Noop, Hot };

// ---------------------------------------------------------------------------
// Run one pass of the victim (warm-up + SAMPLE_ITERS timed chunks, all on
// cpu_hot) with the given tenant state live on cpu_tenant for the whole pass.
// Appends this pass's samples (raw TSC-cycle deltas) to `out`.
//
// Tenant lifecycle: spawn, pin, signal ready; we wait for that ready before
// starting the victim's warm-up, so the tenant is in steady state across
// BOTH the warm-up and the timed section. The tenant's `stop` flag is set,
// and it is joined, only AFTER the victim's timed pass completes.
// ---------------------------------------------------------------------------
static void run_pass(TenantMode mode, int cpu_hot, int cpu_tenant,
                     std::vector<uint32_t> &out) {
  std::atomic<bool> ready{false}, stop{false};
  std::thread tenant;
  bool has_tenant = (mode != TenantMode::Idle);

  if (has_tenant) {
    if (mode == TenantMode::Hot)
      tenant =
          std::thread(tenant_hot, cpu_tenant, std::ref(ready), std::ref(stop));
    else
      tenant =
          std::thread(tenant_noop, cpu_tenant, std::ref(ready), std::ref(stop));
    while (!ready.load(std::memory_order_acquire)) // wait: tenant in position
      _mm_pause();
  }

  if (!pin(cpu_hot)) {
    // Fatal, not logged: an unpinned victim thread could migrate off the
    // contended core mid-run, making the whole pass meaningless.
    fprintf(stderr, "  ! pin cpu%d failed (victim)\n", cpu_hot);
    exit(1);
  }

  std::vector<uint64_t> buf(NoiseCfg::BUF_ELEMS, 1);
  uint64_t acc[NoiseCfg::N_LANES];
  for (int k = 0; k < NoiseCfg::N_LANES; k++)
    acc[k] = NoiseCfg::INIT_SEED + uint64_t(k); // distinct per-lane seed

  for (uint64_t i = 0; i < NoiseCfg::WARM_ITERS; i++)
    victim_chunk(buf.data(), acc);

  size_t base = out.size();
  out.resize(base + NoiseCfg::SAMPLE_ITERS);
  for (uint64_t i = 0; i < NoiseCfg::SAMPLE_ITERS; i++) {
    uint64_t t0 = rdtsc_now();
    victim_chunk(buf.data(), acc);
    uint64_t t1 = rdtsc_now();
    out[base + i] = uint32_t(t1 - t0);
  }

  uint64_t sink = 0;
  for (int k = 0; k < NoiseCfg::N_LANES; k++)
    sink ^= acc[k];
  g_victim_sink.store(sink,
                      std::memory_order_relaxed); // keep acc observably alive

  if (has_tenant) {
    stop.store(true, std::memory_order_relaxed); // only after the timed pass
    tenant.join();
  }
}

// ---------------------------------------------------------------------------
// Aggregate results for one tenant state, pooled across all R repeats plus
// the per-repeat medians (the run-to-run stability check).
// ---------------------------------------------------------------------------
struct StateResult {
  const char *label;
  TenantMode mode;
  std::vector<uint32_t>
      samples; // pooled, all R repeats, unsorted until report time
  std::vector<double> repeat_medians; // ns, one per repeat
};

static void print_state_result(StateResult &sr) {
  std::sort(sr.samples.begin(), sr.samples.end());
  std::sort(sr.repeat_medians.begin(), sr.repeat_medians.end());

  printf("%-22s n=%zu samples over %d repeats\n", sr.label, sr.samples.size(),
         NoiseCfg::R);
  printf("    median %7.1f   min %7.1f  p90 %7.1f  p99 %7.1f  p99.9 %8.1f  "
         "max %9.1f  (ns/chunk)\n",
         pct(sr.samples, 50), pct(sr.samples, 0), pct(sr.samples, 90),
         pct(sr.samples, 99), pct(sr.samples, 99.9), pct(sr.samples, 100));
  printf("    run-to-run per-repeat median: min %7.1f  median %7.1f  max "
         "%7.1f  (ns, n=%d repeats)\n",
         sr.repeat_medians.front(),
         sr.repeat_medians[sr.repeat_medians.size() / 2],
         sr.repeat_medians.back(), NoiseCfg::R);
}

// ---------------------------------------------------------------------------
// Minimal, framework-free self-check: `./sibling_noise --test`. No threads,
// no /sys, no timing hardware needed, so it runs the same on any box / in CI.
// ---------------------------------------------------------------------------
static int run_self_tests() {
  // victim_chunk must actually mutate every lane — guards against the
  // compiler proving the victim's arithmetic is dead and eliding it.
  {
    std::vector<uint64_t> buf(NoiseCfg::BUF_ELEMS, 1);
    uint64_t acc[NoiseCfg::N_LANES];
    uint64_t before[NoiseCfg::N_LANES];
    for (int k = 0; k < NoiseCfg::N_LANES; k++)
      before[k] = acc[k] = NoiseCfg::INIT_SEED + uint64_t(k);
    victim_chunk(buf.data(), acc);
    uint64_t sink = 0;
    for (int k = 0; k < NoiseCfg::N_LANES; k++) {
      if (acc[k] == before[k]) {
        fprintf(stderr, "FAIL victim_chunk: lane %d did not change\n", k);
        return 1;
      }
      sink ^= acc[k];
    }
    if (sink == 0) {
      fprintf(stderr, "FAIL victim_chunk: sink is degenerate zero\n");
      return 1;
    }
  }

  // tenant_step must likewise mutate every lane.
  {
    std::vector<uint64_t> buf(NoiseCfg::TENANT_BUF_ELEMS, 1);
    uint64_t acc[NoiseCfg::TENANT_LANES];
    uint64_t before[NoiseCfg::TENANT_LANES];
    for (int k = 0; k < NoiseCfg::TENANT_LANES; k++)
      before[k] = acc[k] = NoiseCfg::INIT_SEED + uint64_t(k);
    tenant_step(buf.data(), acc);
    for (int k = 0; k < NoiseCfg::TENANT_LANES; k++) {
      if (acc[k] == before[k]) {
        fprintf(stderr, "FAIL tenant_step: lane %d did not change\n", k);
        return 1;
      }
    }
  }

  // Reuse of pct()'s own sanity check, just enough to confirm the shared
  // header (pp_core.hpp) linked in and behaves as smt_pingpong's self-test
  // already established.
  g_tsc_ghz = 1.0;
  std::vector<uint32_t> sorted{10, 20, 30, 40};
  if (pct(sorted, 50) != 25.0) {
    fprintf(stderr, "FAIL pct sanity check\n");
    return 1;
  }

  // choose_sibling_cpu: picks the first sibling that isn't cpu_hot itself,
  // or -1 when the only "sibling" is cpu_hot (SMT off / degenerate list).
  if (choose_sibling_cpu(0, {0, 4}) != 4) {
    fprintf(stderr, "FAIL choose_sibling_cpu: expected 4\n");
    return 1;
  }
  if (choose_sibling_cpu(0, {0}) != -1) {
    fprintf(stderr, "FAIL choose_sibling_cpu: expected -1 (no sibling)\n");
    return 1;
  }
  if (choose_sibling_cpu(0, {}) != -1) {
    fprintf(stderr, "FAIL choose_sibling_cpu: expected -1 (empty list)\n");
    return 1;
  }

  // choose_same_ccx_cpu: picks the first L3 peer that ISN'T also an SMT
  // sibling (i.e. a distinct physical core sharing only L3), skipping
  // siblings even if they appear first in the L3 peer list.
  if (choose_same_ccx_cpu({0, 4, 1, 5}, {0, 4}) != 1) {
    fprintf(stderr,
            "FAIL choose_same_ccx_cpu: expected 1 (first non-sibling peer)\n");
    return 1;
  }
  if (choose_same_ccx_cpu({0, 4}, {0, 4}) != -1) {
    fprintf(stderr, "FAIL choose_same_ccx_cpu: expected -1 (every L3 peer is a "
                    "sibling)\n");
    return 1;
  }
  if (choose_same_ccx_cpu({}, {0, 4}) != -1) {
    fprintf(stderr, "FAIL choose_same_ccx_cpu: expected -1 (no L3 peers)\n");
    return 1;
  }

  printf("all tests passed\n");
  return 0;
}

int main(int argc, char **argv) {
  if (argc == 2 && std::strcmp(argv[1], "--test") == 0)
    return run_self_tests();

  // --same-ccx is the only other accepted flag: the control placement (see
  // file header). No args at all keeps the exact default behavior.
  bool same_ccx = false;
  if (argc == 2 && std::strcmp(argv[1], "--same-ccx") == 0) {
    same_ccx = true;
  } else if (argc != 1) {
    fprintf(stderr, "usage: %s [--same-ccx] (auto-selects cpus)\n", argv[0]);
    return 1;
  }

  check_invariant_tsc(); // warn (non-fatally) before trusting any ns figure
  calibrate();
  printf("TSC: %.4f GHz\n\n", g_tsc_ghz);

  // Auto-select cpu_hot (first online CPU). Unlike the retired design, no
  // ping-pong / responder core is needed — only cpu_hot (victim) and
  // cpu_tenant (chosen below, per placement mode) ever matter.
  auto cpus = online_cpus();
  if (cpus.empty()) {
    fprintf(stderr, "need >=1 online cpu\n");
    return 1;
  }
  int cpu_hot = cpus[0];
  auto sibs = siblings_of(cpu_hot);

  int cpu_tenant;
  const char *placement_label;
  if (!same_ccx) {
    // Default placement: cpu_hot's SMT sibling — shares the physical core
    // (ports, L1, L2, store buffer). Fail fast: this experiment measures
    // sibling contention, so with SMT off (or a topology report claiming no
    // sibling) there is no shared physical core to contend for, and the
    // whole premise is void. This guard only gates the default mode — under
    // --same-ccx a missing SMT sibling is irrelevant, since the tenant never
    // uses it.
    cpu_tenant = choose_sibling_cpu(cpu_hot, sibs);
    if (cpu_tenant == -1) {
      fprintf(stderr,
              "fatal: cpu%d has no SMT sibling (is SMT disabled?) — this "
              "experiment is meaningless without a sibling hardware thread "
              "to host the tenant\n",
              cpu_hot);
      return 1;
    }
    placement_label = "SMT sibling, shares core";
  } else {
    // Control placement: a same-CCX (same L3), DIFFERENT physical core — the
    // same selection smt_pingpong.cpp auto mode uses for its "same L3 / CCX"
    // tier. Fail fast if no such core exists (single-core-per-CCX topology,
    // or unreadable sysfs): --same-ccx has nothing meaningful to measure
    // without a distinct same-CCX core.
    auto l3 = l3_peers_of(cpu_hot);
    cpu_tenant = choose_same_ccx_cpu(l3, sibs);
    if (cpu_tenant == -1) {
      fprintf(stderr,
              "fatal: cpu%d has no same-CCX non-sibling core available — "
              "--same-ccx requires a distinct physical core sharing L3 with "
              "cpu%d (is this a single-core-per-CCX topology, or is sysfs "
              "unreadable?)\n",
              cpu_hot, cpu_hot);
      return 1;
    }
    placement_label = "same-CCX core, shares L3 only";
  }

  printf("victim: cpu%d (hot)\n", cpu_hot);
  printf("tenant: cpu%d (%s)\n", cpu_tenant, placement_label);
  printf("R=%d repeats, %llu samples/pass, %llu warm-up chunks/pass\n\n",
         NoiseCfg::R, (unsigned long long)NoiseCfg::SAMPLE_ITERS,
         (unsigned long long)NoiseCfg::WARM_ITERS);

  if (!same_ccx) {
    printf("PREDICTION: in the MEDIAN, idle ~= noop << hot — a busy sibling "
           "steals issue ports (~1.3-2x slowdown expected); a polite pause "
           "sibling yields ports so it should look ~= idle; mere tenant "
           "presence adds only minor static-partition overhead. The median "
           "is the robust signal; tails remain OS-jitter-sensitive without "
           "core isolation.\n\n");
  } else {
    printf("PREDICTION (control): idle ~= noop ~= hot, all clustered — a "
           "tenant on a separate physical core shares only L3 with the "
           "victim, and the victim's traffic is L1-resident (see BUF_ELEMS), "
           "so it never touches L3 at all. With no on-core resource (issue "
           "ports / L1 / L2 / store buffer) for the tenant to contend for, "
           "there should be nothing here to slow the victim down — this is "
           "the control that isolates the default run's slowdown to on-core "
           "contention, not just chip-wide business.\n\n");
  }

  StateResult idle{"idle (no tenant)", TenantMode::Idle, {}, {}};
  StateResult noop{"noop (pause-spin)", TenantMode::Noop, {}, {}};
  StateResult hot{"hot (busy tenant)", TenantMode::Hot, {}, {}};
  StateResult *states[] = {&idle, &noop, &hot};

  // Interleave passes round-robin across R repeats so thermal/scheduler
  // drift is spread evenly across all three states instead of biasing
  // whichever one happens to run in a sequential block last.
  for (int r = 0; r < NoiseCfg::R; r++) {
    for (StateResult *sr : states) {
      std::vector<uint32_t> pass_samples;
      run_pass(sr->mode, cpu_hot, cpu_tenant, pass_samples);

      std::vector<uint32_t> sorted_pass =
          pass_samples; // pct() needs sorted input
      std::sort(sorted_pass.begin(), sorted_pass.end());
      sr->repeat_medians.push_back(pct(sorted_pass, 50));

      sr->samples.insert(sr->samples.end(), pass_samples.begin(),
                         pass_samples.end());
    }
  }

  for (StateResult *sr : states)
    print_state_result(*sr);

  printf("\nnote: no core isolation — absolute numbers are OS-jitter-"
         "sensitive; compare states within this run only, not across runs or "
         "machines. Check the run-to-run per-repeat median spread above "
         "against the gap between states before treating any ordering as "
         "significant.\n");
  return 0;
}
