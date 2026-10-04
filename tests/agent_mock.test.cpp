// agent_mock.test.cpp -- the end-to-end integration gate.
//
// These tests run the *real* compiler on the *real* example files with the
// deterministic MockLlm, and assert the full loop: compile -> locate -> patch ->
// re-verify, plus the trace and metrics a front end consumes.
//
// Everything is offline.  Two runs of the same input must produce byte-identical
// traces, which is what makes the demo reproducible.

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "fixit/agent.h"
#include "fixit/compiler.h"
#include "fixit/patch.h"

namespace {

namespace fs = std::filesystem;

std::string example_path(const std::string& name) {
  return std::string(FIXIT_EXAMPLES_DIR) + "/buggy/" + name;
}

std::string slurp(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream os;
  os << in.rdbuf();
  return os.str();
}

void write_file(const fs::path& path, const std::string& content) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << content;
}

/// A scratch directory that is unique per test case and removed afterwards.
class Scratch {
 public:
  explicit Scratch(const std::string& label) {
    static int counter = 0;
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = fs::temp_directory_path() /
            ("fixit-test-" + label + "-" + std::to_string(stamp) + "-" + std::to_string(++counter));
    fs::remove_all(path_);
    fs::create_directories(path_);
  }
  ~Scratch() {
    std::error_code ignored;
    fs::remove_all(path_, ignored);
  }
  Scratch(const Scratch&) = delete;
  Scratch& operator=(const Scratch&) = delete;

  const fs::path& path() const { return path_; }

  fs::path seed(const std::string& example) {
    const fs::path target = path_ / example;
    write_file(target, slurp(example_path(example)));
    return target;
  }

 private:
  fs::path path_;
};

/// A single compiler for the whole suite: constructing one probes the dialect,
/// and every case here would otherwise pay that cost again.
const fixit::Compiler& shared_compiler() {
  static const fixit::Compiler compiler = [] {
    fixit::CompilerConfig config;
#ifdef FIXIT_TEST_COMPILER
    config.compiler = FIXIT_TEST_COMPILER;
#endif
    return fixit::Compiler(config);
  }();
  return compiler;
}

fixit::Compiler make_compiler() { return shared_compiler(); }

struct RunOutcome {
  fixit::AgentResult result;
  std::string final_content;
  std::string trace_text;
  nlohmann::json trace;
};

RunOutcome run_agent(const std::string& example, const fs::path& scratch, int iterations = 4) {
  RunOutcome outcome;
  const fs::path file = scratch / example;
  const fs::path trace_path = scratch / "trace.json";

  fixit::Agent agent(fixit::make_standard_tools(scratch.string(), make_compiler()),
                     std::make_unique<fixit::MockLlm>(), make_compiler(), scratch.string());
  agent.set_trace_path(trace_path.string());
  outcome.result = agent.run(example, iterations);
  outcome.final_content = slurp(file);
  outcome.trace_text = slurp(trace_path);
  outcome.trace = nlohmann::json::parse(outcome.trace_text, nullptr, false);
  return outcome;
}

/// True when the compiler is happy with `path`.
bool compiles_clean(const fs::path& path) {
  const fixit::CompileResult result = shared_compiler().compile(path.string());
  return result.clean();
}

/// Validates the trace schema the README documents.
void check_trace_shape(const nlohmann::json& trace, int expected_rounds) {
  REQUIRE(trace.is_array());
  CHECK(static_cast<int>(trace.size()) == expected_rounds);
  int last_round = 0;
  for (const nlohmann::json& entry : trace) {
    REQUIRE(entry.is_object());
    CHECK(entry.contains("round"));
    CHECK(entry.contains("tool_calls"));
    CHECK(entry["tool_calls"].is_array());
    const int round = entry.value("round", 0);
    CHECK(round == last_round + 1);  // rounds are 1,2,3...
    last_round = round;
    if (entry.contains("observations")) {
      REQUIRE(entry["observations"].is_array());
      for (const nlohmann::json& observation : entry["observations"]) {
        CHECK(observation.contains("tool"));
        CHECK(observation.contains("result"));
      }
    }
  }
}

}  // namespace

