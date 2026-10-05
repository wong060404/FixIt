# FixIt

**A C++ library + CLI that closes the loop between compiler output and an LLM's
patches: compile → locate → LLM patch → fuzzy apply → re-verify.**

FixIt is not another wrapper around `patch(1)`. Its patch engine is built for the
patch a model *actually produces* — one with drifted line numbers, missing
context, and whitespace that does not quite match — and when a patch cannot be
applied it returns a structured explanation the model can act on, instead of an
exit code.

---

## 1. What & Why

Agentic C++ repair has one bottleneck that is not the model: **the patch does
not apply.** The model writes a perfectly reasonable edit, `@@ -14,4 +14,5 @@`
is off by two lines because the file changed, and `patch(1)` refuses the whole
diff. The model never learns *why*, so it guesses again and burns another round.

FixIt closes that circle:

| Step | What FixIt does |
|---|---|
| compile | runs the real compiler, parses diagnostics into data (not text) |
| locate | maps a diagnostic to its enclosing function and a numbered snippet |
| patch | scores every plausible position with a sliding window and applies the best one |
| re-verify | recompiles; only the compiler decides whether the repair worked |
| on failure | returns `score`, the closest match, the exact line that differed, and a suggested re-read |

## 2. Innovation

1. **Fuzzy patch engine.** Every candidate position is scored
   (`exact`/`fuzzy`/`mismatch`, normalised so a perfect hunk is `1.00`), searched
   through the window ladder `±0 → ±1 → ±2 → ±5 → ±10 → ±50 → global`, and gated
   at `score ≥ 0.80` with at least one exact line. Line-number drift, missing
   context and trailing-whitespace differences all apply anyway.
2. **Structured failure that feeds the loop.** A refused hunk yields
   `score 0.55 < gate 0.80 … expected line 14 to contain 'return x;' but the
   file has 'return 0;'. Closest match at line 15 … re-read lines 12-18` — text
   designed to be pasted straight back into the next prompt.
3. **The compiler is the only ground truth.** A model that answers `FINAL` still
   triggers a compile; `success` is set only by a clean exit. The mock rules,
   the trace format and the CLI all read from that one source.

## 3. Quick Start

```bash
git clone <your-fork-url> fixit
cd fixit
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j && ctest --test-dir build --output-on-failure
```

Then run the section-0 acceptance command:

```bash
./build/bin/fixit examples/buggy/e1_missing_include.cpp --agent --llm mock --verbose
```

Expected: the transcript in §4, and exit code `0`. Nothing in this path touches
the network.

That command repairs the example in place. Add `--no-write` to repair a scratch
copy instead and leave the working tree byte-identical — which is what the CI
smoke test and the recorded demo use, so both can be re-run at any time.

### Requirements

* **CMake ≥ 3.20, installed and on `PATH`.** The build cannot bootstrap its own
  build system: if `cmake` is missing, install it first (for example
  `brew install cmake`, or `pip install cmake` and put its `bin/` directory on
  `PATH` — a pip-installed CMake works, but only once it is findable as `cmake`).
* A C++20 compiler: gcc 12 / clang 15 or newer. On macOS `clang++` is fine; note
  that an Apple clang build has no JSON diagnostics, which FixIt detects and
  falls back from automatically (ADR-007).
* Python ≥ 3.8 only for the maintenance scripts that regenerate artefacts
  (`tools/gen_golden_cases.py`, `tools/render_heatmap.py`, `tools/gen_expected.sh`).
  The test suite itself never shells out to Python.

Dependencies come from the vendored snapshot in `third_party/`; a checkout that
lacks it (for example a tarball of just the sources) falls back to `FetchContent`
against the pinned upstream tags, which needs network access at configure time.

### Building and debugging in VS Code

The checked-in `.vscode/` folder is set up for this CMake project, so `F5` builds
with CMake first and then debugs `build/bin/*` with lldb:

| Action | How |
| --- | --- |
| Build everything | `Cmd+Shift+B` (task `cmake: build`) |
| Debug the CLI on the file you have open | `F5` → `fixit CLI：對目前檔案跑 --outline（mock LLM）` |
| Debug a test binary | `F5` → `測試：compiler.test` (one entry per test binary) |
| Run the whole suite | task `test: 全部 ctest` |
| Re-generate the build dir | task `cmake: configure` |

