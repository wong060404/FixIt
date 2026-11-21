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
# The binary is chosen from the base URL: an https:// endpoint needs the TLS
# build (README "Using a real model over https"), while a plain-HTTP one -- the
# scripted stub, a local model, or tools/windows/llm-relay.py -- uses build/.
# Override either with $FIXIT_BIN.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BASE_URL="${FIXIT_BASE_URL:-https://socratic.cs.cityu.edu.hk/litellm/v1}"
MODEL="${FIXIT_MODEL:-Socrates}"
CA_BUNDLE="${FIXIT_CA_BUNDLE:-/etc/ssl/cert.pem}"

# An https:// base URL needs the TLS build; a plain-HTTP one needs no TLS, so it
# uses the ordinary build and never a stale one.
if [[ -n "${FIXIT_BIN:-}" ]]; then
  BIN="${FIXIT_BIN}"
elif [[ "${BASE_URL}" == https://* && -x "${ROOT}/build-tls/bin/fixit" ]]; then
  BIN="${ROOT}/build-tls/bin/fixit"
else
  BIN="${ROOT}/build/bin/fixit"
fi

if [[ $# -lt 1 ]]; then
  echo "usage: $(basename "$0") <file.cpp> [fixit options...]" >&2
  exit 2
fi
if [[ ! -x "${BIN}" ]]; then
  echo "no fixit binary at ${BIN}" >&2
  echo "build one first -- see the README section 'Quick Start'" >&2
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
