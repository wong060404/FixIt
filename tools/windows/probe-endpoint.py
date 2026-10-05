"""One cheap request to check an endpoint before running the whole loop.

    python tools/windows/probe-endpoint.py <base-url> <model>

Answers four questions without revealing the key:
  * is the endpoint reachable?
  * does it accept the key?
  * does the model name exist there?
  * does the endpoint accept OpenAI-style tool calling (FixIt's agent needs it)?

The key is read from $FIXIT_API_KEY, $RELAY_KEY_FILE, or
<repository>/.secrets/fixit_key -- the same places the relay looks.  Any key
material an upstream error echoes back is redacted before printing.
"""

import json
import os
import re
import ssl
import sys
import urllib.error
import urllib.request

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
KEY_FILE = os.environ.get("RELAY_KEY_FILE") or os.path.join(REPO, ".secrets", "fixit_key")
KEY = os.environ.get("FIXIT_API_KEY", "").strip()
if not KEY and os.path.exists(KEY_FILE):
    with open(KEY_FILE, "r", encoding="utf-8") as handle:
        KEY = handle.readline().strip()

if len(sys.argv) < 3:
    print(__doc__)
    print("key file   : %s (%s)" % (KEY_FILE, "found" if KEY else "missing"))
    sys.exit(2)

base = sys.argv[1].rstrip("/")
model = sys.argv[2]


def redact(text):
    """An upstream error may echo the credential back; never print one."""
    if KEY and KEY in text:
        text = text.replace(KEY, "<redacted>")
    # Providers mask the credential themselves (`sk-...abcd`); catch that too.
    return re.sub(r"sk-[^\s\"']{2,}", "sk-<redacted>", text)


body = {
    "model": model,
    "messages": [{"role": "user", "content": "Reply with the single word OK."}],
    "stream": False,
    "tools": [
        {
            "type": "function",
            "function": {
                "name": "noop",
                "description": "Does nothing; used only to test tool support.",
                "parameters": {"type": "object", "properties": {}},
            },
        }
    ],
    "tool_choice": "auto",
}

request = urllib.request.Request(base + "/chat/completions",
                                 data=json.dumps(body).encode("utf-8"), method="POST")
request.add_header("Content-Type", "application/json")
if KEY:
    request.add_header("Authorization", "Bearer " + KEY)

try:
    with urllib.request.urlopen(request, timeout=60, context=ssl.create_default_context()) as response:
        status, text = response.status, response.read().decode("utf-8", "replace")
except urllib.error.HTTPError as error:
    status, text = error.code, error.read().decode("utf-8", "replace")
except Exception as error:
    print("transport error: %s" % error)
    print("(wrong URL, no network, or a proxy in the way)")
    sys.exit(2)

print("endpoint : %s" % base)
print("model    : %s" % model)
print("key      : %s" % (("%d chars, %s..." % (len(KEY), KEY[:3])) if KEY else "none"))
print("HTTP     : %s" % status)

try:
    data = json.loads(text)
except ValueError:
    print("body     : non-JSON: %s" % redact(text[:300]))
    sys.exit(1)

if isinstance(data, dict) and data.get("error"):
    print("error    : %s" % redact(json.dumps(data["error"])[:400]))
    sys.exit(1)

choices = data.get("choices") if isinstance(data, dict) else None
if not isinstance(choices, list) or not choices:
    print("body     : %s" % redact(json.dumps(data)[:400]))
    sys.exit(1)

choice = choices[0] if isinstance(choices[0], dict) else {}
message = choice.get("message") if isinstance(choice.get("message"), dict) else {}
print("served   : %s" % data.get("model"))
print("content  : %s" % (message.get("content") or "")[:120].replace("\n", " "))
print("tool_call: %s" % [((call.get("function") or {}).get("name")) for call in (message.get("tool_calls") or [])])
print("finish   : %s" % choice.get("finish_reason"))
print("VERDICT  : endpoint reachable, key accepted, model served, tools field accepted")
