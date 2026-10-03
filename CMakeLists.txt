cmake_minimum_required(VERSION 3.20)

project(fixit
        VERSION 0.1.0
        DESCRIPTION "Fuzzy-patch agentic C++ repair loop"
        LANGUAGES C CXX)

# ---------------------------------------------------------------------------
# Global build settings
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(FIXIT_BUILD_TESTS "Build the Catch2 test suite" ON)
option(FIXIT_BUILD_CLI "Build the fixit command line tool" ON)
option(FIXIT_FETCH_DEPS
       "Force downloading dependencies with FetchContent even when third_party/ is populated"
       OFF)

if(NOT CMAKE_RUNTIME_OUTPUT_DIRECTORY)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
endif()

# ---------------------------------------------------------------------------
# Dependencies -- vendored snapshot in third_party/, FetchContent as fallback
# ---------------------------------------------------------------------------
include(cmake/ThirdParty.cmake)

# ---------------------------------------------------------------------------
# Library
# ---------------------------------------------------------------------------
add_library(fixit
  src/compiler.cpp
  src/codemap.cpp
  src/patch.cpp
  src/agent.cpp
  src/openai_llm.cpp
  src/mock_llm.cpp)

target_include_directories(fixit PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(fixit PUBLIC fixit_third_party)

# Hard quality gate for *our* code only (third-party headers are SYSTEM).
if(MSVC)
  target_compile_options(fixit PRIVATE /W4 /WX)
else()
  target_compile_options(fixit PRIVATE -Wall -Wextra -Werror)
endif()

target_compile_definitions(fixit PUBLIC FIXIT_VERSION="${PROJECT_VERSION}")

# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------
if(FIXIT_BUILD_CLI)
  add_executable(fixit-cli tools/fixit-cli/main.cpp)
  target_link_libraries(fixit-cli PRIVATE fixit)
  set_target_properties(fixit-cli PROPERTIES OUTPUT_NAME fixit)
  if(NOT MSVC)
    target_compile_options(fixit-cli PRIVATE -Wall -Wextra -Werror)
  endif()
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(FIXIT_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
