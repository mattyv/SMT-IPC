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

enable_testing()
add_test(NAME self_test COMMAND smt_pingpong --test)
add_test(NAME sibling_noise_self_test COMMAND sibling_noise --test)
add_test(NAME spsc_pipeline_self_test COMMAND spsc_pipeline --test)
add_test(NAME spsc_pipeline_blocking_probe COMMAND spsc_pipeline --blocking-probe)
