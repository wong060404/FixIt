#!/usr/bin/env bash
# Runs FixIt against a real OpenAI-compatible endpoint (default: the CityU
# LiteLLM proxy with the Socrates model), using a key that never touches the
# command line.
#
#   tools/fixit_with_my_key.sh <file.cpp> [extra fixit options...]
#
# The key is read from, in order:
#   1. $FIXIT_API_KEY
#   2. .secrets/fixit_key   (git-ignored; create it with:
#                            printf '%s\n' 'sk-...' > .secrets/fixit_key
#                            chmod 600 .secrets/fixit_key )
#
# Requires a TLS-enabled build: see README "Using a real model over https".
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="${FIXIT_BIN:-${ROOT}/build-tls/bin/fixit}"
BASE_URL="${FIXIT_BASE_URL:-https://socratic.cs.cityu.edu.hk/litellm/v1}"
MODEL="${FIXIT_MODEL:-Socrates}"
CA_BUNDLE="${FIXIT_CA_BUNDLE:-/etc/ssl/cert.pem}"

if [[ $# -lt 1 ]]; then
  echo "usage: $(basename "$0") <file.cpp> [fixit options...]" >&2
  exit 2
fi
if [[ ! -x "${BIN}" ]]; then
  echo "no TLS-enabled build at ${BIN}" >&2
  echo "build one first -- see the README section 'Using a real model over https'" >&2
  exit 2
fi

KEY="${FIXIT_API_KEY:-}"
if [[ -z "${KEY}" && -f "${ROOT}/.secrets/fixit_key" ]]; then
  KEY="$(head -n1 "${ROOT}/.secrets/fixit_key")"
fi
if [[ -z "${KEY}" ]]; then
  echo "no API key: set FIXIT_API_KEY or create .secrets/fixit_key" >&2
  exit 2
fi

# The key travels in the environment only.  It is never placed in argv (where
# `ps` would show it and where the kernel records it for the process lifetime),
# and FIXIT_API_KEY is what the CLI reads when --api-key is absent.
export FIXIT_API_KEY="${KEY}"
unset KEY
export FIXIT_CA_BUNDLE="${CA_BUNDLE}"

exec "${BIN}" "$1" --agent --llm openai \
  --base-url "${BASE_URL}" --model "${MODEL}" "${@:2}"
