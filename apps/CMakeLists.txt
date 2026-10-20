# Example applications. They reuse the benchmark harness for option parsing
# and histograms.
add_executable(itch itch/itch_main.cpp)
target_link_libraries(itch PRIVATE shmipc_bench_harness)
target_include_directories(itch PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/itch)

add_executable(logger logger/logger_main.cpp)
target_link_libraries(logger PRIVATE shmipc_bench_harness)
target_include_directories(logger PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/logger)

foreach(t itch logger)
  target_compile_options(${t} PRIVATE -Wall -Wextra $<$<BOOL:${SHMIPC_WERROR}>:-Werror>)
endforeach()

if(SHMIPC_BUILD_TESTS)
  shmipc_test(shmipc_apps_tests stress 600 ${PROJECT_SOURCE_DIR}/tests/apps/apps_e2e_test.cpp)
  target_link_libraries(shmipc_apps_tests PRIVATE shmipc_bench_harness)
  target_include_directories(shmipc_apps_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/itch ${CMAKE_CURRENT_SOURCE_DIR}/logger)
endif()
