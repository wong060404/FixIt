// End-to-end tests for the OpenAI-compatible client.
//
// A real loopback HTTP server answers the requests, so the transport, the retry
// policy and the response parsing are exercised for real -- no network access and
// no API key needed.  This is also the only test that proves AI mode works on the
// platform it runs on, instead of only that it compiles.

#include <atomic>
#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <functional>
#include <string>
#include <thread>
#include <utility>

#if defined(_WIN32)
#include <stdlib.h>  // _putenv_s
#endif

#include <catch2/catch_test_macros.hpp>
#include <httplib.h>
#include <nlohmann/json.hpp>

#include "fixit/agent.h"

namespace {

void set_env_var(const char* name, const char* value) {
#if defined(_WIN32)
  _putenv_s(name, value);
#else
  ::setenv(name, value, 1);
#endif
}

/// Serves one handler on 127.0.0.1:ephemeral for the lifetime of the object.
class LoopbackServer {
 public:
  explicit LoopbackServer(std::function<void(const httplib::Request&, httplib::Response&)> handler) {
    server_.Post("/v1/chat/completions", std::move(handler));
    port_ = server_.bind_to_any_port("127.0.0.1");
    thread_ = std::thread([this] { server_.listen_after_bind(); });
    server_.wait_until_ready();
  }
  ~LoopbackServer() {
    server_.stop();
    if (thread_.joinable()) thread_.join();
  }
  LoopbackServer(const LoopbackServer&) = delete;
  LoopbackServer& operator=(const LoopbackServer&) = delete;

  int port() const { return port_; }
  std::string base_url() const { return "http://127.0.0.1:" + std::to_string(port_) + "/v1"; }

 private:
  httplib::Server server_;
  int port_ = -1;
  std::thread thread_;
};

std::string chat_completion(const std::string& content) {
  const nlohmann::json message = {{"role", "assistant"}, {"content", content}};
  const nlohmann::json body = {{"choices", nlohmann::json::array({{{"message", message}}})}};
  return body.dump();
}

}  // namespace

TEST_CASE("the OpenAI client parses content and tool calls", "[openai][http]") {
  std::atomic<bool> saw_bearer{false};
  LoopbackServer server([&saw_bearer](const httplib::Request& request, httplib::Response& response) {
    saw_bearer = request.get_header_value("Authorization") == "Bearer test-key";
    response.status = 200;
    response.set_content(
        R"({"choices":[{"message":{"content":"thinking","tool_calls":[{)"
        R"("id":"call_1","type":"function",)"
        R"("function":{"name":"patch","arguments":"{\"diff\":\"x\"}"}}]}}]})",
        "application/json");
  });
  REQUIRE(server.port() > 0);

  fixit::OpenAiLlm llm(server.base_url(), "test-key", "test-model");
  const fixit::LlmResponse response = llm.chat({}, {});

  CHECK(saw_bearer.load());
  CHECK(response.content == "thinking");
  REQUIRE(response.tool_calls.size() == 1);
  CHECK(response.tool_calls[0].name == "patch");
  CHECK(response.tool_calls[0].args["diff"] == "x");
  CHECK_FALSE(response.is_final);
}

TEST_CASE("a transient server error is retried", "[openai][http][retry]") {
  set_env_var("FIXIT_LLM_RETRY_BASE_MS", "1");
  std::atomic<int> calls{0};
  LoopbackServer server([&calls](const httplib::Request&, httplib::Response& response) {
    if (++calls == 1) {
      response.status = 503;
      response.set_content(R"({"error":{"message":"busy"}})", "application/json");
      return;
    }
    response.status = 200;
    response.set_content(chat_completion("second try"), "application/json");
  });
  REQUIRE(server.port() > 0);

  fixit::OpenAiLlm llm(server.base_url(), "k", "m");
  const fixit::LlmResponse response = llm.chat({}, {});

  CHECK(calls.load() == 2);
  CHECK(response.content == "second try");
  CHECK_FALSE(response.is_final);
}

TEST_CASE("a client error is not retried", "[openai][http][retry]") {
  set_env_var("FIXIT_LLM_RETRY_BASE_MS", "1");
  std::atomic<int> calls{0};
  LoopbackServer server([&calls](const httplib::Request&, httplib::Response& response) {
    ++calls;
    response.status = 401;
    response.set_content(R"({"error":{"message":"bad key"}})", "application/json");
  });
  REQUIRE(server.port() > 0);

  fixit::OpenAiLlm llm(server.base_url(), "k", "m");
  const fixit::LlmResponse response = llm.chat({}, {});

  CHECK(calls.load() == 1);
  CHECK(response.is_final);
}

TEST_CASE("a persistent server error gives up after three attempts", "[openai][http][retry]") {
  set_env_var("FIXIT_LLM_RETRY_BASE_MS", "1");
  std::atomic<int> calls{0};
  LoopbackServer server([&calls](const httplib::Request&, httplib::Response& response) {
    ++calls;
    response.status = 500;
    response.set_content(R"({"error":{"message":"nope"}})", "application/json");
  });
  REQUIRE(server.port() > 0);

  fixit::OpenAiLlm llm(server.base_url(), "k", "m");
  const fixit::LlmResponse response = llm.chat({}, {});

  CHECK(calls.load() == 3);
  CHECK(response.is_final);
}

