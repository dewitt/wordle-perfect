cmake_minimum_required(VERSION 3.26)
project(wordle_perfect VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Export compile commands for tooling
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Default to Release if not specified
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Compiler flags
add_compile_options(
  -Wall -Wextra -Wpedantic
  $<$<CONFIG:Release>:-O3>
  $<$<CONFIG:Release>:-DNDEBUG>
  $<$<CONFIG:Debug>:-g>
  $<$<CONFIG:Debug>:-fsanitize=address>
  $<$<CONFIG:Debug>:-fsanitize=undefined>
)
add_link_options(
  $<$<CONFIG:Debug>:-fsanitize=address>
  $<$<CONFIG:Debug>:-fsanitize=undefined>
)

# Dependencies
find_package(SQLite3 REQUIRED)
find_package(Threads REQUIRED)

# ── Optional Metal GPU acceleration (Apple platforms only) ────────────────────
# On macOS we compile the Obj-C++ Metal scorer and define WP_HAVE_METAL so the
# code paths/benchmarks are available. Elsewhere everything builds without it.
set(WP_METAL_SOURCES)
set(WP_METAL_LIBS)
if(APPLE)
  find_library(METAL_FRAMEWORK Metal)
  find_library(FOUNDATION_FRAMEWORK Foundation)
  if(METAL_FRAMEWORK AND FOUNDATION_FRAMEWORK)
    message(STATUS "Metal GPU acceleration: ENABLED")
    list(APPEND WP_METAL_SOURCES src/gpu_score.mm)
    list(APPEND WP_METAL_LIBS ${METAL_FRAMEWORK} ${FOUNDATION_FRAMEWORK})
    set_source_files_properties(src/gpu_score.mm PROPERTIES
      COMPILE_OPTIONS "-fobjc-arc")
  endif()
endif()

# ── Core library (shared between main CLI and build tool) ─────────────────────
add_library(wordle_core STATIC
  src/pattern.cpp
  src/wordlist.cpp
  src/solver.cpp
  src/database.cpp
  src/binarydb.cpp
  ${WP_METAL_SOURCES}
)
target_include_directories(wordle_core PUBLIC src)
target_link_libraries(wordle_core PUBLIC SQLite::SQLite3 Threads::Threads ${WP_METAL_LIBS})
if(WP_METAL_SOURCES)
  target_compile_definitions(wordle_core PUBLIC WP_HAVE_METAL)
endif()

# ── Main CLI ──────────────────────────────────────────────────────────────────
add_executable(wordle src/main.cpp)
target_link_libraries(wordle PRIVATE wordle_core)

# ── Precomputation tool (the single decision-tree builder) ────────────────────
add_executable(build_db tools/build_db.cpp)
target_link_libraries(build_db PRIVATE wordle_core)

# ── GPU node-scoring benchmark (Metal; issue #12) ─────────────────────────────
add_executable(gpu_bench tools/gpu_bench.cpp)
target_link_libraries(gpu_bench PRIVATE wordle_core)

# ── Tests ─────────────────────────────────────────────────────────────────────
enable_testing()
find_package(Catch2 3 REQUIRED)

add_executable(wordle_tests
  tests/test_pattern.cpp
  tests/test_wordlist.cpp
  tests/test_solver.cpp
  tests/test_database.cpp
  tests/test_binarydb.cpp
)
target_link_libraries(wordle_tests PRIVATE wordle_core Catch2::Catch2WithMain)
# Run tests from build dir so data/ files are accessible at data/words.txt etc.
add_test(NAME wordle_tests COMMAND wordle_tests WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

# ── Install data alongside binaries ──────────────────────────────────────────
configure_file(
  ${CMAKE_SOURCE_DIR}/data/words.txt
  ${CMAKE_BINARY_DIR}/data/words.txt
  COPYONLY
)
configure_file(
  ${CMAKE_SOURCE_DIR}/data/answers.txt
  ${CMAKE_BINARY_DIR}/data/answers.txt
  COPYONLY
)
