cmake_minimum_required(VERSION 3.20)

project(fixit
        VERSION 0.1.0
        DESCRIPTION "Fuzzy-patch agentic C++ repair loop"
        LANGUAGES C CXX)

include(GNUInstallDirs)

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
option(FIXIT_ENABLE_OPENSSL "Link cpp-httplib against a system OpenSSL" OFF)
option(FIXIT_MACOS_KEYCHAIN_CERTS "Use the macOS Keychain for CA certificates" ON)

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
  src/mock_llm.cpp
  src/platform.cpp)

target_include_directories(fixit PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(fixit PUBLIC fixit_third_party)

# Hard quality gate for *our* code only (third-party headers are SYSTEM).
if(MSVC)
  target_compile_options(fixit PRIVATE /W4 /WX)
else()
  target_compile_options(fixit PRIVATE -Wall -Wextra -Werror)
endif()

target_compile_definitions(fixit PUBLIC FIXIT_VERSION="${PROJECT_VERSION}")

# When cpp-httplib is built with OpenSSL on macOS it loads the trust store from
# the Keychain, which needs these two frameworks.  They are attached here, to the
# targets that actually link, rather than only to the INTERFACE library: a static
# library does not propagate a framework requirement to the final executable on
# its own.
if(APPLE AND FIXIT_ENABLE_OPENSSL AND FIXIT_MACOS_KEYCHAIN_CERTS)
  find_library(FIXIT_COREFOUNDATION CoreFoundation REQUIRED)
  find_library(FIXIT_SECURITY Security REQUIRED)
  target_link_libraries(fixit PUBLIC "${FIXIT_COREFOUNDATION}" "${FIXIT_SECURITY}")
endif()

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
# Tooling: the patch-tolerance matrix behind the Wiki heat map
# ---------------------------------------------------------------------------
option(FIXIT_BUILD_TOOLS "Build the measurement tools (patch success matrix)" ON)
if(FIXIT_BUILD_TOOLS)
  add_executable(fixit-matrix tools/matrix.cpp)
  target_link_libraries(fixit-matrix PRIVATE fixit)
  if(NOT MSVC)
    target_compile_options(fixit-matrix PRIVATE -Wall -Wextra -Werror)
  endif()
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(FIXIT_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# Install / export
#
# Lets another CMake project consume the library:
#
#   find_package(FixIt CONFIG REQUIRED)
#   target_link_libraries(myapp PRIVATE FixIt::fixit)
#
# Only the vendored snapshot is exportable: a FetchContent build links external
# targets that have no install location, so the rules are skipped there.
# ---------------------------------------------------------------------------
if(NOT TARGET tree-sitter)
  include(CMakePackageConfigHelpers)

  install(TARGETS fixit fixit_third_party fixit_tree_sitter fixit_tree_sitter_cpp
                  fixit_json fixit_httplib
          EXPORT FixItTargets
          ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
          LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
          RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
          INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

  # The project's own public headers.
  install(DIRECTORY include/fixit DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

  # Third-party headers the public API needs: agent.h exposes <nlohmann/json.hpp>.
  install(DIRECTORY third_party/json/include/nlohmann
          DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/fixit/third_party/json)
  install(DIRECTORY third_party/tree-sitter/lib/include/tree_sitter
          DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/fixit/third_party/tree-sitter)
  install(FILES third_party/cpp-httplib/httplib.h
          DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/fixit/third_party/cpp-httplib)

  install(EXPORT FixItTargets
          FILE FixItTargets.cmake
          NAMESPACE FixIt::
          DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/FixIt)

  configure_package_config_file(cmake/FixItConfig.cmake.in
          ${CMAKE_CURRENT_BINARY_DIR}/FixItConfig.cmake
          INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/FixIt)
  write_basic_package_version_file(
          ${CMAKE_CURRENT_BINARY_DIR}/FixItConfigVersion.cmake
          VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion)
  install(FILES ${CMAKE_CURRENT_BINARY_DIR}/FixItConfig.cmake
                ${CMAKE_CURRENT_BINARY_DIR}/FixItConfigVersion.cmake
          DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/FixIt)
endif()
