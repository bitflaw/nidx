cmake_minimum_required(VERSION 3.15)

project(nidx LANGUAGES C CXX)

set (CMAKE_CXX_STANDARD 23)
set (CMAKE_CXX_STANDARD_REQUIRED ON)
set (CMAKE_C_FLAGS_DEBUG     "-Wall -Wextra -pedantic -ggdb")
set (CMAKE_CXX_FLAGS_DEBUG   "-Wall -Wextra -pedantic -ggdb")
set (CMAKE_C_FLAGS_RELEASE   "-O3")
set (CMAKE_CXX_FLAGS_RELEASE "-O3")
set (CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE INTERNAL "")

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE "Debug")
endif()

option (TEST "build test" OFF)

set (SRCS
  src/nidx.cpp
  vendor/tree-sitter-nix-v0.3.0/parser.c
  vendor/tree-sitter-nix-v0.3.0/scanner.c
  vendor/tree-sitter-v0.26.9/src/lib.c
  vendor/simdjson-v4.6.1/simdjson.cpp
)

set (TSRCS
  src/test.cpp
  vendor/tree-sitter-nix-v0.3.0/parser.c
  vendor/tree-sitter-nix-v0.3.0/scanner.c
  vendor/tree-sitter-v0.26.9/src/lib.c
)

if (TEST)
  add_executable (
    test
    ${TSRCS}
  )
  target_include_directories (test
    PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor/tree-sitter-nix-v0.3.0
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor/tree-sitter-v0.26.9/include
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor/tree-sitter-v0.26.9/src
  )
endif ()

add_executable (
  nidx
  ${SRCS}
)

target_link_libraries (nidx
  PRIVATE
  SQLiteCpp
  sqlite3
  pthread
)

target_include_directories (nidx
  PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/vendor/tree-sitter-nix-v0.3.0
  ${CMAKE_CURRENT_SOURCE_DIR}/vendor/tree-sitter-v0.26.9/include
  ${CMAKE_CURRENT_SOURCE_DIR}/vendor/tree-sitter-v0.26.9/src
  ${CMAKE_CURRENT_SOURCE_DIR}/vendor/simdjson-v4.6.1
)
