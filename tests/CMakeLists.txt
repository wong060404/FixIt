# Catch2 test suite.
#
# Fixture discovery: tests receive the absolute path of tests/fixtures through
# FIXIT_FIXTURES_DIR so that they work from any build directory.

set(FIXIT_FIXTURES_DIR "${CMAKE_CURRENT_SOURCE_DIR}/fixtures")

# The compiler the integration tests exercise.  A caller can pin it with
# -DFIXIT_TEST_COMPILER=clang++; by default it follows the build's C++ compiler,
# which is what CI installs (gcc 12 on ubuntu, clang on macOS).
if(NOT FIXIT_TEST_COMPILER)
  set(FIXIT_TEST_COMPILER "${CMAKE_CXX_COMPILER}")
endif()
set(FIXIT_EXAMPLES_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../examples")

function(fixit_add_test name)
  add_executable(${name} ${ARGN})
  target_link_libraries(${name} PRIVATE fixit Catch2::Catch2WithMain)
  target_compile_definitions(${name} PRIVATE
    FIXIT_FIXTURES_DIR="${FIXIT_FIXTURES_DIR}"
    FIXIT_EXAMPLES_DIR="${FIXIT_EXAMPLES_DIR}"
    FIXIT_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    FIXIT_TEST_COMPILER="${FIXIT_TEST_COMPILER}")
  if(NOT MSVC)
    target_compile_options(${name} PRIVATE -Wall -Wextra -Werror)
  endif()
  add_test(NAME ${name} COMMAND ${name})
  set_tests_properties(${name} PROPERTIES
    ENVIRONMENT "PATH=${CMAKE_BINARY_DIR}/bin:$ENV{PATH}")
endfunction()

fixit_add_test(patch_golden.test patch_golden.test.cpp)
fixit_add_test(compiler.test compiler.test.cpp)
fixit_add_test(codemap.test codemap.test.cpp)
fixit_add_test(agent_mock.test agent_mock.test.cpp)
