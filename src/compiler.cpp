#include "fixit/compiler.h"

#include <cstddef>
#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sys/stat.h>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <nlohmann/json.hpp>

namespace fixit {

namespace {
/// GCC 12 prints `‘x’` (U+2018/U+2019) where clang prints `'x'`.
const char* kLeftSingleQuote = "\xe2\x80\x98";
const char* kRightSingleQuote = "\xe2\x80\x99";
const char* kLeftDoubleQuote = "\xe2\x80\x9c";
const char* kRightDoubleQuote = "\xe2\x80\x9d";

}  // namespace

std::string normalize_quotes(const std::string& text) {
  std::string out;
  out.reserve(text.size());
  for (std::size_t i = 0; i < text.size();) {
    const bool left = text.compare(i, 3, kLeftSingleQuote) == 0 ||
                      text.compare(i, 3, kLeftDoubleQuote) == 0;
    const bool right = text.compare(i, 3, kRightSingleQuote) == 0 ||
                       text.compare(i, 3, kRightDoubleQuote) == 0;
    if (left || right) {
      out += '\'';
      i += 3;
      continue;
    }
    out += text[i];
    ++i;
  }
  return out;
}

namespace {

constexpr std::size_t kMaxDiagnostics = 50;

std::string trim_left(const std::string& s) {
  std::size_t i = 0;
  while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
  return s.substr(i);
}

std::string rstrip(const std::string& s) {
  std::size_t end = s.size();
  while (end > 0 && (s[end - 1] == '\r' || s[end - 1] == '\n' || s[end - 1] == ' ' ||
                     s[end - 1] == '\t')) {
    --end;
  }
  return s.substr(0, end);
}

/// `12 |   int x = x + 1;`  ->  drop everything up to and including the first '|'.
std::string text_after_pipe(const std::string& line) {
  const std::size_t bar = line.find('|');
  if (bar == std::string::npos) return trim_left(line);
  return line.substr(bar + 1);
}

/// `12 |   ^~~~` -> true
bool is_caret_line(const std::string& line) {
  const std::size_t bar = line.find('|');
  if (bar == std::string::npos) return false;
  const std::string rest = line.substr(bar + 1);
  for (char c : rest) {
    if (c == ' ' || c == '\t') continue;
    if (c == '^' || c == '~') return true;
    return false;
  }
  return false;
}

/// The line-number gutter of a caret/source block, or -1.
int gutter_number(const std::string& line) {
  const std::size_t bar = line.find('|');
  if (bar == std::string::npos) return -1;
  const std::string num = rstrip(line.substr(0, bar));
  if (num.empty()) return -1;
  for (char c : num) {
    if (!std::isdigit(static_cast<unsigned char>(c)) && c != ' ' && c != '\t') return -1;
  }
  try {
    return std::stoi(num);
  } catch (...) {
    return -1;
  }
}

std::string format_context_line(int line_no, const std::string& text) {
  std::ostringstream os;
  os << line_no << " | " << text;
  return os.str();
}

// ---------------------------------------------------------------------------
// GCC / plain-text diagnostics
// ---------------------------------------------------------------------------
std::vector<Diagnostic> parse_gcc(const std::string& raw_output) {
  static const std::regex diag_re(
      R"(^([^:]+):(\d+):(\d+):\s+(error|warning|fatal error|note):\s+(.*)$)");

  std::vector<std::string> lines;
  {
    std::istringstream in(raw_output);
    std::string line;
    while (std::getline(in, line)) lines.push_back(rstrip(line));
  }

  std::vector<Diagnostic> out;
  for (std::size_t i = 0; i < lines.size(); ++i) {
    std::smatch m;
    if (!std::regex_match(lines[i], m, diag_re)) continue;

    const std::string kind = m[4].str();
    if (kind == "note") continue;  // notes are not actionable on their own

    Diagnostic d;
    d.file = m[1].str();
    d.line = std::stoi(m[2].str());
    d.col = std::stoi(m[3].str());
    d.level = (kind == "warning") ? DiagLevel::Warning : DiagLevel::Error;
    d.message = normalize_quotes(m[5].str());

    // Look ahead 1-3 lines for the caret block that belongs to this message.
    for (std::size_t j = i + 1; j < lines.size() && j <= i + 3; ++j) {
      const std::string& cand = lines[j];
      // A new diagnostic terminates the look-ahead.
      if (std::regex_match(cand, diag_re)) break;
      if (cand.empty()) break;
      if (gutter_number(cand) == d.line) {
        if (is_caret_line(cand)) {
          // caret line: the source text is the previous gutter line, if any.
          if (j > i + 1 && gutter_number(lines[j - 1]) == d.line) {
            d.context_line = format_context_line(d.line, text_after_pipe(lines[j - 1]));
          }
          break;
        }
        d.context_line = format_context_line(d.line, text_after_pipe(cand));
        if (j + 1 < lines.size() && is_caret_line(lines[j + 1])) {
          // The caret span (carets only) tells us the column extent.
          const std::string carets = text_after_pipe(lines[j + 1]);
          for (std::size_t k = 0; k < carets.size(); ++k) {
            if (carets[k] == '^') {
              d.span.begin_line = d.line;
              d.span.begin_col = static_cast<int>(k) + 1;
              break;
            }
          }
          std::size_t last = std::string::npos;
          for (std::size_t k = 0; k < carets.size(); ++k) {
            if (carets[k] == '^') last = k;
          }
          if (last != std::string::npos) {
            d.span.end_line = d.line;
            d.span.end_col = static_cast<int>(last) + 2;
          }
          break;
        }
        break;
      }
      // Lines without a gutter that are indented (e.g. template noise) belong
      // to this message; keep looking.
    }
    out.push_back(std::move(d));
    if (out.size() >= kMaxDiagnostics) break;
  }
  return out;
}

// ---------------------------------------------------------------------------
// Clang JSON diagnostics
// ---------------------------------------------------------------------------
void collect_json_diagnostics(const nlohmann::json& j, std::vector<Diagnostic>& out) {
  if (!j.is_array()) return;

  // Iterative walk so that arbitrarily deep `children` trees are supported.
  std::vector<const nlohmann::json*> stack;
  for (auto it = j.rbegin(); it != j.rend(); ++it) stack.push_back(&*it);

  while (!stack.empty() && out.size() < kMaxDiagnostics) {
    const nlohmann::json* node = stack.back();
    stack.pop_back();
    if (node == nullptr || !node->is_object()) continue;

    const std::string level = node->value("level", std::string());
    if (level == "error" || level == "warning" || level == "fatal error") {
      Diagnostic d;
      d.level = (level == "warning") ? DiagLevel::Warning : DiagLevel::Error;
      d.message = node->value("message", std::string());
      // `note` children are folded into the parent message the way a human
      // would read them: `primary message: note; note`.
      if (node->contains("children") && (*node)["children"].is_array()) {
        for (const auto& child : (*node)["children"]) {
          if (child.is_object() && child.value("level", std::string()) == "note" &&
              child.contains("message")) {
            d.message += ": " + child.value("message", std::string());
          }
        }
      }
      if (node->contains("location") && (*node)["location"].is_object()) {
        const nlohmann::json& loc = (*node)["location"];
        d.file = loc.value("file", std::string());
        d.line = loc.value("line", 0);
        d.col = loc.value("column", 0);
        if (loc.contains("caret") && loc["caret"].is_object()) {
          d.span.begin_col = loc["caret"].value("column", d.col);
          d.span.begin_line = d.line;
        }
      }
      if (node->contains("spans") && (*node)["spans"].is_array() && !(*node)["spans"].empty()) {
        const nlohmann::json& span = (*node)["spans"][0];
        if (span.is_object()) {
          if (span.contains("line")) d.line = span.value("line", d.line);
          if (span.contains("column")) d.col = span.value("column", d.col);
          if (span.contains("file")) d.file = span.value("file", d.file);
          if (span.contains("start") && span["start"].is_object()) {
            d.span.begin_line = span["start"].value("line", d.line);
            d.span.begin_col = span["start"].value("column", d.col);
          }
          if (span.contains("end") && span["end"].is_object()) {
            d.span.end_line = span["end"].value("line", d.line);
            d.span.end_col = span["end"].value("column", d.col);
          }
        }
      }
      if (node->contains("line")) d.line = node->value("line", d.line);

      // clang's JSON dialect does not echo the source line; reconstruct it when
      // the diagnostic carries the rendered `source` array.
      if (node->contains("source") && (*node)["source"].is_array() && !(*node)["source"].empty()) {
        std::string text;
        for (const auto& piece : (*node)["source"]) {
          if (piece.is_string()) text += piece.get<std::string>();
        }
        d.context_line = format_context_line(d.line, text);
      }
      out.push_back(std::move(d));
    }

    if (node->contains("children") && (*node)["children"].is_array()) {
      const auto& children = (*node)["children"];
      for (auto it = children.rbegin(); it != children.rend(); ++it) stack.push_back(&*it);
    }
  }
}

void sort_and_dedupe(std::vector<Diagnostic>& diags) {
  std::stable_sort(diags.begin(), diags.end(), [](const Diagnostic& a, const Diagnostic& b) {
    if (a.file != b.file) return a.file < b.file;
    if (a.line != b.line) return a.line < b.line;
    if (a.col != b.col) return a.col < b.col;
    return a.message < b.message;
  });
  diags.erase(std::unique(diags.begin(), diags.end(),
                          [](const Diagnostic& a, const Diagnostic& b) {
                            return a.file == b.file && a.line == b.line && a.col == b.col &&
                                   a.level == b.level && a.message == b.message;
                          }),
              diags.end());
  if (diags.size() > kMaxDiagnostics) diags.resize(kMaxDiagnostics);
}

/// fork/exec with stdout+stderr merged into one pipe, killed after `timeout_s`.
struct CapturedProcess {
  int exit_code = -1;
  bool timed_out = false;
  std::string output;
};

CapturedProcess run_capture(const std::vector<std::string>& argv, int timeout_seconds) {
  CapturedProcess result;

  int pipefd[2];
  if (::pipe(pipefd) != 0) {
    result.output = std::string("fixit: pipe() failed: ") + std::strerror(errno);
    return result;
  }

  const pid_t pid = ::fork();
  if (pid < 0) {
    ::close(pipefd[0]);
    ::close(pipefd[1]);
    result.output = std::string("fixit: fork() failed: ") + std::strerror(errno);
    return result;
  }

  if (pid == 0) {
    // Child: merge stderr into the pipe, then hand over to the compiler.
    ::close(pipefd[0]);
    ::dup2(pipefd[1], STDOUT_FILENO);
    ::dup2(pipefd[1], STDERR_FILENO);
    ::close(pipefd[1]);
    std::vector<char*> c_argv;
    c_argv.reserve(argv.size() + 1);
    for (const std::string& a : argv) c_argv.push_back(const_cast<char*>(a.c_str()));
    c_argv.push_back(nullptr);
    ::execvp(c_argv[0], c_argv.data());
    // exec failed -- report through the same pipe so the parent sees it.
    const std::string msg = std::string("fixit: cannot execute '") + argv[0] +
                            "': " + std::strerror(errno) + "\n";
    ssize_t ignored = ::write(STDERR_FILENO, msg.data(), msg.size());
    (void)ignored;
    ::_exit(127);
  }

  ::close(pipefd[1]);
  const int flags = ::fcntl(pipefd[0], F_GETFL, 0);
  ::fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK);

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds);
  std::array<char, 8192> buf{};
  bool eof = false;
  while (!eof) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      result.timed_out = true;
      ::kill(pid, SIGKILL);
      break;
    }
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
    struct pollfd pfd{};
    pfd.fd = pipefd[0];
    pfd.events = POLLIN;
    const int ready = ::poll(&pfd, 1, static_cast<int>(std::max<int64_t>(1, remaining.count())));
    if (ready <= 0) continue;
    const ssize_t n = ::read(pipefd[0], buf.data(), buf.size());
    if (n > 0) {
      result.output.append(buf.data(), static_cast<std::size_t>(n));
    } else if (n == 0) {
      eof = true;
    } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
      break;
    }
  }

  // Drain whatever is left after a timeout kill so no output is lost.
  if (result.timed_out) {
    for (;;) {
      const ssize_t n = ::read(pipefd[0], buf.data(), buf.size());
      if (n <= 0) break;
      result.output.append(buf.data(), static_cast<std::size_t>(n));
    }
  }
  ::close(pipefd[0]);

  int status = 0;
  if (::waitpid(pid, &status, 0) > 0) {
    if (WIFEXITED(status)) {
      result.exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
      result.exit_code = 128 + WTERMSIG(status);
    }
  }
  if (result.timed_out) result.exit_code = -1;
  return result;
}

}  // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
namespace {

}  // namespace

