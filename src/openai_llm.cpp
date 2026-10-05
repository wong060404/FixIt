// OpenAiLlm -- minimal, non-streaming OpenAI-compatible chat client.
//
// Only cpp-httplib is used for the transport.  The API key lives in memory for
// the duration of the call and is never echoed into any output, trace or log:
// see docs/decisions.md ADR-005.

#include <cstddef>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "fixit/agent.h"

namespace fixit {
namespace {

std::string trim_trailing_slash(std::string url) {
  while (!url.empty() && url.back() == '/') url.pop_back();
  return url;
}

/// Splits "https://host:port/v1/foo" into the client base and the path suffix.
struct Endpoint {
  std::string base;   // scheme://host[:port]
  std::string path;   // /v1/foo
  bool valid = false;
};

Endpoint split_url(const std::string& url) {
  Endpoint endpoint;
  const std::size_t scheme_end = url.find("://");
  if (scheme_end == std::string::npos) return endpoint;
  const std::size_t path_begin = url.find('/', scheme_end + 3);
  if (path_begin == std::string::npos) {
    endpoint.base = url;
    endpoint.path = "/v1/chat/completions";
  } else {
    endpoint.base = url.substr(0, path_begin);
    std::string rest = url.substr(path_begin);
    if (rest == "/" || rest.empty()) rest = "/v1/chat/completions";
    // A caller may point straight at the completions endpoint or at a /v1 root.
    if (rest.find("chat/completions") == std::string::npos) {
      while (!rest.empty() && rest.back() == '/') rest.pop_back();
      rest += "/chat/completions";
    }
    endpoint.path = rest;
  }
  endpoint.valid = true;
  return endpoint;
}

nlohmann::json tool_specs_to_json(const std::vector<ToolSpec>& tools) {
  nlohmann::json array = nlohmann::json::array();
  for (const ToolSpec& spec : tools) {
    nlohmann::json parameters = nlohmann::json::parse(spec.json_schema, nullptr, false);
    if (parameters.is_discarded()) parameters = nlohmann::json{{"type", "object"}};
    array.push_back(nlohmann::json{{"type", "function"},
                                   {"function",
                                    {{"name", spec.name},
                                     {"description", spec.description},
                                     {"parameters", std::move(parameters)}}}});
  }
  return array;
}

nlohmann::json messages_to_json(const std::vector<Message>& messages) {
  nlohmann::json array = nlohmann::json::array();
  for (const Message& message : messages) {
    nlohmann::json entry = {{"role", message.role}};
    if (message.role == "tool") {
      // OpenAI-compatible servers expect tool results as plain content.
      entry["content"] = message.content;
    } else if (!message.content.empty()) {
      entry["content"] = message.content;
    } else {
      entry["content"] = "";
    }
    if (message.role == "assistant" && message.tool_calls.is_array() &&
        !message.tool_calls.empty()) {
      entry["tool_calls"] = message.tool_calls;
    }
    array.push_back(std::move(entry));
  }
  return array;
}

}  // namespace

bool openai_tls_available() {
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
  return true;
#else
  return false;
#endif
}

OpenAiLlm::OpenAiLlm(std::string base_url, std::string api_key, std::string model)
    : base_url_(trim_trailing_slash(std::move(base_url))),
      api_key_(std::move(api_key)),
      model_(std::move(model)) {}

LlmResponse OpenAiLlm::chat(const std::vector<Message>& messages,
                            const std::vector<ToolSpec>& tools) {
  LlmResponse response;

  const Endpoint endpoint = split_url(base_url_);
  if (!endpoint.valid) {
    std::cerr << "fixit: invalid --base-url '" << base_url_ << "'\n";
    response.is_final = true;
    return response;
  }

  nlohmann::json body = {
      {"model", model_}, {"messages", messages_to_json(messages)}, {"stream", false}};
  const std::vector<ToolSpec> usable = tools;
  if (!usable.empty()) {
    body["tools"] = tool_specs_to_json(usable);
    body["tool_choice"] = "auto";
  }

  httplib::Headers headers = {{"Content-Type", "application/json"}};
  if (!api_key_.empty()) headers.emplace("Authorization", "Bearer " + api_key_);

  // The key must never reach stdout/stderr, so errors only mention the status.
  const auto handle = [&](const httplib::Result& result) {
    if (!result) {
      std::cerr << "fixit: LLM request failed: " << httplib::to_string(result.error()) << "\n";
      return false;
    }
    if (result->status < 200 || result->status >= 300) {
      std::cerr << "fixit: LLM request returned HTTP " << result->status << "\n";
      nlohmann::json error = nlohmann::json::parse(result->body, nullptr, false);
      if (!error.is_discarded() && error.contains("error")) {
        const nlohmann::json& e = error["error"];
        if (e.is_object() && e.contains("message")) {
          std::cerr << "fixit: " << e["message"].get<std::string>() << "\n";
        }
      }
      return false;
    }
    return true;
  };

  httplib::Result result;
  if (endpoint.base.rfind("https://", 0) == 0) {
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
    httplib::SSLClient client(endpoint.base.substr(8));
    client.set_follow_location(true);
    client.set_connection_timeout(30);
    client.set_read_timeout(120);

    // Where do the trusted CAs come from?  A system OpenSSL does not read the
    // macOS Keychain (that is a SecureTransport/CFNetwork privilege, which is why
    // curl works out of the box), and this machine had no OpenSSL default path
    // either -- so verification failed against a perfectly valid Let's Encrypt
    // certificate.  Point OpenSSL at a bundle explicitly.
    //
    // FIXIT_CA_BUNDLE overrides; otherwise the platform bundle is used when it
    // exists.  Verification is never disabled.
    const char* ca_from_env = std::getenv("FIXIT_CA_BUNDLE");
    std::string ca_bundle = (ca_from_env != nullptr && *ca_from_env != '\0')
                                ? std::string(ca_from_env)
                                : std::string();
#if defined(__APPLE__)
    if (ca_bundle.empty()) {
      // Shipped by macOS (and used by its curl); contains the public roots.
      std::ifstream system_bundle("/etc/ssl/cert.pem");
      if (system_bundle) ca_bundle = "/etc/ssl/cert.pem";
    }
#endif
    if (ca_bundle.empty()) {
      for (const char* candidate : {"/etc/ssl/certs/ca-certificates.crt",
                                    "/etc/pki/tls/certs/ca-bundle.crt",
                                    "/etc/ssl/ca-bundle.pem"}) {
        std::ifstream bundle(candidate);
        if (bundle) {
          ca_bundle = candidate;
          break;
        }
      }
    }
    if (!ca_bundle.empty()) client.set_ca_cert_path(ca_bundle.c_str());
    client.enable_server_certificate_verification(true);
    result = client.Post(endpoint.path, headers, body.dump(), "application/json");
    if (!handle(result)) {
      response.is_final = true;
      return response;
    }
#else
    std::cerr << "fixit: this build has no TLS support; rebuild with "
                 "-DFIXIT_ENABLE_OPENSSL=ON to use https:// endpoints, or point "
                 "--base-url at a plain http:// OpenAI-compatible server.\n";
    response.is_final = true;
    return response;
#endif
  } else {
    const std::string host_port =
        endpoint.base.rfind("http://", 0) == 0 ? endpoint.base.substr(7) : endpoint.base;
    httplib::Client client(host_port);
    client.set_follow_location(true);
    client.set_connection_timeout(30);
    client.set_read_timeout(120);
    result = client.Post(endpoint.path, headers, body.dump(), "application/json");
    if (!handle(result)) {
      response.is_final = true;
      return response;
    }
  }

  nlohmann::json parsed = nlohmann::json::parse(result->body, nullptr, false);
  if (parsed.is_discarded() || !parsed.is_object()) {
    std::cerr << "fixit: LLM response was not JSON\n";
    response.is_final = true;
    return response;
  }

  if (!parsed.contains("choices") || !parsed["choices"].is_array() || parsed["choices"].empty()) {
    std::cerr << "fixit: LLM response contained no choices\n";
    response.is_final = true;
    return response;
  }

  const nlohmann::json& message = parsed["choices"][0].value("message", nlohmann::json::object());
  response.content = message.value("content", std::string());

  if (message.contains("tool_calls") && message["tool_calls"].is_array()) {
    for (const auto& call : message["tool_calls"]) {
      ToolCall tool_call;
      const nlohmann::json& function = call.value("function", nlohmann::json::object());
      tool_call.name = function.value("name", std::string());
      const std::string arguments = function.value("arguments", std::string("{}"));
      tool_call.args = nlohmann::json::parse(arguments, nullptr, false);
      if (tool_call.args.is_discarded()) tool_call.args = nlohmann::json::object();
      if (!tool_call.name.empty()) response.tool_calls.push_back(std::move(tool_call));
    }
  }

  // A plain text answer that says FINAL is treated as the model being done.
  if (response.tool_calls.empty() &&
      response.content.find("FINAL") != std::string::npos) {
    response.is_final = true;
  }
  return response;
}

}  // namespace fixit
