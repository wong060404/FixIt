// codemap.test.cpp -- structural queries over five sample files.
//
// CodeMap wraps tree-sitter-cpp.  These tests pin down the four things the
// repair loop actually asks of it: where are the functions, which one encloses
// a diagnostic, what is included, and does the AST admit a syntax error.

#include <algorithm>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "fixit/codemap.h"

namespace {

std::string fixture(const std::string& name) {
  return std::string(FIXIT_FIXTURES_DIR) + "/codemap/" + name;
}

std::vector<std::string> function_names(const fixit::CodeMap& map) {
  std::vector<std::string> names;
  for (const fixit::FunctionInfo& fn : map.functions()) names.push_back(fn.name);
  return names;
}

bool contains_name(const std::vector<std::string>& names, const std::string& needle) {
  return std::any_of(names.begin(), names.end(),
                     [&](const std::string& n) { return n == needle; });
}

bool contains_name_fragment(const std::vector<std::string>& names, const std::string& fragment) {
  return std::any_of(names.begin(), names.end(),
                     [&](const std::string& n) { return n.find(fragment) != std::string::npos; });
}

}  // namespace

TEST_CASE("functions are discovered with names and ranges", "[codemap]") {
  const fixit::CodeMap map(fixture("functions.cpp"));
  const std::vector<std::string> names = function_names(map);

  CHECK(contains_name(names, "free_function"));
  CHECK(contains_name(names, "scaled"));
  CHECK(contains_name(names, "draw"));
  CHECK(contains_name(names, "width"));
  CHECK(contains_name(names, "describe"));
  CHECK(contains_name(names, "main"));
  CHECK(contains_name_fragment(names, "Widget"));  // the constructor

  const std::vector<fixit::FunctionInfo> functions = map.functions();
  REQUIRE(functions.size() >= 5);
  for (const fixit::FunctionInfo& fn : functions) {
    CHECK(fn.start_line >= 1);
    CHECK(fn.end_line >= fn.start_line);
    CHECK(fn.signature.find('{') == std::string::npos);  // '{' excluded per spec
  }

  const auto it = std::find_if(functions.begin(), functions.end(), [](const fixit::FunctionInfo& f) {
    return f.name == "free_function";
  });
  REQUIRE(it != functions.end());
  CHECK(it->start_line == 5);
  CHECK(it->end_line == 7);
  CHECK(it->signature.find("int free_function(int a, int b)") != std::string::npos);
}

TEST_CASE("enclosing_function picks the innermost definition", "[codemap]") {
  const fixit::CodeMap map(fixture("functions.cpp"));

  SECTION("inside a free function") {
    const auto fn = map.enclosing_function(6, 3);
    REQUIRE(fn.has_value());
    CHECK(fn->name == "free_function");
  }

  SECTION("inside a member function") {
    const auto fn = map.enclosing_function(20, 5);  // inside Widget::draw
    REQUIRE(fn.has_value());
    CHECK(fn->name == "draw");
  }

  SECTION("an inline definition counts as a function") {
    // `Widget() = default;` parses as a defined function, so line 18 belongs to it.
    const auto fn = map.enclosing_function(18, 5);
    REQUIRE(fn.has_value());
    CHECK(fn->name == "Widget");
  }

  SECTION("at file scope there is no enclosing function") {
    CHECK_FALSE(map.enclosing_function(1, 1).has_value());   // an #include
    CHECK_FALSE(map.enclosing_function(13, 1).has_value());  // a namespace
  }
}

TEST_CASE("includes are reported verbatim", "[codemap]") {
  const fixit::CodeMap map(fixture("includes_only.cpp"));
  const std::vector<std::string> includes = map.includes();
  REQUIRE(includes.size() == 4);
  CHECK(includes[0] == "<cstdio>");
  CHECK(includes[1] == "<string>");
  CHECK(includes[2] == "\"local_header.h\"");
  CHECK(includes[3] == "\"../relative/path.h\"");

  // A file with includes but no functions is still valid.
  CHECK(map.functions().empty());
  CHECK_FALSE(map.has_syntax_errors());
}

TEST_CASE("context_snippet uses the %4d | text format", "[codemap]") {
  const fixit::CodeMap map(fixture("functions.cpp"));

  SECTION("a normal window") {
    const std::string snippet = map.context_snippet(6, 1);
    CHECK(snippet ==
          "   5 | int free_function(int a, int b) {\n"
          "   6 |   return a + b;\n"
          "   7 | }\n");
  }

  SECTION("the window is clamped at both ends") {
    const std::string head = map.context_snippet(1, 12);
    CHECK(head.rfind("   1 | ", 0) == 0);
    CHECK(head.find("   0 | ") == std::string::npos);

    const std::string tail = map.context_snippet(map.line_count(), 12);
    CHECK(tail.find(std::to_string(map.line_count()) + " | ") != std::string::npos);
    CHECK(tail.find(std::to_string(map.line_count() + 1) + " | ") == std::string::npos);
  }
}

TEST_CASE("syntax errors are detected", "[codemap]") {
  SECTION("a broken file reports errors") {
    const fixit::CodeMap map(fixture("broken.cpp"));
    CHECK(map.has_syntax_errors());
    CHECK_FALSE(map.syntax_error_lines().empty());
    for (int line : map.syntax_error_lines()) CHECK(line >= 1);
  }

  SECTION("an empty file is not an error") {
    const fixit::CodeMap map(fixture("empty.cpp"));
    CHECK(map.line_count() == 0);
    CHECK(map.functions().empty());
    CHECK(map.includes().empty());
  }

  SECTION("a missing file degrades instead of throwing") {
    const fixit::CodeMap map(fixture("does_not_exist.cpp"));
    CHECK(map.functions().empty());
    CHECK(map.includes().empty());
    CHECK(map.has_syntax_errors());
  }
}

TEST_CASE("modern C++ constructs parse", "[codemap]") {
  const fixit::CodeMap map(fixture("cpp_features.cpp"));
  CHECK_FALSE(map.has_syntax_errors());
  const std::vector<std::string> names = function_names(map);
  CHECK(contains_name(names, "clamp_value"));
  CHECK(contains_name(names, "make_adder"));
  CHECK(contains_name(names, "main"));
  CHECK(map.includes().size() == 1);
}
