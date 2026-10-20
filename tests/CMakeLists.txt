include(FetchContent)
include(GoogleTest)

FetchContent_Declare(googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
  URL_HASH SHA256=7b42b4d6ed48810c5362c265a17faebe90dc2373c885e5216439d37927f02926
  DOWNLOAD_EXTRACT_TIMESTAMP ON
  SYSTEM
)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)
# Third-party code: its own warnings are not ours to fix.
target_compile_options(gtest PRIVATE -w)
target_compile_options(gtest_main PRIVATE -w)

add_executable(shmipc_unit_tests
  unit/cpu_test.cpp
  unit/clock_test.cpp
  unit/affinity_test.cpp
  unit/memory_test.cpp
  unit/wait_on_address_test.cpp
)
target_link_libraries(shmipc_unit_tests PRIVATE shmipc::shmipc GTest::gtest_main)
target_compile_options(shmipc_unit_tests PRIVATE -Wall -Wextra $<$<BOOL:${SHMIPC_WERROR}>:-Werror>)
gtest_discover_tests(shmipc_unit_tests PROPERTIES LABELS unit DISCOVERY_MODE PRE_TEST)
