# SMT-IPC — where do you put two threads that talk to each other?

> **TL;DR — for two threads that pass messages to each other, the fastest place is the *same
> physical core* (its two SMT siblings), as long as the sibling stays *quiet*.** A sibling that's
> only cooperating — mostly `pause`-waiting for its partner — costs about **3%**, and in exchange
> you get the ~50 ns shared-L1/L2 handoff, **~2× faster** (≈1.8× measured) than putting the two
> threads on separate cores in the same CCX. That holds for realistic per-message work (up to a
> couple of µs), and — measured, and it surprised me — it holds whether *one or both* threads are
> busy, **as long as they're paced so they never execute at the same instant** (a paced pipeline's
> consumer waits ~98% of the time, so the two just alternate). The one thing that kills it is
> genuine **overlap** — the two threads grinding on the shared core at the same instant. That
> happens when an *independent* port-hungry tenant lands on the sibling (another process, an IRQ) —
> the full-time, worst case, up to **~1.8×** slower compute (Step 2) — or when your own two threads
> run hot enough that pacing can no longer keep their work windows apart, a milder **~1.45×** at the
> partial duty a paced pipeline reaches (measured). Either way the sibling stops being worth it, and
> it bites at **overlap** — well before the queue ever *saturates*.
> Keeping the sibling *quiet* — only your cooperating, mostly-waiting partner on it — is the trick.
>
> **How to milk it in your design:**
> 1. **Pin your producer and consumer to the two SMT threads of one physical core** — that buys the shared-L1/L2 handoff.
> 2. **Keep everything else off that core.** Pinning steers *your* threads only; the kernel still lands IRQs and other work on the sibling. Isolate it (`isolcpus` + `nohz_full` + `irqaffinity`) or offline the neighbours — a quiet sibling *is* the trick.
> 3. **Don't let them execute at the same instant.** Under matched pacing the two threads alternate, so *both-busy is fine below the crossover* — the sibling still wins even when the producer works as hard as the consumer, because the pacer keeps their work windows *disjoint* (measured: the paced both-busy line lands right on polite). You lose it the moment they actually **overlap** — grind at the same instant because the load outpaces the gap — and that hits *well before* the queue saturates (in the overlap measurement the crossover collapses from ~2 µs to below ~0.5 µs, on rungs the tool confirms are queue-free). Pace the feed, or keep one side mostly waiting, so they never grind simultaneously.
> 4. **Stay under a few µs of work per message.** Past the crossover — or whenever the two must **overlap** (both grinding with no gap to interleave in) — step out to a *same-CCX* core instead (~40 ns slower handoff, but immune to on-core contention).
> 5. **Check before you commit:** `sibling_analyze` predicts, statically from your compiled loops, whether your two specific threads will collide or fit — no timing rig required.

If one thread hands messages to another — a reader feeding a processor, a market-data
decoder feeding a strategy — you get to choose where those two threads run on a modern
many-core chip. Same physical core (the two SMT threads of one core)? Two cores sharing
an L3 slice? Further apart still? The folklore says "siblings are fastest, they share
L1." The folklore is half right, and chasing down the other half is what this repository
is. Three small x86-64 Linux microbenchmarks, run on an AMD Zen 5 box, that build to one
measured answer:

