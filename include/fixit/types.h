#pragma once
// Shared value types for FixIt.
//
// Everything here is deliberately dependency-free (no tree-sitter, no JSON) so
// that downstream agents can reuse the types without pulling in the parsers.

#include <algorithm>
#include <string>
#include <vector>

namespace fixit {

enum class DiagLevel { Error, Warning };

/// Half-open-ish source range as reported by the compiler (1-based).
struct SourceSpan {
  int begin_line = 0;
  int begin_col = 0;
  int end_line = 0;
  int end_col = 0;
};

/// One compiler message.  `context_line` is the caret line the compiler echoed,
/// e.g. `12 |   int x = x + 1;` -- already rendered, ready to show an LLM.
struct Diagnostic {
  std::string file;
  int line = 0;
  int col = 0;
  DiagLevel level = DiagLevel::Error;
  std::string message;
  std::string context_line;
  SourceSpan span;
};

/// Result of one compiler invocation.
struct CompileResult {
  int exit_code = -1;
  bool timed_out = false;
  std::vector<Diagnostic> diagnostics;  // sorted by (file, line, col, message)
  std::string raw_output;

  bool clean() const {
    return std::none_of(diagnostics.begin(), diagnostics.end(),
                        [](const Diagnostic& d) { return d.level == DiagLevel::Error; });
  }

  /// Number of error-level diagnostics.
  std::size_t error_count() const {
    return static_cast<std::size_t>(
        std::count_if(diagnostics.begin(), diagnostics.end(),
                      [](const Diagnostic& d) { return d.level == DiagLevel::Error; }));
  }
};

}  // namespace fixit
