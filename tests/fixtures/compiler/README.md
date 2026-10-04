# Compiler diagnostic fixtures

Golden fixtures for `fixit::parse_diagnostics()` (see `tests/compiler.test.cpp`).
The suite is **offline and deterministic**: it never runs a compiler, it only
replays the `.txt` captures below. `tools/record_compiler_fixtures.sh` is the
only thing that talks to a real compiler.

## Layout

```
<name>.json   metadata: dialect, compiler, source, provenance and the exact
              diagnostics parse_diagnostics() must produce ("expect")
<name>.txt    the raw bytes the compiler wrote to stderr (or stdout)
sources/      the small purpose-built translation units behind the new fixtures
```

The test discovers every `*.json` in this directory, loads `<name>.txt`, and
parses it with `dialect == "clang-json"` selecting the clang/JSON entry point
and `"gcc"` selecting the text entry point.

Extra metadata fields beyond the test schema:

* `"recorded"` — `"machine"` (captured from a real compiler invocation) or
  `"synthetic"` (written by hand; see *Provenance* below).
* `"output_format"` — `"text"` or `"json"`, the serialisation actually on disk.

## The 20 fixtures

| Fixture | dialect | compiler | source | output | recorded | covers |
| --- | --- | --- | --- | --- | --- | --- |
| `e1_missing_include.gcc` | gcc | gcc | `examples/buggy/e1_missing_include.cpp` | text | synthetic | parity pair, missing `;`, multi-error cascade |
| `e2_drift.gcc` | gcc | gcc | `examples/buggy/e2_drift.cpp` | text | synthetic | parity pair, errors far from the top |
| `e3_type_error.gcc` | gcc | gcc | `examples/buggy/e3_type_error.cpp` | text | synthetic | parity pair, `note:` line is dropped |
| `n01_multi_error.clang` | clang-json | clang | `multi_error.cpp` | text | machine | two errors in one file |
| `n02_warning_only.clang` | clang-json | clang | `warning_only.cpp` | text | machine | warning-only output |
| `n03_fatal_error.clang` | clang-json | clang | `missing_header.cpp` | text | machine | `fatal error:` |
| `n04_clean.clang` | clang-json | clang | `clean.cpp` | text | machine | no diagnostics at all (0-byte capture) |
| `n05_dir_prefix.clang` | clang-json | clang | `src/foo.cpp` | text | machine | file path with a directory prefix |
| `n06_note_attached.clang` | clang-json | clang | `note_attached.cpp` | json | synthetic | child `note` folded into the error message |
| `n07_context_line.clang` | clang-json | clang | `context_line.cpp` | json | synthetic | `source` array rebuilt as `context_line` |
| `n08_json_multi_error.clang` | clang-json | clang | `multi_error.cpp` | json | synthetic | JSON array walk; top-level `note` dropped |
| `n09_json_clean.clang` | clang-json | clang | `clean.cpp` | json | synthetic | empty JSON array |
| `n10_json_warning.clang` | clang-json | clang | `warning_only.cpp` | json | synthetic | `warning` level + folded note |
| `n11_json_fatal_error.clang` | clang-json | clang | `missing_header.cpp` | json | synthetic | `fatal error` level |
| `n12_warning_only.gcc` | gcc | gcc | `warning_only.cpp` | text | synthetic | warning-only output |
| `n13_caret_context.gcc` | gcc | gcc | `caret_context.cpp` | text | synthetic | caret block → `context_line` |
| `n14_col1.gcc` | gcc | gcc | `col1_error.cpp` | text | synthetic | diagnostic on column 1 |
| `n15_long_message.gcc` | gcc | gcc | `long_message.cpp` | text | synthetic | ~250-character message |
| `n16_dir_prefix.gcc` | gcc | gcc | `src/foo.cpp` | text | synthetic | file path with a directory prefix |
| `n17_fatal_error.gcc` | gcc | gcc | `missing_header.cpp` | text | synthetic | `fatal error:` + trailing `compilation terminated.` |

Nine fixtures are `gcc`, eleven are `clang-json`.

