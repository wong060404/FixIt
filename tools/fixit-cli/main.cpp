// fixit-cli -- the command line front end described in the README.
//
// Exit codes: 0 clean/fixed, 1 not fixed, 2 usage error, 3 internal error.

#include <cstddef>
#include <chrono>
#include <codecvt>
#include <cstdio>
#include <filesystem>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <locale>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
// The POSIX spellings are not declared by the UCRT under -std=c++20; the
// underscore-prefixed equivalents are the same functions.
#include <io.h>
#include <process.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
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

namespace {

constexpr const char* kVersion = "0.1.0";

#ifdef _WIN32
/// Windows hands `main(int, char**)` its arguments in the ANSI code page, while
/// every path inside FixIt is UTF-8.  Convert once, at startup, so a source file
/// under a non-ASCII directory works instead of throwing inside std::filesystem.
std::string acp_to_utf8(const std::string& text) {
  if (text.empty()) return {};
  const int wide_len = ::MultiByteToWideChar(CP_ACP, 0, text.data(),
                                             static_cast<int>(text.size()), nullptr, 0);
  if (wide_len <= 0) return text;
  std::wstring wide(static_cast<std::size_t>(wide_len), L'\0');
  ::MultiByteToWideChar(CP_ACP, 0, text.data(), static_cast<int>(text.size()), wide.data(),
                        wide_len);
  const int utf8_len =
      ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), wide_len, nullptr, 0, nullptr, nullptr);
  if (utf8_len <= 0) return text;
  std::string utf8(static_cast<std::size_t>(utf8_len), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), wide_len, utf8.data(), utf8_len, nullptr,
                        nullptr);
  return utf8;
}

/// MinGW ships no non-"C" locale, and std::filesystem converts a narrow path to
/// the native wide form through the global locale's codecvt facet.  Installing a
/// UTF-8 facet is what makes the UTF-8 paths above usable.
void enable_utf8_filesystem() {
  try {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    std::locale::global(std::locale(std::locale("C"), new std::codecvt_utf8<wchar_t>));
#pragma GCC diagnostic pop
  } catch (...) {
    // Leave the C locale in place: ASCII paths still work.
  }
}
#endif  // _WIN32

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
  // Single-run iteration count.  `iterations` is the accurate name; the legacy
  // `avg_iterations` key is kept so existing benchmark scripts keep parsing.
  // A multi-run benchmark should aggregate `iterations` itself.
  os << "  \"iterations\": " << iterations << ",\n";
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
    // number attributes it to the file named on the command line -- and the code
    // map then looks for its enclosing function in the wrong file.
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

/// Warnings were parsed and then dropped: `--flag -Wall` still printed "clean",
/// hiding the one thing the user asked the compiler for.
std::size_t warning_count_of(const fixit::CompileResult& result) {
  std::size_t count = 0;
  for (const fixit::Diagnostic& d : result.diagnostics) {
    if (d.level == fixit::DiagLevel::Warning) ++count;
  }
  return count;
}

void print_warnings(const fixit::CompileResult& result, const std::string& indent) {
  std::size_t index = 0;
  for (const fixit::Diagnostic& d : result.diagnostics) {
    if (d.level != fixit::DiagLevel::Warning) continue;
    ++index;
    std::printf("%s[W%zu]  L%-4d %s\n", indent.c_str(), index, d.line, d.message.c_str());
  }
}

