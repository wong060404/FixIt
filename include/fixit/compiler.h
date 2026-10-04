#pragma once
// fixit::Compiler -- run a real compiler and turn its output into data.
//
// The compiler is the only ground truth in FixIt: nothing else may claim that a
// repair worked.

#include <cstddef>
#include <string>
#include <vector>

#include "fixit/types.h"

namespace fixit {

struct CompilerConfig {
  enum class DiagnosticFormat {
    Auto,  ///< probe the compiler once, then pick the best supported dialect
    Text,  ///< force the GCC-style text parser
    Json   ///< force the clang-style JSON parser
  };

  std::string compiler = "g++";  ///< "g++" or "clang++" (anything on PATH works)
  int error_limit = 5;           ///< passed to -ferror-limit; internal cap is 50
  int timeout_seconds = 30;
  std::vector<std::string> extra_flags;
  DiagnosticFormat format = DiagnosticFormat::Auto;
};

struct DiagnosticFlags {
  bool uses_json = false;             ///< parse with the JSON dialect
  std::string json_flag;              ///< flag actually passed, empty when none
  /// Whether this compiler accepts `-ferror-limit=N`.  clang does; GCC does not,
  /// and passing it makes GCC abort with "unrecognized command-line option"
  /// before compiling anything, which yields zero diagnostics and looks exactly
  /// like a clean build.  Probed, never assumed.
  bool supports_error_limit = true;
  std::vector<std::string> extra_flags;  ///< probe-derived flags to append
};

/// Chooses the diagnostic dialect *and* the optional flags for `compiler` by
/// asking it, once.  A compiler that rejects `-fjson-diagnostics` is probed for
/// `-fdiagnostics-format=json`; if that also fails we fall back to the text
/// parser.  `-ferror-limit` is included only when the compiler accepts it, so
/// GCC (which has no such flag) is driven correctly instead of failing early.
/// Everything degrades to a working state rather than a silent one.
DiagnosticFlags probe_diagnostic_flags(const std::string& compiler, int timeout_seconds = 10);

class Compiler {
 public:
  explicit Compiler(CompilerConfig cfg = {});

  /// Runs `<compiler> -std=c++20 -fsyntax-only -ferror-limit=N [extra_flags] <source_file>`.
  /// Never throws for compiler failures; a missing binary surfaces as exit_code -1
  /// with the message in raw_output.
  CompileResult compile(const std::string& source_file) const;

  const CompilerConfig& config() const { return cfg_; }

  /// The exact argv (excluding argv[0]) that compile() would use.  Used by the
  /// CLI's verbose output and by tests.
  std::vector<std::string> command_line(const std::string& source_file) const;

  /// True when the configured compiler looks like clang (drives JSON parsing).
  bool uses_json_diagnostics() const;

 private:
  CompilerConfig cfg_;
  DiagnosticFlags resolved_;  ///< dialect chosen at construction time
};

/// Parse the output of one compiler invocation.  Exposed for fixture tests.
/// `uses_json` selects the clang JSON dialect, otherwise the GCC text dialect.
std::vector<Diagnostic> parse_diagnostics(const std::string& raw_output, bool uses_json);

/// Normalises the typographic quotes GCC 12 uses around identifiers and tokens
/// (`‘vector’`, `‘;’`) to the straight quotes clang uses (`'vector'`, `';'`).
/// Everything downstream -- the mock's repair rules, diffs, prompts -- can then
/// match one spelling.  Any other character is left untouched.
std::string normalize_quotes(const std::string& text);

}  // namespace fixit
