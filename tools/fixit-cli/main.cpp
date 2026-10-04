// fixit-cli -- the command line front end described in the README.
//
// Exit codes: 0 clean/fixed, 1 not fixed, 2 usage error, 3 internal error.

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <unistd.h>

#include "fixit/agent.h"
#include "fixit/codemap.h"
#include "fixit/compiler.h"
#include "fixit/patch.h"

namespace {

constexpr const char* kVersion = "0.1.0";

// ---------------------------------------------------------------------------
// ANSI helpers.  Colour is disabled when stdout is not a TTY or NO_COLOR is
// set, so docs/demo_output.txt stays diffable.
// ---------------------------------------------------------------------------
bool g_colour = true;

std::string red(const std::string& s) { return g_colour ? "\x1b[31m" + s + "\x1b[0m" : s; }
std::string green(const std::string& s) { return g_colour ? "\x1b[32m" + s + "\x1b[0m" : s; }
std::string yellow(const std::string& s) { return g_colour ? "\x1b[33m" + s + "\x1b[0m" : s; }

std::string basename_of(const std::string& path) {
  const std::size_t slash = path.find_last_of("/\\");
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string dirname_of(const std::string& path) {
  const std::size_t slash = path.find_last_of("/\\");
  return slash == std::string::npos ? std::string(".") : path.substr(0, slash);
}

std::string fixed1(double value) {
  std::ostringstream os;
  os.setf(std::ios::fixed);
  os.precision(1);
  os << value;
  return os.str();
}

struct Options {
  std::string file;
  bool agent = false;
  std::string llm = "mock";
  std::string model = "gpt-4o-mini";
  std::string base_url = "https://api.openai.com/v1";
  std::string api_key;
  int iterations = 4;
  bool verbose = false;
  std::string trace;
  std::string metrics;
  std::string compiler = "g++";
  bool no_write = false;  ///< repair a scratch copy and leave the tree untouched
  bool show_timing = true;  ///< --no-timing keeps the transcript byte-reproducible
};

void usage() {
  std::cout <<
      R"(Usage: fixit <file.cpp> [options]

  --agent                 run the repair loop instead of a single compile
  --llm mock|openai       model backend (default: mock)
  --model NAME            model name for --llm openai
  --base-url URL          OpenAI-compatible base URL
  --api-key KEY           API key (or set FIXIT_API_KEY)
  --iterations N          maximum repair rounds (default: 4)
  --verbose               stream compile/patch progress
  --trace PATH            write the deterministic JSON trace
  --metrics PATH          write patch/fix statistics as JSON
  --compiler NAME         compiler binary (default: g++)
  --no-write              repair a scratch copy; the source file is left as is
                          (useful for repeatable demos and CI smoke tests)
  --no-timing             omit wall-clock times, making the output byte-identical
                          across runs (used by tools/record_demo.sh)
  -h, --help              this message

Exit codes: 0 clean or fixed, 1 not fixed, 2 usage error, 3 internal error.
)";
}

std::optional<Options> parse_args(int argc, char** argv, int& exit_code) {
  Options options;
  std::vector<std::string> positional;

  const auto needs_value = [&](int& i, const char* flag) -> std::optional<std::string> {
    if (i + 1 >= argc) {
      std::cerr << "fixit: " << flag << " requires a value\n";
      exit_code = 2;
      return std::nullopt;
    }
    return std::string(argv[++i]);
  };

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      usage();
      exit_code = 0;
      return std::nullopt;
    }
    if (arg == "--agent") {
      options.agent = true;
    } else if (arg == "--no-write") {
      options.no_write = true;
    } else if (arg == "--no-timing") {
      options.show_timing = false;
    } else if (arg == "--verbose") {
      options.verbose = true;
    } else if (arg == "--llm") {
      const auto value = needs_value(i, "--llm");
      if (!value) return std::nullopt;
      options.llm = *value;
    } else if (arg == "--model") {
      const auto value = needs_value(i, "--model");
      if (!value) return std::nullopt;
      options.model = *value;
    } else if (arg == "--base-url") {
      const auto value = needs_value(i, "--base-url");
      if (!value) return std::nullopt;
      options.base_url = *value;
    } else if (arg == "--api-key") {
      const auto value = needs_value(i, "--api-key");
      if (!value) return std::nullopt;
      options.api_key = *value;
    } else if (arg == "--iterations") {
      const auto value = needs_value(i, "--iterations");
      if (!value) return std::nullopt;
      try {
        options.iterations = std::stoi(*value);
      } catch (...) {
        std::cerr << "fixit: --iterations expects an integer\n";
        exit_code = 2;
        return std::nullopt;
      }
      if (options.iterations < 1) {
        std::cerr << "fixit: --iterations must be >= 1\n";
        exit_code = 2;
        return std::nullopt;
      }
    } else if (arg == "--trace") {
      const auto value = needs_value(i, "--trace");
      if (!value) return std::nullopt;
      options.trace = *value;
    } else if (arg == "--metrics") {
      const auto value = needs_value(i, "--metrics");
      if (!value) return std::nullopt;
      options.metrics = *value;
    } else if (arg == "--compiler") {
      const auto value = needs_value(i, "--compiler");
      if (!value) return std::nullopt;
      options.compiler = *value;
    } else if (!arg.empty() && arg[0] == '-' && arg != "-") {
      std::cerr << "fixit: unknown option '" << arg << "'\n";
      exit_code = 2;
      return std::nullopt;
    } else {
      positional.push_back(arg);
    }
  }

  if (positional.size() != 1) {
    std::cerr << "fixit: exactly one source file is required\n";
    exit_code = 2;
    return std::nullopt;
  }
  options.file = positional.front();
  if (options.api_key.empty()) {
    if (const char* env = std::getenv("FIXIT_API_KEY"); env != nullptr) options.api_key = env;
  }
  return options;
}

