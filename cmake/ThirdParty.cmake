# ---------------------------------------------------------------------------
# Dependency wiring.
#
# By default the pinned snapshot under third_party/ is used, which keeps the
# build fully reproducible and offline.  If a component is missing (for example
# in a fresh clone that was not made with --recursive, or when FIXIT_FETCH_DEPS
# is ON) FetchContent pulls the exact same tag from upstream.
# ---------------------------------------------------------------------------
include(FetchContent)

# FetchContent creates the target itself; in that case we only alias it.
set(FIXIT_FETCHED_TARGETS "")

function(fixit_fetch NAME REPO TAG)
  set(_dir "${CMAKE_CURRENT_SOURCE_DIR}/third_party/${NAME}")
  if(FIXIT_FETCH_DEPS OR NOT EXISTS "${_dir}")
    message(STATUS "fixit: fetching ${NAME} @ ${TAG}")
    FetchContent_Declare(${NAME}
      GIT_REPOSITORY "${REPO}"
      GIT_TAG "${TAG}"
      GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(${NAME})
    set(FIXIT_FETCHED_TARGETS "${FIXIT_FETCHED_TARGETS};${NAME}" PARENT_SCOPE)
  else()
    message(STATUS "fixit: using vendored ${NAME}")
  endif()
endfunction()

fixit_fetch(tree-sitter     https://github.com/tree-sitter/tree-sitter.git     v0.26.13)
fixit_fetch(tree-sitter-cpp https://github.com/tree-sitter/tree-sitter-cpp.git v0.23.4)
fixit_fetch(json            https://github.com/nlohmann/json.git               v3.12.0)
fixit_fetch(cpp-httplib     https://github.com/yhirose/cpp-httplib.git         v0.28.0)
if(FIXIT_BUILD_TESTS)
  fixit_fetch(Catch2        https://github.com/catchorg/Catch2.git             v3.8.1)
endif()

# ---------------------------------------------------------------------------
# Normalise into one INTERFACE target so consumers never see third-party
# compile warnings (SYSTEM include dirs) and link orders stay simple.
# ---------------------------------------------------------------------------
add_library(fixit_third_party INTERFACE)

# --- tree-sitter runtime -----------------------------------------------------
if(TARGET tree-sitter)
  target_link_libraries(fixit_third_party INTERFACE tree-sitter)
else()
  # Vendored snapshot: build the runtime ourselves so that the third-party C
  # code is never compiled with our -Werror flags.
  file(GLOB _ts_sources "${CMAKE_CURRENT_SOURCE_DIR}/third_party/tree-sitter/lib/src/*.c")
  list(REMOVE_ITEM _ts_sources
       "${CMAKE_CURRENT_SOURCE_DIR}/third_party/tree-sitter/lib/src/lib.c")
  add_library(fixit_tree_sitter STATIC ${_ts_sources})
  target_include_directories(fixit_tree_sitter
    SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/third_party/tree-sitter/lib/include"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/tree-sitter/lib/src")
  target_compile_definitions(fixit_tree_sitter PRIVATE
    _POSIX_C_SOURCE=200112L _DEFAULT_SOURCE _DARWIN_C_SOURCE)
  set_target_properties(fixit_tree_sitter PROPERTIES C_VISIBILITY_PRESET hidden)
  if(NOT BUILD_SHARED_LIBS)
    target_link_libraries(fixit_tree_sitter INTERFACE $<$<NOT:$<PLATFORM_ID:Windows>>:m>)
    if(NOT APPLE)
      target_link_libraries(fixit_tree_sitter INTERFACE dl pthread)
    endif()
  endif()
  target_link_libraries(fixit_third_party INTERFACE fixit_tree_sitter)
endif()

# --- tree-sitter-cpp grammar ------------------------------------------------
if(TARGET tree-sitter-cpp)
  target_link_libraries(fixit_third_party INTERFACE tree-sitter-cpp)
else()
  add_library(fixit_tree_sitter_cpp STATIC
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/tree-sitter-cpp/src/parser.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/tree-sitter-cpp/src/scanner.c")
  target_include_directories(fixit_tree_sitter_cpp
    SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/third_party/tree-sitter-cpp/src"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/tree-sitter-cpp/src")
  target_link_libraries(fixit_third_party INTERFACE fixit_tree_sitter_cpp)
endif()

# --- nlohmann/json (header only) --------------------------------------------
if(TARGET nlohmann_json::nlohmann_json)
  target_link_libraries(fixit_third_party INTERFACE nlohmann_json::nlohmann_json)
else()
  add_library(fixit_json INTERFACE)
  target_include_directories(fixit_json
    SYSTEM INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/json/include")
  target_link_libraries(fixit_third_party INTERFACE fixit_json)
endif()

# --- cpp-httplib (header only) ----------------------------------------------
# OpenSSL is deliberately NOT a dependency (see docs/decisions.md ADR-004):
# plain HTTP endpoints (Ollama, vLLM, llama.cpp server, test doubles) are
# supported out of the box.  Set FIXIT_ENABLE_OPENSSL=ON when a system OpenSSL
# is available to also reach https:// endpoints.
option(FIXIT_ENABLE_OPENSSL "Link cpp-httplib against a system OpenSSL" OFF)

if(TARGET httplib::httplib)
  target_link_libraries(fixit_third_party INTERFACE httplib::httplib)
else()
  add_library(fixit_httplib INTERFACE)
  target_include_directories(fixit_httplib
    SYSTEM INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/cpp-httplib")
  if(FIXIT_ENABLE_OPENSSL)
    find_package(OpenSSL REQUIRED)
    target_compile_definitions(fixit_httplib INTERFACE CPPHTTPLIB_OPENSSL_SUPPORT)
    target_link_libraries(fixit_httplib INTERFACE OpenSSL::SSL OpenSSL::Crypto)
  else()
    target_compile_definitions(fixit_httplib INTERFACE CPPHTTPLIB_OPENSSL_SUPPORT=0)
  endif()
  target_link_libraries(fixit_third_party INTERFACE fixit_httplib)
endif()

# --- Catch2 (tests only) ----------------------------------------------------
if(FIXIT_BUILD_TESTS AND NOT TARGET Catch2::Catch2WithMain)
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/Catch2/CMakeLists.txt")
    set(CATCH_INSTALL_DOCS OFF CACHE BOOL "" FORCE)
    set(CATCH_INSTALL_EXTRAS OFF CACHE BOOL "" FORCE)
    set(CATCH_BUILD_TESTING OFF CACHE BOOL "" FORCE)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/third_party/Catch2"
                     "${CMAKE_CURRENT_BINARY_DIR}/catch2-build"
                     EXCLUDE_FROM_ALL)
  endif()
endif()
