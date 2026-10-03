#pragma once
// fixit::Agent -- the repair loop that closes the circle:
//   compile -> locate -> LLM patch -> fuzzy apply -> compile again
//
// Termination is decided by the compiler, never by the model's opinion: a FINAL
// answer still triggers one more compile, and only a clean result counts as
// success.

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "fixit/compiler.h"
#include "fixit/patch.h"
#include "fixit/types.h"

namespace fixit {

struct ToolSpec {
  std::string name;
  std::string description;
  std::string json_schema;
};

/// Model-visible conversation turn.
struct Message {
  std::string role;  // "system" | "user" | "assistant" | "tool"
  std::string content;
  nlohmann::json tool_calls = nlohmann::json::array();
  nlohmann::json tool_result = nlohmann::json();
};

struct ToolCall {
  std::string name;
  nlohmann::json args;
};

struct LlmResponse {
  bool is_final = false;
  std::string content;
  std::vector<ToolCall> tool_calls;
};

class ToolRegistry {
 public:
  using Handler = std::function<nlohmann::json(const nlohmann::json&)>;

  void add(std::string name, std::string description, std::string json_schema, Handler fn);
  std::vector<ToolSpec> specs() const;
  nlohmann::json call(const std::string& name, const nlohmann::json& args) const;
  bool has(const std::string& name) const;

 private:
  struct Entry {
    ToolSpec spec;
    Handler fn;
  };
  std::map<std::string, Entry> tools_;
};

class Llm {
 public:
  virtual ~Llm() = default;
  virtual LlmResponse chat(const std::vector<Message>& messages,
                           const std::vector<ToolSpec>& tools) = 0;
};

/// Optional capability: a backend that has to read project files itself (the
/// mock inspects the real source to build its diff) implements this so the
/// agent can hand it the working directory it was constructed with.  The CLI
/// never has to know.
class WorkspaceAware {
 public:
  virtual ~WorkspaceAware() = default;
  virtual void set_workspace(const std::string& workdir) = 0;
};

/// OpenAI-compatible /v1/chat/completions client (non-streaming).
class OpenAiLlm : public Llm {
 public:
  OpenAiLlm(std::string base_url, std::string api_key, std::string model);

  LlmResponse chat(const std::vector<Message>& messages,
                   const std::vector<ToolSpec>& tools) override;

  const std::string& model() const { return model_; }

 private:
  std::string base_url_;
  std::string api_key_;
  std::string model_;
};

/// True when this build can reach https:// endpoints (FIXIT_ENABLE_OPENSSL=ON).
bool openai_tls_available();

/// Rule-based, fully deterministic, offline stand-in for an LLM.
/// Rules and the intentional +1 line drift are documented in the README and in
/// docs/decisions.md.
class MockLlm : public Llm, public WorkspaceAware {
 public:
  MockLlm();

  /// Directory used to resolve the files named in diagnostics, mirroring the
  /// workdir the patch tool uses.  Empty means "current process directory".
  /// The agent sets this automatically.
  void set_workspace(const std::string& workdir) override;
  LlmResponse chat(const std::vector<Message>& messages,
                   const std::vector<ToolSpec>& tools) override;
};

struct AgentResult {
  bool success = false;
  int iterations = 0;
  /// Distinct error-level diagnostics the loop saw at the start; this is the
  /// number of problems repaired when success is true.
  int errors_fixed = 0;
  std::vector<Diagnostic> final_errors;
  std::vector<std::pair<int /*round*/, PatchResult>> patches;
  std::string trace_path;
};

class Agent {
 public:
  Agent(ToolRegistry tools, std::unique_ptr<Llm> llm, Compiler compiler, std::string workdir);

  /// `task` is the natural-language goal; the loop appends compiler output.
  AgentResult run(const std::string& task, int max_iterations = 4);

  void set_trace_path(std::string path) { trace_path_ = std::move(path); }
  void set_verbose(bool on) { verbose_ = on; }

  /// Structured progress events, emitted in execution order, so a front end can
  /// reproduce the exact CLI transcript without re-running anything.
  struct Event {
    enum class Kind { Compile, Patch };
    Kind kind = Kind::Compile;
    int round = 0;  ///< 1-based agent round; 0 for the initial compile
    CompileResult compile;  ///< set for Kind::Compile
    PatchResult patch;      ///< set for Kind::Patch
    /// The terminal compile that confirms (or rejects) success.  Front ends
    /// print it as the outcome line, not as another repair round.
    bool verification = false;
  };
  using Observer = std::function<void(const Event&)>;
  void set_observer(Observer fn) { observer_ = std::move(fn); }

  /// Convenience wrapper used by tests: called for every compile.
  using ProgressFn = std::function<void(int round, const CompileResult&)>;
  void set_progress(ProgressFn fn);

 private:
  void emit_compile(int round, const CompileResult& result, bool verification = false);

  ToolRegistry tools_;
  std::unique_ptr<Llm> llm_;
  Compiler compiler_;
  std::string workdir_;
  std::string trace_path_;
  bool verbose_ = false;
  int errors_fixed_ = 0;
  ProgressFn progress_;
  Observer observer_;
};

/// Builds the three standard tools (compile / read / patch) bound to `workdir`.
ToolRegistry make_standard_tools(std::string workdir);

}  // namespace fixit
