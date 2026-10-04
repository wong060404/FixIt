#!/usr/bin/env bash
# Records the three acceptance demos into docs/demo_output.txt from a pristine
# copy of examples/buggy, so the transcript in the repo is always reproducible.
#
# Usage: tools/record_demo.sh [compiler]
set -euo pipefail

COMPILER="${1:-${CXX:-clang++}}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="${ROOT}/build/bin/fixit"

if [[ ! -x "${BIN}" ]]; then
  echo "build the CLI first: cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j" >&2
  exit 1
fi

WORK="$(mktemp -d)"
trap 'rm -rf "${WORK}"' EXIT
cp "${ROOT}"/examples/buggy/*.cpp "${WORK}/"

OUT="${ROOT}/docs/demo_output.txt"
mkdir -p "$(dirname "${OUT}")"

{
  echo "FixIt v0.1.0 -- recorded demo transcript"
  echo "compiler: $("${COMPILER}" --version | head -1)"
  echo "platform: $(uname -s) $(uname -m)"
  echo "NO_COLOR=1 (ANSI colour stripped so this file is diffable)"
  echo "--no-timing (wall-clock times omitted so this file is byte-reproducible)"
  echo
  echo "NOTE: e1 and e2 are repaired by the deterministic mock model (exit 0)."
  echo "      e3 is deliberately beyond the mock's rule set and is reported as"
  echo "      unfixed (exit 1) -- it is the fixture for a real LLM."
  echo

  for example in e1_missing_include e2_drift e3_type_error; do
    echo "========================================================================"
    echo "\$ fixit ${example}.cpp --agent --llm mock --verbose --compiler ${COMPILER}"
    echo "========================================================================"
    # --no-timing keeps this transcript byte-identical from run to run, so it can
    # be committed as evidence that never drifts.
    ( cd "${WORK}" && NO_COLOR=1 "${BIN}" "${example}.cpp" --agent --llm mock --verbose \
        --no-timing --compiler "${COMPILER}" --trace "${WORK}/${example}.trace.json" ) || true
    echo
  done

  echo "========================================================================"
  echo "trajectory for e1 (deterministic; timestamps excluded by design)"
  echo "========================================================================"
  python3 -m json.tool "${WORK}/e1_missing_include.trace.json" 2>/dev/null \
    | sed -e 's/[[:space:]]*$//' || cat "${WORK}/e1_missing_include.trace.json"
  echo
  echo "========================================================================"
  echo "final content of the repaired e1"
  echo "========================================================================"
  cat "${WORK}/e1_missing_include.cpp"
} > "${OUT}" 2>&1

echo "wrote ${OUT}"
