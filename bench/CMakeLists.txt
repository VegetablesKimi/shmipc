include(FetchContent)

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

# Competitors for the comparison benchmarks: header-only libraries fetched
# from pinned releases (not built: SOURCE_SUBDIR points at nothing), plus
# Boost.Lockfree when Boost is installed (apt: libboost-dev; brew: boost).
option(SHMIPC_BENCH_COMPETITORS "Benchmark against rigtorp, moodycamel and Boost.Lockfree" ON)
if(SHMIPC_BENCH_COMPETITORS)
  FetchContent_Declare(rigtorp_spsc
    URL https://github.com/rigtorp/SPSCQueue/archive/refs/tags/v1.1.tar.gz
    URL_HASH SHA256=10f290c72e60ba0d7999c65926f96fab0b1c2f360ce5bbcfe0cf69448d067af5
    DOWNLOAD_EXTRACT_TIMESTAMP ON SOURCE_SUBDIR none)
  FetchContent_Declare(moodycamel_rwq
    URL https://github.com/cameron314/readerwriterqueue/archive/refs/tags/v1.0.7.tar.gz
    URL_HASH SHA256=532224ed052bcd5f4c6be0ed9bb2b8c88dfe7e26e3eb4dd9335303b059df6691
    DOWNLOAD_EXTRACT_TIMESTAMP ON SOURCE_SUBDIR none)
  FetchContent_Declare(moodycamel_cq
    URL https://github.com/cameron314/concurrentqueue/archive/refs/tags/v1.0.5.tar.gz
    URL_HASH SHA256=4d6368a27492d86011fde5ca0cf386dce7c49cd425aa3d9b063ca6ec373a6ef3
    DOWNLOAD_EXTRACT_TIMESTAMP ON SOURCE_SUBDIR none)
  FetchContent_MakeAvailable(rigtorp_spsc moodycamel_rwq moodycamel_cq)
  add_library(shmipc_competitors INTERFACE)
  target_include_directories(shmipc_competitors SYSTEM INTERFACE
    ${rigtorp_spsc_SOURCE_DIR}/include ${moodycamel_rwq_SOURCE_DIR} ${moodycamel_cq_SOURCE_DIR})
  target_compile_definitions(shmipc_competitors INTERFACE SHMIPC_HAVE_RIGTORP=1 SHMIPC_HAVE_MOODYCAMEL=1)
  find_package(Boost 1.74 QUIET CONFIG)  # BoostConfig.cmake (Boost >= 1.70); FindBoost is deprecated
  if(Boost_FOUND)
    target_link_libraries(shmipc_competitors INTERFACE Boost::headers)
    target_compile_definitions(shmipc_competitors INTERFACE SHMIPC_HAVE_BOOST_LOCKFREE=1)
  else()
    message(STATUS "Boost not found: Boost.Lockfree comparison disabled")
  endif()
endif()

# Aeron (C client + C media driver), opt-in: it is a sizeable build.
#   cmake --preset release -DSHMIPC_BENCH_AERON=ON
option(SHMIPC_BENCH_AERON "Benchmark against Aeron IPC (builds Aeron from source)" OFF)
if(SHMIPC_BENCH_AERON)
  set(BUILD_AERON_DRIVER ON CACHE BOOL "" FORCE)
  set(BUILD_AERON_ARCHIVE_API OFF CACHE BOOL "" FORCE)  # the only part that needs Java
  set(AERON_TESTS OFF CACHE BOOL "" FORCE)
  set(AERON_UNIT_TESTS OFF CACHE BOOL "" FORCE)
  set(AERON_SYSTEM_TESTS OFF CACHE BOOL "" FORCE)
  set(AERON_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
  set(AERON_BUILD_DOCUMENTATION OFF CACHE BOOL "" FORCE)
  set(AERON_INSTALL_TARGETS OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(aeron
    URL https://github.com/real-logic/aeron/archive/refs/tags/1.53.3.tar.gz
    URL_HASH SHA256=b7861c4aa9bd4918c0c3cb3b83b4a631cf48c1403e496008ccbcb950a121f125
    DOWNLOAD_EXTRACT_TIMESTAMP ON)
  FetchContent_MakeAvailable(aeron)
  add_library(shmipc_aeron INTERFACE)
  target_link_libraries(shmipc_aeron INTERFACE aeron_static)
  target_include_directories(shmipc_aeron SYSTEM INTERFACE ${aeron_SOURCE_DIR}/aeron-client/src/main/c)
  target_compile_definitions(shmipc_aeron INTERFACE SHMIPC_HAVE_AERON=1
    SHMIPC_AERONMD_PATH="$<TARGET_FILE:aeronmd>")
endif()

add_library(shmipc_bench_harness
  harness/env.cpp
  harness/histogram.cpp
  harness/options.cpp
)
target_include_directories(shmipc_bench_harness PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(shmipc_bench_harness PUBLIC shmipc::shmipc hdr_histogram_static)
target_compile_options(shmipc_bench_harness PRIVATE -Wall -Wextra $<$<BOOL:${SHMIPC_WERROR}>:-Werror>)

add_executable(shmipc_bench cases/spsc_bench.cpp cases/wait_bench.cpp)
target_link_libraries(shmipc_bench PRIVATE shmipc_bench_harness $<TARGET_NAME_IF_EXISTS:shmipc_competitors>
  $<TARGET_NAME_IF_EXISTS:shmipc_aeron>)
if(SHMIPC_BENCH_AERON)
  add_dependencies(shmipc_bench aeronmd)  # launched by the benchmark (cases/aeron_transport.hpp)
endif()
target_compile_options(shmipc_bench PRIVATE -Wall -Wextra $<$<BOOL:${SHMIPC_WERROR}>:-Werror>)

if(SHMIPC_BUILD_TESTS)
  shmipc_test(shmipc_bench_harness_tests unit 60 harness/harness_test.cpp)
  target_link_libraries(shmipc_bench_harness_tests PRIVATE shmipc_bench_harness)
endif()
