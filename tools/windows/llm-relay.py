"""Local plain-HTTP -> real-HTTPS relay for FixIt's OpenAI-compatible client.

FixIt is built without TLS by default (project decision ADR-004), so it cannot
talk to an https:// endpoint itself.  This relay listens on 127.0.0.1 over plain
HTTP and forwards each request to the endpoint you name, over TLS, using
Python's own trust store.  Point fixit at the relay:

    --base-url http://127.0.0.1:8791/v1

A local model served over plain http (Ollama, llama.cpp, vLLM, LM Studio) needs
no relay at all -- point --base-url straight at it, for example
`http://127.0.0.1:11434/v1` for Ollama.

Start it with the endpoint you actually use:

    set RELAY_TARGET=https://your-endpoint/v1
    python tools/windows/llm-relay.py

The API key is read from $FIXIT_API_KEY or <repository>/.secrets/fixit_key (the
project's own git-ignored location) and is only ever placed in the outgoing
Authorization header: never on a command line, never in the log this writes.

Configuration (environment variables):
    RELAY_TARGET   upstream base URL, required (may include a path prefix)
    RELAY_PORT     listening port                  (default 8791)
    RELAY_KEY_FILE key file, overriding the default above
    FIXIT_API_KEY  the key, if no key file exists
"""

import json
import os
import ssl
import sys
import threading
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlsplit

HERE = os.path.dirname(os.path.abspath(__file__))


def find_repo_root(start):
    """Walk up until the FixIt checkout is found."""
    directory = os.path.abspath(start)
    while True:
        if (os.path.exists(os.path.join(directory, "CMakeLists.txt"))
                and os.path.exists(os.path.join(directory, "src", "compiler.cpp"))):
            return directory
        parent = os.path.dirname(directory)
        if parent == directory:
            raise SystemExit("FixIt repository not found above %s" % start)
        directory = parent


REPO = find_repo_root(HERE)
CACHE = os.path.join(REPO, ".cache")
os.makedirs(CACHE, exist_ok=True)
# Inside .cache/, which .gitignore already covers, so `git status` stays clean.
LOG_FILE = os.path.join(CACHE, "llm-relay.log.jsonl")
KEY_FILE = os.environ.get("RELAY_KEY_FILE") or os.path.join(REPO, ".secrets", "fixit_key")

PORT = int(os.environ.get("RELAY_PORT", "8791"))
TARGET = os.environ.get("RELAY_TARGET", "").rstrip("/")
UPSTREAM_TIMEOUT = 110  # fixit gives up at 120 s

if not TARGET:
    print("RELAY_TARGET is not set.")
    print("Point it at the endpoint you want FixIt to talk to, for example:")
    print('  PowerShell:  $env:RELAY_TARGET = "https://your-endpoint/v1"')
    print('  cmd.exe:     set RELAY_TARGET=https://your-endpoint/v1')
    print("Then run this script again.")
    sys.exit(2)

# The upstream may live under a path prefix (`https://host/litellm/v1`).  fixit
# appends `/chat/completions` to whatever base it is given, so the prefix must
# be added exactly once whether or not the caller repeated it.
_SPLIT = urlsplit(TARGET)
ORIGIN = "%s://%s" % (_SPLIT.scheme, _SPLIT.netloc)
BASE_PATH = _SPLIT.path.rstrip("/")


def upstream_url(path):
    if BASE_PATH:
        if path.startswith(BASE_PATH + "/"):
            pass  # caller already repeated the prefix -- use it as it is
        elif BASE_PATH.endswith("/v1") and path.startswith("/v1/"):
            # caller pointed fixit at a bare /v1; put it back under the prefix
            path = BASE_PATH[: -len("/v1")] + path
        else:
            path = BASE_PATH + path
    return ORIGIN + path


def load_key():
    key = os.environ.get("FIXIT_API_KEY", "").strip()
    if not key and os.path.exists(KEY_FILE):
        with open(KEY_FILE, "r", encoding="utf-8") as handle:
            key = handle.readline().strip()
    return key


KEY = load_key()
SSL_CONTEXT = ssl.create_default_context()


