// MockLlm -- a deterministic, offline stand-in for a real model.
//
// It exists for two reasons:
//   1. the whole test suite must run without network access, and
//   2. it *deliberately* emits diffs whose @@ line numbers are off by one, so
//      every demo run also exercises the fuzzy patch engine.
//
// Rules (first match wins; diagnostics are consumed in the order the compiler
// reported them):
//   R1  undeclared symbol that maps to a standard header -> add the #include
//       right after the last existing #include
//   R2  "expected ';'"                                    -> append the ';'
//   R3  "no matching function" for a mapped symbol        -> force the argument type
//   R4  anything else                                     -> give up (FINAL)
//
// Every diff is declared one line below its true position (drift +1).  See
// docs/decisions.md ADR-003 for the encoding of the insertion case.

#include <algorithm>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "fixit/agent.h"

namespace fixit {
namespace {

/// Symbols whose repair is "include this standard header".
const std::map<std::string, std::string>& include_map() {
  static const std::map<std::string, std::string> kMap = {
      {"FILE", "<cstdio>"},   {"fopen", "<cstdio>"},  {"fclose", "<cstdio>"},
      {"atoi", "<cstdlib>"},  {"abs", "<cstdlib>"},   {"vector", "<vector>"},
      {"max", "<algorithm>"}, {"string", "<string>"}};
  return kMap;
}

/// Symbols whose repair is "make the argument type explicit".
const std::map<std::string, std::string>& cast_map() {
  static const std::map<std::string, std::string> kMap = {
      {"atoi", "static_cast<long>"},
  };
  return kMap;
}

struct PendingError {
  std::string file;
  int line = 0;
  int col = 0;
  std::string message;
};

/// What the freshest compile observation said, beyond the diagnostics.
struct CompileState {
  bool seen = false;
  bool clean = false;
  int exit_code = 0;
  std::size_t error_count = 0;
};

bool contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

std::vector<std::string> read_all_lines(const std::string& path) {
  std::vector<std::string> lines;
  std::ifstream in(path);
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    lines.push_back(line);
  }
  return lines;
}

/// The very first message embeds the compiler output inside a prose envelope
/// ("Task: ... Compile result:\n{...}"), later rounds receive the same shape as
/// a bare JSON tool observation.  Both are handled here.
std::vector<PendingError> pending_errors(const std::vector<Message>& messages,
                                         const std::string& fallback_file,
                                         CompileState* state = nullptr) {
  for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
    if (it->role != "user" && it->role != "tool") continue;

    const std::size_t brace = it->content.find('{');
    const std::string json_text =
        brace == std::string::npos ? it->content : it->content.substr(brace);
    nlohmann::json payload = nlohmann::json::parse(json_text, nullptr, false);
    if (payload.is_discarded() && it->tool_result.is_object()) payload = it->tool_result;
    if (!payload.is_object() || !payload.contains("errors")) continue;

    const nlohmann::json& errors = payload["errors"];
    if (!errors.is_array()) continue;

    if (state != nullptr) {
      state->seen = true;
      state->clean = payload.value("clean", false);
      state->exit_code = payload.value("exit_code", 0);
      state->error_count = errors.size();
    }

    std::vector<PendingError> out;
    for (const auto& e : errors) {
      PendingError pending;
      pending.file = e.value("file", fallback_file);
      pending.line = e.value("line", 0);
      pending.col = e.value("col", 0);
      pending.message = e.value("message", std::string());
      out.push_back(std::move(pending));
    }
    return out;
  }
  return {};
}

/// One `@@ -declared,count +declared,count @@` header whose declared position
/// is the true position + 1.
std::string diff_header(const std::string& file, int declared_start, int count) {
  std::ostringstream os;
  os << "--- a/" << file << "\n+++ b/" << file << "\n@@ -" << declared_start << "," << count
     << " +" << declared_start << "," << count << " @@\n";
  return os.str();
}

LlmResponse patch_response(const std::string& file, const std::string& diff) {
  LlmResponse response;
  ToolCall call;
  call.name = "patch";
  call.args = nlohmann::json{{"file", file}, {"diff", diff}};
  response.tool_calls.push_back(std::move(call));
  return response;
}

LlmResponse give_up() {
  LlmResponse response;
  response.is_final = true;
  response.content = "FINAL";
  return response;
}