namespace {

/// Creates and removes a temp file used to probe compiler capabilities.
class TempFile {
 public:
  explicit TempFile(const std::string& contents) {
    char tmpl[] = "/tmp/fixit-probe-XXXXXX";
    const int fd = ::mkstemp(tmpl);
    if (fd < 0) {
      path_.clear();
      return;
    }
    path_ = tmpl;
    ssize_t ignored = ::write(fd, contents.data(), contents.size());
    (void)ignored;
    ::close(fd);
  }
  ~TempFile() {
    if (!path_.empty()) ::unlink(path_.c_str());
  }
  TempFile(const TempFile&) = delete;
  TempFile& operator=(const TempFile&) = delete;

  const std::string& path() const { return path_; }

 private:
  std::string path_;
};

}  // namespace

DiagnosticFlags probe_diagnostic_flags(const std::string& compiler, int timeout_seconds) {
  DiagnosticFlags flags;

  // Probing spawns the compiler twice, and a repair loop constructs a few
  // Compiler objects.  Cache per (compiler, binary mtime) so the cost is paid
  // once while a rebuilt compiler is still re-probed.
  struct CacheEntry {
    std::string key;
    std::int64_t mtime = 0;
    DiagnosticFlags flags;
  };
  static std::mutex cache_mutex;
  static std::vector<CacheEntry> cache;
  const auto mtime_of = [](const std::string& path) -> std::int64_t {
    struct stat info {};
    if (::stat(path.c_str(), &info) != 0) return 0;
    return static_cast<std::int64_t>(info.st_mtime);
  };
  const CacheEntry key_entry{compiler, mtime_of(compiler), {}};
  {
    std::lock_guard<std::mutex> lock(cache_mutex);
    for (const CacheEntry& entry : cache) {
      if (entry.key == key_entry.key && entry.mtime == key_entry.mtime) return entry.flags;
    }
  }

  // An empty translation unit: any JSON-supported compiler succeeds silently.
  static const char kEmpty[] = "// fixit diagnostic-format probe\n";
  const TempFile probe(kEmpty);

  // Does the compiler *reject* this flag?  Testing for rejection rather than for
  // "clean exit with parseable output" matters: `-ferror-limit=5` is accepted
  // silently (no output at all) while `-fjson-diagnostics` is accepted and
  // produces JSON, so one predicate cannot be "exit 0 and output parses".
  const auto rejects = [&](const std::string& flag) {
    std::vector<std::string> argv{compiler, "-std=c++20", "-fsyntax-only", flag};
    if (!probe.path().empty()) argv.push_back(probe.path());
    const CapturedProcess proc = run_capture(argv, timeout_seconds);
    if (proc.timed_out) return true;  // treat a hang as "not usable"
    const std::string out = proc.output;
    for (const char* marker : {"unrecognized", "unknown argument", "invalid value",
                               "not supported", "unsupported option"}) {
      if (out.find(marker) != std::string::npos) return true;
    }
    // Any other failure is a real compilation problem, not a rejected flag.
    return proc.exit_code != 0;
  };

  // A JSON dialect flag must be accepted *and* actually produce JSON.
  const auto accepts_json = [&](const std::string& flag) {
    if (rejects(flag)) return false;
    std::vector<std::string> argv{compiler, "-std=c++20", "-fsyntax-only", flag};
    if (!probe.path().empty()) argv.push_back(probe.path());
    const CapturedProcess proc = run_capture(argv, timeout_seconds);
    const std::string out = trim_left(proc.output);
    if (out.empty()) return false;
    const nlohmann::json j = nlohmann::json::parse(out, nullptr, false);
    return !j.is_discarded();
  };

  // Probed for every compiler, never inferred from its name.  GCC rejects both
  // JSON flags, so it lands on the text dialect -- but it does so because it was
  // asked, not because of how its path looks.  A wrapper script named `g++` that
  // happens to sit next to a clang binary is a real configuration, and name
  // sniffing got it wrong.
  if (accepts_json("-fjson-diagnostics")) {
    flags.uses_json = true;
    flags.json_flag = "-fjson-diagnostics";
  } else if (accepts_json("-fdiagnostics-format=json")) {
    flags.uses_json = true;
    flags.json_flag = "-fdiagnostics-format=json";
  }

  // Independent of the dialect: does this compiler know `-ferror-limit`?  GCC does
  // not, and passing it makes GCC exit before compiling anything -- zero
  // diagnostics, which the loop would otherwise read as a clean build.
  flags.supports_error_limit = !rejects("-ferror-limit=5");

  {
    std::lock_guard<std::mutex> lock(cache_mutex);
    cache.push_back(CacheEntry{compiler, mtime_of(compiler), flags});
  }
  return flags;
}