TEST_CASE("a malformed body is reported, not thrown", "[openai][http][negative]") {
  LoopbackServer server([](const httplib::Request&, httplib::Response& response) {
    response.status = 200;
    response.set_content("this is not json", "application/json");
  });
  REQUIRE(server.port() > 0);

  fixit::OpenAiLlm llm(server.base_url(), "k", "m");
  const fixit::LlmResponse response = llm.chat({}, {});

  CHECK(response.is_final);
  CHECK(response.tool_calls.empty());
}

TEST_CASE("an unreachable endpoint fails without throwing", "[openai][http][negative]") {
  set_env_var("FIXIT_LLM_RETRY_BASE_MS", "0");
  // Port 9 (discard) on loopback: nothing listens, so the connection is refused.
  fixit::OpenAiLlm llm("http://127.0.0.1:9/v1", "k", "m");
  const fixit::LlmResponse response = llm.chat({}, {});
  CHECK(response.is_final);
  CHECK(response.tool_calls.empty());
}

// ---------------------------------------------------------------------------
// End to end: the CLI, the HTTP client, the agent loop and the patch engine
// working together against a fake model.  This is the AI-mode equivalent of the
// demo smoke test: it fails if any of the four stops talking to the others.
// ---------------------------------------------------------------------------

#if defined(_WIN32)
#include <stdio.h>  // _popen / _pclose
#endif

namespace {

#if defined(_WIN32)
FILE* cli_popen(const char* command, const char* mode) { return _popen(command, mode); }
int cli_pclose(FILE* pipe) { return _pclose(pipe); }
int decode_status(int status) { return status; }
#else
#include <sys/wait.h>  // WIFEXITED / WEXITSTATUS
FILE* cli_popen(const char* command, const char* mode) { return ::popen(command, mode); }
int cli_pclose(FILE* pipe) { return ::pclose(pipe); }
int decode_status(int status) { return WIFEXITED(status) ? WEXITSTATUS(status) : -1; }
#endif

/// A directory that removes itself.
class ScratchDir {
 public:
  ScratchDir() {
    static int counter = 0;
    path_ = std::filesystem::temp_directory_path() /
            ("fixit-e2e-" + std::to_string(++counter));
    std::filesystem::create_directories(path_);
  }
  ~ScratchDir() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }
  ScratchDir(const ScratchDir&) = delete;
  ScratchDir& operator=(const ScratchDir&) = delete;
  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

std::string run_cli(const std::string& command, int& exit_code) {
  exit_code = -1;
  std::string output;
  FILE* pipe = cli_popen((command + " 2>&1").c_str(), "r");
  if (pipe == nullptr) return output;
  std::array<char, 4096> buffer{};
  std::size_t read = 0;
  while ((read = std::fread(buffer.data(), 1, buffer.size(), pipe)) > 0) {
    output.append(buffer.data(), read);
  }
  exit_code = decode_status(cli_pclose(pipe));
  return output;
}

}  // namespace

TEST_CASE("the CLI repairs a file end to end through a fake model", "[openai][e2e]") {
  ScratchDir scratch;
  const std::filesystem::path source = scratch.path() / "broken.cpp";
  {
    std::ofstream out(source, std::ios::binary | std::ios::trunc);
    out << "int main() {\n  int x = 1\n  return x;\n}\n";
  }

  std::atomic<int> chats{0};
  LoopbackServer server([&chats](const httplib::Request&, httplib::Response& response) {
    response.status = 200;
    if (++chats == 1) {
      // The model's first answer: one patch call with a drifting hunk header.
      response.set_content(
          R"({"choices":[{"message":{"content":"","tool_calls":[{)"
          R"("type":"function","function":{"name":"patch",)"
          R"("arguments":"{\"file\":\"broken.cpp\",\"diff\":\"--- a/broken.cpp\\n)"
          R"(+++ b/broken.cpp\\n@@ -3,1 +3,1 @@\\n-  int x = 1\\n+  int x = 1;\\n\"}"}}]}}]})",
          "application/json");
      return;
    }
    response.set_content(R"({"choices":[{"message":{"content":"FINAL"}}]})", "application/json");
  });
  REQUIRE(server.port() > 0);

  std::string command = "\"" + std::string(FIXIT_CLI_PATH) + "\" broken.cpp --agent --llm openai";
  command += " --base-url \"" + server.base_url() + "\"";
  command += " --model fake-model --api-key test-key --verbose";
  command += " --no-timing --compiler \"" + std::string(FIXIT_TEST_COMPILER) + "\"";
#ifdef _WIN32
  // cmd /c strips the first and last quote when the line holds more than two, so
  // wrap the whole command in one extra pair (same trick as the CLI tests).
  command = "\"" + command + "\"";
#endif

  std::string output;
  int exit_code = -1;
  {
    const std::string previous = std::filesystem::current_path().string();
    std::filesystem::current_path(scratch.path());
    output = run_cli(command, exit_code);
    std::filesystem::current_path(previous);
  }

  INFO(output);
  CHECK(chats.load() >= 1);
  CHECK(exit_code == 0);
  CHECK(output.find("Fixed 1 errors") != std::string::npos);
  CHECK(output.find("hunk 1") != std::string::npos);

  std::ifstream repaired(source, std::ios::binary);
  std::ostringstream body;
  body << repaired.rdbuf();
  CHECK(body.str().find("int x = 1;") != std::string::npos);
}

TEST_CASE("TLS availability reports what the build actually has", "[openai][tls]") {
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
  CHECK(fixit::openai_tls_available());
#else
  CHECK_FALSE(fixit::openai_tls_available());
#endif
}
