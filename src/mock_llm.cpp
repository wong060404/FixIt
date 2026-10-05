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

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cctype>
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
    // The same failure mode, as each compiler words it:
    //   clang: "no member named 'vector' in namespace 'std'"
    //          "use of undeclared identifier 'x'"
    //   GCC  : "'vector' is not a member of 'std'"
    //          "'x' was not declared in this scope"
    // Quotes are already normalised to ASCII by parse_diagnostics(), so one
    // spelling suffices here.
    return contains(message, "undeclared identifier") || contains(message, "was not declared") ||
           contains(message, "not declared in this scope") || contains(message, "no member named") ||
           contains(message, "does not name a") || contains(message, "is not a member of");
  };

  // Only real `#include` directives count -- a usage such as
  // `std::vector<std::string> words;` contains "<vector>" too, and treating that
  // as an existing include would make the rule add nothing.
  // Some diagnostics name the symbol without quotes -- clang's "use of
  // undeclared identifier 'x'" does quote it, but "no member named x" forms and
  // several GCC messages do not.  Fall back to the identifier after the phrase.
  const auto bare_symbol = [](const std::string& message, const std::string& phrase) {
    const std::size_t at = message.find(phrase);
    if (at == std::string::npos) return std::string();
    std::size_t begin = at + phrase.size();
    while (begin < message.size() && message[begin] == ' ') ++begin;
    std::size_t end = begin;
    while (end < message.size()) {
      const char c = message[end];
      if (std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_') {
        ++end;
      } else {
        break;
      }
    }
    return message.substr(begin, end - begin);
  };

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
    // Symbols can be quoted or bare depending on the compiler and the phrasing.
    const std::string bare = bare_symbol(error.message, "undeclared identifier");
    const std::string bare_scope = bare_symbol(error.message, "not declared in this scope");
    for (const auto& [symbol, header] : include_map()) {
      const bool named = contains(error.message, "'" + symbol + "'") || bare == symbol ||
                         bare_scope == symbol;
      if (!named) continue;
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

    // True insertion point: right after the last #include.  A pure insertion
    // means "place the new lines AFTER the 1-based line old_start" (the standard
    // unified-diff reading, matching GNU diff and patch), and for an insertion the
    // rendered header is that same number -- so `last` is exactly the position
    // wanted.  The earlier `last + 2` compensated for an engine that inserted
    // *before* old_start and therefore landed the #include inside the next
    // function.
    add_hunk(error.file, last, body, /*insert=*/true);
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
      // clang: "expected ';' at end of declaration"
      // GCC  : "expected ',' or ';' before 'return'"
      const bool wants_semicolon = contains(error.message, "expected ';'") ||
                                   (contains(error.message, "expected '") &&
                                    contains(error.message, "';'"));
      if (!wants_semicolon) continue;
      if (error.line <= 0) continue;

      const std::vector<std::string> lines = read_all_lines(resolve(error.file));
      if (static_cast<std::size_t>(error.line) > lines.size()) continue;

      // Which line is missing the semicolon?  The compilers anchor this
      // differently, and the "before X" form points at the token that follows the
      // incomplete line rather than at the line itself:
      //   clang: "expected ';' at end of declaration"   -> the incomplete line
      //   GCC  : "expected ',' or ';' before 'return'"  -> the next line
      // Instead of trusting one interpretation, try the reported line and then the
      // line above it, and repair the first one that plausibly needs a semicolon.
      // That accepts either anchor without guessing which compiler produced it.
      const int reported = error.line;
      const int alternates[2] = {reported, reported - 1};
      for (int candidate : alternates) {
        if (candidate < 1 || static_cast<std::size_t>(candidate) > lines.size()) continue;
        const std::string original = lines[static_cast<std::size_t>(candidate) - 1];
        const std::string trimmed = normalize_line(original);
        if (trimmed.empty()) continue;
        if (trimmed.back() == ';') continue;  // already repaired; try the other line
        // Appending a semicolon to a brace, a label or a preprocessor line never
        // helps; a real statement does not end in one of those.
        if (trimmed.back() == '{' || trimmed.back() == '}' || trimmed.back() == ':' ||
            trimmed[0] == '#') {
          continue;
        }
        add_hunk(error.file, candidate, "-" + original + "\n+" + original + ";\n");
        break;
      }
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
      // Every replacement header carries the intentional +1 drift.  An insertion
      // stores its final position directly: the +1 shift used to absorb the
      // engine placing inserts before old_start, which it no longer does.
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
