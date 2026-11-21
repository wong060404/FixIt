#!/usr/bin/env python3
"""A scripted OpenAI-compatible server, for testing FixIt's AI mode offline.

The Catch2 suite drives `MockLlm` in-process, so nothing in it exercises the
parts that only exist for a real backend: the HTTP client, the OpenAI JSON
shapes, the tool-call round trip and the message history that a strict server
would reject if it were built wrongly.  This server closes that gap.  It listens
on 127.0.0.1, speaks the same wire protocol, and follows a fixed plan: read the
file, patch the fault the compiler named, answer FINAL once the re-compile is
clean.

It is a test double, not a model.  It recognises the two fault shapes planted in
`examples/buggy/e1` and `e2` -- a missing `<vector>` include and a missing
semicolon -- and gives up on anything else, which is why e3 is expected to end
unrepaired.  For a real model see the relay (tools/windows/llm-relay.py) or
point `--base-url` at a local server such as Ollama.

    python3 tools/stub_llm_server.py --port 18081 --verbose

    ./build/bin/fixit examples/buggy/e1_missing_include.cpp \
        --agent --llm openai --base-url http://127.0.0.1:18081/v1 \
        --api-key stub --model stub --no-write --iterations 6

`--drift N` shifts every hunk header by N lines, so the fuzzy patch engine has
to search for the position instead of trusting the number.  `tools/ai_mode_smoke.sh`
uses it to make the fuzzy path part of the acceptance test.

Exit codes are FixIt's, not this server's: a repair is 0, a refusal is 1.
"""

import argparse
import json
import os
import re
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

# A read is answered with at most this many lines, which is plenty for the
# fixtures and keeps the message history small.
MAX_READ_LINES = 400


def log(enabled, message):
    if enabled:
        print("[stub] " + message, file=sys.stderr, flush=True)


def compile_errors(messages):
    """The error messages from the most recent `Compile result:` payload."""
    for message in reversed(messages):
        if message.get("role") != "user":
            continue
        text = message.get("content") or ""
        match = re.search(r"Compile result:\s*(\{.*\})", text, re.S)
        if not match:
            continue
        try:
            payload = json.loads(match.group(1))
        except ValueError:
            continue
        return [str(error.get("message", "")) for error in payload.get("errors", [])]
    return []


def last_compile_is_clean(messages):
    for message in reversed(messages):
        if message.get("role") != "user":
            continue
        text = message.get("content") or ""
        if "Compile result:" not in text:
            continue
        return re.search(r'"clean":\s*true', text) is not None
    return False


def read_result_after_last_patch(messages):
    """The numbered `read` output, but only if it postdates the last patch.

    A diff built from pre-patch numbering is the stale-diff case the engine has
    to refuse, so a competent model re-reads after every edit.  Doing the same
    here keeps the stub honest and exercises the read tool.
    """
    last_patch = -1
    for index, message in enumerate(messages):
        if message.get("role") != "assistant":
            continue
        for call in message.get("tool_calls") or []:
            if (call.get("function") or {}).get("name") == "patch":
                last_patch = index
    for index, message in enumerate(messages):
        if index <= last_patch or message.get("role") != "tool":
            continue
        try:
            payload = json.loads(message.get("content") or "{}")
        except ValueError:
            continue
        if "outline" in payload and "content" in payload:
            return payload["content"]
    return None


def unnumber(text):
    """Turn the read tool's `   12 | line` output back into plain lines."""
    lines = []
    for raw in text.splitlines():
        match = re.match(r"^\s*\d+ \| ?(.*)$", raw)
        if match:
            lines.append(match.group(1))
    return lines


def hunk(name, lines, index, removed, added, drift):
    """One unified-diff hunk around a 0-based index, with optional drift."""
    before = lines[max(0, index - 2):index]
    after = lines[index + len(removed):index + len(removed) + 2]
    start = index - len(before) + 1 + drift
    out = [
        "--- a/%s" % name,
        "+++ b/%s" % name,
        "@@ -%d,%d +%d,%d @@" % (start, len(before) + len(removed) + len(after),
                                 start, len(before) + len(added) + len(after)),
    ]
    out += [" " + line for line in before]
    out += ["-" + line for line in removed]
    out += ["+" + line for line in added]
    out += [" " + line for line in after]
    return "\n".join(out)


