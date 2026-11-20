// fixit-cli -- the command line front end described in the README.
//
// Exit codes: 0 clean/fixed, 1 not fixed, 2 usage error, 3 internal error.

#include <cstddef>
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

#ifdef _WIN32
// The POSIX spellings are not declared by the UCRT under -std=c++20; the
// underscore-prefixed equivalents are the same functions.
#include <io.h>
#include <process.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>  // CommandLineToArgvW
#define fixit_getpid ::_getpid
#define fixit_isatty ::_isatty
#define fixit_fileno ::_fileno
#else
#include <unistd.h>
#define fixit_getpid ::getpid
#define fixit_isatty ::isatty
#define fixit_fileno ::fileno
#endif

#include "fixit/agent.h"
#include "fixit/codemap.h"
#include "fixit/compiler.h"
#include "fixit/patch.h"
#include "fixit/platform.h"

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
  bool outline = false;     ///< print the CodeMap view of the file and stop
  /// Extra flags handed to the compiler verbatim (`-I`, `-D`, `-std=`, ...).
  /// Without this, any file that includes a project header fails with
  /// "'x.h' file not found" before FixIt can do anything.
  std::vector<std::string> extra_flags;
};

void usage(std::ostream& out) {
  out <<
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
  --outline               print the structural map (functions, includes, syntax
                          errors) instead of compiling
  -I DIR, --include DIR   add a compiler include path (repeatable)
  -D NAME[=VALUE]         define a preprocessor symbol (repeatable)
  --flag FLAG             pass any other compiler flag verbatim (repeatable)

  Example:
    fixit src/parser.cpp -Iinclude -Ithird_party/lib -DDEBUG=1
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
      usage(std::cout);
      exit_code = 0;
      return std::nullopt;
    }
    if (arg == "--agent") {
      options.agent = true;
    } else if (arg == "--no-write") {
      options.no_write = true;
    } else if (arg == "--no-timing") {
      options.show_timing = false;
    } else if (arg == "--outline") {
      options.outline = true;
    } else if (arg == "-I" || arg == "--include") {
      const auto value = needs_value(i, arg.c_str());
      if (!value) return std::nullopt;
      options.extra_flags.push_back("-I" + *value);
    } else if (arg.rfind("-I", 0) == 0 && arg.size() > 2) {
      options.extra_flags.push_back(arg);  // -Iinclude, the joined form
    } else if (arg == "-D" || arg == "--define") {
      const auto value = needs_value(i, arg.c_str());
      if (!value) return std::nullopt;
      options.extra_flags.push_back("-D" + *value);
    } else if (arg.rfind("-D", 0) == 0 && arg.size() > 2) {
      options.extra_flags.push_back(arg);
    } else if (arg == "--flag") {
      const auto value = needs_value(i, "--flag");
      if (!value) return std::nullopt;
      options.extra_flags.push_back(*value);
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
    // The caller prints the usage text to stderr; --help prints it to stdout.
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
  std::ofstream out(fixit::to_path(path), std::ios::binary | std::ios::trunc);
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

void print_error_list(const std::vector<fixit::Diagnostic>& diagnostics, const std::string& indent,
                      const std::string& target_basename) {
  std::size_t index = 0;
  for (const fixit::Diagnostic& d : diagnostics) {
    if (d.level != fixit::DiagLevel::Error) continue;
    std::ostringstream label;
    label << "[E" << ++index << "]";
    // A diagnostic can point into an included header.  Printing only the line
    // number would attribute it to the file named on the command line, and the
    // code map would then look for its enclosing function in the wrong file.
    if (!d.file.empty() && basename_of(d.file) != target_basename) {
      std::printf("%s%-6s %s:%d %s\n", indent.c_str(), label.str().c_str(),
                  basename_of(d.file).c_str(), d.line, d.message.c_str());
      continue;
    }
    std::printf("%s%-6s L%-4d %s\n", indent.c_str(), label.str().c_str(), d.line, d.message.c_str());
  }
}

void print_errors(const fixit::CompileResult& result, const std::string& indent,
                  const std::string& target_basename) {
  print_error_list(result.diagnostics, indent, target_basename);
}

}  // namespace

