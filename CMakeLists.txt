cmake_minimum_required(VERSION 3.24)

if(POLICY CMP0135)
  cmake_policy(SET CMP0135 NEW)
endif()
if(POLICY CMP0169)
  cmake_policy(SET CMP0169 OLD)
endif()

project(BoltStream VERSION 0.1.0 LANGUAGES CXX)

option(BOLTSTREAM_WARNINGS_AS_ERRORS "Treat BoltStream warnings as errors" ON)
option(BOLTSTREAM_BUILD_TESTS "Build BoltStream tests" ON)
option(BOLTSTREAM_BUILD_BENCHMARKS "Build BoltStream benchmarks" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(FetchContent)
include(GNUInstallDirs)
find_package(Threads REQUIRED)

function(boltstream_git_sha out_var)
  if(DEFINED ENV{BOLTSTREAM_GIT_SHA} AND NOT "$ENV{BOLTSTREAM_GIT_SHA}" STREQUAL "")
    set(${out_var} "$ENV{BOLTSTREAM_GIT_SHA}" PARENT_SCOPE)
    return()
  endif()

  execute_process(
    COMMAND git rev-parse --short=12 HEAD
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    OUTPUT_VARIABLE git_sha
    ERROR_QUIET
    OUTPUT_STRIP_TRAILING_WHITESPACE)

  if(git_sha STREQUAL "")
    set(git_sha "unknown")
  endif()
  set(${out_var} "${git_sha}" PARENT_SCOPE)
endfunction()

boltstream_git_sha(BOLTSTREAM_GIT_SHA)
if(NOT CMAKE_BUILD_TYPE)
  set(BOLTSTREAM_BUILD_TYPE "Unknown")
else()
  set(BOLTSTREAM_BUILD_TYPE "${CMAKE_BUILD_TYPE}")
endif()
set(BOLTSTREAM_COMPILER
    "${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")

configure_file(
  "${CMAKE_SOURCE_DIR}/cmake/build_config.h.in"
  "${CMAKE_BINARY_DIR}/generated/boltstream/build_config.h"
  @ONLY)

FetchContent_Declare(
  boost_headers
  URL https://archives.boost.io/release/1.91.0/source/boost_1_91_0.tar.bz2
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_GetProperties(boost_headers)
if(NOT boost_headers_POPULATED)
  FetchContent_Populate(boost_headers)
endif()

set(YAML_CPP_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  yaml_cpp
  URL https://github.com/jbeder/yaml-cpp/archive/refs/tags/yaml-cpp-0.9.0.tar.gz
  URL_HASH SHA256=25cb043240f828a8c51beb830569634bc7ac603978e0f69d6b63558dadefd49a
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(yaml_cpp)

set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(ZSTD_BUILD_DICTBUILDER OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_DEPRECATED OFF CACHE BOOL "" FORCE)
set(ZSTD_LEGACY_SUPPORT OFF CACHE BOOL "" FORCE)
set(ZSTD_MULTITHREAD_SUPPORT OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  zstd
  URL https://github.com/facebook/zstd/releases/download/v1.5.7/zstd-1.5.7.tar.gz
  URL_HASH SHA256=eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  SOURCE_SUBDIR build/cmake)
FetchContent_MakeAvailable(zstd)

add_library(boost_headers INTERFACE)
target_include_directories(boost_headers SYSTEM INTERFACE "${boost_headers_SOURCE_DIR}")
target_compile_definitions(boost_headers INTERFACE BOOST_ASIO_HEADER_ONLY BOOST_ASIO_NO_DEPRECATED)
if(WIN32)
  target_compile_definitions(boost_headers INTERFACE _WIN32_WINNT=0x0A00)
endif()

add_library(boltstream_warnings INTERFACE)
if(MSVC)
  target_compile_options(boltstream_warnings INTERFACE /W4)
  if(BOLTSTREAM_WARNINGS_AS_ERRORS)
    target_compile_options(boltstream_warnings INTERFACE /WX)
  endif()
else()
  target_compile_options(boltstream_warnings INTERFACE -Wall -Wextra -Wpedantic)
  if(BOLTSTREAM_WARNINGS_AS_ERRORS)
    target_compile_options(boltstream_warnings INTERFACE -Werror)
    target_compile_options(boltstream_warnings INTERFACE
                           $<$<CXX_COMPILER_ID:Clang>:-Wno-error=c2y-extensions>)
  endif()
endif()

add_library(boltstream_core
  src/benchmark/statistics.cpp
  src/broker/group_coordinator.cpp
  src/broker/options.cpp
  src/broker/server.cpp
  src/client/async_client.cpp
  src/common/build_info.cpp
  src/compression/compression.cpp
  src/config/config.cpp
  src/observability/logger.cpp
  src/observability/metrics.cpp
  src/protocol/protocol.cpp
  src/replication/simulation.cpp
  src/storage/offset_store.cpp
  src/storage/partition_log.cpp)

target_include_directories(boltstream_core
  PUBLIC
    "${CMAKE_SOURCE_DIR}/include"
    "${CMAKE_BINARY_DIR}/generated")
target_link_libraries(boltstream_core
  PUBLIC
    boost_headers
    Threads::Threads
  PRIVATE
    boltstream_warnings
    libzstd_static
    yaml-cpp::yaml-cpp)
if(WIN32)
  target_link_libraries(boltstream_core PUBLIC ws2_32 mswsock)
endif()

add_executable(boltstream-server src/tools/server_main.cpp)
target_link_libraries(boltstream-server PRIVATE boltstream_core boltstream_warnings)

add_executable(boltstream-producer tools/producer_cli.cpp)
target_link_libraries(boltstream-producer PRIVATE boltstream_core boltstream_warnings)

add_executable(boltstream-consumer tools/consumer_cli.cpp)
target_link_libraries(boltstream-consumer PRIVATE boltstream_core boltstream_warnings)

add_executable(boltstream-bench tools/bench_cli.cpp)
target_link_libraries(boltstream-bench PRIVATE boltstream_core boltstream_warnings)

add_executable(boltstream-logtool tools/logtool_cli.cpp)
target_link_libraries(boltstream-logtool PRIVATE boltstream_core boltstream_warnings)

add_executable(boltstream-admin tools/admin_cli.cpp)
target_link_libraries(boltstream-admin PRIVATE boltstream_core boltstream_warnings)

add_executable(boltstream-replication-sim tools/replication_sim_cli.cpp)
target_link_libraries(boltstream-replication-sim PRIVATE boltstream_core boltstream_warnings)

install(TARGETS
  boltstream-server
  boltstream-producer
  boltstream-consumer
  boltstream-bench
  boltstream-admin
  boltstream-replication-sim
  boltstream-logtool
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(FILES config/boltstream.example.yaml DESTINATION "${CMAKE_INSTALL_SYSCONFDIR}/boltstream")

include(CTest)
if(BOLTSTREAM_BUILD_TESTS AND BUILD_TESTING)
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.17.0.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(googletest)

  add_executable(boltstream_tests
    tests/benchmark_statistics_tests.cpp
    tests/build_info_tests.cpp
    tests/compression_tests.cpp
    tests/client_broker_tests.cpp
    tests/config_tests.cpp
    tests/group_coordinator_tests.cpp
    tests/metrics_tests.cpp
    tests/options_tests.cpp
    tests/protocol_tests.cpp
    tests/replication_simulation_tests.cpp
    tests/storage_tests.cpp)
  target_link_libraries(boltstream_tests PRIVATE boltstream_core GTest::gtest_main boltstream_warnings)
  include(GoogleTest)
  gtest_discover_tests(boltstream_tests)
endif()

if(BOLTSTREAM_BUILD_BENCHMARKS)
  set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_WERROR OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(
    googlebenchmark
    URL https://github.com/google/benchmark/archive/refs/tags/v1.9.4.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(googlebenchmark)

  add_executable(boltstream-microbench benchmarks/microbench.cpp)
  if(TARGET benchmark)
    target_compile_options(benchmark PRIVATE $<$<CXX_COMPILER_ID:Clang>:-Wno-c2y-extensions>)
  endif()
  if(TARGET benchmark_main)
    target_compile_options(benchmark_main PRIVATE $<$<CXX_COMPILER_ID:Clang>:-Wno-c2y-extensions>)
  endif()
  target_compile_options(boltstream-microbench
                         PRIVATE $<$<CXX_COMPILER_ID:Clang>:-Wno-c2y-extensions>)
  target_link_libraries(boltstream-microbench
                        PRIVATE benchmark::benchmark boltstream_core boltstream_warnings)
  install(TARGETS boltstream-microbench RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
endif()
