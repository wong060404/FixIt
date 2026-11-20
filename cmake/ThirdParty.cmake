# ---------------------------------------------------------------------------
# Dependency wiring.
#
# By default the pinned snapshot under third_party/ is used, which keeps the
# build fully reproducible and offline.  If a component is missing (for example
# in a fresh clone that was not made with --recursive, or when FIXIT_FETCH_DEPS
# is ON) FetchContent pulls the exact same tag from upstream.
#
# Include directories are split into a BUILD_INTERFACE (the vendored source tree)
# and an INSTALL_INTERFACE (where `cmake --install` puts the headers) so that the
# exported targets work from an installed tree, not just from this checkout.
# ---------------------------------------------------------------------------
include(FetchContent)
include(GNUInstallDirs)

set(FIXIT_THIRD_PARTY_INSTALL_INCLUDE "${CMAKE_INSTALL_INCLUDEDIR}/fixit/third_party")

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

# These tags must match what third_party/ actually contains.  They drifted once
# (the snapshot held cpp-httplib 0.59.0 while this pinned v0.28.0, and Catch2
# 3.16.0 against v3.8.1), which would have made the offline snapshot and the
# FetchContent fallback behave differently -- and the vendored cpp-httplib carries
# a local patch, so a different upstream revision need not even compile.
# tree-sitter 0.28.0 is not released yet (the newest tag is v0.27.0), so the
# snapshot cannot be named by tag.  Pinning the commit the snapshot was taken from
# is the only way to keep the two paths identical; bump both together.
fixit_fetch(tree-sitter     https://github.com/tree-sitter/tree-sitter.git     752c612a1359f00e4c113593837a0c1e880214e3)
fixit_fetch(tree-sitter-cpp https://github.com/tree-sitter/tree-sitter-cpp.git v0.23.4)
fixit_fetch(json            https://github.com/nlohmann/json.git               v3.12.0)
fixit_fetch(cpp-httplib     https://github.com/yhirose/cpp-httplib.git         v0.59.0)
if(FIXIT_BUILD_TESTS)
  fixit_fetch(Catch2        https://github.com/catchorg/Catch2.git             v3.16.0)
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
    SYSTEM PUBLIC
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/third_party/tree-sitter/lib/include>
      $<INSTALL_INTERFACE:${FIXIT_THIRD_PARTY_INSTALL_INCLUDE}/tree-sitter>
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/tree-sitter/lib/src")
  target_compile_definitions(fixit_tree_sitter PRIVATE
    _POSIX_C_SOURCE=200112L _DEFAULT_SOURCE _DARWIN_C_SOURCE)
  set_target_properties(fixit_tree_sitter PROPERTIES C_VISIBILITY_PRESET hidden)
  if(NOT BUILD_SHARED_LIBS)
    target_link_libraries(fixit_tree_sitter INTERFACE $<$<NOT:$<PLATFORM_ID:Windows>>:m>)
    if(NOT APPLE AND NOT WIN32)
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
  # The grammar's own headers (tree_sitter/parser.h) are needed only while
  # compiling parser.c/scanner.c, so this stays PRIVATE: a consumer of FixIt
  # never includes them, and exporting an install path for them would point at a
  # directory the install rules do not create.
  target_include_directories(fixit_tree_sitter_cpp
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/tree-sitter-cpp/src")
  target_link_libraries(fixit_third_party INTERFACE fixit_tree_sitter_cpp)
endif()

# --- nlohmann/json (header only) --------------------------------------------
if(TARGET nlohmann_json::nlohmann_json)
  target_link_libraries(fixit_third_party INTERFACE nlohmann_json::nlohmann_json)
else()
  add_library(fixit_json INTERFACE)
  target_include_directories(fixit_json
    SYSTEM INTERFACE
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/third_party/json/include>
      $<INSTALL_INTERFACE:${FIXIT_THIRD_PARTY_INSTALL_INCLUDE}/json>)
  target_link_libraries(fixit_third_party INTERFACE fixit_json)
endif()

# --- cpp-httplib (header only) ----------------------------------------------
# OpenSSL is deliberately NOT a dependency (see docs/decisions.md ADR-004):
# plain HTTP endpoints (Ollama, vLLM, llama.cpp server, test doubles) are
# supported out of the box.  Set -DFIXIT_TLS=openssl when a system OpenSSL
# is available to also reach https:// endpoints.
if(TARGET httplib::httplib)
  # FetchContent brings its own target, so the vendored rules below never run and
  # FIXIT_TLS would be silently ignored -- asking for TLS and quietly getting a
  # build without it is worse than failing.
  set(FIXIT_TLS_ACTIVE FALSE)
  if(FIXIT_TLS STREQUAL "openssl")
    find_package(OpenSSL REQUIRED)
    target_compile_definitions(fixit_third_party INTERFACE CPPHTTPLIB_OPENSSL_SUPPORT)
    target_link_libraries(fixit_third_party INTERFACE OpenSSL::SSL OpenSSL::Crypto)
    set(FIXIT_TLS_ACTIVE TRUE)
    message(STATUS "fixit: TLS enabled (OpenSSL ${OpenSSL_VERSION})")
  elseif(NOT FIXIT_TLS STREQUAL "off")
    message(STATUS "fixit: FetchContent build; TLS follows cpp-httplib's own target "
                   "(pass -DFIXIT_TLS=openssl to force it)")
  endif()
  target_link_libraries(fixit_third_party INTERFACE httplib::httplib)
else()
  add_library(fixit_httplib INTERFACE)
  target_include_directories(fixit_httplib
    SYSTEM INTERFACE
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/third_party/cpp-httplib>
      $<INSTALL_INTERFACE:${FIXIT_THIRD_PARTY_INSTALL_INCLUDE}/cpp-httplib>)
  set(FIXIT_TLS_ACTIVE FALSE)
  if(FIXIT_TLS STREQUAL "openssl")
    find_package(OpenSSL REQUIRED)
  elseif(FIXIT_TLS STREQUAL "auto")
    find_package(OpenSSL QUIET)
  endif()
  if(OpenSSL_FOUND AND NOT FIXIT_TLS STREQUAL "off")
    message(STATUS "fixit: TLS enabled (OpenSSL ${OpenSSL_VERSION})")
    target_compile_definitions(fixit_httplib INTERFACE CPPHTTPLIB_OPENSSL_SUPPORT)
    target_link_libraries(fixit_httplib INTERFACE OpenSSL::SSL OpenSSL::Crypto)
    set(FIXIT_TLS_ACTIVE TRUE)
  else()
    message(STATUS
      "fixit: building without TLS; https:// endpoints need -DFIXIT_TLS=openssl")
    target_compile_definitions(fixit_httplib INTERFACE CPPHTTPLIB_OPENSSL_SUPPORT=0)
  endif()
  if(WIN32)
    # cpp-httplib reaches for CreateFile2 and friends, which MinGW hides unless
    # _WIN32_WINNT says Windows 10.  tools/windows/build_fixit.ps1 passed these
    # two defines by hand, so the CMake path silently diverged and failed with
    # "'::CreateFile2' has not been declared" -- define them on the target both
    # paths share.
    target_compile_definitions(fixit_httplib INTERFACE _WIN32_WINNT=0x0A00 WINVER=0x0A00)
    # Plain-HTTP cpp-httplib compiles against Winsock on Windows.
    target_link_libraries(fixit_httplib INTERFACE ws2_32)
    if(FIXIT_TLS_ACTIVE)
      # With OpenSSL on Windows, cpp-httplib verifies the peer chain against the
      # *Windows* certificate store through WinCrypt (CertGetCertificateChain and
      # friends).  Those symbols live in crypt32, so a TLS-enabled MinGW build
      # fails to link without it -- a plain http build never notices.
      target_link_libraries(fixit_httplib INTERFACE crypt32)
    endif()
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