/// A compile that failed without producing a single parsed diagnostic is the most
/// confusing output this tool can print: "0 errors" next to "not fixed".  It means
/// the compiler never got as far as analysing the file -- typically because it could
/// not start its own sub-tools (as.exe, ld.exe), which on Windows happens when the
/// compiler's bin directory is missing from PATH.  Say that, instead of "0 errors".
/// Creates the private copy that --no-write repairs, and reports *why* it could not
/// when it cannot.  This is the one step that can fail for reasons outside the
/// tool's control (a full or read-only TEMP, an antivirus lock, a leftover
/// directory that cannot be written into), and when it did fail silently the
/// compiler reported "no such file", which read as a broken tool.  Try the system
/// temp directory first, then the source file's own directory; a hidden copy there
/// is removed again when the run ends.
bool make_scratch_copy(const std::string& source,
                       const std::string& executable,
                       std::filesystem::path& scratch_dir,
                       std::filesystem::path& copy_target,
                       std::string& failure) {
  // A ladder of writable homes, because any single one can be unusable: a full or
  // read-only TEMP, Windows' Controlled Folder Access (or an antivirus) locking the
  // user's own folders, or a source file that lives on a read-only share.  The
  // program's own directory is a legitimate home: if the tool was built there, it is
  // writable by definition.
  std::vector<std::filesystem::path> roots;
  std::error_code error;
  const std::filesystem::path system_temp = std::filesystem::temp_directory_path(error);
  if (!error && !system_temp.empty()) roots.push_back(system_temp);
  if (const char* local = std::getenv("LOCALAPPDATA")) {
    roots.push_back(std::filesystem::path(local) / "fixit");
  }
  if (const char* cache = std::getenv("XDG_CACHE_HOME")) {
    roots.push_back(std::filesystem::path(cache) / "fixit");
  } else if (const char* home = std::getenv("HOME")) {
    roots.push_back(std::filesystem::path(home) / ".cache" / "fixit");
  }
  if (!executable.empty()) {
    std::error_code local;
    const std::filesystem::path exe = std::filesystem::absolute(executable, local);
    if (!local && !exe.parent_path().empty()) roots.push_back(exe.parent_path());
  }
  const std::filesystem::path source_dir = std::filesystem::path(source).parent_path();
  if (!source_dir.empty()) roots.push_back(source_dir);
  std::error_code cwd_error;
  const std::filesystem::path cwd = std::filesystem::current_path(cwd_error);
  if (!cwd_error) roots.push_back(cwd);

  const std::string unique =
      std::to_string(fixit_getpid()) + "-" +
      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
  const std::string name = "fixit-no-write-" + unique;

  std::ostringstream attempts;
  for (const std::filesystem::path& root : roots) {
    std::error_code local;
    const std::filesystem::path dir = root / name;
    std::filesystem::create_directories(dir, local);
    if (!local) {
      const std::filesystem::path target = dir / std::filesystem::path(source).filename();
      std::filesystem::copy_file(source, target,
                                 std::filesystem::copy_options::overwrite_existing, local);
      if (!local && std::filesystem::is_regular_file(target, local)) {
        scratch_dir = dir;
        copy_target = target;
        return true;
      }
      if (!local) local = std::make_error_code(std::errc::no_such_file_or_directory);
      std::error_code ignored;
      std::filesystem::remove_all(dir, ignored);
    }
    attempts << "  " << dir.string() << ": " << local.message() << "\n";
  }
  failure = attempts.str();
  return false;
}

/// Expands `~`, `%VARS%` and a stray pair of surrounding quotes, so a path can be
/// pasted from Explorer or written the way a person says it.
std::string expand_user_path(const std::string& raw) {
  std::string value = raw;
  if (value.size() >= 2 && ((value.front() == '"' && value.back() == '"') ||
                            (value.front() == '\'' && value.back() == '\''))) {
    value = value.substr(1, value.size() - 2);
  }
  const auto home = []() -> std::string {
    if (const char* profile = std::getenv("USERPROFILE")) return profile;
    if (const char* h = std::getenv("HOME")) return h;
    return {};
  };
  if (!value.empty() && value[0] == '~') {
    const std::string base = home();
    if (!base.empty()) value = base + value.substr(1);
  }
  std::string expanded;
  for (std::size_t i = 0; i < value.size();) {
    if (value[i] == '%') {
      const std::size_t end = value.find('%', i + 1);
      if (end != std::string::npos) {
        const std::string name = value.substr(i + 1, end - i - 1);
        if (const char* env = std::getenv(name.c_str())) {
          expanded += env;
          i = end + 1;
          continue;
        }
      }
    }
    expanded += value[i++];
  }
  return expanded;
}

/// Finds the file the user meant without making them build an absolute path by
/// hand.  Tried in order: the literal path; the usual user folders joined with
/// the given path (`Downloads\\f.cpp`, `~/Downloads/f.cpp`); and, for anything
/// that does not exist yet, the same file *name* inside those folders.  When it
/// has to guess it says which file it picked, and when a name matches more than
/// one file it refuses instead of choosing, because repairing the wrong file
/// silently would be worse than an error.
std::string resolve_input_path(const std::string& raw) {
  const std::filesystem::path literal = expand_user_path(raw);
  std::error_code error;
  if (std::filesystem::is_regular_file(literal, error)) return literal.string();

  std::vector<std::filesystem::path> roots;
  const auto add_root = [&roots](const std::string& base) {
    if (base.empty()) return;
    std::error_code local;
    const std::filesystem::path root(base);
    if (!std::filesystem::is_directory(root, local)) return;
    for (const std::filesystem::path& candidate : roots) {
      if (candidate == root) return;
    }
    roots.push_back(root);
  };
  for (const char* variable : {"USERPROFILE", "HOME"}) {
    const char* base = std::getenv(variable);
    if (!base) continue;
    add_root(base);
    for (const char* sub : {"Downloads", "Desktop", "Documents", "OneDrive\\Desktop",
                            "OneDrive\\Documents"}) {
      add_root((std::filesystem::path(base) / sub).string());
    }
  }

  std::vector<std::filesystem::path> matches;
  const auto consider = [&matches](const std::filesystem::path& candidate) {
    std::error_code local;
    if (!std::filesystem::is_regular_file(candidate, local)) return;
    for (const std::filesystem::path& seen : matches) {
      if (seen == candidate) return;
    }
    matches.push_back(candidate);
  };
  for (const std::filesystem::path& root : roots) {
    consider(root / literal);
    consider(root / literal.filename());
  }

  if (matches.size() == 1) {
    // stdout on purpose: this is information, not an error, and PowerShell
    // paints anything on stderr red.
    std::cout << "fixit: using '" << matches.front().string() << "'"
              << "  (resolved from '" << raw << "')\n";
    return matches.front().string();
  }
  if (matches.size() > 1) {
    std::cerr << "fixit: '" << raw << "' matches several files; pass the full path:\n";
    for (const std::filesystem::path& match : matches) {
      std::cerr << "  " << match.string() << "\n";
    }
  }
  return literal.string();
}

