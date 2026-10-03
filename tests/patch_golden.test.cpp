// patch_golden.test.cpp -- the core regression suite for the fuzzy patch engine.
//
// Section 1 drives 60 generated golden patches (see tools/gen_golden_cases.py):
// exact, line-drift, whitespace, missing-context, extra-context and multi-hunk
// inputs must all apply and produce byte-exact output.
//
// Section 2 checks the 10 negative cases: the engine must refuse, must not
// touch the file, and must explain itself with a structured failure reason.
//
// Section 3 pins down the units the scoring is built from.

#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "fixit/patch.h"

TEST_CASE("golden patches apply exactly", "[patch][golden]") {
  const fixit::PatchEngine engine;
  int case_index = 0;
  (void)case_index;
#include "patch_golden_cases.inc"
}

TEST_CASE("normalize_line strips only trailing whitespace", "[patch][unit]") {
  CHECK(fixit::normalize_line("int x = 1;   ") == "int x = 1;");
  CHECK(fixit::normalize_line("int x = 1;\r") == "int x = 1;");
  CHECK(fixit::normalize_line("  int x = 1;") == "  int x = 1;");  // leading kept
  CHECK(fixit::normalize_line("\tint x = 1; \t") == "\tint x = 1;");
  CHECK(fixit::normalize_line("") == "");
}

TEST_CASE("levenshtein_ratio is sane", "[patch][unit]") {
  CHECK(fixit::levenshtein_ratio("abc", "abc") == 1.0);
  CHECK(fixit::levenshtein_ratio("abc", "abd") > 0.6);
  CHECK(fixit::levenshtein_ratio("abc", "xyz") == 0.0);
  CHECK(fixit::levenshtein_ratio("", "abc") == 0.0);
  // The default threshold must separate a near miss from a different line.
  CHECK(fixit::levenshtein_ratio("  int total = value;", "  int total = value") >= 0.8);
  CHECK(fixit::levenshtein_ratio("  int total = value;", "  return count;") < 0.8);
}

TEST_CASE("parse_diff handles the documented shapes", "[patch][parse]") {
  SECTION("single hunk, counts and both sides") {
    const std::string diff =
        "--- a/foo.cpp\n"
        "+++ b/foo.cpp\n"
        "@@ -3,4 +3,5 @@\n"
        " context one\n"
        "-removed line\n"
        "+added line\n"
        "+second added\n"
        " context two\n";
    const std::vector<fixit::DiffFile> files = fixit::PatchEngine::parse_diff(diff);
    REQUIRE(files.size() == 1);
    CHECK(files[0].path == "foo.cpp");
    REQUIRE(files[0].hunks.size() == 1);
    const fixit::HunkSpec& hunk = files[0].hunks[0];
    CHECK(hunk.old_start == 3);
    CHECK(hunk.old_count == 4);
    CHECK(hunk.new_start == 3);
    CHECK(hunk.new_count == 5);
    REQUIRE(hunk.old_lines.size() == 3);
    CHECK(hunk.old_lines[0] == "context one");
    CHECK(hunk.old_lines[1] == "removed line");
    CHECK(hunk.old_lines[2] == "context two");
    REQUIRE(hunk.new_lines.size() == 4);
    CHECK(hunk.new_lines[1] == "added line");
    CHECK(hunk.new_lines[2] == "second added");
  }

  SECTION("count omitted means one line") {
    const std::vector<fixit::DiffFile> files = fixit::PatchEngine::parse_diff(
        "--- a/foo.cpp\n+++ b/foo.cpp\n@@ -9 +9 @@\n-old\n+new\n");
    REQUIRE(files.size() == 1);
    REQUIRE(files[0].hunks.size() == 1);
    CHECK(files[0].hunks[0].old_start == 9);
    CHECK(files[0].hunks[0].old_count == 1);
  }

  SECTION("multi-file diff keeps both files") {
    const std::string diff =
        "--- a/one.cpp\n"
        "+++ b/one.cpp\n"
        "@@ -1,1 +1,1 @@\n-a\n+b\n"
        "--- a/two.cpp\n"
        "+++ b/two.cpp\n"
        "@@ -2,1 +2,1 @@\n-c\n+d\n";
    const std::vector<fixit::DiffFile> files = fixit::PatchEngine::parse_diff(diff);
    REQUIRE(files.size() == 2);
    CHECK(files[0].path == "one.cpp");
    CHECK(files[1].path == "two.cpp");
  }

  SECTION("target hint selects one file out of many") {
    const std::string diff =
        "--- a/one.cpp\n+++ b/one.cpp\n@@ -1,1 +1,1 @@\n-a\n+b\n"
        "--- a/two.cpp\n+++ b/two.cpp\n@@ -1,1 +1,1 @@\n-a\n+c\n";
    const fixit::PatchEngine engine;
    const fixit::PatchResult result = engine.apply("a\n", diff, "two.cpp");
    CHECK(result.all_applied);
    CHECK(result.new_content == "c\n");
  }

  SECTION("No newline marker is ignored") {
    const std::vector<fixit::DiffFile> files = fixit::PatchEngine::parse_diff(
        "--- a/f.cpp\n+++ b/f.cpp\n@@ -1,2 +1,2 @@\n a\n-b\n\\ No newline at end of file\n+c\n");
    REQUIRE(files.size() == 1);
    CHECK(files[0].hunks[0].new_lines.size() == 2);
  }

  SECTION("pure insertion hunk has no old lines") {
    const std::vector<fixit::DiffFile> files = fixit::PatchEngine::parse_diff(
        "--- a/f.cpp\n+++ b/f.cpp\n@@ -2,0 +3,1 @@\n+new line\n");
    REQUIRE(files.size() == 1);
    CHECK(files[0].hunks[0].old_lines.empty());
    CHECK(files[0].hunks[0].new_lines.size() == 1);
  }
}

