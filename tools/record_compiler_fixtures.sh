#!/usr/bin/env bash
# =============================================================================
# record_compiler_fixtures.sh -- re-record tests/fixtures/compiler/*.txt
# =============================================================================
#
# WHAT THE FIXTURES ARE
#   tests/compiler.test.cpp drives fixit::parse_diagnostics() over every pair
#     tests/fixtures/compiler/<name>.txt    raw bytes a compiler wrote to stderr
#     tests/fixtures/compiler/<name>.json   the expected parse result
#   A `.txt` is golden: the suite is offline and deterministic, so it never
#   invokes a compiler itself.  This script is the only thing that talks to one.
#
# HOW A FIXTURE IS RECORDED
#   clang : clang++ -std=c++20 -fsyntax-only -ferror-limit=5
#                    -fdiagnostics-format=json
#           `json` is what the suite wants, but Apple clang 17 only accepts
#           `clang`/`msvc`/`vi`, so the script probes the flag once and falls
#           back to the clang text format with a loud message.  Both shapes are
#           parsed by fixit::parse_diagnostics(), which falls back to the text
#           parser when the payload is not JSON.
#   gcc   : g++ -std=c++20 -fsyntax-only -ferror-limit=5
#           Recording is SKIPPED, with a loud message, whenever the `g++` on
#           PATH is not real GCC.  On macOS /usr/bin/g++ is a symlink to clang,
#           and clang output must never end up in a `.gcc.txt`.
#           Real GCC spells the error cap `-fmax-errors=`, so the script probes
#           `-ferror-limit=` and falls back to `-fmax-errors=` when rejected.
#
# WHAT IS *NOT* REGENERATED
#   The `.json` metadata is hand-maintained; this script only refreshes `.txt`.
#   Two families of `.txt` are hand-written and listed in SYNTHETIC below:
#     * every `.gcc.txt`               (no real GCC on this machine)
#     * n06..n11 `.clang.txt`          (no JSON-emitting clang on this machine)
#   They use recorded `"recorded": "synthetic"` in their `.json`.  On a machine
#   with real GCC or a JSON-capable clang those files are rewritten here, and
#   the matching `expect` blocks must then be reviewed and re-baselined.
#
# SAFETY / REPEATABILITY
#   * Idempotent: two consecutive runs write byte-identical files, and the three
#     checked-in example captures (e1/e2/e3 `.clang.txt`) reproduce exactly.
#   * Never deletes anything, never writes a fixture it does not own, and never
#     writes `.gcc.txt` unless real GCC was positively identified.
#
# USAGE
#   tools/record_compiler_fixtures.sh          # record everything available
#   CLANGXX=clang++-20 GXX=g++-14 tools/record_compiler_fixtures.sh
# =============================================================================
set -euo pipefail

REPO_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
FIXTURE_DIR="$REPO_ROOT/tests/fixtures/compiler"
SOURCE_DIR="$FIXTURE_DIR/sources"

CLANGXX="${CLANGXX:-clang++}"
GXX="${GXX:-g++}"

STD_FLAG="-std=c++20"
ERROR_LIMIT=5

# --------------------------------------------------------------------------
# id | dialect | directory the compiler runs in | source path (relative to it)
# --------------------------------------------------------------------------
RECORDINGS=(
  "e1_missing_include|clang|$REPO_ROOT|examples/buggy/e1_missing_include.cpp"
  "e1_missing_include|gcc|$REPO_ROOT|examples/buggy/e1_missing_include.cpp"
  "e2_drift|clang|$REPO_ROOT|examples/buggy/e2_drift.cpp"
  "e2_drift|gcc|$REPO_ROOT|examples/buggy/e2_drift.cpp"
  "e3_type_error|clang|$REPO_ROOT|examples/buggy/e3_type_error.cpp"
  "e3_type_error|gcc|$REPO_ROOT|examples/buggy/e3_type_error.cpp"
  "n01_multi_error|clang|$SOURCE_DIR|multi_error.cpp"
  "n02_warning_only|clang|$SOURCE_DIR|warning_only.cpp"
  "n03_fatal_error|clang|$SOURCE_DIR|missing_header.cpp"
  "n04_clean|clang|$SOURCE_DIR|clean.cpp"
  "n05_dir_prefix|clang|$SOURCE_DIR|src/foo.cpp"
  "n12_warning_only|gcc|$SOURCE_DIR|warning_only.cpp"
  "n13_caret_context|gcc|$SOURCE_DIR|caret_context.cpp"
  "n14_col1|gcc|$SOURCE_DIR|col1_error.cpp"
  "n15_long_message|gcc|$SOURCE_DIR|long_message.cpp"
  "n16_dir_prefix|gcc|$SOURCE_DIR|src/foo.cpp"
  "n17_fatal_error|gcc|$SOURCE_DIR|missing_header.cpp"
)

# Hand-written fixtures: intentionally never written by this script.
SYNTHETIC=(
  n06_note_attached
  n07_context_line
  n08_json_multi_error
  n09_json_clean
  n10_json_warning
  n11_json_fatal_error
)

info() { printf '  %s\n' "$*"; }
warn() { printf 'warning: %s\n' "$*" >&2; }
die() {
  printf 'error: %s\n' "$*" >&2
  exit 1
}

