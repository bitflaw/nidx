cmake_minimum_required(VERSION 3.15)

project("nidx" LANGUAGES CXX)

set (CMAKE_CXX_STANDARD 23)
set (CMAKE_CXX_STANDARD_REQUIRED ON)
set (CMAKE_CXX_FLAGS_DEBUG   "-Wall -Wextra -pedantic -ggdb")
set (CMAKE_CXX_FLAGS_RELEASE "-03")
set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE INTERNAL "")

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE "Debug")
endif()

set (SRCS
  src/nidx.cpp
  src/simdjson.cpp
)

add_executable (
  nidx
  ${SRCS}
)

target_link_libraries (nidx
  PRIVATE
  SQLiteCpp
  sqlite3
  pthread
  # dl
)

target_include_directories (nidx PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
