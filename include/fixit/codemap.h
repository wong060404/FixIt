#pragma once
// fixit::CodeMap -- structural view of a single translation unit (tree-sitter).

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace fixit {

struct FunctionInfo {
  std::string name;
  std::string signature;
  int start_line = 0;  // 1-based, inclusive
  int end_line = 0;    // 1-based, inclusive
};

class CodeMap {
 public:
  /// Reads the file and parses it.  Missing files / unparsable input do not
  /// throw: the map simply reports no functions and has_syntax_errors() == true.
  explicit CodeMap(const std::string& file_path);
  CodeMap(const CodeMap&) = delete;
  CodeMap& operator=(const CodeMap&) = delete;
  CodeMap(CodeMap&&) noexcept;
  CodeMap& operator=(CodeMap&&) noexcept;
  ~CodeMap();

  std::vector<FunctionInfo> functions() const;

  /// Innermost function_definition containing (line, col); nullopt when the
  /// position is at file scope.
  std::optional<FunctionInfo> enclosing_function(int line, int col) const;

  /// Every #include target verbatim, including its delimiters: `<cstdio>`, `"x.h"`.
  std::vector<std::string> includes() const;

  /// Lines [line-radius, line+radius] clamped to 1..EOF, formatted as
  /// `%4d | %s`.  The caller decides whether to mark the error line.
  std::string context_snippet(int line, int radius = 12) const;

  /// 1-based line numbers of ERROR / MISSING nodes in the AST.
  std::vector<int> syntax_error_lines() const;

  bool has_syntax_errors() const;

  int line_count() const;
  const std::string& path() const { return path_; }
  const std::string& source() const { return source_; }

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  std::string path_;
  std::string source_;
};

}  // namespace fixit