void print_compile_failure(const fixit::CompileResult& result, const std::string& indent) {
  if (result.timed_out) {
    std::cout << "  " << red("✗ the compiler timed out") << "\n";
  } else if (result.exit_code == 127 || result.exit_code == -1) {
    std::cout << "  " << red("✗ the compiler could not be executed") << "\n";
    std::cout << indent << "hint: put the compiler's bin folder on PATH -- on Windows,"
              << " .\\fixit.cmd does that for you\n";
  } else {
    std::cout << "  " << red("✗ the compiler exited " + std::to_string(result.exit_code) +
                             " without reporting a diagnostic for this file") << "\n";
    std::cout << indent
              << "hint: it probably could not start its own tools (as.exe/ld.exe).  Put its\n";
    std::cout << indent
              << "bin folder on PATH -- on Windows, .\\fixit.cmd does that for you.\n";
  }
  if (!result.raw_output.empty()) {
    std::cout << indent << "compiler output:" << "\n";
    std::istringstream lines(result.raw_output);
    std::string line;
    while (std::getline(lines, line)) {
      std::cout << indent << "  " << line << "\n";
    }
  }
}

}  // namespace

int main(int argc, char** argv) {
  // Exit code 3 is documented as "internal error".  Until now nothing could
  // return it, so a std::bad_alloc or a library type_error escaped main and the
  // process died with a signal instead -- indistinguishable from a crash caused
  // by the user's own file.  Report it as the documented code.
  try {

#ifdef _WIN32
    // Convert the ANSI command line to UTF-8 and make std::filesystem use it,
    // so a file under a non-ASCII directory is usable.
    enable_utf8_filesystem();
    // Our output is UTF-8; tell the console so the box-drawing characters are
    // rendered correctly instead of as the legacy code page.
    ::SetConsoleOutputCP(CP_UTF8);
    std::vector<std::string> utf8_args;
    utf8_args.reserve(static_cast<std::size_t>(argc));
    for (int i = 0; i < argc; ++i) utf8_args.push_back(acp_to_utf8(argv[i]));
    std::vector<char*> utf8_argv;
    utf8_argv.reserve(utf8_args.size());
    for (std::string& arg : utf8_args) utf8_argv.push_back(arg.data());
    argv = utf8_argv.data();
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
    // The argument is resolved once, here: everything downstream (the compiler,
    // CodeMap, the patch tools) must see the same absolute path.
    const std::string input_file = resolve_input_path(options.file);

    std::string original_content;
    {
      // A directory opens successfully with std::ifstream, and the compiler then
      // reports nothing for it -- which used to read as "clean".  Require a
      // regular file so an obvious typo can never look like a passing build.
      std::error_code error;
      if (!std::filesystem::is_regular_file(input_file, error)) {
        std::cerr << "fixit: '" << input_file << "' is not a readable file\n";
        return 2;
      }
      std::ifstream probe(std::filesystem::path(input_file), std::ios::binary);
      if (!probe) {
        std::cerr << "fixit: cannot open '" << input_file << "'\n";
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
    config.extra_flags = options.extra_flags;
    if (options.no_write) {
      // The repair runs on a scratch copy in a temp directory, so a header that
      // sits next to the original source would no longer be found.  Keep the
      // original directory on the include path.
      config.extra_flags.push_back("-I" + dirname_of(input_file));
    }
    const fixit::Compiler compiler(config);

    // --no-write repairs a private copy in a scratch directory, so the working
    // tree is byte-identical afterwards and the demo can be repeated verbatim.
    //
    // ScratchGuard is RAII: it removes the directory on *every* exit path,
    // including the --outline early return and any exception.  The previous
    // manual cleanup lambda was only called on two paths and therefore leaked
    // the scratch directory in --outline mode.
    std::string scratch_dir;
    std::string target_file = input_file;
    if (options.no_write) {
      std::filesystem::path scratch_path;
      std::filesystem::path copy_path;
      std::string failure;
      if (!make_scratch_copy(input_file, argv[0] != nullptr ? argv[0] : "", scratch_path,
                             copy_path, failure)) {
        std::cerr << "fixit: cannot create the --no-write scratch copy. Tried:\n" << failure;
        std::cerr << "  hint: run without --no-write, or allow fixit.exe in Windows Security >\n"
                     "        Virus & threat protection > Ransomware protection >\n"
                     "        Controlled folder access.\n";
        return 2;
      }
      scratch_dir = scratch_path.string();
      target_file = copy_path.string();
    }
    struct ScratchGuard {
      std::filesystem::path path;
      ~ScratchGuard() {
        if (!path.empty()) {
          std::error_code error;
          std::filesystem::remove_all(path, error);
        }
      }
    } scratch_guard{scratch_dir};

    const std::string display_file = basename_of(input_file);
    const std::string workdir = options.no_write ? scratch_dir : dirname_of(input_file);

    const auto started = std::chrono::steady_clock::now();
    const auto elapsed = [&] {
      return std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    };

    // ---- outline mode --------------------------------------------------------
    if (options.outline) {
      const fixit::CodeMap map(input_file);
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
      // A script asking for the outline must be able to tell a broken file from a
      // healthy one without parsing the text it just printed.
      return map.has_syntax_errors() ? 1 : 0;
    }

    // ---- compile-only mode ---------------------------------------------------
    if (!options.agent) {
      const fixit::CompileResult result = compiler.compile(target_file);
      std::cout << "══ FixIt v" << kVersion << " · compile mode (" << options.compiler << ") ══\n\n";
      std::cout << "$ " << options.compiler << " -std=c++20 -fsyntax-only";
      for (const std::string& flag : options.extra_flags) std::cout << " " << flag;
      std::cout << " " << display_file << "\n";
      if (result.exit_code == 127) {
        // Documented as exit code 127: the compiler could not be started at all,
        // which is an environment problem, not a verdict about the file.
        print_compile_failure(result, "    ");
        return 127;
      }
      if (result.clean()) {
        const std::size_t warnings = warning_count_of(result);
        std::cout << "  " << green("✓ clean");
        if (warnings > 0) {
          std::cout << " (" << warnings << (warnings == 1 ? " warning" : " warnings") << ")";
        }
        std::cout << "\n";
        if (warnings > 0) print_warnings(result, "    ");
        return 0;
      }
      if (result.error_count() == 0) {
        print_compile_failure(result, "    ");
      } else {
        std::cout << "  " << red("✗ " + std::to_string(result.error_count()) +
                                 (result.error_count() == 1 ? " error" : " errors"))
                  << "\n";
        print_errors(result, "    ", display_file);
      }
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
        for (const std::string& flag : options.extra_flags) std::cout << " " << flag;
        std::cout << " " << display_file << "\n";
          if (event.compile.clean()) {
            std::cout << "  " << green("✓ clean") << "\n";
          } else {
            const std::size_t errors = event.compile.error_count();
            if (errors == 0) {
              print_compile_failure(event.compile, "    ");
            } else {
              std::cout << "  " << red("✗ " + std::to_string(errors) +
                                       (errors == 1 ? " error" : " errors"))
                        << "\n";
              print_errors(event.compile, "    ", display_file);
            }
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
        for (const std::string& flag : options.extra_flags) std::cout << " " << flag;
        std::cout << " " << display_file << "\n";
        if (event.compile.clean()) {
          std::cout << "  " << green("✓ clean") << "\n";
          return;
        }
        const std::size_t errors = event.compile.error_count();
        if (errors == 0) {
          print_compile_failure(event.compile, "    ");
        } else {
          std::cout << "  " << red("✗ " + std::to_string(errors) + (errors == 1 ? " error" : " errors"))
                    << "\n";
          print_errors(event.compile, "    ", display_file);
        }

        if (!event.compile.diagnostics.empty()) {
          const fixit::Diagnostic& first = event.compile.diagnostics.front();
          if (!first.file.empty() && basename_of(first.file) != display_file) {
            // The error is in an included header: its line number means nothing in
            // the file being repaired, so report the location instead of mapping it
            // onto an unrelated function of the wrong file.
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