#ifdef _WIN32
/// Rebuilds argv as UTF-8 from the wide command line.
///
/// The CRT converts GetCommandLineW() into the ANSI code page before `main` sees
/// it, so on a CJK or accented Windows install every non-ASCII argument arrives
/// as mojibake -- and a path built from those bytes either throws inside
/// std::filesystem or names a file nobody asked for.  Recovering the UTF-16
/// command line is the only way to see the name the user actually typed.
/// The storage is static because the returned pointers are used until exit.
std::vector<char*>& utf8_argv_from_command_line(int& argc) {
  static std::vector<std::string> owned;
  static std::vector<char*> pointers;
  owned.clear();
  pointers.clear();
  int wide_argc = 0;
  LPWSTR* wide = ::CommandLineToArgvW(::GetCommandLineW(), &wide_argc);
  if (wide == nullptr) return pointers;
  owned.reserve(static_cast<std::size_t>(wide_argc));
  for (int i = 0; i < wide_argc; ++i) {
    const int length = ::WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, nullptr, 0, nullptr, nullptr);
    if (length <= 1) {
      owned.emplace_back();
      continue;
    }
    std::string argument(static_cast<std::size_t>(length), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, argument.data(), length, nullptr, nullptr);
    argument.resize(static_cast<std::size_t>(length) - 1);  // drop the terminator
    owned.push_back(argument);
  }
  ::LocalFree(wide);
  argc = wide_argc;
  for (std::string& argument : owned) pointers.push_back(argument.data());
  return pointers;
}

/// Sets the console code page to UTF-8 for the lifetime of the object and
/// restores the previous one afterwards.  FixIt's output is UTF-8 by
/// construction; on a CJK console it rendered as mojibake unless the user ran
/// `chcp 65001` first -- a step the manual had to spell out.
class ScopedConsoleUtf8 {
 public:
  ScopedConsoleUtf8() : previous_(::GetConsoleOutputCP()) { ::SetConsoleOutputCP(CP_UTF8); }
  ~ScopedConsoleUtf8() {
    if (previous_ != 0) ::SetConsoleOutputCP(previous_);
  }
  ScopedConsoleUtf8(const ScopedConsoleUtf8&) = delete;
  ScopedConsoleUtf8& operator=(const ScopedConsoleUtf8&) = delete;

 private:
  UINT previous_;
};
#endif  // _WIN32

