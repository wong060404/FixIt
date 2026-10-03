#include "fixit/agent.h"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>

#include "fixit/codemap.h"
#include "fixit/patch.h"

namespace fixit {
namespace {

constexpr const char* kSystemPrompt =
    "You are a C++ repair agent. Fix all compile errors using the tools "
    "(compile / read / patch). When compile is clean, respond FINAL.";

std::string basename_only(const std::string& path) {
  const std::size_t slash = path.find_last_of("/\\");
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

/// Resolves `path` against `workdir` and refuses to escape it.
bool resolve_in_workdir(const std::string& workdir, const std::string& path, std::string& out) {
  if (path.empty()) return false;
  const std::string joined = (path.front() == '/') ? path : workdir + "/" + path;
  if (joined.find("/../") != std::string::npos) return false;
  out = joined;
  return true;
}

std::string slurp(const std::string& path, bool& ok) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    ok = false;
    return {};
  }
  std::ostringstream os;
  os << in.rdbuf();
  ok = true;
  return os.str();
}

nlohmann::json diagnostics_to_json(const CompileResult& result) {
  nlohmann::json errors = nlohmann::json::array();
  nlohmann::json warnings = nlohmann::json::array();
  for (const Diagnostic& d : result.diagnostics) {
    nlohmann::json entry = {
        {"file", basename_only(d.file)}, {"line", d.line}, {"col", d.col}, {"message", d.message}};
    if (!d.context_line.empty()) entry["context_line"] = d.context_line;
    if (d.level == DiagLevel::Error) {
      errors.push_back(std::move(entry));
    } else {
      warnings.push_back(std::move(entry));
    }
  }
  return nlohmann::json{{"clean", result.clean()},
                        {"timed_out", result.timed_out},
                        {"exit_code", result.exit_code},
                        {"errors", std::move(errors)},
                        {"warnings", std::move(warnings)}};
}

/// The observation a model always gets after an edit round: what the compiler
/// says *now*.  Without this the next round would refine a stale error list.
std::string compile_feedback_text(const CompileResult& result) {
  const nlohmann::json payload = diagnostics_to_json(result);
  std::ostringstream os;
  os << "Compile result:\n" << payload.dump(2);
  return os.str();
}

int int_arg(const nlohmann::json& args, const char* key, int fallback) {
  if (!args.is_object() || !args.contains(key)) return fallback;
  const nlohmann::json& v = args[key];
  if (v.is_number_integer()) return v.get<int>();
  if (v.is_string()) {
    try {
      return std::stoi(v.get<std::string>());
    } catch (...) {
      return fallback;
    }
  }
  return fallback;
}

std::string string_arg(const nlohmann::json& args, const char* key) {
  if (!args.is_object() || !args.contains(key)) return {};
  const nlohmann::json& v = args[key];
  return v.is_string() ? v.get<std::string>() : std::string();
}

HunkReport::Status status_from_string(const std::string& s) {
  if (s == "applied") return HunkReport::Status::Applied;
  if (s == "fuzzy_applied") return HunkReport::Status::FuzzyApplied;
  return HunkReport::Status::Failed;
}

/// Rebuilds the structured patch result from the patch tool's JSON answer so
/// that metrics and verbose output keep full fidelity.
PatchResult patch_result_from_json(const nlohmann::json& observation) {
  PatchResult result;
  result.all_applied = observation.value("all_applied", false);
  if (observation.contains("reports") && observation["reports"].is_array()) {
    for (const auto& entry : observation["reports"]) {
      HunkReport report;
      report.hunk_index = entry.value("hunk", 0);
      report.declared_pos = entry.value("declared_pos", 0);
      report.matched_pos = entry.value("matched_pos", 0);
      report.score = entry.value("score", 0.0);
      report.status = status_from_string(entry.value("status", std::string("failed")));
      report.failure_reason = entry.value("failure_reason", std::string());
      result.reports.push_back(std::move(report));
    }
  }
  return result;
}

}  // namespace

// ---------------------------------------------------------------------------
// ToolRegistry
// ---------------------------------------------------------------------------
void ToolRegistry::add(std::string name, std::string description, std::string json_schema,
                       Handler fn) {
  Entry entry;
  entry.spec = ToolSpec{std::move(name), std::move(description), std::move(json_schema)};
  entry.fn = std::move(fn);
  tools_[entry.spec.name] = std::move(entry);
}

std::vector<ToolSpec> ToolRegistry::specs() const {
  std::vector<ToolSpec> out;
  out.reserve(tools_.size());
  for (const auto& [name, entry] : tools_) out.push_back(entry.spec);
  return out;
}

bool ToolRegistry::has(const std::string& name) const { return tools_.count(name) > 0; }

