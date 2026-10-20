include(FetchContent)
include(GoogleTest)  # directory-scoped: needed here for gtest_discover_tests in this dir

FetchContent_Declare(hdr_histogram
  URL https://github.com/HdrHistogram/HdrHistogram_c/archive/refs/tags/0.11.8.tar.gz
  URL_HASH SHA256=bb95351a6a8b242dc9be1f28562761a84d4cf0a874ffc90a9b630770a6468e94
  DOWNLOAD_EXTRACT_TIMESTAMP ON
  SYSTEM
)
set(HDR_HISTOGRAM_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(HDR_HISTOGRAM_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(HDR_LOG_REQUIRED DISABLED CACHE STRING "" FORCE)  # no zlib: we never write .hlog files
FetchContent_MakeAvailable(hdr_histogram)
target_compile_options(hdr_histogram_static PRIVATE -w)

add_library(shmipc_bench_harness
  harness/env.cpp
  harness/histogram.cpp
  harness/options.cpp
)
target_include_directories(shmipc_bench_harness PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(shmipc_bench_harness PUBLIC shmipc::shmipc hdr_histogram_static)
target_compile_options(shmipc_bench_harness PRIVATE -Wall -Wextra $<$<BOOL:${SHMIPC_WERROR}>:-Werror>)

add_executable(shmipc_bench cases/spsc_bench.cpp)
target_link_libraries(shmipc_bench PRIVATE shmipc_bench_harness)
target_compile_options(shmipc_bench PRIVATE -Wall -Wextra $<$<BOOL:${SHMIPC_WERROR}>:-Werror>)

if(SHMIPC_BUILD_TESTS)
  shmipc_test(shmipc_bench_harness_tests unit 60 harness/harness_test.cpp)
  target_link_libraries(shmipc_bench_harness_tests PRIVATE shmipc_bench_harness)
endif()