# True only when `$1` identifies itself as GCC and not as clang.
is_real_gcc() {
  local cc="$1"
  command -v "$cc" >/dev/null 2>&1 || return 1
  local banner
  banner="$("$cc" --version 2>&1 || true) $("$cc" -dumpversion 2>&1 || true)"
  case "$banner" in
    *clang* | *Clang* | *LLVM*) return 1 ;;
  esac
  return 0
}

# Echo a compiler flag the given compiler accepts, or nothing.
first_accepted_flag() {
  local cc="$1"
  shift
  local tmp
  tmp="$(mktemp "${TMPDIR:-/tmp}/fixit-probe-XXXXXX.cpp")"
  printf 'int main() { return 0; }\n' >"$tmp"
  local flag
  for flag in "$@"; do
    if "$cc" $STD_FLAG -fsyntax-only "$flag" "$tmp" >/dev/null 2>&1; then
      rm -f -- "$tmp"
      printf '%s' "$flag"
      return 0
    fi
  done
  rm -f -- "$tmp"
  return 1
}

# True when the clang on PATH can emit the JSON diagnostic format.
clang_supports_json() {
  local tmp
  tmp="$(mktemp "${TMPDIR:-/tmp}/fixit-probe-XXXXXX.cpp")"
  printf 'int main() { return 0; }\n' >"$tmp"
  local ok=1
  if "$CLANGXX" $STD_FLAG -fsyntax-only -fdiagnostics-format=json "$tmp" >/dev/null 2>&1; then
    ok=0
  fi
  rm -f -- "$tmp"
  return $ok
}

# record <dialect> <fixture-id> <workdir> <source>
# Writes <fixture-id>.<dialect>.txt atomically; the compiler's exit status is
# expected to be non-zero for most fixtures and is deliberately ignored.
record() {
  local dialect="$1" id="$2" workdir="$3" source="$4"
  local out="$FIXTURE_DIR/$id.$dialect.txt"
  local tmp="$FIXTURE_DIR/.record-$id.$dialect.$$"
  local -a cmd
  local flags="$5"

  if [[ "$dialect" == "clang" ]]; then
    cmd=("$CLANGXX" $STD_FLAG -fsyntax-only "-ferror-limit=$ERROR_LIMIT")
    # shellcheck disable=SC2206  # deliberate word split of the chosen format flag
    [[ -n "$flags" ]] && cmd+=($flags)
  else
    cmd=("$GXX" $STD_FLAG -fsyntax-only "$flags")
  fi

  (cd "$workdir" && "${cmd[@]}" "$source") >"$tmp" 2>&1 || true

  if [[ "$dialect" == "gcc" ]] && grep -qE '[0-9]+ (error|warning)s? generated\.' "$tmp"; then
    rm -f -- "$tmp"
    die "refusing to write clang output to $id.gcc.txt (looks like clang, not GCC)"
  fi

  mv -- "$tmp" "$out"
  info "$id.$dialect.txt  <- ${cmd[*]} $source"
}

main() {
  [[ -d "$FIXTURE_DIR" ]] || die "missing fixture directory: $FIXTURE_DIR"

  printf 'recording compiler fixtures into %s\n' "$FIXTURE_DIR"

  # --- clang ---------------------------------------------------------------
  if ! command -v "$CLANGXX" >/dev/null 2>&1; then
    die "clang compiler not found on PATH: $CLANGXX"
  fi
  local clang_format="-fdiagnostics-format=clang"
  if clang_supports_json; then
    clang_format="-fdiagnostics-format=json"
    info "$CLANGXX supports -fdiagnostics-format=json"
  else
    warn "$CLANGXX has no JSON diagnostic format; recording clang text output."
    warn "the .clang.txt captures stay text and the parser falls back accordingly."
  fi

  # --- gcc -----------------------------------------------------------------
  local record_gcc=0
  local gcc_limit_flag=""
  if is_real_gcc "$GXX"; then
    record_gcc=1
    gcc_limit_flag="$(first_accepted_flag "$GXX" "-ferror-limit=$ERROR_LIMIT" \
                                       "-fmax-errors=$ERROR_LIMIT" || true)"
    [[ -n "$gcc_limit_flag" ]] || die "$GXX accepted neither -ferror-limit nor -fmax-errors"
    info "real GCC detected: $("$GXX" --version 2>&1 | head -n 1)"
  else
    warn "g++ on PATH is not real GCC ($("$GXX" --version 2>&1 | head -n 1 || echo 'not found'))"
    warn "skipping every .gcc.txt; clang output is never written to a .gcc.txt."
    warn "the checked-in GCC fixtures are hand-written and stay as they are."
  fi

  local entry id dialect workdir source
  for entry in "${RECORDINGS[@]}"; do
    IFS='|' read -r id dialect workdir source <<<"$entry"
    if [[ "$dialect" == "gcc" ]]; then
      [[ "$record_gcc" -eq 1 ]] || continue
      record gcc "$id" "$workdir" "$source" "$gcc_limit_flag"
    else
      record clang "$id" "$workdir" "$source" "$clang_format"
    fi
  done

  printf 'left untouched (hand-written, "recorded": "synthetic"):\n'
  for id in "${SYNTHETIC[@]}"; do
    info "$id.clang.txt"
  done
  printf 'done\n'
}

main "$@"
