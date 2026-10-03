// compiler.test.cpp -- golden fixtures for fixit::parse_diagnostics().
//
// Every tests/fixtures/compiler/<name>.json is one fixture.  It names the
// matching <name>.txt (raw compiler output) and the exact diagnostics that
// fixit::parse_diagnostics() must extract from it.  The suite is offline and
// deterministic: it never invokes a compiler.  Use
// tools/record_compiler_fixtures.sh to refresh the captures and see
// tests/fixtures/compiler/README.md for how each one was recorded.

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "fixit/compiler.h"

namespace {

namespace fs = std::filesystem;

/// The corpus size is part of the contract: it catches a fixture that was
/// deleted or accidentally left behind outside the fixture directory.
constexpr std::size_t kFixtureCount = 20;

fs::path fixtures_dir() { return fs::path(FIXIT_FIXTURES_DIR) / "compiler"; }

std::string read_file(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  REQUIRE(in.good());
  std::ostringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

bool ends_with(const std::string& text, const std::string& suffix) {
  return text.size() >= suffix.size() &&
         text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string level_name(fixit::DiagLevel level) {
  return level == fixit::DiagLevel::Warning ? "warning" : "error";
}

/// One entry of a fixture's "expect" array.
struct Expected {
  int line = 0;
  int col = 0;
  std::string level;
  std::string message;
  bool has_context_line = false;
  std::string context_line;
};

struct Fixture {
  std::string name;  ///< "<id>.<dialect>", from the .json file name
  fs::path txt_path;
  std::string dialect;   ///< "gcc" | "clang-json"  (selects the parser entry point)
  std::string compiler;  ///< "gcc" | "clang"       (the compiler that produced it)
  std::string source;    ///< every diagnostic must name a file ending with this
  std::string recorded;  ///< "machine" | "synthetic"
  std::vector<Expected> expect;
};

/// Every *.json under FIXIT_FIXTURES_DIR/compiler, in a stable order.
std::vector<fs::path> discover_fixtures() {
  const fs::path dir = fixtures_dir();
  REQUIRE(fs::is_directory(dir));
  std::vector<fs::path> paths;
  for (const fs::directory_entry& entry : fs::directory_iterator(dir)) {
    if (entry.is_regular_file() && entry.path().extension() == ".json") {
      paths.push_back(entry.path());
    }
  }
  std::sort(paths.begin(), paths.end());
  return paths;
}

Fixture load_fixture(const fs::path& json_path) {
  Fixture fixture;
  fixture.name = json_path.stem().string();
  fixture.txt_path = json_path.parent_path() / (fixture.name + ".txt");

  const nlohmann::json meta = nlohmann::json::parse(read_file(json_path), nullptr, false);
  REQUIRE_FALSE(meta.is_discarded());
  REQUIRE(meta.is_object());

  fixture.dialect = meta.value("dialect", std::string());
  fixture.compiler = meta.value("compiler", std::string());
  fixture.source = meta.value("source", std::string());
  fixture.recorded = meta.value("recorded", std::string());

  REQUIRE((fixture.dialect == "gcc" || fixture.dialect == "clang-json"));
  REQUIRE((fixture.compiler == "gcc" || fixture.compiler == "clang"));
  REQUIRE((fixture.recorded == "machine" || fixture.recorded == "synthetic"));
  REQUIRE_FALSE(fixture.source.empty());
  // `<id>.gcc` must be parsed by the text dialect and `<id>.clang` by clang's.
  REQUIRE(ends_with(fixture.name, fixture.dialect == "gcc" ? ".gcc" : ".clang"));

  REQUIRE(meta.contains("expect"));
  for (const nlohmann::json& entry : meta["expect"]) {
    Expected expected;
    expected.line = entry.value("line", 0);
    expected.col = entry.value("col", 0);
    expected.level = entry.value("level", std::string());
    expected.message = entry.value("message", std::string());
    expected.has_context_line = entry.contains("context_line");
    if (expected.has_context_line) {
      expected.context_line = entry.value("context_line", std::string());
    }
    REQUIRE(expected.line > 0);
    REQUIRE(expected.col > 0);
    REQUIRE((expected.level == "error" || expected.level == "warning"));
    fixture.expect.push_back(std::move(expected));
  }
  return fixture;
}

std::vector<fixit::Diagnostic> parse_fixture(const Fixture& fixture) {
  return fixit::parse_diagnostics(read_file(fixture.txt_path),
                                  fixture.dialect == "clang-json");
}

std::size_t error_count(const std::vector<fixit::Diagnostic>& diags) {
  return static_cast<std::size_t>(std::count_if(
      diags.begin(), diags.end(),
      [](const fixit::Diagnostic& d) { return d.level == fixit::DiagLevel::Error; }));
}

/// The set of (line, column) anchors of the error-level diagnostics.
std::vector<std::pair<int, int>> error_positions(
    const std::vector<fixit::Diagnostic>& diags) {
  std::vector<std::pair<int, int>> positions;
  for (const fixit::Diagnostic& d : diags) {
    if (d.level == fixit::DiagLevel::Error) positions.emplace_back(d.line, d.col);
  }
  std::sort(positions.begin(), positions.end());
  positions.erase(std::unique(positions.begin(), positions.end()), positions.end());
  return positions;
}

bool ordered_by_position(const std::vector<fixit::Diagnostic>& diags) {
  return std::is_sorted(diags.begin(), diags.end(),
                        [](const fixit::Diagnostic& a, const fixit::Diagnostic& b) {
                          return std::tuple(a.file, a.line, a.col, a.message) <
                                 std::tuple(b.file, b.line, b.col, b.message);
                        });
}

}  // namespace

TEST_CASE("compiler fixtures parse to the recorded diagnostics", "[compiler][fixtures]") {
  for (const fs::path& json_path : discover_fixtures()) {
    DYNAMIC_SECTION(json_path.filename().string()) {
      const Fixture fixture = load_fixture(json_path);
      const std::vector<fixit::Diagnostic> diags = parse_fixture(fixture);

      // 1. every diagnostic must point into the fixture's declared source file.
      for (const fixit::Diagnostic& d : diags) {
        INFO("diagnostic file: " << d.file);
        REQUIRE(ends_with(d.file, fixture.source));
      }

      // 2. parse_diagnostics() hands back an already-ordered list.
      INFO("fixture: " << fixture.name);
      REQUIRE(ordered_by_position(diags));

      // 3. the diagnostics themselves, in order.
      REQUIRE(diags.size() == fixture.expect.size());
      // `min` keeps the loop in bounds even if a future Catch2 release lets a
      // failed REQUIRE fall through inside a DYNAMIC_SECTION.
      const std::size_t count = std::min(diags.size(), fixture.expect.size());
      for (std::size_t i = 0; i < count; ++i) {
        const Expected& expected = fixture.expect[i];
        INFO("fixture " << fixture.name << ", diagnostic #" << i << ": " << diags[i].file
                        << ':' << diags[i].line << ':' << diags[i].col << " ("
                        << diags[i].message << ')');
        REQUIRE(diags[i].line == expected.line);
        REQUIRE(diags[i].col == expected.col);
        REQUIRE(level_name(diags[i].level) == expected.level);
        REQUIRE(diags[i].message == expected.message);
        if (expected.has_context_line) {
          REQUIRE(diags[i].context_line == expected.context_line);
        }
      }
    }
  }
}

TEST_CASE("the compiler fixture corpus is complete", "[compiler][fixtures]") {
  const std::vector<fs::path> paths = discover_fixtures();
  REQUIRE(paths.size() == kFixtureCount);

  std::size_t gcc = 0;
  std::size_t clang = 0;
  for (const fs::path& json_path : paths) {
    const Fixture fixture = load_fixture(json_path);
    INFO("fixture: " << fixture.name);
    REQUIRE(fs::is_regular_file(fixture.txt_path));
    if (fixture.dialect == "gcc") {
      ++gcc;
    } else {
      ++clang;
    }
  }
  // Both dialects must be represented, and together they are the whole corpus.
  REQUIRE(gcc > 0);
  REQUIRE(clang > 0);
  REQUIRE(gcc + clang == kFixtureCount);

  // The parity pairs need the checked-in clang reference capture next to the
  // hand-written gcc twin.
  for (const char* base : {"e1_missing_include", "e2_drift", "e3_type_error"}) {
    INFO("parity base: " << base);
    REQUIRE(fs::is_regular_file(fixtures_dir() / (std::string(base) + ".clang.txt")));
    REQUIRE(fs::is_regular_file(fixtures_dir() / (std::string(base) + ".gcc.txt")));
    REQUIRE(fs::is_regular_file(fixtures_dir() / (std::string(base) + ".gcc.json")));
  }
}

TEST_CASE("gcc and clang agree on the parity fixtures", "[compiler][fixtures][parity]") {
  for (const char* base : {"e1_missing_include", "e2_drift", "e3_type_error"}) {
    DYNAMIC_SECTION("parity pair " << base) {
      const fs::path gcc_txt = fixtures_dir() / (std::string(base) + ".gcc.txt");
      const fs::path clang_txt = fixtures_dir() / (std::string(base) + ".clang.txt");
      REQUIRE(fs::is_regular_file(gcc_txt));
      REQUIRE(fs::is_regular_file(clang_txt));

      const std::vector<fixit::Diagnostic> gcc =
          fixit::parse_diagnostics(read_file(gcc_txt), false);
      const std::vector<fixit::Diagnostic> clang =
          fixit::parse_diagnostics(read_file(clang_txt), true);

      REQUIRE(error_count(gcc) > 0);
      REQUIRE(error_count(clang) > 0);
      REQUIRE(error_count(gcc) == error_count(clang));
      REQUIRE(error_positions(gcc) == error_positions(clang));

      const std::string source = std::string(base) + ".cpp";
      for (const fixit::Diagnostic& d : gcc) REQUIRE(ends_with(d.file, source));
      for (const fixit::Diagnostic& d : clang) REQUIRE(ends_with(d.file, source));
    }
  }
}

// ---------------------------------------------------------------------------
// Cross-compiler parity, verified for real.
//
// The fixture pair test above compares two *captures*.  It cannot tell the
// difference between "the two compilers agree" and "someone aligned them by
// hand" -- and on a machine without real GCC the captures are hand-written, so
// they are aligned by construction (see fixtures/compiler/README.md).
//
// This test removes that doubt where it can: when both a real clang++ and a real
// g++ are present it compiles e1..e3 with each one, parses the live output of
// both dialects, and requires identical (line, col) error sets.  On a machine
// without real GCC it reports a skip instead of a false pass.
// ---------------------------------------------------------------------------
namespace {

/// Resolves `name` to an absolute path, or an empty path when absent.
fs::path which(const std::string& name) {
  const char* path_env = std::getenv("PATH");
  if (path_env == nullptr) return {};
  std::stringstream stream(path_env);
  std::string directory;
  while (std::getline(stream, directory, ':')) {
    if (directory.empty()) continue;
    const fs::path candidate = fs::path(directory) / name;
    std::error_code error;
    if (fs::is_regular_file(candidate, error)) return fs::absolute(candidate);
  }
  return {};
}

/// True for a compiler that really is GCC (Apple's /usr/bin/g++ is clang).
bool is_real_gcc(const fs::path& compiler) {
  const std::string quoted = "'" + compiler.string() + "'";
  FILE* pipe = ::popen((quoted + " --version 2>&1").c_str(), "r");
  if (pipe == nullptr) return false;
  std::string output;
  char buffer[256];
  std::size_t total = 0;
  while (std::fgets(buffer, sizeof(buffer), pipe) != nullptr && total < 4096) {
    output += buffer;
    total = output.size();
  }
  const int status = ::pclose(pipe);
  if (status != 0) return false;
  return output.find("clang") == std::string::npos && output.find("Free Software Foundation") != std::string::npos;
}

std::string shell_quote(const std::string& text) {
  std::string quoted = "'";
  for (char c : text) {
    if (c == '\'') {
      quoted += "'\\''";
    } else {
      quoted += c;
    }
  }
  quoted += "'";
  return quoted;
}

}  // namespace

TEST_CASE("real gcc and real clang agree on e1..e3", "[compiler][parity][live]") {
  const fs::path clang = which("clang++");
  const fs::path gcc = which("g++");
  if (clang.empty() || gcc.empty() || !is_real_gcc(gcc)) {
    SKIP("live parity needs both a real g++ and a real clang++ on PATH; the "
         "checked-in fixture pair test still runs");
  }

  for (const char* base : {"e1_missing_include", "e2_drift", "e3_type_error"}) {
    DYNAMIC_SECTION("live parity " << base) {
      const fs::path source = fs::path(FIXIT_EXAMPLES_DIR) / "buggy" / (std::string(base) + ".cpp");
      REQUIRE(fs::is_regular_file(source));

      const auto compile = [&](const fs::path& compiler, bool json) {
        std::string command = shell_quote(compiler.string()) +
                              " -std=c++20 -fsyntax-only -ferror-limit=5";
        if (json) command += " -fdiagnostics-format=json";
        command += " " + shell_quote(source.string()) + " 2>&1";
        FILE* pipe = ::popen(command.c_str(), "r");
        REQUIRE(pipe != nullptr);
        std::string output;
        char buffer[4096];
        std::size_t read = 0;
        while ((read = std::fread(buffer, 1, sizeof(buffer), pipe)) > 0) {
          output.append(buffer, read);
        }
        ::pclose(pipe);
        return output;
      };

      // clang's JSON dialect is only available on some builds (Apple clang 17
      // rejects the flag), so ask the same probe the library uses and fall back
      // to clang's text output when it is unavailable.
      const fixit::Compiler clang_probe(
          fixit::CompilerConfig{clang.string(), 5, 10, {},
                                fixit::CompilerConfig::DiagnosticFormat::Auto});
      const bool clang_speaks_json = clang_probe.uses_json_diagnostics();
      const std::string clang_output = compile(clang, clang_speaks_json);

      const std::vector<fixit::Diagnostic> gcc_diagnostics =
          fixit::parse_diagnostics(compile(gcc, false), false);
      const std::vector<fixit::Diagnostic> clang_diagnostics =
          fixit::parse_diagnostics(clang_output, clang_speaks_json);

      INFO("clang dialect: " << (clang_speaks_json ? "json" : "text"));
      REQUIRE(error_count(gcc_diagnostics) > 0);
      REQUIRE(error_count(clang_diagnostics) > 0);
      CHECK(error_count(gcc_diagnostics) == error_count(clang_diagnostics));
      CHECK(error_positions(gcc_diagnostics) == error_positions(clang_diagnostics));
    }
  }
}