In addition this directory keeps three **reference captures without a `.json`** —
`e1_missing_include.clang.txt`, `e2_drift.clang.txt`, `e3_type_error.clang.txt`.
They are the checked-in, machine-recorded clang half of the three parity pairs;
the test asserts that the `(line, column)` set and the error count of each
`.clang.txt` match its hand-written `.gcc.txt` twin. They are deliberately not
part of the 20 metadata-backed fixtures.

## Provenance — what is machine-recorded and what is hand-written

The machine used to seed these fixtures is macOS with **Apple clang 17** and
**no real GCC**:

* **`recorded: machine`** — recorded by
  `tools/record_compiler_fixtures.sh` from a real `clang++`. Apple clang 17
  rejects `-fdiagnostics-format=json` (only `clang`, `msvc` and `vi` exist), so
  the script falls back to clang's text format and these captures are text.
  `fixit::parse_diagnostics()` handles that exactly as intended for an
  unexpected clang build: it finds no JSON payload and falls back to the text
  parser, so the `clang-json` entry point is still exercised.
* **`recorded: synthetic`, GCC dialect** — real GCC is not installed here, and
  `g++` on this machine is a symlink to clang, so no `.gcc.txt` may be
  machine-recorded. Every GCC fixture is written by hand in GCC's documented
  output shape (`file:line:col: level: message`, a `%5d | src` gutter line and a
  caret line). On a machine with real GCC,
  `tools/record_compiler_fixtures.sh` overwrites them and the `expect` blocks
  must be reviewed against the real output.
* **`recorded: synthetic`, clang JSON dialect** — the six `n06`…`n11` fixtures
  serialise, in clang's documented `-fdiagnostics-format=json` object schema,
  diagnostics that were *actually* produced by the clang text captures of the
  same sources (same message text, same file/line/column, same note). Only the
  serialisation is written by hand, because no clang here can emit it. Their
  `sources/*.cpp` files are the ones the real text captures came from, so the
  two can be compared side by side.
* Everything under `sources/` is purpose-built for this suite.

### The parity pairs are aligned by construction

GCC's error recovery is not clang's: for `e1`/`e2` a real GCC stops cascading
earlier than clang does, and for `e3` GCC anchors the conversion error on the
argument rather than on the call. The hand-written `.gcc.txt` files therefore
carry the same `(line, column)` anchors as the checked-in clang captures, which
is what makes the cross-dialect parity assertion meaningful and stable. The
message *wording* is GCC's own (for example `'vector' in namespace 'std' does
not name a template type`); the cascade positions are shared with clang by
design. Re-recording on a real GCC machine will change them, and the parity test
is what will point that out.

## Regenerating

```sh
tools/record_compiler_fixtures.sh
```

The script is idempotent and never deletes anything. It skips every `.gcc.txt`
unless it can positively identify a real GCC (it greps `g++ --version` /
`-dumpversion` for `clang`), and it writes a `.gcc.txt` only after a second
guard rejects payloads that look like clang's. It records clang with
`-fdiagnostics-format=json` when that is supported and with the clang text
format otherwise, and it does not touch the synthetic clang JSON fixtures.

When a capture changes, review the matching `.json` `expect` block: the script
refreshes the raw output, it does not re-derive the expectations.


## Parity: lines yes, columns no

The three `examples/buggy` sources are compiled by both dialects and compared two
ways:

* `compiler.test` compares the **captures** (`.gcc.txt` vs `.clang.txt`).
* a second case compares **live output** from a real `g++` and a real `clang++`,
  skipping itself when either is absent.

Both compare error **line** sets exactly, and deliberately do not compare columns.
Real compilers disagree about which token a diagnostic points at.  The clearest
case is `e3_type_error.cpp`'s `count_words(value)`:

| compiler | anchor | why |
| --- | --- | --- |
| clang | `13:15` | the identifier being called with a bad argument |
| GCC | `13:25` | the argument expression that cannot be converted |

The hand-written GCC fixtures use the clang column so the synthetic corpus stays
internally consistent; re-recording them on a GCC machine will change those
columns, and both parity cases are written so that this is expected.  What the
repair loop needs is the line -- that is what selects the hunk context -- and the
two compilers agree on lines for all three sources.
