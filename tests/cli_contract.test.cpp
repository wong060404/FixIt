// cli_contract.test.cpp -- the CLI's observable contract, exercised end to end.
//
// These cases run the built binary the way a user or CI would, because the
// interesting failures (a directory that "compiles clean", a missing key, a
// non-zero exit that must not read as success) live in the wiring between the
// library and the executable, not in any single module.

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace {

namespace fs = std::filesystem;

/// Absolute path of the CLI under test (set by CMake).
std::string cli_path() { return std::string(FIXIT_CLI_PATH); }

struct Run {
  int exit_code = -1;
  std::string output;
};

/// Runs the CLI with `args` and captures merged stdout/stderr.
Run run_cli(const std::vector<std::string>& args, const std::string& cwd = {}) {
  std::string command;
  if (!cwd.empty()) command += "cd " + cwd + " && ";
  command += "\"" + cli_path() + "\"";
  for (const std::string& arg : args) command += " \"" + arg + "\"";
  command += " 2>&1";

  Run result;
  FILE* pipe = ::popen(command.c_str(), "r");
  REQUIRE(pipe != nullptr);
  char buffer[4096];
  std::size_t read = 0;
  while ((read = std::fread(buffer, 1, sizeof(buffer), pipe)) > 0) {
    result.output.append(buffer, read);
  }
  const int status = ::pclose(pipe);
  result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  return result;
}

/// A scratch directory unique to one test case.
class Scratch {
 public:
  explicit Scratch(const std::string& label) {
    path_ = fs::temp_directory_path() / ("fixit-cli-" + label + "-" + std::to_string(static_cast<long>(::getpid())));
    fs::remove_all(path_);
    fs::create_directories(path_);
  }
  ~Scratch() {
    std::error_code ignored;
    fs::remove_all(path_, ignored);
  }
  const fs::path& path() const { return path_; }

 private:
  fs::path path_;
};

void write(const fs::path& path, const std::string& content) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << content;
}

}  // namespace

TEST_CASE("usage errors exit 2", "[cli]") {
  Scratch scratch("usage");

  SECTION("no arguments") {
    const Run run = run_cli({});
    CHECK(run.exit_code == 2);
    CHECK(run.output.find("exactly one source file") != std::string::npos);
    CHECK(run.output.find("Usage") != std::string::npos);
  }

  SECTION("an unknown option") {
    const Run run = run_cli({"x.cpp", "--definitely-not-an-option"});
    CHECK(run.exit_code == 2);
    CHECK(run.output.find("unknown option") != std::string::npos);
  }

  SECTION("a missing file") {
    const Run run = run_cli({(scratch.path() / "absent.cpp").string()});
    CHECK(run.exit_code == 2);
    CHECK(run.output.find("is not a readable file") != std::string::npos);
  }

  SECTION("a directory instead of a file") {
    // A directory opens with std::ifstream and the compiler says nothing about
    // it, so this used to exit 0 and read as "clean".
    const Run run = run_cli({scratch.path().string()});
    CHECK(run.exit_code == 2);
    CHECK(run.output.find("not a readable file") != std::string::npos);
    CHECK(run.output.find("clean") == std::string::npos);
  }

  SECTION("openai without a key") {
    // FIXIT_API_KEY must be scrubbed for this invocation, so build the command
    // with an explicit `env -u`.
    // The file must exist: the key check happens after the file check.
    const fs::path present = scratch.path() / "present.cpp";
    write(present, "int main() { return 0; }\n");
    const std::string command = "env -u FIXIT_API_KEY \"" + cli_path() + "\" \"" +
                                present.string() + "\" --agent --llm openai 2>&1";
    Run run;
    FILE* pipe = ::popen(command.c_str(), "r");
    REQUIRE(pipe != nullptr);
    char buffer[2048];
    std::size_t read = 0;
    while ((read = std::fread(buffer, 1, sizeof(buffer), pipe)) > 0) {
      run.output.append(buffer, read);
    }
    const int status = ::pclose(pipe);
    run.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    CHECK(run.exit_code == 2);
    CHECK(run.output.find("requires --api-key") != std::string::npos);
  }
}

TEST_CASE("compile mode reports clean and broken files", "[cli]") {
  Scratch scratch("compile");
  const fs::path good = scratch.path() / "good.cpp";
  const fs::path bad = scratch.path() / "bad.cpp";
  write(good, "int main() { return 0; }\n");
  write(bad, "int main() { int x = 1 return 0; }\n");

  SECTION("a clean file exits 0") {
    const Run run = run_cli({good.string()});
    CHECK(run.exit_code == 0);
    CHECK(run.output.find("clean") != std::string::npos);
  }

  SECTION("a broken file exits 1 and names the error") {
    const Run run = run_cli({bad.string()});
    CHECK(run.exit_code == 1);
    CHECK(run.output.find("[E1]") != std::string::npos);
    // The wording is compiler-specific -- clang says "expected ';' at end of
    // declaration", GCC "expected ',' or ';' before 'return'" -- so match the
    // shared part rather than one compiler's phrasing.
    CHECK(run.output.find("expected") != std::string::npos);
    CHECK(run.output.find("';'") != std::string::npos);
  }
}