`cmake` is not installed system-wide on this machine; the tasks and the CMake
Tools integration point at the bundled copy in
`../.buildtools/cmake/data/bin/cmake`.

**Do not debug with `C/C++: 建置使用中檔案`.** That auto-generated task compiles a
single `.cpp` in isolation, which cannot work here:

* `src/*.cpp` and `tests/*.cpp` need the CMake-provided include paths (Catch2,
  tree-sitter, `httplib`), so a lone `clang++ file.cpp` fails with
  `'catch2/catch_test_macros.hpp' file not found`.
* `examples/buggy/*.cpp` are **intentionally broken** fixtures — a failed compile
  is their designed behaviour, not an environment problem.

If `F5` ever reports *"Errors exist after running preLaunchTask"*, run
`cmake: build` once and read the compile output: it is a real compiler error, or
the wrong kind of file was being compiled in isolation.

## 4. Demo Transcript

Recorded from a real run — the full file is [`docs/demo_output.txt`](docs/demo_output.txt).

```
══ FixIt v0.1.0 · agent mode (mock) ══

── iteration 1 ----------------------------
$ clang++ -fsyntax-only e1_missing_include.cpp
  ✗ 5 errors
    [E1]   L10   expected ';' at end of declaration
    [E2]   L14   no member named 'vector' in namespace 'std'
  → context: fn parse_count() [L9–L12]
  → patch…
    ✓ hunk 1 @ L9    (exact)
── iteration 2 ----------------------------
$ clang++ -fsyntax-only e1_missing_include.cpp
  ✗ 1 error
    [E1]   L11   expected ';' at end of declaration
  → patch…
    ✓ hunk 1 @ L11   (fuzzy, drift-1)
$ clang++ -fsyntax-only e1_missing_include.cpp
  ✓ clean
✔ Fixed 5 errors in 2 iterations (0.9s)
```

`e2_drift.cpp` carries the same two bugs ~50 lines further down, so every
`@@` position the mock emits is wrong — and the window search still repairs it.
`e3_type_error.cpp` is deliberately outside the mock's rule set: the loop reports
it unfixed (exit `1`) rather than pretending, which is the fixture for a real
model.

## 5. Architecture

```
              ┌──────────────────────── fixit-cli ────────────────────────┐
              │  parse args · verbose transcript · --trace · --metrics    │
              └───────────────┬───────────────────────────────────────────┘
                              │
                      ┌───────▼────────┐        structured events
                      │  fixit::Agent  │◄────── Compile / Patch observations
                      └───┬────────┬───┘
        tool calls        │        │        messages + tool schemas
      ┌───────────────────┘        └────────────────────┐
      │                                                 │
┌─────▼──────────┐  ┌──────────────┐  ┌──────────┐  ┌───▼─────────┐
│ ToolRegistry   │  │ CodeMap      │  │ Compiler │  │ Llm         │
│ compile        │  │ tree-sitter  │  │ g++ /    │  │ ├ MockLlm   │
│ read           │  │ · functions  │  │ clang++  │  │ └ OpenAiLlm │
│ patch ─────────┼─►│ · includes   │  │ JSON or  │  │  (httplib)  │
└────────────────┘  │ · snippets   │  │ text     │  └─────────────┘
                    └──────────────┘  └──────────┘
                            │
                    ┌───────▼────────┐
                    │  PatchEngine   │  scoring · sliding window · gate
                    │  (the core)    │  structured failure reports
                    └────────────────┘
```

Everything below `Agent` is a plain library: link `libfixit` and drive the loop
yourself, or reuse `PatchEngine` on its own to make *someone else's* diffs apply.

## 6. Modules

### 6.1 `fixit::Compiler` — [`include/fixit/compiler.h`](include/fixit/compiler.h)

```cpp
struct CompilerConfig {
  std::string compiler = "g++";
  int error_limit = 5;
  int timeout_seconds = 30;
  std::vector<std::string> extra_flags;
  DiagnosticFormat format = DiagnosticFormat::Auto;  // Auto | Text | Json
};

Compiler compiler(CompilerConfig{.compiler = "clang++"});
CompileResult result = compiler.compile("src/parse_config.cpp");
```

* Runs `<compiler> -std=c++20 -fsyntax-only -ferror-limit=N [flags] <file>` through
  `fork`/`exec` with stdout and stderr merged into one pipe.