TEST_CASE("e1 is repaired by the mock agent", "[agent][integration]") {
  Scratch scratch("e1");
  scratch.seed("e1_missing_include.cpp");
  RunOutcome outcome = run_agent("e1_missing_include.cpp", scratch.path());

  CHECK(outcome.result.success);
  CHECK(outcome.result.iterations >= 2);
  CHECK(outcome.result.iterations <= 4);
  CHECK(outcome.result.final_errors.empty());
  CHECK(outcome.result.patches.size() == static_cast<std::size_t>(outcome.result.iterations));

  // The shared library and the missing semicolon are both repaired.
  CHECK(outcome.final_content.find("#include <vector>") != std::string::npos);
  CHECK(outcome.final_content.find("  int n = 3;") != std::string::npos);
  CHECK(compiles_clean(scratch.path() / "e1_missing_include.cpp"));

  // Every patch in the loop passed the gate.
  for (const auto& [round, patch] : outcome.result.patches) {
    CHECK(round >= 1);
    CHECK(patch.all_applied);
  }
  check_trace_shape(outcome.trace, outcome.result.iterations);
}

TEST_CASE("e2 tolerates 50 lines of drift", "[agent][integration]") {
  Scratch scratch("e2");
  scratch.seed("e2_drift.cpp");
  RunOutcome outcome = run_agent("e2_drift.cpp", scratch.path());

  CHECK(outcome.result.success);
  CHECK(outcome.final_content.find("#include <vector>") != std::string::npos);
  CHECK(outcome.final_content.find("  int n = 3;") != std::string::npos);
  CHECK(compiles_clean(scratch.path() / "e2_drift.cpp"));

  // At least one hunk must have been applied away from its declared position,
  // which is the whole point of this fixture.
  bool saw_fuzzy = false;
  for (const auto& [round, patch] : outcome.result.patches) {
    (void)round;
    for (const fixit::HunkReport& report : patch.reports) {
      if (report.status == fixit::HunkReport::Status::FuzzyApplied) saw_fuzzy = true;
    }
  }
  CHECK(saw_fuzzy);
  check_trace_shape(outcome.trace, outcome.result.iterations);
}

TEST_CASE("e3 is left unfixed without crashing", "[agent][integration]") {
  Scratch scratch("e3");
  scratch.seed("e3_type_error.cpp");
  const std::string before = slurp(scratch.path() / "e3_type_error.cpp");
  RunOutcome outcome = run_agent("e3_type_error.cpp", scratch.path());

  CHECK_FALSE(outcome.result.success);
  CHECK_FALSE(outcome.result.final_errors.empty());
  // The mock has no rule for these errors, so it must not have touched the file.
  CHECK(outcome.final_content == before);
  // ... and it must still have written a well-formed trace.
  check_trace_shape(outcome.trace, outcome.result.iterations);
}

TEST_CASE("the agent is deterministic", "[agent][integration]") {
  const std::string first = [&] {
    Scratch scratch("det1");
    scratch.seed("e1_missing_include.cpp");
    return run_agent("e1_missing_include.cpp", scratch.path()).trace_text;
  }();
  const std::string second = [&] {
    Scratch scratch("det2");
    scratch.seed("e1_missing_include.cpp");
    return run_agent("e1_missing_include.cpp", scratch.path()).trace_text;
  }();
  // Same input, same compiler, same mock: byte-identical trace.
  CHECK(first == second);
  CHECK_FALSE(first.empty());
}

TEST_CASE("a clean file short-circuits the loop", "[agent][integration]") {
  Scratch scratch("clean");
  const fs::path file = scratch.path() / "clean.cpp";
  write_file(file, "int main() { return 0; }\n");

  fixit::Agent agent(fixit::make_standard_tools(scratch.path().string(), make_compiler()),
                     std::make_unique<fixit::MockLlm>(), make_compiler(), scratch.path().string());
  const fixit::AgentResult result = agent.run("clean.cpp", 4);
  CHECK(result.success);
  CHECK(result.iterations == 0);  // no model call was needed
  CHECK(result.patches.empty());
}

