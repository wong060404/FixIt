#include "fixit/codemap.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <utility>

#include <tree_sitter/api.h>

extern "C" const TSLanguage* tree_sitter_cpp(void);

namespace fixit {
namespace {

std::string slice(const std::string& source, const TSNode& node) {
  const uint32_t start = ts_node_start_byte(node);
  const uint32_t end = ts_node_end_byte(node);
  if (start >= end || start >= source.size()) return {};
  const std::size_t bounded_end = std::min<std::size_t>(end, source.size());
  return source.substr(start, bounded_end - start);
}

std::string node_type(const TSNode& node) {
  const char* t = ts_node_type(node);
  return t == nullptr ? std::string() : std::string(t);
}

bool is_null(const TSNode& node) { return ts_node_is_null(node); }

/// Recursively finds the identifier that names a declarator:
/// `int foo(int)` -> foo, `A::B::bar()` -> bar, `operator+` -> operator+.
bool find_declarator_name(const TSNode& node, const std::string& source, std::string& out) {
  const std::string type = node_type(node);
  if (type == "identifier" || type == "field_identifier" || type == "type_identifier" ||
      type == "operator_name" || type == "destructor_name" || type == "qualified_identifier") {
    out = slice(source, node);
    if (type == "qualified_identifier") {
      // keep the full spelling (`A::B::bar`); the simple name is the tail
      return true;
    }
    return true;
  }
  if (type == "function_declarator" || type == "pointer_declarator" ||
      type == "reference_declarator" || type == "parenthesized_declarator" ||
      type == "abstract_function_declarator") {
    TSNode inner = ts_node_child_by_field_name(node, "declarator", 10);
    if (!is_null(inner)) return find_declarator_name(inner, source, out);
  }
  const uint32_t count = ts_node_named_child_count(node);
  for (uint32_t i = 0; i < count; ++i) {
    if (find_declarator_name(ts_node_named_child(node, i), source, out)) return true;
  }
  return false;
}

/// Walks the AST and reports every ERROR / MISSING node, with 1-based lines.
void collect_errors(const TSNode& node, std::vector<int>& lines) {
  if (ts_node_is_error(node) || ts_node_is_missing(node)) {
    lines.push_back(static_cast<int>(ts_node_start_point(node).row) + 1);
  }
  const uint32_t count = ts_node_child_count(node);
  for (uint32_t i = 0; i < count; ++i) {
    collect_errors(ts_node_child(node, i), lines);
  }
}

std::vector<std::string> split_lines(const std::string& source) {
  std::vector<std::string> lines;
  std::string current;
  for (char c : source) {
    if (c == '\n') {
      if (!current.empty() && current.back() == '\r') current.pop_back();
      lines.push_back(current);
      current.clear();
    } else {
      current.push_back(c);
    }
  }
  if (!current.empty()) {
    if (current.back() == '\r') current.pop_back();
    lines.push_back(current);
  }
  return lines;
}

}  // namespace

struct CodeMap::Impl {
  TSParser* parser = nullptr;
  TSTree* tree = nullptr;
  std::vector<std::string> lines;
  bool parsed = false;
};

CodeMap::CodeMap(const std::string& file_path) : impl_(std::make_unique<Impl>()), path_(file_path) {
  impl_->lines.push_back(std::string());  // 1-based indexing convenience

  std::ifstream in(file_path, std::ios::binary);
  if (!in) return;
  std::ostringstream buffer;
  buffer << in.rdbuf();
  source_ = buffer.str();

  const std::vector<std::string> raw = split_lines(source_);
  impl_->lines.insert(impl_->lines.end(), raw.begin(), raw.end());

  impl_->parser = ts_parser_new();
  if (impl_->parser == nullptr) return;
  ts_parser_set_language(impl_->parser, tree_sitter_cpp());
  impl_->tree = ts_parser_parse_string(impl_->parser, nullptr, source_.c_str(),
                                       static_cast<uint32_t>(source_.size()));
  impl_->parsed = impl_->tree != nullptr;
}

CodeMap::CodeMap(CodeMap&&) noexcept = default;
CodeMap& CodeMap::operator=(CodeMap&&) noexcept = default;

CodeMap::~CodeMap() {
  if (impl_) {
    if (impl_->tree != nullptr) ts_tree_delete(impl_->tree);
    if (impl_->parser != nullptr) ts_parser_delete(impl_->parser);
  }
}

std::vector<FunctionInfo> CodeMap::functions() const {
  std::vector<FunctionInfo> out;
  if (!impl_ || !impl_->parsed) return out;

  TSQueryError error_type = TSQueryErrorNone;
  uint32_t error_offset = 0;
  static const char* kFnPattern = "(function_definition) @fn";
  TSQuery* query = ts_query_new(tree_sitter_cpp(), kFnPattern,
                                static_cast<uint32_t>(std::strlen(kFnPattern)), &error_offset,
                                &error_type);
  if (query == nullptr) return out;

  TSQueryCursor* cursor = ts_query_cursor_new();
  ts_query_cursor_exec(cursor, query, ts_tree_root_node(impl_->tree));

  TSQueryMatch match{};
  while (ts_query_cursor_next_match(cursor, &match)) {
    for (uint32_t i = 0; i < match.capture_count; ++i) {
      const TSNode node = match.captures[i].node;
      if (node_type(node) != "function_definition") continue;

      FunctionInfo info;
      info.start_line = static_cast<int>(ts_node_start_point(node).row) + 1;
      info.end_line = static_cast<int>(ts_node_end_point(node).row) + 1;

      const TSNode declarator = ts_node_child_by_field_name(node, "declarator", 10);
      if (!is_null(declarator)) find_declarator_name(declarator, source_, info.name);

      // signature = from the start of the definition up to (excluding) '{'
      const TSNode body = ts_node_child_by_field_name(node, "body", 4);
      const uint32_t sig_end =
          is_null(body) ? ts_node_end_byte(node) : ts_node_start_byte(body);
      const uint32_t sig_start = ts_node_start_byte(node);
      if (sig_end > sig_start && sig_end <= source_.size()) {
        info.signature = source_.substr(sig_start, sig_end - sig_start);
        while (!info.signature.empty() &&
               (info.signature.back() == ' ' || info.signature.back() == '\n' ||
                info.signature.back() == '\t' || info.signature.back() == '\r')) {
          info.signature.pop_back();
        }
      }
      out.push_back(std::move(info));
    }
  }

  ts_query_cursor_delete(cursor);
  ts_query_delete(query);
  return out;
}

std::optional<FunctionInfo> CodeMap::enclosing_function(int line, int col) const {
  (void)col;  // kept in the signature for future nested-scope refinement
  std::optional<FunctionInfo> best;
  for (const FunctionInfo& fn : functions()) {
    if (line < fn.start_line || line > fn.end_line) continue;
    // Innermost wins: the tightest range containing the position.
    if (!best || (fn.end_line - fn.start_line) <= (best->end_line - best->start_line)) {
      best = fn;
    }
  }
  return best;
}

std::vector<std::string> CodeMap::includes() const {
  std::vector<std::string> out;
  if (!impl_ || !impl_->parsed) return out;

  TSQueryError error_type = TSQueryErrorNone;
  uint32_t error_offset = 0;
  static const char* kPattern =
      "(preproc_include path: (_) @path)";
  TSQuery* query = ts_query_new(tree_sitter_cpp(), kPattern,
                               static_cast<uint32_t>(std::strlen(kPattern)), &error_offset,
                               &error_type);
  if (query == nullptr) return out;

  TSQueryCursor* cursor = ts_query_cursor_new();
  ts_query_cursor_exec(cursor, query, ts_tree_root_node(impl_->tree));

  TSQueryMatch match{};
  while (ts_query_cursor_next_match(cursor, &match)) {
    for (uint32_t i = 0; i < match.capture_count; ++i) {
      out.push_back(slice(source_, match.captures[i].node));
    }
  }

  ts_query_cursor_delete(cursor);
  ts_query_delete(query);
  return out;
}

std::string CodeMap::context_snippet(int line, int radius) const {
  if (!impl_ || impl_->lines.size() <= 1) return {};

  const int last = static_cast<int>(impl_->lines.size()) - 1;
  const int begin = std::max(1, line - radius);
  const int end = std::min(last, line + radius);

  std::ostringstream os;
  for (int i = begin; i <= end; ++i) {
    os << std::setw(4) << i << " | " << impl_->lines[static_cast<std::size_t>(i)] << "\n";
  }
  return os.str();
}

std::vector<int> CodeMap::syntax_error_lines() const {
  std::vector<int> lines;
  if (!impl_ || !impl_->parsed) return lines;
  collect_errors(ts_tree_root_node(impl_->tree), lines);
  std::sort(lines.begin(), lines.end());
  lines.erase(std::unique(lines.begin(), lines.end()), lines.end());
  return lines;
}

bool CodeMap::has_syntax_errors() const {
  if (!impl_) return true;
  if (path_.empty()) return true;
  if (!impl_->parsed) return true;
  if (source_.empty()) return true;  // a missing/unreadable file lands here
  return !syntax_error_lines().empty();
}

int CodeMap::line_count() const {
  if (!impl_) return 0;
  return static_cast<int>(impl_->lines.size()) - 1;
}

}  // namespace fixit
