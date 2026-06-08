cmake_minimum_required(VERSION 3.14)
project(jiamiaodb VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -O2 -pthread")

# Dependencies
find_package(CURL REQUIRED)

# Source files
file(GLOB_RECURSE SOURCES src/*.cpp)

add_executable(jiamiaodb ${SOURCES})
target_include_directories(jiamiaodb PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/vendor
)
target_link_libraries(jiamiaodb PRIVATE CURL::libcurl)

# Install
install(TARGETS jiamiaodb DESTINATION bin)
