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

enable_testing()
add_test(NAME self_test COMMAND smt_pingpong --test)
add_test(NAME sibling_noise_self_test COMMAND sibling_noise --test)
