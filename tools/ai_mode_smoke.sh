#!/usr/bin/env bash
# Offline acceptance test for FixIt's AI mode (--agent --llm openai).
#
# The Catch2 suite never opens a socket: MockLlm answers in-process.  This
# script drives the real binary over the real HTTP path instead, against the
# scripted OpenAI-compatible server in tools/stub_llm_server.py, and asserts the
# verdict the compiler reaches:
#
#   e1  missing <vector> + missing ';'   repaired          exit 0
#   e2  the same faults, 50 lines down   repaired          exit 0
#   e3  type errors the stub cannot fix  refused           exit 1
#
# The stub's hunks are deliberately drifted by 6 lines, so e2 passing also
# proves the sliding-window search found the position rather than trusting the
# header -- and e1/e2 must print a "fuzzy" hunk for the same reason.
#
# No network, no API key, no model.  --no-write keeps every fixture byte-identical.
#
#   tools/ai_mode_smoke.sh
#   FIXIT_BIN=build-check/bin/fixit tools/ai_mode_smoke.sh
#   FIXIT_STUB_PORT=18090 tools/ai_mode_smoke.sh
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="${FIXIT_BIN:-${ROOT}/build/bin/fixit}"
PYTHON="${PYTHON:-python3}"
PORT="${FIXIT_STUB_PORT:-}"
DRIFT=6
ITERATIONS=6
LOG="$(mktemp "${TMPDIR:-/tmp}/fixit-stub.XXXXXX")"

hash_of() {
  if command -v shasum >/dev/null 2>&1; then
    shasum "$1" | cut -d' ' -f1
  else
    sha256sum "$1" | cut -d' ' -f1
  fi
}

if [[ ! -x "${BIN}" ]]; then
  echo "no executable at ${BIN} -- build first (README section 3), or set FIXIT_BIN" >&2
  exit 2
fi

# Ask the OS for a free port unless one was pinned: a fixed default collides
# with a stub left over from an earlier run, and the failure then looks like a
# FixIt problem rather than a port problem.
if [[ -z "${PORT}" ]]; then
  PORT="$("${PYTHON}" - <<'PY'
import socket
sock = socket.socket()
sock.bind(("127.0.0.1", 0))
print(sock.getsockname()[1])
sock.close()
PY
)"
fi

"${PYTHON}" "${ROOT}/tools/stub_llm_server.py" --port "${PORT}" --drift "${DRIFT}" \
  >"${LOG}" 2>&1 &
STUB_PID=$!

cleanup() {
  kill "${STUB_PID}" 2>/dev/null
  wait "${STUB_PID}" 2>/dev/null
  rm -f "${LOG}"
}
trap cleanup EXIT INT TERM

ready=0
for ((attempt = 0; attempt < 60; attempt++)); do
  if "${PYTHON}" - "${PORT}" <<'PY' >/dev/null 2>&1
import socket, sys
sock = socket.socket()
sock.settimeout(0.2)
try:
    sock.connect(("127.0.0.1", int(sys.argv[1])))
except OSError:
    sys.exit(1)
finally:
    sock.close()
PY
  then
    ready=1
    break
  fi
  sleep 0.1
done
if [[ "${ready}" -ne 1 ]]; then
  echo "the stub server did not come up on port ${PORT}" >&2
  sed 's/^/  /' "${LOG}" >&2
  exit 2
fi

passed=0
failed=0

check() {
  local name="$1" expected="$2" want_text="$3"
  local file="${ROOT}/examples/buggy/${name}"
  local before after output status verdict

  before="$(hash_of "${file}")"
  output="$("${BIN}" "${file}" --agent --llm openai \
    --base-url "http://127.0.0.1:${PORT}/v1" --api-key stub --model stub \
    --no-write --iterations "${ITERATIONS}" --no-timing --verbose 2>&1)"
  status=$?
  after="$(hash_of "${file}")"

  verdict="ok"
  if [[ "${status}" -ne "${expected}" ]]; then
    verdict="exit ${status}, expected ${expected}"
  elif [[ "${before}" != "${after}" ]]; then
    verdict="the fixture was modified despite --no-write"
  elif [[ -n "${want_text}" && "${output}" != *"${want_text}"* ]]; then
    verdict="output does not mention '${want_text}'"
  fi

  if [[ "${verdict}" == "ok" ]]; then
    printf '  PASS  %-26s exit %d\n' "${name}" "${status}"
    passed=$((passed + 1))
  else
    printf '  FAIL  %-26s %s\n' "${name}" "${verdict}"
    printf '%s\n' "${output}" | sed 's/^/          /'
    failed=$((failed + 1))
  fi
}

echo "FixIt AI mode -- offline acceptance (stub on 127.0.0.1:${PORT}, drift ${DRIFT})"
echo
check e1_missing_include.cpp 0 "Fixed 5 errors"
check e2_drift.cpp 0 "fuzzy"
check e3_type_error.cpp 1 "Not fixed"
echo
if [[ "${failed}" -eq 0 ]]; then
  echo "${passed} passed, 0 failed"
  exit 0
fi
echo "${passed} passed, ${failed} failed"
exit 1
