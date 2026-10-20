cmake_minimum_required(VERSION 3.25)

if(APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
  # os_sync_wait_on_address (cross-process wait/wake) first shipped in macOS 14.4.
  set(CMAKE_OSX_DEPLOYMENT_TARGET "14.4" CACHE STRING "Minimum macOS version")
endif()

project(shmipc VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(SHMIPC_BUILD_TESTS "Build the test suites" ${PROJECT_IS_TOP_LEVEL})
option(SHMIPC_WERROR "Treat warnings as errors" ${PROJECT_IS_TOP_LEVEL})
set(SHMIPC_SANITIZER "" CACHE STRING "Sanitizers to enable: '', 'address,undefined' or 'thread'")
# Bit mask of ordering sites (see include/shmipc/platform/ordering.hpp) that are
# deliberately weakened to memory_order_relaxed. Only for the bug-injection experiment.
set(SHMIPC_WEAKEN_ORDERING "0" CACHE STRING "Bit mask of ordering sites to weaken (bug injection)")

if(SHMIPC_SANITIZER)
  add_compile_options(-fsanitize=${SHMIPC_SANITIZER} -fno-omit-frame-pointer -fno-sanitize-recover=all)
  add_link_options(-fsanitize=${SHMIPC_SANITIZER})
endif()

add_library(shmipc
  src/platform/affinity.cpp
  src/platform/clock.cpp
  src/platform/memory.cpp
  src/platform/wait_on_address.cpp
)
add_library(shmipc::shmipc ALIAS shmipc)
target_include_directories(shmipc PUBLIC
  $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)
target_compile_features(shmipc PUBLIC cxx_std_20)
target_compile_definitions(shmipc PUBLIC SHMIPC_WEAKEN_ORDERING=${SHMIPC_WEAKEN_ORDERING})
target_compile_options(shmipc PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow
  $<$<BOOL:${SHMIPC_WERROR}>:-Werror>)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  find_package(Threads REQUIRED)
  target_link_libraries(shmipc PUBLIC Threads::Threads rt)
endif()

if(SHMIPC_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
