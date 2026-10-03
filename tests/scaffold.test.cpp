#include <catch2/catch_test_macros.hpp>

// Temporary scaffold test: proves the Catch2 wiring and the library link before
// the per-module suites land.  Replaced by the real module tests below.
#include "fixit/types.h"

TEST_CASE("scaffold: empty compile result is clean", "[scaffold]") {
  fixit::CompileResult r;
  REQUIRE(r.clean());
  REQUIRE(r.error_count() == 0);
}
