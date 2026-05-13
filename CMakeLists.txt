cmake_minimum_required(VERSION 3.20)
project(mdfeed_itch CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

add_compile_options(
  -Wall -Wextra -Wpedantic
  -Wshadow -Wconversion -Wsign-conversion
  -Wno-unused-parameter
  -fno-omit-frame-pointer
)

option(MDFEED_ITCH_ASAN "Build with AddressSanitizer" OFF)
option(MDFEED_ITCH_UBSAN "Build with UndefinedBehaviorSanitizer" OFF)
option(MDFEED_ITCH_FUZZ "Build libFuzzer targets" OFF)
option(MDFEED_ITCH_BENCH "Build benchmark binary" ON)

if(MDFEED_ITCH_ASAN)
  add_compile_options(-fsanitize=address)
  add_link_options(-fsanitize=address)
endif()
if(MDFEED_ITCH_UBSAN)
  add_compile_options(-fsanitize=undefined -fno-sanitize-recover=undefined)
  add_link_options(-fsanitize=undefined)
endif()

include(FetchContent)
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.15.2
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_library(mdfeed_itch_core STATIC
  src/core/parser.cpp
  src/core/depth_book.cpp
  src/core/feed_handler.cpp
  src/net/multicast.cpp
  src/net/tcp_recovery.cpp
  src/recovery/gap_detector.cpp
  src/recovery/snapshot_client.cpp
  src/sim/publisher.cpp
  src/sim/snapshot_server.cpp
  src/obs/histogram.cpp
  src/obs/clock.cpp
)
target_include_directories(mdfeed_itch_core PUBLIC src)

add_executable(mdfeed_itch src/main.cpp)
target_link_libraries(mdfeed_itch PRIVATE mdfeed_itch_core)

enable_testing()

function(mdfeed_add_test name)
  add_executable(${name} ${ARGN})
  target_link_libraries(${name} PRIVATE mdfeed_itch_core GTest::gtest_main)
  add_test(NAME ${name} COMMAND ${name})
endfunction()

mdfeed_add_test(parser_test          tests/unit/parser_test.cpp)
mdfeed_add_test(depth_book_test      tests/unit/depth_book_test.cpp)
mdfeed_add_test(gap_detector_test    tests/unit/gap_detector_test.cpp)
mdfeed_add_test(snapshot_client_test tests/unit/snapshot_client_test.cpp)
mdfeed_add_test(multicast_test       tests/integration/multicast_test.cpp)
mdfeed_add_test(gap_fill_test        tests/integration/gap_fill_test.cpp)

if(MDFEED_ITCH_FUZZ)
  add_executable(parser_fuzz tests/fuzz/parser_fuzz.cpp)
  target_link_libraries(parser_fuzz PRIVATE mdfeed_itch_core)
  target_compile_options(parser_fuzz PRIVATE -fsanitize=fuzzer,address)
  target_link_options(parser_fuzz PRIVATE -fsanitize=fuzzer,address)
endif()

if(MDFEED_ITCH_BENCH)
  add_executable(handler_bench bench/handler_bench.cpp)
  target_link_libraries(handler_bench PRIVATE mdfeed_itch_core)
endif()
