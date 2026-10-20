include(FetchContent)
include(GoogleTest)

FetchContent_Declare(googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
  URL_HASH SHA256=7b42b4d6ed48810c5362c265a17faebe90dc2373c885e5216439d37927f02926
  DOWNLOAD_EXTRACT_TIMESTAMP ON
  SYSTEM
)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)

# RapidCheck has no releases; pin a commit.
FetchContent_Declare(rapidcheck
  URL https://github.com/emil-e/rapidcheck/archive/2c3c4365aca21ef4e612768fdc95b1ce8b39a651.tar.gz
  URL_HASH SHA256=d7c87d2a728b9cd7adec09aed44f34938cd1b0928b373f9b490c5b2f6d5ab9dc
  DOWNLOAD_EXTRACT_TIMESTAMP ON
  SYSTEM
)
set(RC_ENABLE_GTEST ON CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(googletest rapidcheck)
# Third-party code: its own warnings are not ours to fix.
foreach(dep gtest gtest_main rapidcheck)
  target_compile_options(${dep} PRIVATE -w)
endforeach()

function(shmipc_test name label timeout)
  add_executable(${name} ${ARGN})
  target_link_libraries(${name} PRIVATE shmipc::shmipc GTest::gtest_main)
  target_compile_options(${name} PRIVATE -Wall -Wextra $<$<BOOL:${SHMIPC_WERROR}>:-Werror>)
  gtest_discover_tests(${name} PROPERTIES LABELS ${label} TIMEOUT ${timeout} DISCOVERY_MODE PRE_TEST)
endfunction()

shmipc_test(shmipc_unit_tests unit 60
  unit/affinity_test.cpp
  unit/broadcast_test.cpp
  unit/clock_test.cpp
  unit/cpu_test.cpp
  unit/liveness_test.cpp
  unit/memory_test.cpp
  unit/mpsc_test.cpp
  unit/process_test.cpp
  unit/record_test.cpp
  unit/region_test.cpp
  unit/segment_test.cpp
  unit/seqlock_test.cpp
  unit/spsc_byte_ring_test.cpp
  unit/spsc_queue_test.cpp
  unit/wait_on_address_test.cpp
  unit/wait_strategy_test.cpp
)

shmipc_test(shmipc_property_tests property 300
  property/broadcast_property_test.cpp
  property/mpsc_property_test.cpp
  property/spsc_property_test.cpp
)
target_link_libraries(shmipc_property_tests PRIVATE rapidcheck_gtest)

shmipc_test(shmipc_stress_tests stress 600
  stress/broadcast_stress_test.cpp
  stress/mpsc_stress_test.cpp
  stress/spsc_stress_test.cpp
)

# Crash-recovery tests. SHMIPC_FAULT_INJECTION turns the SHMIPC_FAULT_POINT
# markers in the (header-only) channels into hooks that a child process can use
# to die at an exact protocol step.
shmipc_test(shmipc_fault_tests unit 120
  fault/mpsc_unblock_test.cpp
)
target_compile_definitions(shmipc_fault_tests PRIVATE SHMIPC_FAULT_INJECTION)

shmipc_test(shmipc_chaos_tests chaos 900
  chaos/chaos_test.cpp
)
target_compile_definitions(shmipc_chaos_tests PRIVATE SHMIPC_FAULT_INJECTION)