Compiler::Compiler(CompilerConfig cfg) : cfg_(std::move(cfg)) {
  // Every mode asks the compiler what it accepts.  Only the *parsing* mode is
  // forced; the optional flags are never assumed, because the default
  // (`supports_error_limit = true`) would send a clang-only flag to GCC and make
  // it exit without compiling anything -- the failure this probing exists for.
  const DiagnosticFlags probed = probe_diagnostic_flags(cfg_.compiler);
  switch (cfg_.format) {
    case CompilerConfig::DiagnosticFormat::Text:
      resolved_ = probed;
      resolved_.uses_json = false;
      break;
    case CompilerConfig::DiagnosticFormat::Json:
      resolved_ = probed;
      resolved_.uses_json = true;
      break;
    case CompilerConfig::DiagnosticFormat::Auto:
      resolved_ = probed;
      break;
  }
}

bool Compiler::uses_json_diagnostics() const { return resolved_.uses_json; }

std::vector<std::string> Compiler::command_line(const std::string& source_file) const {
  std::vector<std::string> argv;
  argv.push_back(cfg_.compiler);
  argv.push_back("-std=c++20");
  argv.push_back("-fsyntax-only");
  if (resolved_.supports_error_limit) {
    argv.push_back("-ferror-limit=" + std::to_string(cfg_.error_limit));
  }
  // Only ask for the JSON dialect when we are going to parse JSON.  Testing the
  // flag alone sent `-fjson-diagnostics` even under DiagnosticFormat::Text,
  // which made the compiler emit JSON that the text parser then read as zero
  // diagnostics -- a failed compile that looked like it had no errors at all.
  if (resolved_.uses_json && !resolved_.json_flag.empty()) argv.push_back(resolved_.json_flag);
  for (const std::string& flag : cfg_.extra_flags) argv.push_back(flag);
  argv.push_back(source_file);
  return argv;
}