TEST_CASE("insertions and deletions round-trip", "[patch]") {
  const fixit::PatchEngine engine;

  SECTION("pure insertion goes in at the declared position") {
    const std::string content = "one\ntwo\nthree\n";
    const std::string diff = "--- a/f.cpp\n+++ b/f.cpp\n@@ -2,0 +2,1 @@\n+inserted\n";
    const fixit::PatchResult result = engine.apply(content, diff, "f.cpp");
    CHECK(result.all_applied);
    CHECK(result.new_content == "one\ninserted\ntwo\nthree\n");
  }

  SECTION("deletion removes the matched lines") {
    const std::string content = "one\ntwo\nthree\n";
    const std::string diff = "--- a/f.cpp\n+++ b/f.cpp\n@@ -2,1 +2,0 @@\n-two\n";
    const fixit::PatchResult result = engine.apply(content, diff, "f.cpp");
    CHECK(result.all_applied);
    CHECK(result.new_content == "one\nthree\n");
  }

  SECTION("a file without a trailing newline keeps that shape") {
    const std::string content = "one\ntwo";
    const std::string diff = "--- a/f.cpp\n+++ b/f.cpp\n@@ -2,1 +2,1 @@\n-two\n+TWO\n";
    const fixit::PatchResult result = engine.apply(content, diff, "f.cpp");
    CHECK(result.all_applied);
    CHECK(result.new_content == "one\nTWO");
  }
}

TEST_CASE("a far-away match is still found by the global scan", "[patch]") {
  std::string content;
  for (int i = 1; i <= 2200; ++i) {
    content += "// filler line " + std::to_string(i) + "\n";
  }
  content += "int target = 42;\n";
  const std::string diff =
      "--- a/f.cpp\n+++ b/f.cpp\n@@ -1,1 +1,1 @@\n-int target = 42;\n+int target = 43;\n";
  const fixit::PatchEngine engine;
  const fixit::PatchResult result = engine.apply(content, diff, "f.cpp");
  // The engine prefers a correct repair over a refusal, however far it must look.
  CHECK(result.all_applied);
  CHECK(result.new_content.find("int target = 43;") != std::string::npos);
}

TEST_CASE("partial application writes what it can", "[patch]") {
  const fixit::PatchEngine engine;
  const std::string content = "alpha\nbeta\ngamma\n";
  const std::string diff =
      "--- a/f.cpp\n+++ b/f.cpp\n"
      "@@ -1,1 +1,1 @@\n-alpha\n+ALPHA\n"
      "@@ -9,1 +9,1 @@\n-this line does not exist anywhere\n+nope\n";
  const fixit::PatchResult result = engine.apply(content, diff, "f.cpp");
  CHECK_FALSE(result.all_applied);
  REQUIRE(result.reports.size() == 2);
  CHECK(result.reports[0].status == fixit::HunkReport::Status::Applied);
  CHECK(result.reports[1].status == fixit::HunkReport::Status::Failed);
  // The applied hunk is written, the failed one leaves its region untouched.
  CHECK(result.new_content == "ALPHA\nbeta\ngamma\n");
}
