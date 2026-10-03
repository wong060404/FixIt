// expected_artifacts.test.cpp -- keeps examples/buggy/expected/ honest.
//
// Every example ships two artefacts:
//   <name>.expected.cpp     the content the mock loop reaches
//   <name>.trajectory.json  {success, iterations, errors before/after each round}
//
// The artefact *shape* is asserted here.  The trajectory is compared with a live
// run for the things that are compiler-independent (success and the error counts
// the model was shown), while the iteration count is only checked to be within
// the loop's own budget: how many rounds a compiler needs is a property of that
// compiler's error recovery, not of FixIt.

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "fixit/agent.h"
#include "fixit/compiler.h"

namespace {

namespace fs = std::filesystem;

std::string slurp(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream os;
  os << in.rdbuf();
  return os.str();
}

fs::path examples_dir() { return fs::path(FIXIT_EXAMPLES_DIR) / "buggy"; }

struct Example {
  std::string name;
  bool should_succeed;
  int initial_errors;
};

const std::vector<Example>& examples() {
  static const std::vector<Example> kExamples = {
      {"e1_missing_include", true, 5},
      {"e2_drift", true, 5},
      {"e3_type_error", false, 2},
  };
  return kExamples;
}

}  // namespace

TEST_CASE("every example ships a trajectory and an expected file", "[expected][artifacts]") {
  for (const Example& example : examples()) {
    DYNAMIC_SECTION(example.name) {
      const fs::path trajectory = examples_dir() / "expected" / (example.name + ".trajectory.json");
      const fs::path expected_cpp = examples_dir() / "expected" / (example.name + ".expected.cpp");
      REQUIRE(fs::exists(trajectory));
      REQUIRE(fs::exists(expected_cpp));

      const nlohmann::json parsed = nlohmann::json::parse(slurp(trajectory), nullptr, false);
      REQUIRE_FALSE(parsed.is_discarded());

      // Schema
      CHECK(parsed.value("success", !example.should_succeed) == example.should_succeed);
      CHECK(parsed.value("exit_code", -1) == (example.should_succeed ? 0 : 1));
      CHECK(parsed.contains("iterations"));
      CHECK(parsed.contains("initial_errors"));
      CHECK(parsed.contains("final_errors"));
      CHECK(parsed["rounds"].is_array());

      // The first round shows the model the errors the compiler really produced.
      CHECK(parsed.value("initial_errors", -1) == example.initial_errors);
      REQUIRE_FALSE(parsed["rounds"].empty());
      CHECK(parsed["rounds"][0].value("errors_before", -1) == example.initial_errors);

      // Success means the last verification compile was clean.
      if (example.should_succeed) {
        CHECK(parsed.value("final_errors", -1) == 0);
      } else {
        CHECK(parsed.value("final_errors", 0) > 0);
      }

      // Round to round the reported error count never increases.
      int previous = example.initial_errors;
      for (const nlohmann::json& round : parsed["rounds"]) {
        REQUIRE(round.contains("tool_calls"));
        CHECK(round["tool_calls"].is_array());
        const int before = round.value("errors_before", -1);
        const int after = round.value("errors_after", -1);
        CHECK(before == previous);
        if (example.should_succeed) CHECK(after <= before);
        previous = after;
      }
    }
  }
}

TEST_CASE("the recorded expected content is what the mock loop produces", "[expected][artifacts]") {
  for (const Example& example : examples()) {
    DYNAMIC_SECTION(example.name) {
      const fs::path work = fs::temp_directory_path() / ("fixit-expected-" + example.name);
      fs::remove_all(work);
      fs::create_directories(work);
      fs::copy_file(examples_dir() / (example.name + ".cpp"), work / (example.name + ".cpp"),
                    fs::copy_options::overwrite_existing);

      fixit::CompilerConfig config;
#ifdef FIXIT_TEST_COMPILER
      config.compiler = FIXIT_TEST_COMPILER;
#endif
      fixit::Agent agent(fixit::make_standard_tools(work.string()),
                         std::make_unique<fixit::MockLlm>(), fixit::Compiler(config), work.string());
      const fixit::AgentResult result = agent.run(example.name + ".cpp", 4);

      CHECK(result.success == example.should_succeed);
      CHECK(result.errors_fixed == example.initial_errors);
      CHECK(slurp(work / (example.name + ".cpp")) ==
            slurp(examples_dir() / "expected" / (example.name + ".expected.cpp")));

      // The recorded budget is the loop's own default.
      const nlohmann::json trajectory =
          nlohmann::json::parse(slurp(examples_dir() / "expected" /
                                      (example.name + ".trajectory.json")),
                                nullptr, false);
      REQUIRE_FALSE(trajectory.is_discarded());
      CHECK(trajectory.value("iterations", 0) >= 1);
      CHECK(trajectory.value("iterations", 0) <= 4);

      std::error_code ignored;
      fs::remove_all(work, ignored);
    }
  }
}