CompileResult Compiler::compile(const std::string& source_file) const {
  CompileResult result;
  const CapturedProcess proc = run_capture(command_line(source_file), cfg_.timeout_seconds);

  result.exit_code = proc.exit_code;
  result.timed_out = proc.timed_out;
  result.raw_output = proc.output;
  if (proc.timed_out) {
    result.raw_output += "\nfixit: compiler timed out after " +
                         std::to_string(cfg_.timeout_seconds) + "s\n";
  }

  result.diagnostics = parse_diagnostics(proc.output, uses_json_diagnostics());
  sort_and_dedupe(result.diagnostics);
  return result;
}

std::vector<Diagnostic> parse_diagnostics(const std::string& raw_output, bool uses_json) {
  std::vector<Diagnostic> out;

  if (uses_json) {
    // clang emits either one JSON array or one object per line; accept both.
    const std::string trimmed = trim_left(raw_output);
    if (!trimmed.empty() && (trimmed[0] == '[' || trimmed[0] == '{')) {
      nlohmann::json parsed = nlohmann::json::parse(raw_output, nullptr, false);
      if (!parsed.is_discarded()) {
        collect_json_diagnostics(parsed, out);
      } else {
        // Fall back to per-line JSON objects.
        std::istringstream in(raw_output);
        std::string line;
        while (std::getline(in, line)) {
          if (trim_left(line).empty()) continue;
          nlohmann::json one = nlohmann::json::parse(line, nullptr, false);
          if (one.is_discarded()) continue;
          std::vector<Diagnostic> chunk;
          collect_json_diagnostics(one, chunk);
          for (Diagnostic& d : chunk) out.push_back(std::move(d));
        }
      }
    }
  }

  if (out.empty()) {
    out = parse_gcc(raw_output);
  }
  return out;
}

}  // namespace fixit