![Sibling vs same-CCX placement crossover: sibling−same-CCX latency as consumer work grows, for three producer regimes. Polite and paced-both-busy track each other and cross zero in a ~2–3.7 µs band (sibling wins below it). The overlapping-both-busy line crosses below ~0.5 µs and shoots off the top (+781 ns at 1.7 µs, then saturates). The dashed line is sibling_analyze's static estimate; its W* band is shaded.](docs/crossover.svg)

*Reading the three measured lines:* the x-axis is a ladder of per-message work from ~20 ns to
~7 µs; the y-axis is how much slower the SMT-sibling placement is than a same-CCX core (below zero
= sibling wins). All three feed the *same* consumer; they differ in what the **producer** does.
**Polite** (teal): the producer just stamps and pushes each message — near-zero work. **Both busy,
paced apart** (amber): the producer does the *same* work as the consumer at every rung, but the
pacer widens the arrival gap to fit both, so their work windows stay *disjoint* — they never run at
the same instant. It lands right on top of polite (that's the point: pacing recovers the
polite result even with a busy producer). **Both busy, overlapping** (red): same symmetric work,
but the gap is sized for the consumer alone, so the producer's work genuinely *overlaps* the
consumer's — the real "both threads are victims of each other" case. That one crosses below ~0.5 µs
and rockets up. See Step 4 for the full story.

**A processing consumer is faster on the producer's SMT sibling — but only up to a couple
of microseconds of work per message. Past that, a separate core in the same CCX wins.** For
most real pipelines, which do well under a few µs of work per message, the sibling wins —
*if* you keep everything else off that core. Getting to that sentence takes four steps, each its
own experiment.

## Step 1 — siblings really do give the fastest handoff

`smt_pingpong` measures the raw thing: two pinned threads bounce a cache line back and
forth (a monotonically increasing sequence number through two 128-byte-isolated flags,
so a stale value can never satisfy the wait), both hot-spinning, and it times the round
trip with fenced `rdtsc`. No wakeups, no work — the best-case handoff between two live
spinners.

```
INITIATOR (timed)                RESPONDER
t0 = rdtsc
store a = i   --line a-->         spin until a == i
                                  store b = i
spin until b == i  <--line b--
t1 = rdtsc ; sample = t1 - t0
```

It runs that across three placements the machine's topology gives you:

```mermaid
flowchart TB
  subgraph CCX0["CCX 0 · shared L3"]
    L30["L3 slice"]
    subgraph CORE0["Core 0"]
      cpu0["cpu0"]
      cpu12["cpu12 — SMT sibling"]
      l1a["L1/L2 (private)"]
    end
    CORE1["Core 1 · cpu1"]
  end
  subgraph CCX1["CCX 1 · different L3"]
    L31["L3 slice"]
    CORE4["Core 4 · cpu4"]
  end
  cpu0 --- l1a
  cpu12 --- l1a
  l1a --- L30
  CORE1 --- L30
  CORE4 --- L31
  L30 <-->|on-die interconnect| L31
  cpu0 -. "① SMT sibling — shares L1/L2" .- cpu12
  cpu0 == "② same-CCX — core→core via L3" ==> CORE1
  cpu0 == "③ cross-CCX — over the interconnect" ==> CORE4
  classDef hot fill:#1f6feb,color:#fff,stroke:#0d1117;
  classDef sib fill:#2ea043,color:#fff,stroke:#0d1117;
  class cpu0 hot; class cpu12 sib;
```

The ordering is unambiguous, at every percentile: **SMT sibling ~50 ns, same-CCX ~90 ns,
cross-CCX ~700 ns** (median RTT, pause spinner). Crossing an L3/CCX boundary is a ~7–8× cliff.
So if raw handoff latency were the whole story, you'd always pick the sibling — the line
never leaves the shared L1/L2.

*(Absolutes are noisy — this box has no core isolation and boost/governor aren't locked,
so the deep tail p99.99/max is OS jitter, not hardware. The ordering is the robust part;
compare rows only within one run.)*

## Step 2 — but a busy sibling is poison

The catch: those two SMT threads don't just share cache, they share the physical core's
execution ports, store buffer, and front-end. In the ping-pong the sibling is a *cooperative*
responder, so you never see the downside. `sibling_noise` isolates it: it runs a genuinely
port-hungry victim (8 independent multiply lanes, L1-resident) on one thread and puts a
*tenant* on its sibling — idle, politely pausing, or busy with the same port-hungry work.

```mermaid
flowchart LR
  T0["victim (hot)"]
  T1["tenant (sibling)"]
  subgraph CORE["One physical core"]
    PORTS["shared exec ports"]
    L1L2["shared L1/L2"]
    PORTS --> L1L2
  end
  T0 --> PORTS
  T1 -. "contends for the same ports" .-> PORTS
  classDef hot fill:#1f6feb,color:#fff,stroke:#0d1117;
  classDef sib fill:#2ea043,color:#fff,stroke:#0d1117;
  class T0 hot; class T1 sib;
```

| Tenant on the sibling | victim median | vs idle |
|---|---:|---:|
| idle | 370.7 ns | — |
| polite (`_mm_pause`) | 380.7 ns | +3% |
| busy (port-hungry) | 671.2 ns | **1.81×** |

A busy sibling nearly **doubles** the victim's work. So the fast handoff from Step 1 comes
with a hazard: put anything port-hungry on your hot thread's sibling and you pay for it.
(A merely-*present*, politely-pausing sibling costs almost nothing — hold that thought, it's
the whole point later.)

## Step 3 — the hazard is strictly on-core

Is that 1.8× about "a busy neighbor somewhere on the chip," or specifically about sharing
the *core*? `sibling_noise --same-ccx` reruns the identical experiment with the tenant moved
to a *different* physical core in the same CCX — it shares the L3, but not the ports, L1, or
L2. The port-bound victim is L1-resident, so it never even touches the L3:

| Tenant placement | idle | polite | busy |
|---|---:|---:|---:|
| SMT sibling (shares core) | 370.7 | 380.7 | **671.2** |
| same-CCX core (shares L3 only) | 370.7 | 370.7 | **370.7** |

A busy neighbor *core* costs the victim **nothing** — all three states are identical. The
1.8× is purely on-core port contention between SMT siblings. This is exactly why "just pin
your thread" isn't enough: a hog on your *sibling* wrecks you, a hog on the *next core over*
is invisible.

## Step 4 — but a real partner is polite, so who actually wins?

Steps 2 and 3 make the sibling look dangerous — but they used an *independent* busy tenant.
In a real pipeline the partner isn't independent: the producer spends most of its time
*waiting* for the consumer, and a polite `pause`-waiter costs only ~3% (Step 2's middle row),
not 1.8×. So the worst case rarely fires. Which means the honest question isn't "is the
sibling risky" — it's **at what amount of consumer work does the sibling's ~40 ns handoff
edge get eaten by the contention on that work?**

`spsc_pipeline --proc-sweep` measures it directly: a producer paces messages through a real
[SPSC queue](https://github.com/rigtorp/SPSCQueue) to a consumer doing a tunable, port-bound
amount of work per message, and it compares **sibling vs same-CCX placement** end-to-end as
that work grows. Pacing is matched to each work level so the queue stays near-empty — the
handoff latency is on the critical path, not hidden behind a backlog.

The result is the graph at the top. In numbers (Δ = sibling − same-CCX; negative = sibling
faster):

| consumer work / msg | sibling p50 | same-CCX p50 | Δ (ns) |
|---:|---:|---:|---:|
| 20 ns | 70 | 120 | **−50** |
| 120 ns | 180 | 220 | −40 |
| 451 ns | 511 | 561 | −50 |
| 1.7 µs | 1833 | 1853 | −20 |
| 6.9 µs | 7093 | 7033 | **+60** |

Sibling holds a steady ~50 ns lead while the work is light, the lead erodes past ~1.7 µs,
and same-CCX pulls ahead by ~7 µs — an interpolated **crossover around ~2–3 µs** (the exact
point wanders run-to-run; the stable part is the 1.7–6.9 µs bracket and the sign-flip). Because
the producer stays polite, the crossover lands in the *microseconds*, not the tens of
nanoseconds you'd guess from the 1.8× busy-sibling figure. The practical reading: a consumer
doing less than a couple of µs of work per message — the common case — is faster on the SMT
sibling, provided the producer stays polite and nothing else runs on that core.

**Does that survive *both* threads being busy?** This is the "both threads are victims of each
other" question, and the answer turns entirely on one thing: **do they actually run at the same
instant?** Both `--both-busy` variants make the producer do the *same* per-message work as the
consumer at every rung (a symmetric sweep — at the 2 µs rung both spend ~2 µs, at the 500 ns rung
both spend ~500 ns). They differ only in the pacing gap, and that difference is the whole story.

**Paced apart (`--both-busy`, the amber line).** The arrival gap is widened to fit *both* threads'
work (visible in the tool's `gap` column — roughly double the polite gap). That is enough to keep
the two work windows **disjoint**: the producer works during the gap while the consumer waits, and
they never execute simultaneously (`waited-fraction ≈ 0.99`, `proc_ratio ≈ 1.0`). Result: the line
lands right on top of polite — sibling still wins below the ~2–3 µs crossover. This isn't luck, it's
the schedule: if you space two busy threads so their compute never overlaps, they don't contend,
full stop. (The amber and teal lines *do* pull apart slightly at the 6.9 µs rung — +170 ns vs
+50 ns — but not because the sibling suffers: its latency there is identical to the polite run to
within the ~10 ns measurement quantum. The gap comes from the *same-CCX* line getting ~110 ns
**faster** than its own solo baseline when the neighbouring core is grinding — a CPU-boost/residency
quirk on the other tier, not a sibling contention tax. Don't read it as "busy producer hurts the
sibling.")

**Overlapping (`--both-busy-overlap`, the red line).** Same symmetric work, but the gap is sized for
the consumer *alone*, so the producer's compute genuinely overlaps the consumer's. Now the two
grind on the same physical core's execution ports at the same instant — and the sibling falls apart:
the crossover collapses from ~2 µs to **below ~0.5 µs**, and the sibling runs **+781 ns slower** at
1.7 µs. That +781 is genuine on-core port contention, measured directly: the consumer's own compute
(`proc-insitu`, timed around just the kernel — no queue wait in the bracket) runs `proc_ratio ≈ 1.45`,
i.e. ~45 % slower than solo. That's the *same mechanism* as Step 2's busy-sibling tax but at partial
duty — the producer only occupies the core ~55–65 % of each gap — so it's milder than Step 2's
**1.8×**, which is what a *full-time* independent tenant costs. (In other words the red line isn't
even the worst case; a continuously-busy neighbour is worse.) By the 6.9 µs rung the contention
(~1.54×) exceeds the sweep's 1.5× pacing headroom (`PROC_SWEEP_HEADROOM`), so the queue can no longer
be kept empty and end-to-end latency becomes queue-drift dominated (tens to hundreds of µs,
run-dependent) — the tool flags that rung `INVALID` and it's dropped from the graph. That drop is
*conservative*: with more headroom the rung would be valid and show the sibling ~**+3.7 µs** worse,
not recovering.

On the rungs that *are* valid (`waited-fraction 0.92–0.99`, so the p50 latency is genuinely
queue-free), you can see the loss is on-core contention and not queueing. `backpressure` stays 0
throughout — but that's a weak signal on its own (it only trips when the queue hits its 4096-slot
cap, which never happens); the real evidence is the clean waited-fraction on the sub-saturation
rungs. So the honest rule has two halves:

- **Pace them apart** (bounded rate, or one side mostly waiting) and the sibling wins for work under
  ~2 µs whether one *or both* sides are busy — pacing turns a busy producer back into a polite one.
- **Let them overlap** (both grinding at a rate the gap can't separate) and the sibling loses *early
  and hard* — crossover below ~0.5 µs, then a cliff — well before the queue saturates. That's the
  regime Step 2's on-core contention (up to 1.8× for a full-time tenant) governs.

> **Why the numbers here have moved twice.** First pass: the both-busy line looked *identical* to
> polite because `--both-busy` had a wiring bug — the flag parsed but the producer workload never
> reached the sweep, so it silently ran the polite experiment. Second pass (after fixing that): the
> line was genuinely busy but still ≈ polite, and this section wrongly credited a "momentary overlap
> tax on the sibling" for the high-end divergence — a fable review showed the sibling's numbers
> never moved and the paced gap makes overlap *impossible* by construction. Both are now corrected,
> and `--both-busy-overlap` was added so the real overlap regime is measured rather than argued.

**Scope, so this isn't over-read:** the message source is an in-memory ring, *by design* — a
real socket's `recv()` is microseconds and would swamp this nanosecond-scale placement signal
entirely. This answers "given a message already in hand, does placement or processing weight
decide who's faster," not "is it fast enough for a live feed." And with no core isolation on
this box, the crossover *direction* reproduces **as long as the box is quiet and unthrottled**
(watch the `proc_insitu_ratio` column reads ≈ 1 — on a loaded or low-power box the producer stops
staying polite and the sign can flip); the exact point (~2–3 µs) is the softer part.

## So, for HFT

- **Siblings are the fastest handoff, and for realistic per-message work they win** — the
  producer waits politely, so the contention penalty stays small and the ~40 ns handoff edge
  carries you to a couple of µs of consumer work.
- **The catch is everything *else* on the core.** The win holds only if the sibling is your
  cooperating partner (or empty). Any independent port-hungry tenant — another process, an
  IRQ handler, a kernel thread — brings the 1.8×. Pinning steers *your* thread, not the
  kernel's; to truly own the core you need `isolcpus`+`nohz_full`+`irqaffinity` or offlining
  across **both** SMT threads, which is why HFT shops isolate hot cores or disable SMT outright.
- **Heavy per-message work (> ~3 µs) or an untrusted sibling → step out to a same-CCX core.**
  You give up ~40 ns of handoff for immunity to on-core contention.
- **Keep tightly-coupled threads inside one CCX regardless** — the cross-CCX cliff is ~7–8× and
  dwarfs any of this.

## Step 5 — predicting it for *your* threads, without running anything

Steps 1–4 *measure* the crossover — but measuring means building the timing rig and running a
sweep, on the target hardware, for every workload you're curious about. `sibling_analyze` asks
the same question **statically**: given your actual producer and consumer loops, can you tell —
from the compiled instructions, before running anything — whether they'll be faster as SMT
siblings or on separate cores? It's a linter for thread placement.

**You point it at your two hot loops.** Bracket each thread's steady-state loop with a matched
pair of markers from `sibling_marks.hpp`:

```cpp
#include "sibling_marks.hpp"
for (;;) {
  auto* m = q.front(); if (!m) { _mm_pause(); continue; }
  SIBLING_REGION_BEGIN("consumer");                 // opens the region
  for (int r = 0; r < rounds; r++) process(*m);     // the per-message work
  SIBLING_REGION_END("consumer");                   // closes it (matched by name)
  q.pop();
}
```

`SIBLING_REGION_BEGIN("consumer")` and `SIBLING_REGION_END("consumer")` are a pair — the name
string ties them together, so you can mark the producer's loop and the consumer's loop
distinctly in one file. They expand to assembler-comment markers that tag *exactly which
instructions* the tool analyses. Three rules keep that honest, and the tool lints for all three:
**wrap the whole loop, not one iteration** (the model treats the marked span as a steady-state
body repeated forever); **keep the queue push/pop and any fences *outside* the region** (their
cost is the cross-core handoff, already counted separately — including them here would
double-count it); and **no un-inlined `call` inside** (its callee is invisible to the model, so
the tool refuses rather than analyse half the work).

**How it works, in a sentence:** the tool compiles your marked loops, feeds their instructions
to `llvm-mca` — a model of the CPU's execution ports — and asks *if these two loops ran at the
same time on one physical core, would they demand more of any execution port than the core can
supply?* If yes, they'll fight, and it says split them; if no, the sibling's faster handoff
wins, and it estimates how much work-per-message you can afford before the fight would outweigh
the handoff savings.

**The output tells you what to do, in plain words** (the shipped `examples/spsc_marked.cpp`):

```
RESULT: these two loops will contend on the load/store unit (together they
        demand 1.15x what one core supplies) -> place them on SEPARATE cores.
```

When the loops *don't* oversubscribe a port, it flips to a budget instead — *"Placement budget
~1650 ns of work per message; your consumer does ~100 ns/msg → the SMT sibling is faster."* The
raw numbers (which port, the contention multiplier, the budget and its range) print underneath
as `detail:` lines, and any lint warning rides right next to the verdict. *(One knob:
`--consumer-iters-per-msg N` tells it how many loop iterations make one message, since
`llvm-mca` counts iterations, not messages; without it, it prints the budget but withholds the
recommendation rather than guess.)*

**What's actually predicted, and what isn't — read this before trusting the budget.** The `W*`
number is *mostly not* the llvm-mca model. It's `W* = Δh / (ε + duty·(C−1))`, and at the crossover
the `duty` term is tiny, so `W*` collapses to **Δh/ε ≈ 50 ns / 3% ≈ 1.67 µs** — the *measured*
handoff edge (Step 1) over the *measured* presence tax (Step 2). Plugging in the model's
contention `C` moves that by **~0.8%** (13 ns). So the budget is ~99% two measured constants, not a
static prediction; and `calib_scale` is *fit* so the model's `C` reproduces Step 2's 1.81× — that
magnitude agrees by construction, not by independent check. **The genuinely predictive output is
the verdict** — *does a specific execution port oversubscribe when these two loops run together,
and which one* — read straight from llvm-mca on your compiled code. Trust the `COLLIDES`/`fit`
call and the named port; treat `W*` as an order-of-magnitude budget seeded by the measured
handoff numbers — roughly the crossover Steps 1–4 already give you, not a separate result.

**How close is `W*` to the measured crossover?** ~1.65 µs predicted vs ~2.4–3.7 µs measured (it
wanders run-to-run): same order of magnitude, within a factor of ~2 — all a compile-time screen
that runs *nothing* can honestly claim. The overlay at the top is `sibling_analyze --emit-model`
(dashed) against the measured curves; `scripts/plot_crossover.py --check` asserts they agree
numerically — but since the model's `C` barely moves the curve, that check mostly confirms the
Δh/ε constants line up, not that the port model is right. It's same-machine only (the mca model is
µarch-specific), so regenerate `docs/crossover_data.csv` from `spsc_pipeline --proc-sweep` on your
own box first.

**It's a screening linter, not an oracle — and the limits are load-bearing.** `llvm-mca` sees
execution ports and front-end dispatch and *nothing else*: it assumes perfect caches and store
buffers and a single instruction stream. So a `COLLIDES` verdict (the ports genuinely
oversubscribe) is trustworthy, while a "no collision" only clears the *compute* side — for
anything memory-heavy, confirm with `sibling_noise`/`spsc_pipeline`, which stay the ground
truth. The tool also diffs your code compiled with and without the markers, to catch a marker
that accidentally changed what ships (e.g. blocked vectorisation).

## Build & run

```sh
cmake -B build && cmake --build build && ctest --test-dir build   # builds all 4 + self-tests
```

```sh
./smt_pingpong            # handoff latency across the three placements
./smt_pingpong 0 12       # explicit CPU pair (validated; fatal on bad args / pin failure)
./sibling_noise           # busy-sibling contention (tenant on the SMT sibling)
./sibling_noise --same-ccx  # control: tenant on a same-CCX core instead
./spsc_pipeline           # the placement × processing-weight crossover (Step 4)
./spsc_pipeline --both-busy          # producer as busy as consumer, PACED apart (stays ~= polite)
./spsc_pipeline --both-busy-overlap  # producer as busy as consumer, OVERLAPPING (sibling loses below ~0.5us)
./sibling_analyze t.cpp --profile p   # STATIC: predict placement for marked threads (Step 5)
./sibling_analyze --calibrate 1.81    # derive calib_scale from a measured busy-sibling ratio
<tool> --test             # pure-logic self-checks, no timing hardware needed
```

The three runtime tools share `pp_core.hpp` (TSC calibration, pinning, percentiles, sysfs
topology discovery). Bad CPU args and pin failures are always fatal — an unpinned pair measures
nothing. **x86-64 Linux only** (`rdtsc`, `_mm_pause`, sysfs). `sibling_analyze` is separate and
dependency-light: its `--test` needs only a C++ compiler, and its analysis path additionally
needs `g++` and `llvm-mca` (report format validated against LLVM 18–20) on `PATH`; edit `example.profile`
into your own machine's numbers first.

*(An earlier, larger version of `spsc_pipeline` also carried a separate wait-strategy study —
spin vs pause vs futex-blocking consumers under steady and bursty arrival. It answered a
different question (latency vs core-utilization) and is preserved on the
[`full-wait-strategy-study`](https://github.com/mattyv/SMT-IPC/tree/full-wait-strategy-study)
branch to keep this one focused on the placement result.)*

## Caveats

- **No core isolation on this box:** absolute tails (p99.9+) are OS-jitter-sensitive; compare
  within one run, not across runs or machines. Metrics derived from the pacing schedule
  (`waited-fraction`, `late-publish`) are the robust cross-run signals.
- **TSC** calibrated against `steady_clock`, assumes invariant TSC (`constant_tsc nonstop_tsc`).
- **Run the crossover at normal power, not low-power/throttled.** The result assumes the
  paced producer stays *polite* — check the `proc_insitu_ratio` column reads ≈ 1. Under a
  throttled clock the producer can't keep pace politely, runs hot (ratio ≈ 2+), and same-CCX
  wins at every level — the crossover inverts. Not a fragile result, a wrong-conditions one.
- **Not measured:** throughput/bandwidth, contention from third parties, and real-socket I/O
  (whose µs-scale syscalls would dominate every ns-scale result here).
- **`mwaitx` was evaluated and dropped:** the consumer waits with `_mm_pause`, not the AMD
  hardware wait-on-address — on this Zen 5 box `mwaitx` woke at ~260 ns p50 vs ~60 ns for a
  plain spin (>4× slower, ~350–480 ns timeout floor), so it isn't viable for a sub-µs handoff.
