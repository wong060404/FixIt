#!/usr/bin/env bash
# Regenerates examples/buggy/expected/ for e1..e3 from a real mock run.
#
#   tools/gen_expected.sh [compiler]
#
# For every example it records:
#   <name>.expected.cpp     the final file content the mock reaches
#   <name>.trajectory.json  per-round summary {errors_before, errors_after, ...}
#
# The trajectory's *shape* is asserted by tests/expected_artifacts.test.cpp; the
# exact iteration count follows the compiler's error recovery, so it is recorded
# but not hard-asserted.
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
mkdir -p "${ROOT}/examples/buggy/expected"

for name in e1_missing_include e2_drift e3_type_error; do
  cp "${ROOT}/examples/buggy/${name}.cpp" "${WORK}/${name}.cpp"
  trace="${WORK}/${name}.trace.json"

  set +e
  ( cd "${WORK}" && "${BIN}" "${name}.cpp" --agent --llm mock \
      --compiler "${COMPILER}" --trace "${trace}" >/dev/null 2>&1 )
  exit_code=$?
  set -e

  cp "${WORK}/${name}.cpp" "${ROOT}/examples/buggy/expected/${name}.expected.cpp"

  MOCK_TRACE="${trace}" \
  EXIT_CODE="${exit_code}" \
  COMPILER="${COMPILER}" \
  EXPECTED="${ROOT}/examples/buggy/expected/${name}.trajectory.json" \
    python3 "${ROOT}/tools/summarise_trace.py"
done

echo "wrote ${ROOT}/examples/buggy/expected/"
