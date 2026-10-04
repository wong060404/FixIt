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
  std::vector<std::string> extra_flags;  ///< probe-derived flags to append
};

/// Chooses the diagnostic dialect for `compiler`.  A compiler that rejects
/// `-fjson-diagnostics` is probed for `-fdiagnostics-format=json`; if that also
/// fails we fall back to the plain text parser, so an unexpected clang build
/// degrades instead of losing every diagnostic.
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

}  // namespace fixit