* **Dialect selection is probed, not assumed.** A clang that rejects
  `-fjson-diagnostics` is probed for `-fdiagnostics-format=json`; if neither
  works the text parser is used, so an unusual clang degrades instead of losing
  every diagnostic. `CompilerConfig::format` can force either parser.
* Diagnostics are sorted by `(file, line, col, message)`, de-duplicated and
  capped at 50. `CompileResult::clean()` is the loop's ground truth.
* Timeout kills the child and keeps the output captured so far
  (`timed_out == true`).

### 6.2 `fixit::CodeMap` — [`include/fixit/codemap.h`](include/fixit/codemap.h)

```cpp
CodeMap map("src/parse_config.cpp");
map.functions();                       // {name, signature, start_line, end_line}
map.enclosing_function(14, 5);         // innermost function containing the caret
map.includes();                        // {"<cstdio>", "\"local.h\""}
map.context_snippet(14, 12);           //   13 |   ...
map.has_syntax_errors();
```

`signature` is the declaration text from the first line of the definition up to
(but excluding) the body's `{`. `context_snippet` is `%4d | text`, clamped to the
file, which is exactly the shape a model can quote back. A missing or unparsable
file never throws: the map degrades and `has_syntax_errors()` returns `true`.

### 6.3 `fixit::PatchEngine` — [`include/fixit/patch.h`](include/fixit/patch.h) ★

```cpp
PatchEngine engine;  // PatchConfig{max_drift=200, fuzzy_line_threshold=0.8, gate=0.8}
PatchResult result = engine.apply(file_content, unified_diff, "parse_config.cpp");
result.all_applied;        // every hunk passed the gate
result.new_content;        // applied hunks written, refused hunks untouched
result.reports;            // per-hunk status, score, positions, top candidates
result.failure_summary();  // ready to feed back to the model
```

Scoring for candidate position `p`:

```
exact(p)    = signature lines equal after trailing-whitespace normalisation
fuzzy(p)    = lines not exactly equal but Levenshtein ratio >= 0.8
mismatch(p) = everything else
score(p)    = (0.6*exact + 0.3*fuzzy - 0.2*mismatch) / (0.6 * signature.size())
```

The denominator normalises by the best attainable raw score so a perfect hunk is
`1.00` and the documented `gate = 0.8` is meaningful (see
[`docs/decisions.md`](docs/decisions.md) ADR-002 — the literal formula in the
original brief tops out at `0.60` and would fail its own gate). A hunk applies
only when `score >= gate` **and** `exact >= 1`; hitting inside `±0` is `Applied`,
anything else is `FuzzyApplied`.

Failure output looks like this and is the whole point:

```
Patch failed for parse_config.cpp:
- Hunk 1: APPLIED @ line 8 (exact, score 1.00)
- Hunk 2: FAILED (score 0.55 < gate 0.80. Declared position line 14 but the
  closest match is line 15 (score 0.70, exact 1/3). Window tried: +/-0..+/-50
  plus global scan. Context mismatch: expected line 15 to contain 'return x;'
  but the file has 'return 0;'.). Closest match at line 15
  Suggested fix: re-read lines 12-18 and retry with corrected context.
```

### 6.4 `fixit::Agent` — [`include/fixit/agent.h`](include/fixit/agent.h)

```cpp
Agent agent(make_standard_tools(workdir), std::make_unique<MockLlm>(),
            Compiler(CompilerConfig{.compiler = "g++"}), workdir);
agent.set_trace_path("trace.json");
agent.set_observer([](const Agent::Event& event) { /* live progress */ });
AgentResult result = agent.run("parse_config.cpp", /*max_iterations=*/4);
```

The three standard tools — `compile {file}`, `read {file,start,end}`,
`patch {file,diff}` — are in `make_standard_tools`, and any additional tool is
just another `ToolRegistry::add`. `MockLlm` is rule-based and offline; when it
emits a patch it deliberately declares the wrong line (`+1`), so every demo also
exercises the fuzzy path. `OpenAiLlm` speaks `/v1/chat/completions` over
`cpp-httplib`.

### 6.5 Using a real model over https

The default build has no TLS, because OpenSSL is not one of the four permitted
dependencies.  `https://` endpoints therefore need a TLS-enabled build:

```bash
cmake -B build-tls -DCMAKE_BUILD_TYPE=Release -DFIXIT_ENABLE_OPENSSL=ON
cmake --build build-tls -j
```

`find_package(OpenSSL)` locates a system OpenSSL.  Two macOS specifics are worth
knowing, both learned the hard way on this project:

