// examples/spsc_marked.cpp — a worked example for sibling_analyze (README
// Step 5). It marks representative producer and consumer hot loops in the shape
// spsc_pipeline actually uses: a light producer that fills a message, and a
// port-bound consumer that runs the independent-lane imul kernel
// (process_msg_lanes) whose contention this whole repo is about.
//
// This is illustrative source for the tool, not a runnable benchmark. Analyse:
//   ./sibling_analyze examples/spsc_marked.cpp --profile example.profile
//   ./sibling_analyze examples/spsc_marked.cpp --profile example.profile
//   --emit-model > model.csv
//
// Marking follows the contract in sibling_marks.hpp: the markers wrap the
// STEADY-STATE inner loop, and the queue push/pop (coherence traffic, = the Dh
// term) is deliberately OUTSIDE the marked spans.
#include "sibling_marks.hpp"
#include <cstdint>

static constexpr uint64_t MIX = 2654435761ull;

// Consumer: the port-bound per-message work — 6 independent imul-accumulate
// lanes (the process_msg_lanes shape), run for `rounds`. Per the marking
// contract the markers wrap the LOOP, not its body: a "memory"-clobber barrier
// inside the body would perturb the very codegen we mean to measure (and
// sibling_analyze would warn). mca then models the loop body (6 imuls + the
// back-edge) as the repeated steady-state block.
uint64_t consumer(const uint64_t *payload, uint32_t rounds) {
  uint64_t lane[6];
  for (int k = 0; k < 6; k++)
    lane[k] = payload[k];
  SIBLING_REGION_BEGIN("consumer");
  for (uint32_t r = 0; r < rounds; r++)
    for (int k = 0; k < 6; k++)
      lane[k] = lane[k] * MIX + payload[k]; // independent lanes, no cross-dep
  SIBLING_REGION_END("consumer");
  uint64_t acc = 0;
  for (int k = 0; k < 6; k++)
    acc += lane[k];
  return acc;
}

// Producer: a light publish path — fill a 6-word payload with a cheap mix and
// store it. Store/ALU bound, not imul-bound, so it is a POLITE tenant: it
// contends little on the consumer's multiply pipe (the model's low-C, presence-
// tax-dominated regime, which is why the real crossover lands in microseconds).
// Markers wrap the loop, same contract as the consumer.
void producer(uint64_t *out, uint64_t seed) {
  uint64_t s = seed;
  SIBLING_REGION_BEGIN("producer");
  for (int k = 0; k < 6; k++) {
    s = s * MIX + 1;
    out[k] = s;
  }
  SIBLING_REGION_END("producer");
}