/// The mock reads files the same way the patch tool does.  `MockLlm::set_workdir`
/// sets the directory; when it is empty the current process directory is used.
std::string g_workdir;

std::string resolve(const std::string& file) {
  if (file.empty() || file.front() == '/' || g_workdir.empty()) return file;
  return g_workdir + "/" + file;
}

int last_include_line(const std::vector<std::string>& lines) {
  int last = 0;
  for (std::size_t i = 0; i < lines.size(); ++i) {
    const std::string& line = lines[i];
    const std::size_t first = line.find_first_not_of(" \t");
    if (first != std::string::npos && line.compare(first, 8, "#include") == 0) {
      last = static_cast<int>(i) + 1;
    }
  }
  return last;
}

}  // namespace

MockLlm::MockLlm() = default;

void MockLlm::set_workspace(const std::string& workdir) { g_workdir = workdir; }

LlmResponse MockLlm::chat(const std::vector<Message>& messages, const std::vector<ToolSpec>& tools) {
  (void)tools;

  std::string fallback_file;
  for (const Message& m : messages) {
    if (m.role != "user") continue;
    const std::size_t pos = m.content.find("File: ");
    if (pos == std::string::npos) continue;
    const std::size_t begin = pos + 6;
    const std::size_t end = m.content.find('\n', begin);
    fallback_file = m.content.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
    break;
  }
  while (!fallback_file.empty() && (fallback_file.back() == '\r' || fallback_file.back() == ' ')) {
    fallback_file.pop_back();
  }

  CompileState state;
  const std::vector<PendingError> errors = pending_errors(messages, fallback_file, &state);

  // The compiler reported a failure but produced no diagnostics: the tool could
  // not run it at all.  No rule can fix that, and pretending otherwise would
  // hide a broken environment behind a "nothing to do" answer.
  if (state.seen && !state.clean && state.error_count == 0) {
    LlmResponse response;
    response.is_final = true;
    response.content =
        "FINAL: the compiler could not be executed (exit code " + std::to_string(state.exit_code) +
        "); check the --compiler setting or the toolchain on PATH.";
    return response;
  }

  if (errors.empty()) return give_up();

  // ---- R1: missing include -------------------------------------------------
  // One patch call per round, as a real model would emit: every rule builds a
  // hunk and the hunks are applied together, so positions stay consistent.
  struct Hunk {
    int position = 0;   ///< placeholder: see the `insert` flag
    std::string text;   ///< the body lines, already +/- prefixed
    bool insert = false;  ///< true -> position already includes the drift
  };
  std::map<std::string, std::vector<Hunk>> per_file;
  const auto add_hunk = [&](const std::string& file, int position, const std::string& text,
                            bool insert = false) {
    per_file[file].push_back(Hunk{position, text, insert});
  };

  const auto needs_include = [](const std::string& message) {
    // "was not declared" / "not declared in this scope" (GCC + clang) and
    // clang's "no member named 'X' in namespace 'std'" are the same failure
    // mode for this repair rule.
    return contains(message, "was not declared") || contains(message, "not declared in this scope") ||
           contains(message, "no member named");
  };

  // Only real `#include` directives count -- a usage such as
  // `std::vector<std::string> words;` contains "<vector>" too, and treating that
  // as an existing include would make the rule add nothing.
  const auto include_already_present = [](const std::vector<std::string>& lines,
                                          const std::string& header) {
    for (const std::string& line : lines) {
      const std::size_t first = line.find_first_not_of(" \t");
      if (first == std::string::npos || line.compare(first, 8, "#include") != 0) continue;
      if (line.find(header) != std::string::npos) return true;
    }
    return false;
  };

  // Collect the headers that are *actually* missing.  A diagnostic can name a
  // symbol whose header arrives transitively (libc++ <string> pulls in
  // <cstdio>), and such a symbol must not shadow the header that really matters.
  for (const PendingError& error : errors) {
    if (!needs_include(error.message)) continue;

    const std::vector<std::string> lines = read_all_lines(resolve(error.file));
    const int last = last_include_line(lines);
    if (last <= 0) continue;

    std::string body;
    std::vector<std::string> added_headers;
    for (const auto& [symbol, header] : include_map()) {
      if (!contains(error.message, "'" + symbol + "'")) continue;
      if (include_already_present(lines, header)) continue;
      // All symbols that need the same header fold into one #include.
      if (std::find(added_headers.begin(), added_headers.end(), header) != added_headers.end()) {
        continue;
      }
      added_headers.push_back(header);
      body += "+#include " + header + "\n";
    }
    if (body.empty()) continue;
    // Keep the include block readable: add a blank line when the original
    // code continues immediately after the last existing #include.
    if (static_cast<std::size_t>(last) < lines.size()) {
      const std::string& next = lines[static_cast<std::size_t>(last)];
      if (!next.empty()) body += "+\n";
    }

    // True insertion point: right after the last #include.  The engine reads a
    // pure insertion as "place the new lines before this 1-based line", so the
    // rendered old_start is `last + 2` (see the Hunk::insert flag).
    add_hunk(error.file, last + 2, body, /*insert=*/true);
    break;  // one include batch per round, like one edit a model would make
  }

  // ---- R2: missing semicolon ----------------------------------------------
  // Only when no include was added this round: adding lines above would shift
  // every line number in the same diff, which is exactly the situation the
  // engine's cumulative delta has to model.  A real model avoids the trap by
  // sending one edit per call, and so do we -- which also keeps the two rules
  // on separate rounds, as the demo transcript shows.
  if (per_file.empty()) {
    for (const PendingError& error : errors) {
      if (!contains(error.message, "expected ';'")) continue;
      if (error.line <= 0) continue;

      const std::vector<std::string> lines = read_all_lines(resolve(error.file));
      if (static_cast<std::size_t>(error.line) > lines.size()) continue;
      const std::string original = lines[static_cast<std::size_t>(error.line) - 1];
      if (!original.empty() && original.back() == ';') continue;  // already repaired

      add_hunk(error.file, error.line, "-" + original + "\n+" + original + ";\n");
    }
  }

  if (!per_file.empty()) {
    const std::string file = per_file.begin()->first;
    std::vector<Hunk>& hunks = per_file.begin()->second;
    std::stable_sort(hunks.begin(), hunks.end(),
                     [](const Hunk& a, const Hunk& b) { return a.position < b.position; });

    // A single patch call carrying every hunk, each with the +1 drift applied.
    std::string diff = "--- a/" + file + "\n+++ b/" + file + "\n";
    for (const Hunk& hunk : hunks) {
      // Every header carries the intentional +1 drift.  For an insertion the
      // stored position is already the drifted anchor, because the engine
      // places inserted lines *before* old_start.
      const int declared = hunk.insert ? hunk.position : hunk.position + 1;
      const int count = hunk.insert ? 0 : 1;
      diff += "@@ -" + std::to_string(declared) + "," + std::to_string(count) + " +" +
              std::to_string(declared) + "," + std::to_string(count) + " @@\n";
      diff += hunk.text;
    }
    return patch_response(file, diff);
  }

  // ---- R3: explicit argument type -----------------------------------------
  for (const PendingError& error : errors) {
    if (!contains(error.message, "no matching function")) continue;
    for (const auto& [symbol, replacement] : cast_map()) {
      if (!contains(error.message, symbol)) continue;

      const std::vector<std::string> lines = read_all_lines(resolve(error.file));
      if (error.line <= 0 || static_cast<std::size_t>(error.line) > lines.size()) continue;
      const std::string original = lines[static_cast<std::size_t>(error.line) - 1];

      const std::string needle = symbol + "(";
      const std::size_t at = original.find(needle);
      if (at == std::string::npos) continue;
      const std::size_t arg_begin = at + needle.size();
      std::size_t arg_end = arg_begin;
      int depth = 1;
      while (arg_end < original.size() && depth > 0) {
        if (original[arg_end] == '(') ++depth;
        if (original[arg_end] == ')') --depth;
        if (depth == 0) break;
        ++arg_end;
      }
      if (arg_end >= original.size()) continue;

      const std::string argument = original.substr(arg_begin, arg_end - arg_begin);
      std::string fixed = original;
      fixed.replace(at, arg_end - at, replacement + "(" + argument + ")");

      std::ostringstream diff;
      diff << diff_header(error.file, error.line + 1, 1);
      diff << "-" << original << "\n";
      diff << "+" << fixed << "\n";
      return patch_response(error.file, diff.str());
    }
  }

  // ---- R4: give up --------------------------------------------------------
  return give_up();
}

}  // namespace fixit