/// Aggregated numbers for --metrics.
struct Metrics {
  int exact = 0;
  int drift = 0;
  int whitespace = 0;
  int missing_context = 0;
  int total_hunks = 0;
  int failed_hunks = 0;
  int applied = 0;
};

void classify(const fixit::HunkReport& report, Metrics& metrics) {
  ++metrics.total_hunks;
  if (report.status == fixit::HunkReport::Status::Applied) {
    ++metrics.exact;
    ++metrics.applied;
    return;
  }
  if (report.status == fixit::HunkReport::Status::FuzzyApplied) {
    ++metrics.drift;
    ++metrics.applied;
    return;
  }
  ++metrics.failed_hunks;
  if (report.failure_reason.find("exact 0") != std::string::npos) {
    ++metrics.missing_context;
  } else {
    ++metrics.whitespace;
  }
}

void write_metrics(const std::string& path, const Metrics& metrics, bool success, int iterations,
                   double seconds) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    std::cerr << "fixit: cannot write metrics to " << path << "\n";
    return;
  }
  std::ostringstream os;
  os.setf(std::ios::fixed);
  os.precision(3);
  os << "{\n";
  os << "  \"patch_success\": {\"exact\": " << metrics.exact << ", \"drift\": " << metrics.drift
     << ", \"whitespace\": " << metrics.whitespace << ", \"missing_ctx\": " << metrics.missing_context
     << "},\n";
  os << "  \"total_hunks\": " << metrics.total_hunks << ",\n";
  os << "  \"failed_hunks\": " << metrics.failed_hunks << ",\n";
  os << "  \"fix_rate\": " << (success ? "1.0" : "0.0") << ",\n";
  os << "  \"avg_iterations\": " << iterations << ",\n";
  os << "  \"total_time_ms\": " << static_cast<long long>(seconds * 1000.0) << "\n";
  os << "}\n";
  out << os.str();
}

void print_error_list(const std::vector<fixit::Diagnostic>& diagnostics, const std::string& indent) {
  std::size_t index = 0;
  for (const fixit::Diagnostic& d : diagnostics) {
    if (d.level != fixit::DiagLevel::Error) continue;
    std::ostringstream label;
    label << "[E" << ++index << "]";
    std::printf("%s%-6s L%-4d %s\n", indent.c_str(), label.str().c_str(), d.line, d.message.c_str());
  }
}

void print_errors(const fixit::CompileResult& result, const std::string& indent) {
  print_error_list(result.diagnostics, indent);
}

}  // namespace