int main(int argc, char** argv) {
#ifdef _WIN32
  {
    int wide_argc = argc;
    std::vector<char*>& recovered = utf8_argv_from_command_line(wide_argc);
    if (!recovered.empty()) {
      argv = recovered.data();
      argc = wide_argc;
    }
  }
#endif
  // Exit code 3 is documented as "internal error".  Until now nothing could
  // return it, so a std::bad_alloc or a library type_error escaped main and the
  // process died with a signal instead -- indistinguishable from a crash caused
  // by the user's own file.  Report it as the documented code.
  try {
#ifdef _WIN32
    const ScopedConsoleUtf8 console_utf8;
#endif

    if (const char* no_colour = std::getenv("NO_COLOR"); no_colour != nullptr && *no_colour != '\0') {
      g_colour = false;
    } else if (fixit_isatty(fixit_fileno(stdout)) == 0) {
      g_colour = false;
    }

    int exit_code = 3;
    const std::optional<Options> parsed = parse_args(argc, argv, exit_code);
    if (!parsed) {
      // A usage mistake deserves the usage text, not just one line of complaint.
      if (exit_code == 2) usage(std::cerr);
      return exit_code;
    }
    const Options options = *parsed;

    std::string original_content;
    {
      // A directory opens successfully with std::ifstream, and the compiler then
      // reports nothing for it -- which used to read as "clean".  Require a
      // regular file so an obvious typo can never look like a passing build.
      std::error_code error;
      if (!std::filesystem::is_regular_file(fixit::to_path(options.file), error)) {
        std::cerr << "fixit: '" << options.file << "' is not a readable file\n";
        return 2;
      }
      std::ifstream probe(fixit::to_path(options.file), std::ios::binary);
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

    // Flags go to the compiler *and* to the echoed command line, so the two can
    // never disagree.
    std::vector<std::string> extra_flags = options.extra_flags;

    // --no-write repairs a private copy in a scratch directory, so the working
    // tree is byte-identical afterwards and the demo can be repeated verbatim.
    std::string scratch_dir;
    std::string target_file = options.file;
    if (options.no_write) {
      std::error_code error;
      const std::filesystem::path scratch =
          std::filesystem::temp_directory_path(error) /
          fixit::to_path("fixit-no-write-" + std::to_string(fixit_getpid()));
      std::filesystem::create_directories(scratch, error);
      const std::filesystem::path copy = scratch / fixit::to_path(basename_of(options.file));
      std::filesystem::copy_file(fixit::to_path(options.file), copy,
                                 std::filesystem::copy_options::overwrite_existing, error);
      scratch_dir = fixit::from_path(scratch);
      target_file = fixit::from_path(copy);
      // The copy lives in a scratch directory, so a relative #include inside the
      // file no longer resolves: the original directory has to stay on the
      // include path, or every project header turns into a phantom
      // "No such file or directory" that the real build does not have.  A file
      // named without any directory ("main.cpp") lives in the current directory,
      // which therefore has to be made absolute first.
      std::error_code cwd_error;
      std::string original_dir = dirname_of(options.file);
      if (original_dir.empty() || original_dir == ".") {
        original_dir = fixit::from_path(std::filesystem::current_path(cwd_error));
      }
      if (!original_dir.empty()) {
        extra_flags.push_back("-iquote");
        extra_flags.push_back(original_dir);
        extra_flags.push_back("-I");
        extra_flags.push_back(original_dir);
      }
    }

    fixit::CompilerConfig config;
    config.compiler = options.compiler;
    config.extra_flags = extra_flags;
    const fixit::Compiler compiler(config);
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

    // ---- outline mode --------------------------------------------------------
    if (options.outline) {
      const fixit::CodeMap map(options.file);
      std::cout << "══ FixIt v" << kVersion << " · outline mode ══\n\n";
      std::cout << display_file << ": " << map.line_count() << " lines, "
                << map.functions().size() << " functions\n";
      if (map.has_syntax_errors()) {
        std::cout << "  " << red("✗ the AST contains syntax errors");
        const std::vector<int> lines = map.syntax_error_lines();
        if (!lines.empty()) {
          std::cout << " on line";
          if (lines.size() > 1) std::cout << "s";
          for (std::size_t i = 0; i < lines.size() && i < 10; ++i) {
            std::cout << (i == 0 ? " " : ", ") << lines[i];
          }
        }
        std::cout << "\n";
      } else {
        std::cout << "  " << green("✓ no syntax errors") << "\n";
      }
      std::cout << "\n  includes\n";
      for (const std::string& include : map.includes()) {
        std::cout << "    " << include << "\n";
      }
      std::cout << "\n  functions\n";
      for (const fixit::FunctionInfo& fn : map.functions()) {
        std::printf("    L%-4d - L%-4d  %s\n", fn.start_line, fn.end_line, fn.name.c_str());
      }
      return 0;
    }

    // ---- compile-only mode ---------------------------------------------------
    if (!options.agent) {
      const fixit::CompileResult result = compiler.compile(target_file);
      cleanup_scratch();
      std::cout << "══ FixIt v" << kVersion << " · compile mode (" << options.compiler << ") ══\n\n";
      std::cout << "$ " << options.compiler << " -std=c++20 -fsyntax-only";
      for (const std::string& flag : extra_flags) std::cout << " " << flag;
      std::cout << " " << display_file << "\n";
      if (result.exit_code == 127) {
        // Documented as exit code 127.  It used to be rendered as "✗ 0 errors",
        // which reads like a clean file with a formatting bug.
        std::cout << "  " << red("✗ the compiler could not be executed") << "\n";
        std::cout << "    check --compiler (currently '" << options.compiler << "') or PATH\n";
        return 127;
      }
      if (result.clean()) {
        std::cout << "  " << green("✓ clean") << "\n";
        return 0;
      }
      std::cout << "  " << red("✗ " + std::to_string(result.error_count()) +
                               (result.error_count() == 1 ? " error" : " errors"))
                << "\n";
      print_errors(result, "    ", display_file);
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

    // Set when the compiler could not be started at all (exit code 127).
    bool compiler_unavailable = false;

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
      if (event.kind == fixit::Agent::Event::Kind::Compile && event.compile.exit_code == 127) {
        compiler_unavailable = true;
      }
      if (!options.verbose) return;  // metrics are collected after the run

      if (event.kind == fixit::Agent::Event::Kind::Compile) {
        const std::string fingerprint = fingerprint_of(event.compile);
        if (event.verification) {
          // Terminal confirmation: report the outcome without pretending a new
          // repair round happened.
          if (fingerprint == last_fingerprint) return;
          last_fingerprint = fingerprint;
          std::cout << "$ " << options.compiler << " -fsyntax-only";
        for (const std::string& flag : extra_flags) std::cout << " " << flag;
        std::cout << " " << display_file << "\n";
          if (event.compile.clean()) {
            std::cout << "  " << green("✓ clean") << "\n";
          } else {
            const std::size_t errors = event.compile.error_count();
            std::cout << "  " << red("✗ " + std::to_string(errors) +
                                     (errors == 1 ? " error" : " errors"))
                      << "\n";
            print_errors(event.compile, "    ", display_file);
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

        std::cout << "$ " << options.compiler << " -fsyntax-only";
        for (const std::string& flag : extra_flags) std::cout << " " << flag;
        std::cout << " " << display_file << "\n";
        if (event.compile.clean()) {
          std::cout << "  " << green("✓ clean") << "\n";
          return;
        }
        const std::size_t errors = event.compile.error_count();
        std::cout << "  " << red("✗ " + std::to_string(errors) + (errors == 1 ? " error" : " errors"))
                  << "\n";
        print_errors(event.compile, "    ", display_file);

        if (!event.compile.diagnostics.empty()) {
          const fixit::Diagnostic& first = event.compile.diagnostics.front();
          if (!first.file.empty() && basename_of(first.file) != display_file) {
            // The error is in an included header: its line number means nothing
            // in the file being repaired, so report the location instead of
            // mapping it onto an unrelated function of the wrong file.
            std::cout << "  " << yellow("→ context: " + basename_of(first.file) + ":" +
                                        std::to_string(first.line) +
                                        " (included header, not the file under repair)")
                      << "\n";
            return;
          }
          fixit::CodeMap map(target_file);
          if (const auto fn = map.enclosing_function(first.line, 1)) {
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
        // No counting here: `result.patches` below is the single source of truth.
        // Counting in both places doubled every metric whenever --verbose was on.
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

    if (compiler_unavailable) {
      std::cerr << "fixit: cannot execute the compiler '" << options.compiler
                << "' (exit code 127); check --compiler or PATH\n";
      return 127;
    }

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
      print_error_list(result.final_errors, "    ", display_file);
    }
    return 1;

  } catch (const std::exception& error) {
    std::cerr << "fixit: internal error: " << error.what() << "\n";
    return 3;
  } catch (...) {
    std::cerr << "fixit: internal error\n";
    return 3;
  }
}