nlohmann::json ToolRegistry::call(const std::string& name, const nlohmann::json& args) const {
  const auto it = tools_.find(name);
  if (it == tools_.end()) {
    return nlohmann::json{{"error", "unknown tool: " + name}};
  }
  try {
    return it->second.fn(args);
  } catch (const std::exception& e) {
    return nlohmann::json{{"error", std::string("tool threw: ") + e.what()}};
  } catch (...) {
    return nlohmann::json{{"error", "tool threw an unknown exception"}};
  }
}

// ---------------------------------------------------------------------------
// Standard tools
// ---------------------------------------------------------------------------
ToolRegistry make_standard_tools(std::string workdir) {
  ToolRegistry registry;

  registry.add("compile", "Compile a C++ source file and return its diagnostics.",
               R"({"type":"object","properties":{"file":{"type":"string"}},"required":["file"]})",
               [workdir](const nlohmann::json& args) -> nlohmann::json {
                 const std::string file = string_arg(args, "file");
                 std::string resolved;
                 if (!resolve_in_workdir(workdir, file, resolved)) {
                   return nlohmann::json{{"error", "invalid file argument"}};
                 }
                 const Compiler compiler;
                 const CompileResult result = compiler.compile(resolved);
                 nlohmann::json out = diagnostics_to_json(result);
                 out["file"] = basename_only(file);
                 return out;
               });

  registry.add("read", "Read a line range of a file; lines are numbered for quoting.",
               R"({"type":"object","properties":{"file":{"type":"string"},"start":{"type":"integer"},"end":{"type":"integer"}},"required":["file","start","end"]})",
               [workdir](const nlohmann::json& args) -> nlohmann::json {
                 const std::string file = string_arg(args, "file");
                 std::string resolved;
                 if (!resolve_in_workdir(workdir, file, resolved)) {
                   return nlohmann::json{{"error", "invalid file argument"}};
                 }
                 bool ok = false;
                 const std::string content = slurp(resolved, ok);
                 if (!ok) return nlohmann::json{{"error", "cannot read " + file}};

                 const auto total = static_cast<int>(std::count(content.begin(), content.end(), '\n'));
                 int start = int_arg(args, "start", 1);
                 int end = int_arg(args, "end", total);
                 start = std::max(1, start);
                 end = std::max(start, std::min(end, total));

                 std::istringstream in(content);
                 std::ostringstream out;
                 std::string line;
                 int number = 0;
                 while (std::getline(in, line) && number <= end) {
                   ++number;
                   if (number < start) continue;
                   std::ostringstream formatted;
                   formatted.width(4);
                   formatted << number;
                   out << formatted.str() << " | " << line << "\n";
                 }
                 return nlohmann::json{{"content", out.str()}, {"start", start}, {"end", end}};
               });

  registry.add("patch", "Apply a unified diff to a file with the fuzzy patch engine.",
               R"({"type":"object","properties":{"file":{"type":"string"},"diff":{"type":"string"}},"required":["file","diff"]})",
               [workdir](const nlohmann::json& args) -> nlohmann::json {
                 const std::string file = string_arg(args, "file");
                 const std::string diff = string_arg(args, "diff");
                 std::string resolved;
                 if (!resolve_in_workdir(workdir, file, resolved)) {
                   return nlohmann::json{{"error", "invalid file argument"}};
                 }
                 bool ok = false;
                 const std::string content = slurp(resolved, ok);
                 if (!ok) return nlohmann::json{{"error", "cannot read " + file}};

                 const PatchEngine engine;
                 const PatchResult result = engine.apply(content, diff, basename_only(file));
                 if (result.all_applied) {
                   std::ofstream out(resolved, std::ios::binary | std::ios::trunc);
                   if (!out) return nlohmann::json{{"error", "cannot write " + file}};
                   out << result.new_content;
                 }

                 nlohmann::json reports = nlohmann::json::array();
                 for (const HunkReport& r : result.reports) {
                   const char* status = r.status == HunkReport::Status::Applied ? "applied"
                                        : r.status == HunkReport::Status::FuzzyApplied
                                            ? "fuzzy_applied"
                                            : "failed";
                   nlohmann::json entry = {{"hunk", r.hunk_index},
                                           {"status", status},
                                           {"declared_pos", r.declared_pos},
                                           {"matched_pos", r.matched_pos},
                                           {"score", r.score}};
                   if (r.status == HunkReport::Status::Failed) {
                     entry["failure_reason"] = r.failure_reason;
                   }
                   reports.push_back(std::move(entry));
                 }
                 return nlohmann::json{
                     {"all_applied", result.all_applied},
                     {"reports", std::move(reports)},
                     {"failure_summary", result.failure_summary(basename_only(file))}};
               });

  return registry;
}

// ---------------------------------------------------------------------------
// Agent
// ---------------------------------------------------------------------------
void Agent::set_progress(ProgressFn fn) {
  progress_ = std::move(fn);
  if (!progress_) {
    observer_ = nullptr;
    return;
  }
  ProgressFn captured = progress_;
  observer_ = [captured](const Event& event) {
    if (event.kind == Event::Kind::Compile) captured(event.round, event.compile);
  };
}