int main(int argc, char** argv) {
  if (const char* no_colour = std::getenv("NO_COLOR"); no_colour != nullptr && *no_colour != '\0') {
    g_colour = false;
  } else if (::isatty(fileno(stdout)) == 0) {
    g_colour = false;
  }

  int exit_code = 3;
  const std::optional<Options> parsed = parse_args(argc, argv, exit_code);
  if (!parsed) return exit_code;
  const Options options = *parsed;

  std::string original_content;
  {
    std::ifstream probe(options.file, std::ios::binary);
    if (!probe) {
      std::cerr << "fixit: cannot open '" << options.file << "'\n";
      return 2;
    }
    std::ostringstream buffer;
    buffer << probe.rdbuf();
    original_content = buffer.str();
  }

  if (options.llm == "openai" && options.api_key.empty()) {
    std::cerr << "fixit: --llm openai requires --api-key or the FIXIT_API_KEY environment "
                 "variable\n";
    return 2;
  }
  if (options.llm != "mock" && options.llm != "openai") {
    std::cerr << "fixit: unknown --llm value '" << options.llm << "' (expected mock or openai)\n";
    return 2;
  }

  fixit::CompilerConfig config;
  config.compiler = options.compiler;
  const fixit::Compiler compiler(config);

  // --no-write repairs a private copy in a scratch directory, so the working
  // tree is byte-identical afterwards and the demo can be repeated verbatim.
  std::string scratch_dir;
  std::string target_file = options.file;
  if (options.no_write) {
    std::error_code error;
    const std::filesystem::path scratch =
        std::filesystem::temp_directory_path(error) /
        ("fixit-no-write-" + std::to_string(::getpid()));
    std::filesystem::create_directories(scratch, error);
    std::filesystem::copy_file(options.file, scratch / basename_of(options.file),
                               std::filesystem::copy_options::overwrite_existing, error);
    scratch_dir = scratch.string();
    target_file = (scratch / basename_of(options.file)).string();
  }
  const auto cleanup_scratch = [&] {
    if (!scratch_dir.empty()) {
      std::error_code error;
      std::filesystem::remove_all(scratch_dir, error);
    }
  };

  const std::string display_file = basename_of(options.file);
  const std::string workdir = options.no_write ? scratch_dir : dirname_of(options.file);

  const auto started = std::chrono::steady_clock::now();
  const auto elapsed = [&] {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
  };

  // ---- compile-only mode ---------------------------------------------------
  if (!options.agent) {
    const fixit::CompileResult result = compiler.compile(target_file);
    cleanup_scratch();
    std::cout << "══ FixIt v" << kVersion << " · compile mode (" << options.compiler << ") ══\n\n";
    std::cout << "$ " << options.compiler << " -std=c++20 -fsyntax-only " << display_file << "\n";
    if (result.clean()) {
      std::cout << "  " << green("✓ clean") << "\n";
      return 0;
    }
    std::cout << "  " << red("✗ " + std::to_string(result.error_count()) +
                             (result.error_count() == 1 ? " error" : " errors"))
              << "\n";
    print_errors(result, "    ");
    return 1;
  }

  // ---- agent mode ----------------------------------------------------------
  std::cout << "══ FixIt v" << kVersion << " · agent mode (" << options.llm << ") ══\n\n";

  std::unique_ptr<fixit::Llm> llm;
  if (options.llm == "mock") {
    llm = std::make_unique<fixit::MockLlm>();
  } else {
    if (!fixit::openai_tls_available() && options.base_url.rfind("https://", 0) == 0) {
      std::cerr << "fixit: this build has no TLS support; point --base-url at an http:// "
                   "OpenAI-compatible server or rebuild with -DFIXIT_ENABLE_OPENSSL=ON\n";
      return 2;
    }
    llm = std::make_unique<fixit::OpenAiLlm>(options.base_url, options.api_key, options.model);
  }

  // The tools share the loop's compiler, so a `compile` call from the model and
  // the agent's own verification can never disagree.
  fixit::ToolRegistry tools = fixit::make_standard_tools(workdir, compiler);
  fixit::Agent agent(std::move(tools), std::move(llm), compiler, workdir);
  agent.set_trace_path(options.trace);

  Metrics metrics;
  // One compile per iteration is printed.  The engine re-compiles to verify a
  // patch, which can report the identical state; that duplicate is folded into
  // the next iteration heading instead of being printed twice.
  int printed_round = -1;
  std::string last_fingerprint;
  const auto fingerprint_of = [](const fixit::CompileResult& result) {
    std::ostringstream os;
    os << result.exit_code << '|' << (result.clean() ? 1 : 0);
    for (const fixit::Diagnostic& d : result.diagnostics) {
      os << '|' << d.line << ':' << d.col << ':' << static_cast<int>(d.level) << ':' << d.message;
    }
    return os.str();
  };
  agent.set_observer([&](const fixit::Agent::Event& event) {
    if (!options.verbose) return;  // metrics are collected after the run

    if (event.kind == fixit::Agent::Event::Kind::Compile) {
      const std::string fingerprint = fingerprint_of(event.compile);
      if (event.verification) {
        // Terminal confirmation: report the outcome without pretending a new
        // repair round happened.
        if (fingerprint == last_fingerprint) return;
        last_fingerprint = fingerprint;
        std::cout << "$ " << options.compiler << " -fsyntax-only " << display_file << "\n";
        if (event.compile.clean()) {
          std::cout << "  " << green("✓ clean") << "\n";
        } else {
          const std::size_t errors = event.compile.error_count();
          std::cout << "  " << red("✗ " + std::to_string(errors) +
                                   (errors == 1 ? " error" : " errors"))
                    << "\n";
          print_errors(event.compile, "    ");
        }
        return;
      }
      const int heading = event.round == 0 ? 1 : event.round;
      if (heading == printed_round && fingerprint == last_fingerprint) return;
      if (heading != printed_round) {
        printed_round = heading;
        std::cout << "── iteration " << heading << " " << std::string(28, '-') << "\n";
      }
      last_fingerprint = fingerprint;

      std::cout << "$ " << options.compiler << " -fsyntax-only " << display_file << "\n";
      if (event.compile.clean()) {
        std::cout << "  " << green("✓ clean") << "\n";
        return;
      }
      const std::size_t errors = event.compile.error_count();
      std::cout << "  " << red("✗ " + std::to_string(errors) + (errors == 1 ? " error" : " errors"))
                << "\n";
      print_errors(event.compile, "    ");

      if (!event.compile.diagnostics.empty()) {
        fixit::CodeMap map(target_file);
        if (const auto fn = map.enclosing_function(event.compile.diagnostics.front().line, 1)) {
          std::cout << "  " << yellow("→ context: fn " + fn->name + "() [L" +
                                      std::to_string(fn->start_line) + "–L" +
                                      std::to_string(fn->end_line) + "]")
                    << "\n";
        }
      }
      return;
    }

    // Patch event
    std::cout << "  " << yellow("→ patch…") << "\n";
    for (const fixit::HunkReport& report : event.patch.reports) {
      classify(report, metrics);
      if (report.status == fixit::HunkReport::Status::Applied) {
        std::cout << "    " << green("✓ hunk " + std::to_string(report.hunk_index) + " @ L" +
                                     std::to_string(report.matched_pos))
                  << "    (exact)\n";
      } else if (report.status == fixit::HunkReport::Status::FuzzyApplied) {
        const int drift = report.matched_pos - report.declared_pos;
        std::cout << "    " << green("✓ hunk " + std::to_string(report.hunk_index) + " @ L" +
                                     std::to_string(report.matched_pos))
                  << "   (fuzzy, drift" << (drift >= 0 ? "+" : "") << drift << ")\n";
      } else {
        std::cout << "    " << red("✗ hunk " + std::to_string(report.hunk_index) + " failed")
                  << "\n";
        std::cout << "      " << report.failure_reason << "\n";
      }
    }
  });

  const fixit::AgentResult result = agent.run(display_file, options.iterations);
  cleanup_scratch();

  // Single source of truth for the counters: the patches the loop recorded.
  for (const auto& [round, patch] : result.patches) {
    (void)round;
    for (const fixit::HunkReport& report : patch.reports) classify(report, metrics);
  }

  if (!options.metrics.empty()) {
    write_metrics(options.metrics, metrics, result.success, result.iterations, elapsed());
  }

  const std::string suffix =
      options.show_timing ? " (" + fixed1(elapsed()) + "s)" : std::string();
  if (result.success) {
    std::cout << green("✔ Fixed " + std::to_string(result.errors_fixed) + " errors in " +
                       std::to_string(result.iterations) + " iterations" + suffix)
              << "\n";
    return 0;
  }

  std::cout << red("✗ Not fixed after " + std::to_string(result.iterations) + " iterations" +
                   suffix) << "\n";
  if (!result.final_errors.empty()) {
    std::cout << "  remaining errors:\n";
    print_error_list(result.final_errors, "    ");
  }
  return 1;
}
