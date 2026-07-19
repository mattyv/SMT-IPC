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
// PERTURBATION NOTE: the inline asm carries a "memory" clobber, which orders
// MEMORY accesses across the marker — but it does NOT pin register-only
// dataflow. The compiler may still hoist purely-register work (an ALU chain
// with no memory operand) out of a marked span, leaving the region empty; that
// is why the marked region should wrap a LOOP whose body touches memory, not a
// straight-line register computation. sibling_analyze guards this from both
// sides: it hard-errors on a marked region that ends up with zero compute
// instructions (the total-hoist case), and it builds your TU twice — with
// markers and with them compiled out (-DSIBLING_MARKERS_OFF) — warning if the
// marked functions' compute instruction mix differs. That catches only
// marker-INDUCED codegen changes (chiefly blocked autovectorisation), NOT a
// hoist that happens identically with and without the markers. A volatile asm
// INSIDE a loop body is the usual cause of such a change, which is the main
// reason markers belong around the loop, not in it.
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
