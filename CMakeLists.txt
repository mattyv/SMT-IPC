# x86-64 Linux only (rdtsc, _mm_pause, sysfs topology) — see README.
cmake_minimum_required(VERSION 3.20)
project(smt_pingpong CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Default to Release (-O3) so a plain `cmake ..` matches the documented
# g++ -O3 build line; override with -DCMAKE_BUILD_TYPE= as usual.
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Threads REQUIRED)

add_executable(smt_pingpong smt_pingpong.cpp)
target_link_libraries(smt_pingpong PRIVATE Threads::Threads)

add_executable(sibling_noise sibling_noise.cpp)
target_link_libraries(sibling_noise PRIVATE Threads::Threads)

# rigtorp/SPSCQueue: header-only lock-free SPSC queue used by spsc_pipeline.
# Pinned to a full commit SHA (not a floating tag) so upstream cannot silently
# change what this pipeline builds against; the tag name is recorded alongside
# for human reference. SHA taken from `git ls-remote --tags` at the v1.1 tag.
include(FetchContent)
FetchContent_Declare(
  spscqueue
  GIT_REPOSITORY https://github.com/rigtorp/SPSCQueue.git
  GIT_TAG 565a5149d54930463d58cb0f69b978d439555e66 # v1.1
)
FetchContent_MakeAvailable(spscqueue)

add_executable(spsc_pipeline spsc_pipeline.cpp)
target_link_libraries(spsc_pipeline PRIVATE Threads::Threads SPSCQueue::SPSCQueue)

# sibling_analyze: the STATIC placement linter. No threads/hardware — it shells
# out to g++ and llvm-mca at RUN time (for real analysis), but the binary itself
# and its --test self-check need only the standard library. Kept dependency-free
# on purpose so `--test` runs in any CI image without llvm installed.
add_executable(sibling_analyze sibling_analyze.cpp)

enable_testing()
add_test(NAME self_test COMMAND smt_pingpong --test)
add_test(NAME sibling_noise_self_test COMMAND sibling_noise --test)
add_test(NAME spsc_pipeline_self_test COMMAND spsc_pipeline --test)
add_test(NAME sibling_analyze_self_test COMMAND sibling_analyze --test)

# Finding-1 regression probe: an INTEGRATION check (runs the real binary's
# --mca-file -> --json path) that print_report()/print_json() actually emit
# the effective-cycles consumer work figure, not just that a helper function
# computes it correctly in isolation. See scripts/check_f1_residual.sh for
# the full rationale — needs only sh/awk, no llvm-mca, so it runs
# unconditionally alongside the other --test self-checks.
add_test(NAME sibling_analyze_f1_residual_probe
  COMMAND sh ${CMAKE_SOURCE_DIR}/scripts/check_f1_residual.sh
    $<TARGET_FILE:sibling_analyze>
    ${CMAKE_SOURCE_DIR}/examples/f1_residual_latency_bound.mca
    ${CMAKE_SOURCE_DIR}/examples/f1_residual_latency_bound.profile)

# Optional: end-to-end crossover check (static model vs measured curve). Needs
# llvm-mca (the analysis engine) and python3, so it is registered ONLY when both
# are present — the plain gcc CI image skips it and runs just the pure --test
# above. This is a SAME-MACHINE self-consistency check (see the script header);
# a generous tolerance keeps it robust across the uarch it happens to run on.
# NAMES lists the versioned binaries this parser is validated against (18-20;
# see sibling_analyze.cpp's parser header) before the unversioned name, since
# most distros ship ONLY the versioned binary (no "llvm-mca" symlink) — a bare
# "llvm-mca" NAMES-only search silently finds nothing on those systems and the
# whole crossover_check test goes missing without a word.
find_program(LLVM_MCA NAMES llvm-mca-20 llvm-mca-19 llvm-mca-18 llvm-mca)
find_program(PYTHON3 NAMES python3)
if(LLVM_MCA AND PYTHON3)
  add_test(NAME crossover_check
    COMMAND ${CMAKE_COMMAND} -E env sh
      ${CMAKE_SOURCE_DIR}/scripts/run_crossover_check.sh
      $<TARGET_FILE:sibling_analyze>
      ${CMAKE_SOURCE_DIR}/examples/spsc_marked.cpp
      ${CMAKE_SOURCE_DIR}/example.profile
      ${CMAKE_SOURCE_DIR}/docs/crossover_data.csv
      ${CMAKE_SOURCE_DIR}/scripts/plot_crossover.py
      ${LLVM_MCA})
  set_tests_properties(crossover_check PROPERTIES WORKING_DIRECTORY
    ${CMAKE_SOURCE_DIR})
endif()
