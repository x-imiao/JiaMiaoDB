cmake_minimum_required(VERSION 3.14)
project(jiamiaodb VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} --std=c++17 -Wall -Wextra -O2 -pthread")

# Dependencies
find_package(CURL REQUIRED)

# Source files (all .cpp under src/, excluding main.cpp)
file(GLOB_RECURSE ALL_SOURCES src/*.cpp)
list(REMOVE_ITEM ALL_SOURCES "${CMAKE_SOURCE_DIR}/src/main.cpp")

# Static library — shared by main executable and tests
add_library(jiamiaodb_lib STATIC ${ALL_SOURCES})
target_include_directories(jiamiaodb_lib PUBLIC
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/vendor
)
target_link_libraries(jiamiaodb_lib PUBLIC CURL::libcurl)

# Main executable
add_executable(jiamiaodb src/main.cpp)
target_link_libraries(jiamiaodb PRIVATE jiamiaodb_lib)

# Install
install(TARGETS jiamiaodb DESTINATION bin)

# ── Tests ───────────────────────────────────────────────────
option(BUILD_TESTS "Build unit tests" ON)
if(BUILD_TESTS)
    enable_testing()
    file(GLOB TEST_SOURCES tests/*.cpp)
    add_executable(jiamiaodb_test ${TEST_SOURCES})
    target_link_libraries(jiamiaodb_test PRIVATE jiamiaodb_lib)
    add_test(NAME jiamiaodb_test COMMAND jiamiaodb_test)
endif()
