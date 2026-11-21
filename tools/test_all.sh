#!/usr/bin/env bash
# One command that runs every test layer FixIt has on macOS/Linux:
#
#   L1  build (must be warning-free) + the six Catch2 suites
#   L2  the CLI on the file error cases: repairable, unrepairable, clean,
#       and the usage/IO errors that must exit 2 without writing anything
#   L3  AI mode over the real HTTP path, against the scripted model
#
#   tools/test_all.sh
#   CMAKE=/path/to/cmake tools/test_all.sh        # non-default toolchain
#   FIXIT_BIN=build-check/bin/fixit tools/test_all.sh
#
# Exit code: 0 only when every check passed.  Nothing touches the network and no
# API key is needed; the fixtures stay byte-identical because every run uses
# --no-write.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${FIXIT_BUILD_DIR:-${ROOT}/build}"
BIN="${FIXIT_BIN:-${BUILD}/bin/fixit}"
CMAKE="${CMAKE:-${ROOT}/../.buildtools/cmake/data/bin/cmake}"
CTEST="${CTEST:-}"
if [[ -z "${CTEST}" ]]; then
  if [[ -x "$(dirname "${CMAKE}")/ctest" ]]; then
    CTEST="$(dirname "${CMAKE}")/ctest"
  else
    CTEST=ctest
  fi
fi
[[ "${BIN}" != /* ]] && BIN="${ROOT}/${BIN}"

failures=0
passed=0

section() { printf '\n== %s ==\n' "$1"; }

check() { # label expected-exit command...
  local label="$1" expected="$2"
  shift 2
  local output status
  output="$("$@" 2>&1)"
  status=$?
  if [[ "${status}" -eq "${expected}" ]]; then
    printf '  PASS  %-34s exit %d\n' "${label}" "${status}"
    passed=$((passed + 1))
  else
    printf '  FAIL  %-34s exit %d, expected %d\n' "${label}" "${status}" "${expected}"
    printf '%s\n' "${output}" | sed 's/^/          /'
    failures=$((failures + 1))
  fi
}

# ---------------------------------------------------------------- L1: build --
section "L1  build + unit tests"
if "${CMAKE}" --build "${BUILD}" --parallel; then
  printf '  PASS  %-34s\n' "cmake --build ${BUILD##*/}"
  passed=$((passed + 1))
else
  printf '  FAIL  build failed\n'
  failures=$((failures + 1))
fi
if "${CTEST}" --test-dir "${BUILD}" --output-on-failure; then
  printf '  PASS  %-34s\n' "ctest (6 suites)"
  passed=$((passed + 1))
else
  printf '  FAIL  ctest failed\n'
  failures=$((failures + 1))
fi

[[ -x "${BIN}" ]] || { echo "no executable at ${BIN}" >&2; exit 2; }

# ------------------------------------------------------- L2: file contracts --
section "L2  file error cases (CLI contract)"

TMP="$(mktemp -d "${TMPDIR:-/tmp}/fixit-cases.XXXXXX")"
trap 'rm -rf "${TMP}"' EXIT

shasum_of() { if command -v shasum >/dev/null 2>&1; then shasum "$1" | cut -d' ' -f1; else sha256sum "$1" | cut -d' ' -f1; fi; }
before="$(shasum_of "${ROOT}/examples/buggy/e1_missing_include.cpp")"

# Repairable, unrepairable, already clean.
check "e1 missing ';' + <vector>"   0 "${BIN}" "${ROOT}/examples/buggy/e1_missing_include.cpp" --agent --llm mock --no-write --no-timing
check "e2 same faults, 50 lines down" 0 "${BIN}" "${ROOT}/examples/buggy/e2_drift.cpp" --agent --llm mock --no-write --no-timing
check "e3 type error, mock gives up"  1 "${BIN}" "${ROOT}/examples/buggy/e3_type_error.cpp" --agent --llm mock --no-write --no-timing
check "e4 already clean"              0 "${BIN}" "${ROOT}/examples/buggy/e4_testing.cpp" --agent --llm mock --no-write --no-timing
check "outline on a broken file"      0 "${BIN}" "${ROOT}/examples/buggy/e1_missing_include.cpp" --outline

# Usage / IO errors: every one must be exit 2, never a crash.
check "missing file"                  2 "${BIN}" "${ROOT}/examples/buggy/nope.cpp" --agent --llm mock --no-write
check "directory instead of file"     2 "${BIN}" "${ROOT}/examples/buggy" --agent --llm mock --no-write
check "no arguments"                  2 "${BIN}"
check "two source files"              2 "${BIN}" "${ROOT}/examples/buggy/e1_missing_include.cpp" "${ROOT}/examples/buggy/e2_drift.cpp"
check "unknown option"                2 "${BIN}" "${ROOT}/examples/buggy/e1_missing_include.cpp" --bogus
check "openai without a key"          2 env -u FIXIT_API_KEY "${BIN}" "${ROOT}/examples/buggy/e1_missing_include.cpp" --agent --llm openai --no-write

# A file that includes a project header: without -I the compiler stops before any
# real diagnostic; with -I it is clean.
printf '#include "fixit/compiler.h"\nint main() { return 0; }\n' > "${TMP}/uses_project_header.cpp"
check "project header, no -I"         1 "${BIN}" "${TMP}/uses_project_header.cpp" --agent --llm mock --no-write --no-timing
check "project header, with -I"       0 "${BIN}" "${TMP}/uses_project_header.cpp" -Iinclude -Ithird_party/json/include --agent --llm mock --no-write --no-timing

if [[ "$(shasum_of "${ROOT}/examples/buggy/e1_missing_include.cpp")" == "${before}" ]]; then
  printf '  PASS  %-34s\n' "fixtures unchanged (--no-write)"
  passed=$((passed + 1))
else
  printf '  FAIL  a fixture was modified despite --no-write\n'
  failures=$((failures + 1))
fi

# ------------------------------------------------------------- L3: AI mode ---
section "L3  AI mode (scripted model, real HTTP path)"
if FIXIT_BIN="${BIN}" "${ROOT}/tools/ai_mode_smoke.sh"; then
  passed=$((passed + 1))
else
  failures=$((failures + 1))
fi

# ---------------------------------------------------------------- summary ---
printf '\n== summary ==\n'
printf '  %d passed, %d failed\n' "${passed}" "${failures}"
[[ "${failures}" -eq 0 ]] || exit 1
