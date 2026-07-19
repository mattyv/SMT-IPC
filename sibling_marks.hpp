// ============================================================================
// sibling_marks.hpp — mark the hot regions of your IPC threads for
// sibling_analyze (the static llvm-mca placement linter).
// ----------------------------------------------------------------------------
// You wrap the STEADY-STATE hot loop of each thread you want analysed. Give the
// producer's publish loop one name and the consumer's processing loop another;
// sibling_analyze runs each marked region through llvm-mca, extracts its
// per-execution-port demand, overlays the two, and predicts whether the pair
// will contend when co-scheduled on the two SMT threads of one physical core.
//
//   SIBLING_REGION_BEGIN("consumer");
//   ... the tight loop body you actually ship ...
//   SIBLING_REGION_END("consumer");
//
// These expand to llvm-mca's own `# LLVM-MCA-BEGIN <name>` / `# LLVM-MCA-END`
// assembler-comment markers, emitted via an inline-asm statement so they land
// in the compiler's output at the right spot. Any number of distinctly-named
// regions may appear in one translation unit.
//
// MARKING CONTRACT (read this — the analysis is only as honest as the marking):
//   * Put the markers AROUND the loop, never inside a single iteration. mca
//     analyses the marked span as an infinitely-repeated straight-line block;
//     that model is meaningful for a steady-state loop body and meaningless for
//     one-time setup or a rarely-taken branch.
//   * Keep the queue push/pop and any lock-prefixed / fenced handoff
//     instructions OUT of the region. Their real cost is cross-core coherence
//     traffic, which mca cannot see and which is already accounted for
//     separately as the handoff term (Δh, measured by smt_pingpong). Counting
//     them inside the region would double-count them, wrongly. sibling_analyze
//     lints for this and warns.
//   * A `call` that the compiler does not inline hides the callee's entire
//     demand from mca — sibling_analyze treats a call inside a region as a hard
//     error, because the resulting vector is silently empty.
//
// PERTURBATION NOTE: the inline asm carries a "memory" clobber so the compiler
// cannot hoist memory operations across the marker (which would move work out
// of the region you meant to measure). That same barrier can itself perturb
// codegen — most sharply, a volatile asm inside a loop body can defeat
// autovectorisation. This is the other reason markers belong around the loop,
// not in it. sibling_analyze additionally builds your TU twice — once with
// markers and once with them compiled out (-DSIBLING_MARKERS_OFF) — and warns
// if the instruction mix differs, so a perturbation that changed what ships is
// caught rather than silently analysed.
//
// x86 only (the markers are assembler comments; the concept is x86 SMT). Header
// is dependency-free and safe to include anywhere.
// ============================================================================
#pragma once

#ifdef SIBLING_MARKERS_OFF
// Analysis build B: markers compiled to nothing, so sibling_analyze can diff
// the resulting codegen against the marked build and detect perturbation.
#define SIBLING_REGION_BEGIN(name) ((void)0)
#define SIBLING_REGION_END(name) ((void)0)
#else
#define SIBLING_REGION_BEGIN(name)                                             \
  __asm__ volatile("# LLVM-MCA-BEGIN " name ::: "memory")
#define SIBLING_REGION_END(name)                                               \
  __asm__ volatile("# LLVM-MCA-END " name ::: "memory")
#endif