* cpp-httplib loads the trust store from the **Keychain** on Apple, which needs
  `CoreFoundation` and `Security`; the build links them automatically.  Pass
  `-DFIXIT_MACOS_KEYCHAIN_CERTS=OFF` to skip that and use a CA bundle instead.
* A system OpenSSL does **not** read the Keychain, so `https://` fails with
  `SSL server verification failed` even against a valid Let's Encrypt
  certificate.  `OpenAiLlm` therefore points OpenSSL at a bundle explicitly:
  `$FIXIT_CA_BUNDLE` if set, otherwise `/etc/ssl/cert.pem` on macOS (the bundle
  macOS ships for its own curl), otherwise the usual Linux paths.  Verification
  is never disabled.

Then run against any OpenAI-compatible endpoint:

```bash
export FIXIT_API_KEY=sk-...          # or: --api-key

./build-tls/bin/fixit your_file.cpp --agent --llm openai \
  --base-url https://your-endpoint/v1 \
  --model your-model --verbose --no-write
```

`tools/fixit_with_my_key.sh` wraps this for a project-local setup: it reads the
key from `$FIXIT_API_KEY` or a git-ignored `.secrets/fixit_key`, keeps it out of
`argv` (so it cannot appear in `ps` output or a log), and unexports it before the
child runs, so no trace or metrics file can contain it.

### 6.5 CLI

```
fixit <file.cpp> [--agent] [--llm mock|openai] [--model NAME]
      [--base-url URL] [--api-key KEY | env FIXIT_API_KEY]
      [--iterations N] [--verbose] [--trace PATH] [--metrics PATH]
      [--compiler NAME] [--no-write]
```

Without `--agent` it compiles once and prints the diagnostics. Exit codes:
`0` clean or repaired, `1` not repaired, `2` usage error (including
`--llm openai` without a key), `3` internal error.  `--no-timing` omits
wall-clock reports so output can be diffed between runs — that is how
[`docs/demo_output.txt`](docs/demo_output.txt) stays byte-reproducible. ANSI colour is suppressed when
stdout is not a TTY or `NO_COLOR` is set, which keeps `docs/demo_output.txt`
diffable.

## 6.6 Regression worth knowing about

A hunk that **inserts** lines while quoting surrounding context used to consume one
file line too many, silently deleting the line after the insertion point.  It was
found by driving the engine with a real model rather than by the golden corpus —
every generated case replaced lines, so none combined quoted context with an added
line.

The apply path now derives the replaced span from the hunk's old side (context plus
removals) and only widens it towards a *larger* count declared in the header, which
is the case the header exists to rescue.  `[patch][regression]` in
`tests/patch_golden.test.cpp` covers it, and ADR-028 records the defect, the two
reverted attempts and the final fix.

## 7. Testing

```bash
ctest --test-dir build --output-on-failure
```

| Suite | Contents |
|---|---|
| `patch_golden.test` | **60 golden patches** (10 exact, 10 line-drift, 10 whitespace, 10 missing-context, 10 extra-context, 10 multi-hunk) asserting `all_applied` and byte-exact output, plus **10 negative cases** asserting refusal, untouched content, and a `failure_reason` that contains the score, the gate, the window and the offending line |
| `compiler.test` | 20 fixtures (recorded clang output + GCC-format output) and a gcc/clang **parity** check over `e1`–`e3`: identical `(line, col)` sets |
| `codemap.test` | five sample files: function lists, innermost `enclosing_function`, verbatim includes, snippet format and clamping, syntax errors, missing files |
| `agent_mock.test` | full loop on `e1`/`e2` (success, clean compile, patch reports), `e3` unfixed without crashing, trace schema, byte-identical repeat runs, tool dispatch and sandboxing, and an unusable compiler failing in one round |
| `expected_artifacts.test` | `examples/buggy/expected/` matched against a live mock run, plus the committed demo transcript being timing-free |
| `cli_contract.test` | the binary's observable behaviour: exit codes, usage errors, a directory rejected instead of "compiling clean", `--outline`, `--no-write` leaving the file byte-identical, `--no-timing` |

The golden cases are generated by [`tools/gen_golden_cases.py`](tools/gen_golden_cases.py)
into `tests/patch_golden_cases.inc`, which is committed — the C++ suite never
needs Python.