def summarise(request_body, response_body):
    """A log line that describes a round without ever containing the key."""
    try:
        request = json.loads(request_body.decode("utf-8", "replace"))
        response = json.loads(response_body.decode("utf-8", "replace"))
    except ValueError:
        return {"note": "non-JSON body"}
    message = {}
    if isinstance(response, dict) and isinstance(response.get("choices"), list) and response["choices"]:
        choice = response["choices"][0]
        if isinstance(choice, dict) and isinstance(choice.get("message"), dict):
            message = choice["message"]
    calls = []
    for call in message.get("tool_calls") or []:
        if isinstance(call, dict):
            calls.append((call.get("function") or {}).get("name"))
    return {
        "messages": len(request.get("messages", [])),
        "model": request.get("model"),
        "tools_offered": len(request.get("tools", [])),
        "tool_calls": calls,
        "content_head": (message.get("content") or "")[:200],
    }


class Relay(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "fixit-llm-relay"

    def log_message(self, fmt, *args):  # keep the console readable
        pass

    def _send(self, status, body):
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        self._send(
            200,
            json.dumps(
                {
                    "relay": "ok",
                    "target": TARGET,
                    "api_key_present": bool(KEY),
                    "hint": "POST /v1/chat/completions",
                }
            ).encode("utf-8"),
        )

    def do_POST(self):
        # Lets the helper script stop this relay deterministically instead of
        # killing a launcher PID and hoping the child dies with it.
        if self.path == "/shutdown":
            self._send(200, json.dumps({"relay": "stopping"}).encode("utf-8"))
            threading.Thread(target=self.server.shutdown, daemon=True).start()
            return

        try:
            length = int(self.headers.get("Content-Length") or 0)
        except ValueError:
            length = 0
        request_body = self.rfile.read(length) if length else b"{}"

        request = urllib.request.Request(upstream_url(self.path), data=request_body, method="POST")
        request.add_header("Content-Type", "application/json")
        if KEY:
            request.add_header("Authorization", "Bearer " + KEY)

        status = 502
        try:
            with urllib.request.urlopen(request, timeout=UPSTREAM_TIMEOUT, context=SSL_CONTEXT) as response:
                status = response.status
                response_body = response.read()
        except urllib.error.HTTPError as error:
            status = error.code
            response_body = error.read()
        except Exception as error:  # DNS, TLS, timeout -- report it as JSON for fixit
            response_body = json.dumps(
                {"error": {"message": "relay could not reach %s: %s" % (TARGET, error)}}
            ).encode("utf-8")

        try:
            with open(LOG_FILE, "a", encoding="utf-8") as handle:
                handle.write(json.dumps({"status": status, **summarise(request_body, response_body)},
                                        ensure_ascii=False) + "\n")
        except OSError:
            pass

        self._send(status, response_body)


class RelayServer(ThreadingHTTPServer):
    # On Windows SO_REUSEADDR (which Python sets when allow_reuse_address is on)
    # lets a *second* relay bind the same port without any error, and the OS then
    # hands requests to either one -- so a stale relay for a different endpoint
    # can silently receive your traffic, key included.  Refuse to share.
    allow_reuse_address = False
    daemon_threads = True


def main():
    if not KEY:
        print("warning: no API key found; set FIXIT_API_KEY or create %s" % KEY_FILE)
    try:
        server = RelayServer(("127.0.0.1", PORT), Relay)
    except OSError as error:
        print("cannot listen on 127.0.0.1:%d: %s" % (PORT, error))
        print("another relay is already running on that port.  Either keep using it,")
        print("or set RELAY_PORT to a free port before starting this one.")
        return 2
    print("repository : %s" % REPO)
    print("relay      : http://127.0.0.1:%d  ->  %s" % (PORT, TARGET))
    print("key loaded : %s" % ("yes" if KEY else "no"))
    print("log        : %s" % LOG_FILE)
    print("press Ctrl+C to stop (the key is only held in memory)")
    server.serve_forever()
    return 0


if __name__ == "__main__":
    sys.exit(main())