TEST_CASE("tool registry exposes and dispatches the standard tools", "[agent][tools]") {
  Scratch scratch("tools");
  const fs::path file = scratch.path() / "tool_target.cpp";
  write_file(file, "one\ntwo\nthree\n");

  const fixit::ToolRegistry registry = fixit::make_standard_tools(scratch.path().string(), shared_compiler());
  const std::vector<fixit::ToolSpec> specs = registry.specs();
  REQUIRE(specs.size() == 3);
  CHECK(registry.has("compile"));
  CHECK(registry.has("read"));
  CHECK(registry.has("patch"));
  for (const fixit::ToolSpec& spec : specs) {
    CHECK_FALSE(spec.description.empty());
    const nlohmann::json schema = nlohmann::json::parse(spec.json_schema, nullptr, false);
    CHECK_FALSE(schema.is_discarded());
  }

  SECTION("read returns numbered lines") {
    const nlohmann::json result =
        registry.call("read", nlohmann::json{{"file", "tool_target.cpp"}, {"start", 2}, {"end", 3}});
    REQUIRE(result.contains("content"));
    CHECK(result["content"].get<std::string>() == "   2 | two\n   3 | three\n");
  }

  SECTION("patch writes the file and reports the hunks") {
    const nlohmann::json result = registry.call(
        "patch",
        nlohmann::json{{"file", "tool_target.cpp"},
                       {"diff", "--- a/tool_target.cpp\n+++ b/tool_target.cpp\n"
                                "@@ -2,1 +2,1 @@\n-two\n+TWO\n"}});
    CHECK(result.value("all_applied", false));
    CHECK(slurp(file) == "one\nTWO\nthree\n");
    REQUIRE(result.contains("reports"));
    CHECK(result["reports"][0].value("status", std::string()) == "applied");
  }

  SECTION("unknown tools and bad arguments return errors, never throw") {
    CHECK(registry.call("nope", nlohmann::json::object()).contains("error"));
    CHECK(registry.call("read", nlohmann::json{{"file", "missing.cpp"}}).contains("error"));
    CHECK(registry.call("read", nlohmann::json{{"file", "../escape.cpp"}}).contains("error"));
  }
}

TEST_CASE("an unusable compiler fails loudly and quickly", "[agent][negative]") {
  Scratch scratch("broken-compiler");
  const fs::path file = scratch.path() / "victim.cpp";
  write_file(file, "int main() { return 0 }\n");

  fixit::CompilerConfig config;
  config.compiler = "/nonexistent/fixit-no-such-compiler";
  fixit::Agent agent(fixit::make_standard_tools(scratch.path().string(), fixit::Compiler(config)),
                     std::make_unique<fixit::MockLlm>(), fixit::Compiler(config),
                     scratch.path().string());
  agent.set_trace_path((scratch.path() / "trace.json").string());

  const fixit::AgentResult result = agent.run("victim.cpp", 4);

  // No repair, but also no crash and no busy loop: the mock recognises that the
  // compiler itself is the problem and stops.
  CHECK_FALSE(result.success);
  CHECK(result.iterations <= 2);
  CHECK(result.patches.empty());
  // The file is untouched.
  CHECK(slurp(file) == "int main() { return 0 }\n");

  // The trace is still well formed and explains the situation.
  const nlohmann::json trace =
      nlohmann::json::parse(slurp(scratch.path() / "trace.json"), nullptr, false);
  REQUIRE(trace.is_array());
  REQUIRE_FALSE(trace.empty());
  REQUIRE(trace[0].contains("compile_before"));
  CHECK(trace[0]["compile_before"].value("clean", true) == false);
  CHECK(trace[0]["compile_before"].value("exit_code", 0) == 127);
}