void Agent::emit_compile(int round, const CompileResult& result, bool verification) {
  if (observer_) {
    Event event;
    event.kind = Event::Kind::Compile;
    event.round = round;
    event.compile = result;
    event.verification = verification;
    observer_(event);
  }
}

Agent::Agent(ToolRegistry tools, std::unique_ptr<Llm> llm, Compiler compiler, std::string workdir)
    : tools_(std::move(tools)),
      llm_(std::move(llm)),
      compiler_(std::move(compiler)),
      workdir_(std::move(workdir)) {
  // A backend that resolves files itself gets the same working directory the
  // tools use, so behaviour cannot diverge between the two.
  if (auto* aware = dynamic_cast<WorkspaceAware*>(llm_.get()); aware != nullptr) {
    aware->set_workspace(workdir_);
  }
}

AgentResult Agent::run(const std::string& task, int max_iterations) {
  AgentResult result;
  result.trace_path = trace_path_;
  if (!llm_) return result;

  const std::string source_path =
      (task.find('/') == std::string::npos) ? workdir_ + "/" + task : task;
  const std::string source_name = basename_only(source_path);

  nlohmann::json trace = nlohmann::json::array();

  // Round 0: the compiler always gets the first word.
  CompileResult compiled = compiler_.compile(source_path);
  errors_fixed_ = static_cast<int>(compiled.error_count());
  emit_compile(0, compiled);
  if (compiled.clean()) {
    result.success = true;
    result.errors_fixed = 0;
    result.final_errors = compiled.diagnostics;
    return result;
  }

  std::vector<Message> messages;
  messages.push_back(Message{"system", kSystemPrompt, nlohmann::json::array(), nlohmann::json()});
  {
    const nlohmann::json payload = diagnostics_to_json(compiled);
    std::ostringstream user;
    user << "Task: " << task << "\n"
         << "File: " << source_name << "\n"
         << "Compile result:\n"
         << payload.dump(2);
    messages.push_back(Message{"user", user.str(), nlohmann::json::array(), nlohmann::json()});
  }

  const std::vector<ToolSpec> specs = tools_.specs();

  for (int round = 1; round <= max_iterations; ++round) {
    result.iterations = round;
    // The compile that this round's model call is based on.
    emit_compile(round, compiled);

    const LlmResponse response = llm_->chat(messages, specs);

    nlohmann::json round_entry = {{"round", round}, {"tool_calls", nlohmann::json::array()}};
    if (!response.content.empty()) round_entry["assistant"] = response.content;

    if (response.is_final) {
      round_entry["llm_final"] = true;
      trace.push_back(std::move(round_entry));
      // A FINAL claim is only worth what the compiler says it is worth.
      compiled = compiler_.compile(source_path);
      result.final_errors = compiled.diagnostics;
      result.success = compiled.clean();
      break;
    }

    // Execute the requested tools and append their raw results as observations.
    nlohmann::json observations = nlohmann::json::array();
    for (const ToolCall& call : response.tool_calls) {
      const nlohmann::json observation = tools_.call(call.name, call.args);

      if (call.name == "patch" && observation.is_object() && observation.contains("reports")) {
        PatchResult patch = patch_result_from_json(observation);
        if (observer_) {
          Event event;
          event.kind = Event::Kind::Patch;
          event.round = round;
          event.patch = patch;
          observer_(event);
        }
        result.patches.emplace_back(round, std::move(patch));
      }

      round_entry["tool_calls"].push_back(
          nlohmann::json{{"name", call.name}, {"args", call.args}});
      observations.push_back(nlohmann::json{{"tool", call.name}, {"result", observation}});
      messages.push_back(Message{"tool", observation.dump(2), nlohmann::json::array(), observation});
    }
    round_entry["observations"] = std::move(observations);
    trace.push_back(std::move(round_entry));

    // Always re-verify with the compiler, whatever the model claimed, and feed
    // the fresh result back so the next round never works from a stale list.
    compiled = compiler_.compile(source_path);
    result.final_errors = compiled.diagnostics;
    trace.back()["compile_after"] = diagnostics_to_json(compiled);
    messages.push_back(Message{"user", compile_feedback_text(compiled),
                               nlohmann::json::array(), nlohmann::json()});
    if (compiled.clean()) {
      result.success = true;
      result.iterations = round;  // model rounds, i.e. the trace length
      emit_compile(round, compiled, /*verification=*/true);
      break;
    }
    if (response.tool_calls.empty()) break;  // nothing further to try
  }

  result.errors_fixed = errors_fixed_;
  if (!trace_path_.empty()) {
    std::ofstream out(trace_path_, std::ios::binary | std::ios::trunc);
    if (out) out << trace.dump(2) << "\n";
  }
  return result;
}

}  // namespace fixit