TEST_CASE("outline mode describes the file without compiling", "[cli]") {
  Scratch scratch("outline");
  const fs::path source = scratch.path() / "shapes.cpp";
  write(source,
        "#include <vector>\n"
        "\n"
        "int first(int value) {\n"
        "  return value;\n"
        "}\n"
        "\n"
        "int second(int value) {\n"
        "  return value + 1;\n"
        "}\n");

  const Run run = run_cli({source.string(), "--outline"});
  CHECK(run.exit_code == 0);
  CHECK(run.output.find("2 functions") != std::string::npos);
  CHECK(run.output.find("first") != std::string::npos);
  CHECK(run.output.find("second") != std::string::npos);
  CHECK(run.output.find("<vector>") != std::string::npos);
  CHECK(run.output.find("no syntax errors") != std::string::npos);
}

TEST_CASE("--no-write leaves the source byte-identical", "[cli]") {
  Scratch scratch("nowrite");
  const fs::path source = scratch.path() / "victim.cpp";
  const std::string original = "int main() { int x = 1 return x; }\n";
  write(source, original);

  const Run run = run_cli({source.string(), "--agent", "--llm", "mock", "--no-write"});
  CHECK(run.exit_code == 1);  // the mock cannot repair this one

  std::ifstream in(source, std::ios::binary);
  std::ostringstream buffer;
  buffer << in.rdbuf();
  CHECK(buffer.str() == original);
}

TEST_CASE("--no-timing keeps the summary free of wall clock values", "[cli]") {
  Scratch scratch("timing");
  const fs::path source = scratch.path() / "clean.cpp";
  write(source, "int main() { return 0; }\n");

  const Run with_timing = run_cli({source.string()});
  const Run without_timing = run_cli({source.string(), "--no-timing"});
  CHECK(with_timing.exit_code == 0);
  CHECK(without_timing.exit_code == 0);
  CHECK(without_timing.output.find("s)") == std::string::npos);
}

TEST_CASE("include paths and defines reach the compiler", "[cli]") {
  Scratch scratch("flags");
  fs::create_directories(scratch.path() / "include");
  write(scratch.path() / "include" / "math_utils.h", "#pragma once\nint add(int a, int b);\n");
  const fs::path source = scratch.path() / "main.cpp";
  write(source, "#include \"math_utils.h\"\n\nint main() {\n  return add(1, 2)\n}\n");

  // The wording differs: clang says "file not found", GCC "No such file or
  // directory".  Assert the fault, not one compiler's phrasing.
  const auto header_missing = [](const std::string& output) {
    return output.find("file not found") != std::string::npos ||
           output.find("No such file or directory") != std::string::npos;
  };

  SECTION("without -I the header cannot be found") {
    const Run run = run_cli({source.string()});
    CHECK(run.exit_code == 1);
    CHECK(header_missing(run.output));
  }

  SECTION("-I DIR resolves the header and exposes the real error") {
    const Run run = run_cli({source.string(), "-I", (scratch.path() / "include").string()});
    CHECK(run.exit_code == 1);
    CHECK_FALSE(header_missing(run.output));
    CHECK(run.output.find("-I" + (scratch.path() / "include").string()) != std::string::npos);
    // The genuine fault must be reported now.
    CHECK(run.output.find("';'") != std::string::npos);
  }

  SECTION("the joined -IDIR form works too") {
    const Run run = run_cli({source.string(), "-I" + (scratch.path() / "include").string()});
    CHECK_FALSE(header_missing(run.output));
  }

  SECTION("--flag passes anything verbatim") {
    const Run run = run_cli({source.string(), "--flag",
                             "-I" + (scratch.path() / "include").string()});
    CHECK_FALSE(header_missing(run.output));
  }

  SECTION("-D defines a symbol") {
    const fs::path guarded = scratch.path() / "guarded.cpp";
    write(guarded, "#ifdef USE_ALT\nint alt() { return 1; }\n#else\n#error USE_ALT is not defined\n#endif\n");
    const Run without = run_cli({guarded.string()});
    CHECK(without.exit_code == 1);
    const Run with = run_cli({guarded.string(), "-D", "USE_ALT"});
    CHECK(with.exit_code == 0);
  }
}