def plan_patch(name, lines, errors, drift):
    """Choose the next edit from what the compiler complained about."""
    joined = "\n".join(lines)
    text = " ".join(errors)
    if "vector" in text and "#include <vector>" not in joined:
        last_include = 0
        for index, line in enumerate(lines):
            if line.startswith("#include"):
                last_include = index
        return hunk(name, lines, last_include + 1, [], ["#include <vector>"], drift)
    if "expected" in text and "';'" in text:
        for index, line in enumerate(lines):
            if re.match(r"^\s*int\s+\w+\s*=\s*\d+\s*$", line):
                return hunk(name, lines, index, [line], [line.rstrip() + ";"], drift)
    return None


def source_name(messages):
    for message in messages:
        match = re.search(r"File:\s*(\S+)", message.get("content") or "")
        if match:
            return match.group(1)
    return "source.cpp"


class Stub(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    verbose = False
    drift = 0
    max_patches = 3

    def log_message(self, fmt, *args):
        log(self.verbose, fmt % args)

    def do_GET(self):
        self.reply({"status": "ok",
                    "hint": "POST /v1/chat/completions",
                    "drift": self.drift})

    def do_POST(self):
        if not self.path.rstrip("/").endswith("/chat/completions"):
            self.send_error(404, "unknown path")
            return
        try:
            length = int(self.headers.get("Content-Length") or 0)
        except ValueError:
            length = 0
        try:
            body = json.loads(self.rfile.read(length) or b"{}")
        except ValueError:
            self.send_error(400, "body is not JSON")
            return

        messages = body.get("messages") or []
        model = body.get("model", "stub")
        name = source_name(messages)
        if not body.get("tools"):
            self.chat(model, "FINAL", [])
            return
        if last_compile_is_clean(messages):
            log(self.verbose, "compile is clean -> FINAL")
            self.chat(model, "FINAL", [])
            return

        patches = sum(
            1 for message in messages if message.get("role") == "assistant"
            for call in (message.get("tool_calls") or [])
            if (call.get("function") or {}).get("name") == "patch")
        if patches >= self.max_patches:
            log(self.verbose, "patch budget spent -> FINAL")
            self.chat(model, "FINAL", [])
            return

        content = read_result_after_last_patch(messages)
        if content is None:
            log(self.verbose, "-> read " + name)
            self.chat(model, None, [{
                "id": "call_read", "type": "function",
                "function": {"name": "read",
                             "arguments": json.dumps({"file": name, "start": 1,
                                                      "end": MAX_READ_LINES})}}])
            return

        diff = plan_patch(name, unnumber(content), compile_errors(messages), self.drift)
        if diff is None:
            log(self.verbose, "no rule matches these errors -> FINAL")
            self.chat(model, "FINAL", [])
            return
        log(self.verbose, "-> patch " + name)
        self.chat(model, None, [{
            "id": "call_patch_%d" % patches, "type": "function",
            "function": {"name": "patch",
                         "arguments": json.dumps({"file": name, "diff": diff})}}])

    def chat(self, model, content, tool_calls):
        message = {"role": "assistant", "content": content}
        if tool_calls:
            message["content"] = None
            message["tool_calls"] = tool_calls
        self.reply({
            "id": "chatcmpl-stub",
            "object": "chat.completion",
            "model": model,
            "choices": [{"index": 0, "message": message,
                         "finish_reason": "tool_calls" if tool_calls else "stop"}],
            "usage": {"prompt_tokens": 1, "completion_tokens": 1, "total_tokens": 2},
        })

    def reply(self, payload):
        body = json.dumps(payload).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


class Server(ThreadingHTTPServer):
    daemon_threads = True
    # On Windows SO_REUSEADDR lets a second stub bind the same port and the OS
    # then splits requests between them, which would make a run irreproducible
    # (tools/windows/llm-relay.py documents the same trap).  On POSIX it only
    # lifts TIME_WAIT, so a quick restart after Ctrl-C keeps working.
    allow_reuse_address = os.name != "nt"


def main():
    parser = argparse.ArgumentParser(description="Scripted OpenAI-compatible server for FixIt.")
    parser.add_argument("--port", type=int, default=18081,
                        help="port to listen on (default 18081)")
    parser.add_argument("--drift", type=int, default=0,
                        help="shift every hunk header by N lines (exercise fuzzy matching)")
    parser.add_argument("--verbose", action="store_true",
                        help="log each round's decision to stderr")
    args = parser.parse_args()

    Stub.verbose = args.verbose
    Stub.drift = args.drift
    try:
        server = Server(("127.0.0.1", args.port), Stub)
    except OSError as error:
        print("cannot listen on 127.0.0.1:%d: %s" % (args.port, error), file=sys.stderr)
        print("is another stub already running?  Stop it or set --port.", file=sys.stderr)
        return 2
    print("[stub] listening on http://127.0.0.1:%d/v1 (drift %d)" % (args.port, args.drift),
          file=sys.stderr, flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
