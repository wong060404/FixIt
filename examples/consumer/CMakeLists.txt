# A minimal consumer of an *installed* FixIt, used to verify the package config.
#
# Against an install prefix produced by `cmake --install <build> --prefix <dir>`:
#
#   cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/fixit/install
#   cmake --build build
#   ./build/fixit-consumer
#
# The `FixIt::fixit` target carries the public headers, the static library and the
# vendored third-party dependencies, so nothing else has to be configured here.
cmake_minimum_required(VERSION 3.20)
project(fixit-consumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(FixIt CONFIG REQUIRED)

add_executable(fixit-consumer main.cpp)
target_link_libraries(fixit-consumer PRIVATE FixIt::fixit)
