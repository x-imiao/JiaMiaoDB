cmake_minimum_required(VERSION 3.14)
project(jmdb VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} --std=c++17 -Wall -Wextra -O2 -pthread")

# Dependencies
find_package(CURL REQUIRED)

# Source files (all .cpp under src/, excluding main.cpp)
file(GLOB_RECURSE ALL_SOURCES src/*.cpp)
list(REMOVE_ITEM ALL_SOURCES "${CMAKE_SOURCE_DIR}/src/main.cpp")

# Static library — shared by main executable and tests
add_library(jmdb_lib STATIC ${ALL_SOURCES})
target_include_directories(jmdb_lib PUBLIC
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/vendor
)
target_link_libraries(jmdb_lib PUBLIC CURL::libcurl)

# Main executable
add_executable(JMDB src/main.cpp)
target_link_libraries(JMDB PRIVATE jmdb_lib)

# Install
install(TARGETS JMDB DESTINATION bin)

# ── Tests ───────────────────────────────────────────────────
option(BUILD_TESTS "Build unit tests" ON)
if(BUILD_TESTS)
    enable_testing()
    file(GLOB TEST_SOURCES tests/*.cpp)
    add_executable(JMDB_test ${TEST_SOURCES})
    target_link_libraries(JMDB_test PRIVATE jmdb_lib)
    add_test(NAME JMDB_test COMMAND JMDB_test)
endif()
