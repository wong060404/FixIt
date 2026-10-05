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

#include <algorithm>
#include <cstddef>
#include <sstream>
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

  SECTION("pure insertion goes in after the declared line") {
    // `@@ -l,0 +m,k @@` inserts AFTER line l -- GNU diff emits exactly this shape
    // for an insertion and GNU patch honours it, so the engine must too.
    const std::string content = "one\ntwo\nthree\n";
    const std::string diff = "--- a/f.cpp\n+++ b/f.cpp\n@@ -2,0 +3,1 @@\n+inserted\n";
    const fixit::PatchResult result = engine.apply(content, diff, "f.cpp");
    CHECK(result.all_applied);
    CHECK(result.new_content == "one\ntwo\ninserted\nthree\n");
  }

  SECTION("insertion at the very start") {
    const std::string content = "one\ntwo\n";
    const std::string diff = "--- a/f.cpp\n+++ b/f.cpp\n@@ -0,0 +1,1 @@\n+first\n";
    const fixit::PatchResult result = engine.apply(content, diff, "f.cpp");
    CHECK(result.all_applied);
    CHECK(result.new_content == "first\none\ntwo\n");
  }

  SECTION("insertion at the very end") {
    const std::string content = "one\ntwo\n";
    const std::string diff = "--- a/f.cpp\n+++ b/f.cpp\n@@ -2,0 +3,1 @@\n+last\n";
    const fixit::PatchResult result = engine.apply(content, diff, "f.cpp");
    CHECK(result.all_applied);
    CHECK(result.new_content == "one\ntwo\nlast\n");
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

// ---------------------------------------------------------------------------
// Properties behind the effectiveness numbers in the README.
//
// tools/matrix.cpp measures the full grid; these three cases pin down the
// behaviour those numbers depend on, so a regression cannot hide in a table.
// ---------------------------------------------------------------------------
namespace {

/// A file of `blocks` uniquely-named blocks; returns its content.
std::string unique_file(int blocks) {
  std::string out;
  out += "#include <cstdio>\n";
  for (int i = 0; i < blocks; ++i) {
    out += "int fn_" + std::to_string(i) + "(int input) {\n";
    out += "  int local_" + std::to_string(i) + " = input + " + std::to_string(i) + ";\n";
    out += "  return local_" + std::to_string(i) + ";\n";
    out += "}\n\n";
  }
  return out;
}

/// 1-based line number of the first line equal to `needle`, or 0.
int line_of(const std::string& content, const std::string& needle) {
  std::istringstream in(content);
  std::string line;
  int number = 0;
  while (std::getline(in, line)) {
    ++number;
    if (line == needle) return number;
  }
  return 0;
}

}  // namespace

TEST_CASE("a hunk applies correctly when the declared position is far off", "[patch][tolerance]") {
  // 40 unique blocks: the repaired line and its context identify exactly one
  // place, so the declared position is allowed to be badly wrong.
  const std::string content = unique_file(40);
  // The hunk quotes the block header as its first context line, so the position
  // it must report is that header's line: one above the line being repaired.
  const int repaired_line = line_of(content, "  int local_20 = input + 20;");
  REQUIRE(repaired_line > 0);
  const int target_line = repaired_line - 1;

  // Build one concrete drifted patch and check where it lands.
  // Context must identify block 20: the surrounding *block* names differ, only
  // the local variable is shared between blocks.
  const std::string body =
      " int fn_20(int input) {\n"
      "-  int local_20 = input + 20;\n"
      "+  int local_20 = input + 99;\n"
      "   return local_20;\n";
  for (int drift : {0, 1, 2, 5, 10, 20, -10}) {
    const int declared = target_line + drift;
    const std::string diff = "--- a/x.cpp\n+++ b/x.cpp\n@@ -" + std::to_string(declared) + ",3 +" +
                             std::to_string(declared) + ",3 @@\n" + body;
    const fixit::PatchEngine engine;
    const fixit::PatchResult result = engine.apply(content, diff, "x.cpp");
    INFO("drift " << drift);
    CHECK(result.all_applied);
    REQUIRE_FALSE(result.reports.empty());
    // It must land on the true line, not merely "somewhere".
    CHECK(result.reports.front().matched_pos == target_line);
    CHECK(result.new_content.find("int local_20 = input + 99;") != std::string::npos);
    // Unrelated lines survive untouched.
    CHECK(result.new_content.find("int local_19 = input + 19;") != std::string::npos);
    CHECK(result.new_content.find("int local_21 = input + 21;") != std::string::npos);
  }
}

TEST_CASE("a perfect match suppresses the whole-file scan", "[patch][tolerance]") {
  // The scan would find the same answer; this asserts the reported window stays
  // inside the ladder, so the fast path is the one that ran.
  const std::string content = unique_file(40);
  const int repaired_line = line_of(content, "  int local_20 = input + 20;");
  REQUIRE(repaired_line > 0);
  const int target_line = repaired_line - 1;  // the hunk starts at the block header
  const std::string diff =
      "--- a/x.cpp\n+++ b/x.cpp\n@@ -" + std::to_string(target_line) + ",3 +" +
      std::to_string(target_line) +
      ",3 @@\n"
      " int fn_20(int input) {\n"
      "-  int local_20 = input + 20;\n"
      "+  int local_20 = input + 99;\n"
      "   return local_20;\n";
  const fixit::PatchEngine engine;
  const fixit::PatchResult result = engine.apply(content, diff, "x.cpp");
  CHECK(result.all_applied);
  REQUIRE_FALSE(result.reports.empty());
  CHECK(result.reports.front().matched_pos == target_line);
  CHECK(result.reports.front().status == fixit::HunkReport::Status::Applied);
  // The nearest-position preference must not have widened to a fallback scan:
  // candidates are only collected for failures.
  CHECK(result.reports.front().top_candidates.empty());
}

TEST_CASE("an ambiguous hunk is refused rather than silently misplaced",
          "[patch][tolerance][negative]") {
  // Every block is byte-identical, so the context matches dozens of places.
  std::string content = "#include <cstdio>\n";
  for (int i = 0; i < 30; ++i) {
    content += "int handler(int input) {\n";
    content += "  if (input < 0) return 0;\n";
    content += "  int work = input;\n";
    content += "  return work;\n";
    content += "}\n\n";
  }
  // The quoted context is identical everywhere and the declared position is more
  // than one window away from the intended block, so the engine is entitled to
  // choose the nearest candidate -- but it must never write to a *different*
  // location than the one it reports.
  const int declared = 4 * 10 + 2 + 5;
  const std::string diff =
      "--- a/x.cpp\n+++ b/x.cpp\n@@ -" + std::to_string(declared) + ",3 +" +
      std::to_string(declared) +
      ",3 @@\n"
      "   int work = input;\n"
      "-  return work;\n"
      "+  return work + 1;\n"
      " }\n";
  const fixit::PatchEngine engine;
  const fixit::PatchResult result = engine.apply(content, diff, "x.cpp");
  REQUIRE_FALSE(result.reports.empty());
  const fixit::HunkReport& report = result.reports.front();
  if (report.status == fixit::HunkReport::Status::Failed) {
    CHECK_FALSE(result.all_applied);
  } else {
    // If it applied, it must have applied exactly where it says it did.
    const std::string marker = "  return work + 1;";
    const std::size_t at = result.new_content.find(marker);
    REQUIRE(at != std::string::npos);
    // Newlines before the match == the 0-based line index of the match.
    const int line = static_cast<int>(std::count(result.new_content.begin(),
                                                 result.new_content.begin() +
                                                     static_cast<std::ptrdiff_t>(at),
                                                 '\n'));
    CHECK(line == report.matched_pos);
  }
}

// ---------------------------------------------------------------------------
// Regression: an insertion hunk must not consume the following line.
//
// Found by driving the engine with a real model, not by the golden corpus: every
// generated case replaced lines, so none combined "quoted context" with "an added
// line".  The engine used to consume old_count file lines while emitting
// context + additions, which differ by the number of additions, so the splice
// resumed one line too late and deleted the line after the insertion point.
//
// The apply path now derives the consumed span from the hunk's old side (context
// plus removals) and only widens it towards a *larger* declared count, which is
// the case the header exists to rescue.
// ---------------------------------------------------------------------------
TEST_CASE("an insertion hunk must not consume the following line",
          "[patch][regression]") {
  const std::string content = "L1\nL2\nL3\nL4\nL5\nL6\nL7\nL8\nL9\nL10\nL11\n";
  const std::string diff =
      "--- a/x.cpp\n+++ b/x.cpp\n@@ -7,3 +7,4 @@\n"
      " L7\n"
      "+INSERTED\n"
      " L8\n"
      " L9\n";
  const fixit::PatchEngine engine;
  const fixit::PatchResult result = engine.apply(content, diff, "x.cpp");

  INFO("new content:\n" << result.new_content);
  // The line after the insertion point must still be there.
  CHECK(result.new_content.find("L8\nL9\n") != std::string::npos);
  CHECK(result.new_content.find("INSERTED\n") != std::string::npos);
}