Every test is offline and deterministic: `MockLlm` has no I/O, and the
integration test asserts that two runs of the same input produce byte-identical
traces.

## 8. Reusability Justification

> tree-sitter-cpp 只能解析、cpp-httplib 只能送 HTTP、diffutils 只能機械式套用 diff。
> **沒有現成 C++ 庫能閉環「compiler diagnostics → code location → LLM patch →
> fuzzy apply → re-verify」並把失敗原因結構化回傳給 LLM**。FixIt 補上這層，任何
> agentic coding 工具（CI 修復 bot、IDE 插件）可直接 link。

Concretely: `libfixit` installs as a normal CMake target with a header-only-style
surface (`include/fixit/*.h`), no globals, no CLI dependency. A CI repair bot can
use just `Compiler` + `PatchEngine`; an IDE plugin can use `CodeMap` alone; an
agent product can take the whole `Agent`.

## 8a. Measured effectiveness

`./build/bin/fixit-matrix` sweeps well-formed diffs whose imperfection is
controlled, and `python3 tools/render_heatmap.py` turns the result into the heat
maps under `docs/`. Current numbers (200 trials per cell):

| Corpus | Drift tolerance |
|---|---|
| Unique lines, 3 context lines | **100%** up to ±20 lines of drift, 99% at ±50 |
| Realistic boilerplate (`}`, blank lines, near-identical bodies) | **100%** up to ±20, 99% at ±50 |
| Every block byte-identical | 100% up to ±2, **0% at ±5 and beyond** |

The first two rows are the claim the project makes. The third row is its honest
limit: when the context matches several places equally well *and* the declared
position is wrong, no algorithm can recover the intent — FixIt applies to the
nearest candidate, which the sweep counts as a miss. Both the trailing-whitespace
and the missing-context impairments are included in every row. See
[`docs/wiki_outline.md`](docs/wiki_outline.md) and
[`docs/patch_success_matrix.json`](docs/patch_success_matrix.json).

## 9. Roadmap

* **Real-LLM evaluation** — the harness is ready (`--llm openai --base-url`); the
  fixtures and metrics need to be run against a hosted model, including the
  `e3` case the mock deliberately refuses.
* **Multi-file projects** — `PatchEngine::parse_diff` already returns several
  `DiffFile`s and `Agent` can patch any of them; the missing piece is a build
  system adapter that produces per-file diagnostics for a whole target.
* **Package managers** — vcpkg/Conan recipes so consumers do not build
  tree-sitter themselves.
* **Success-rate heat map** — `--metrics` emits the JSON the Wiki chapter needs;
  the plotting step and the video walkthrough remain.

## 10. Team & Division of Work

| Member (git account) | Module / artefact |
|---|---|
| _fill in_ | `Compiler` + compiler fixtures (`src/compiler.cpp`, `tests/compiler.test.cpp`) |
| _fill in_ | `CodeMap` + samples (`src/codemap.cpp`, `tests/codemap.test.cpp`) |
| _fill in_ | `PatchEngine` + golden patches (`src/patch.cpp`, `tests/patch_golden.test.cpp`) |
| _fill in_ | `Agent` + LLM backends + CLI (`src/agent.cpp`, `src/*_llm.cpp`, `tools/fixit-cli/`) |
| _fill in_ | CI, docs, Wiki, demo video (`.github/workflows/ci.yml`, `docs/`, `README.md`) |

## Layout

```
fixit/
├── include/fixit/{compiler,codemap,patch,agent,types}.h
├── src/{compiler,codemap,patch,agent,openai_llm,mock_llm}.cpp
├── tools/fixit-cli/main.cpp
├── tools/{gen_golden_cases.py,record_demo.sh,record_compiler_fixtures.sh}
├── tests/{patch_golden,compiler,codemap,agent_mock}.test.cpp
├── tests/fixtures/{compiler,codemap}/
├── examples/buggy/{e1_missing_include,e2_drift,e3_type_error}.cpp
├── examples/buggy/expected/
├── docs/{decisions.md,demo_output.txt}
└── third_party/            # pinned snapshot; FetchContent is the fallback
```

## Design decisions

Every ambiguity in the original brief is resolved and recorded in
[`docs/decisions.md`](docs/decisions.md) (ADR format: context / decision /
rationale) — the scoring normalisation, the window ladder, partial-apply
semantics, the mock's deliberate drift, dependency pinning, the API-key policy,
and the platform notes.
