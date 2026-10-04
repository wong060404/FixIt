# Decisions (ADR log)

Every place where the brief left room for interpretation is recorded here with
the context, the decision taken, and why.  The rule applied throughout: **when
the spec is ambiguous, take the simplest behaviour that keeps the core loop
honest, and write it down.**

---

## ADR-001 — Position convention inside the patch engine

**Context.** The hunk struct stores 1-based `old_start` (from `@@`), reports are
necessarily 1-based (they are shown to humans and models), but the file is a
`std::vector<std::string>` indexed from 0.  The first implementation mixed the
two: the search produced a 1-based anchor while the splice used it as a 0-based
offset.

**Decision.** The search path is **0-based end to end**: a candidate position
`pos` means the signature would occupy file indices `[pos, pos + size)`.  The
conversion to 1-based happens exactly once, when a `HunkReport` is built
(`pos + 1`).  The convention is documented at the top of the "Candidate
evaluation" section in `src/patch.cpp`.

**Rationale.** The bug this prevents is silent and severe: a drifted hunk
rewrites the line *above* the one it matched.  Golden cases 1–60 now include
enough drift and whitespace variety that any future convention slip fails
loudly.

---

## ADR-002 — Normalising the hunk score

**Context.** The brief specifies

```
score(p) = (0.6*exact + 0.3*fuzzy - 0.2*mismatch) / signature.size()
```

together with `gate = 0.8` and demo output that prints `score 1.00` for an exact
hunk.  Those three cannot all hold: for a perfect hunk (`exact == size`) the
formula yields `0.6`, so **every** hunk would fail its own gate and the demo
output would be unreachable.

**Decision.** Divide by the best attainable raw score instead of by the line
count:

```
maximum  = 0.6 * signature.size()
score(p) = (0.6*exact + 0.3*fuzzy - 0.2*mismatch) / maximum
```

A perfect hunk scores `1.00`, `fuzzy_line_threshold` and `gate` keep the
documented default values `0.8` and `0.8`, and the demo transcript matches the
brief.

**Rationale.** Two of the three constraints can be preserved exactly (the
weights and the gate); only the denominator is corrected, and it is corrected in
the direction that makes the documented thresholds meaningful.  The alternative —
keeping the literal formula and lowering the gate to `0.48` — would have made
"exact" and "mostly wrong" indistinguishable in the reported scores.

---

## ADR-003 — Encoding the mock's deliberate line drift

**Context.** The brief requires the mock to declare `@@` positions one line off
so every demo exercises the fuzzy path.  For a *replacement* hunk the encoding is
obvious (declared = true + 1).  For a *pure insertion* the engine interprets
`old_start` as "place the new lines before this 1-based line", which interacts
with the same `+1`.

**Decision.** Replacement hunks declare `true_position + 1`.  The single
insertion rule (adding a missing `#include`) declares `last_include_line + 2`,
which after the engine's anchor arithmetic inserts immediately *after* the last
existing `#include`.  Both are implemented in `src/mock_llm.cpp` and pinned by
`agent_mock.test`.

**Rationale.** The intent is "the model's guess is off by one", not "the model
emits a broken patch", and the engine's insertion semantics are the simplest
reading of the unified-diff format.

---

## ADR-004 — No TLS by default; OpenSSL is opt-in

**Context.** The brief allows exactly four dependencies and forbids others.
`cpp-httplib`'s HTTPS support needs OpenSSL, which is not one of them.

**Decision.** `FIXIT_ENABLE_OPENSSL` is `OFF` by default: plain `http://`
OpenAI-compatible endpoints (Ollama, vLLM, llama.cpp server, test doubles) work
out of the box.  With `-DFIXIT_ENABLE_OPENSSL=ON` the build links a system
OpenSSL and `https://` endpoints work too.  `fixit::openai_tls_available()`
reports which build is in effect, and `--llm openai` against an `https://` URL
fails with exit code `2` and an actionable message rather than a confusing
transport error.

**Rationale.** It keeps the dependency list exactly as specified while leaving
the common "local model server" path fully functional, and it never silently
pretends TLS exists.

**Note.** The vendored `third_party/cpp-httplib/httplib.h` carries a small local
patch (marked with a comment): upstream includes `<openssl/*.h>` outside the
`CPPHTTPLIB_OPENSSL_SUPPORT` guard, and `#ifdef` treats `-D...=0` as enabled.
Both are normalised so `FIXIT_ENABLE_OPENSSL=OFF` really means off.

---

## ADR-005 — API-key handling

**Context.** The brief requires the key to come only from `--api-key` or
`FIXIT_API_KEY` and never be logged or hard-coded.

**Decision.** The key lives in `OpenAiLlm` only.  `--trace` never records it (the
trace contains tool calls and observations, and the dispatch loop only records
the tool *name*), the metrics writer never sees it, and transport errors print
the HTTP status and the server's `error.message` only.  No default or placeholder
key exists anywhere in the tree.

**Rationale.** A leaked key in a committed trace is the one mistake this project
could not walk back.

---

## ADR-006 — Dependencies: vendored snapshot *and* pinned FetchContent

**Context.** The brief allows git submodules or `FetchContent`.  Submodules
require a remote that CI can reach; `FetchContent` requires network at configure
time and cannot build offline.

**Decision.** Both.  `third_party/` holds a pinned source snapshot (the default;
fully offline and reproducible), and `cmake/ThirdParty.cmake` falls back to
`FetchContent` with the same tags whenever a component is missing — a fresh
`git clone` without the snapshot still configures, and `-DFIXIT_FETCH_DEPS=ON`
forces the download path.

Pinned revisions:

| Component | Version |
|---|---|
| tree-sitter runtime | 0.28 development snapshot (language ABI 15) |
| tree-sitter-cpp | v0.23.4 (grammar ABI 15) |
| nlohmann/json | v3.12.0 |
| cpp-httplib | v0.28.0 |
| Catch2 | v3.8.1 |

**Rationale.** The acceptance criterion is "one command from a fresh clone", and
a snapshot cannot be broken by an upstream force-push or a network outage at the
wrong moment.  The fallback keeps the repo usable if the snapshot is stripped.

**Note.** Vendored third-party sources are never compiled with the project's
`-Wall -Wextra -Werror`: they are their own targets with `SYSTEM` includes, so
upstream warnings cannot break our build and our warnings cannot be hidden.

---

## ADR-007 — Clang JSON dialect is probed, never assumed

**Context.** The brief specifies `-fjson-diagnostics`.  Apple clang 17 rejects it
(`unknown argument`) and wants `-fdiagnostics-format=json`.  A build that blindly
passes the documented flag silently loses every diagnostic — the worst possible
failure mode for a tool whose ground truth is compiler output.

**Decision.** `Compiler` probes the configured compiler once (against an empty
translation unit), preferring `-fjson-diagnostics`, then
`-fdiagnostics-format=json`, and finally falls back to the text parser.  The
chosen dialect is cached for the lifetime of the `Compiler`; `CompilerConfig::format`
can force `Text` or `Json`.  `parse_diagnostics()` is exposed so fixtures can
test both dialects without spawning a process.

**Rationale.** The spec's flag is tried first, so a clang that supports it behaves
exactly as written; everything else degrades to a working state instead of a
silent one.

---

## ADR-008 — Compiler output capture and timeout

**Context.** The diagnostic list must be deterministic and nothing may throw.

**Decision.** `fork`/`exec`, `stdout` and `stderr` merged into one pipe, read
non-blocking under `poll` with a deadline.  On timeout the child is `SIGKILL`ed,
the pipe is drained, `timed_out = true` and `exit_code = -1`; the partial output
is still parsed and returned.  A failed `exec` writes
`fixit: cannot execute '<name>': …` into the same pipe, so a missing compiler
surfaces as ordinary output with exit code `127` rather than an exception.
Diagnostics are sorted by `(file, line, col, message)`, de-duplicated, and capped
at 50.  GCC notes are dropped (they are not independently actionable); clang
notes are folded into their parent message as `message: note`.

**Rationale.** A repair loop must be able to say "the compiler itself did not
run" without crashing, and the same input must always produce the same list.

---

## ADR-009 — CodeMap specifics

**Context.** `enclosing_function(line, col)`, `signature` and
`context_snippet()` leave detail open.

**Decision.**
* `signature` is the source text from the start of the definition to the byte
  before the body node, with trailing whitespace stripped — so it never contains
  the `{` (this matches the brief and makes the text directly quotable).
* `enclosing_function` returns the **tightest** range containing the position.
  `col` is accepted but unused: with one translation unit and no macro expansion
  there is no position for which the column changes the answer.  Inline
  definitions (`Widget() = default;`) count as functions, because tree-sitter
  parses them as `function_definition`.
* `context_snippet` prints `%4d | %s` and is clamped to `1..EOF`; it never emits
  a line beyond the end of file.
* A missing, empty or unparsable file is not an error: the map reports no
  functions and `has_syntax_errors() == true`.  No CodeMap method throws.

**Rationale.** The snippet format is what a model quotes back into a patch, and
"degrades, never throws" is required for a tool that runs inside a loop.

---

## ADR-010 — MockLlm rule details

**Context.** The brief fixes R1–R4 and the symbol→header table, but not how a
rule interacts with clang's error recovery, nor what happens when a symbol's
header is already reachable transitively.

**Decision.**
* **At most one rule fires per round, and it batches every missing header it
  found.** Adding a line above a second hunk would shift that hunk's position
  inside the same diff; one edit per call is also what a real model does.
* **A header is only "missing" if no real `#include` line provides it.** A usage
  such as `std::vector<std::string> words;` contains the literal `<vector>` and
  must not be mistaken for an existing include.
* **R1 also matches clang's `no member named 'X' in namespace 'std'`**, which is
  the same failure mode as `was not declared` for this rule.
* **The files a rule reads are resolved against the same workdir the `patch`
  tool uses** (`WorkspaceAware`); the agent injects it, so a caller cannot
  accidentally let the two diverge.
* **The rule table is the brief's table, unchanged.** A symbol that the standard
  library exposes transitively (libc++'s `<string>` pulls in `<cstdio>`, so
  `FILE`/`printf` resolve without an include) still gets its header added — a
  harmless edit that is also correct on libstdc++.  `e1` therefore uses
  `std::vector`, which is genuinely missing, so the include rule is *exercised*
  rather than merely executed.

**Rationale.** These are the smallest choices that keep the demo honest: the
repair a rule proposes must be the one the compiler actually demanded.

---

## ADR-011 — The agent feeds compiler output back every round

**Context.** The brief says tool results are appended as observations.  It does
not spell out that the *recompile* after a patch is also an observation.

**Decision.** After the tools of round *N* run, the agent compiles, records the
result in the trace as `compile_after`, and appends it as a message before round
*N+1* begins.  The model therefore never refines a stale error list.

**Rationale.** Without this the loop silently repairs the errors it saw at
iteration 1 — line numbers and all — which is exactly the class of bug this
project exists to eliminate.  It was found by the end-to-end test, not by
inspection, and it is now covered by `agent_mock.test`.

---

## ADR-012 — What "iterations" means

**Context.** The loop compiles before the first model call, once per round, and
once more to verify.  The demo counts the final clean compile as an iteration.

**Decision.** `AgentResult::iterations` counts **model rounds** and equals the
length of the trace array (rounds 1..N).  The terminal verification compile is
delivered to observers with `Event::verification == true`; the CLI prints it as
the outcome line (`✓ clean`) without opening a new `── iteration ──` heading.

**Rationale.** One number, one meaning, checkable against the trace.  The demo
still shows the clean compile, which is what makes the transcript convincing.

---

## ADR-013 — Golden-patch corpus and fixture provenance

**Context.** 60 golden patches plus 20 compiler fixtures are required; this
machine has only Apple clang 17 (on macOS `g++` is a symlink to clang).

**Decision.**
* The 60 positive and 10 negative patch cases are *generated* by
  `tools/gen_golden_cases.py` and committed as `tests/patch_golden_cases.inc`, so
  the C++ suite has no Python dependency but the corpus stays reproducible.
* The corpus deliberately contains a base file with two similar functions, which
  caught a real class of bug: an ambiguous diff that matches the wrong function.
  Cases always disambiguate their target.
* Compiler fixtures recorded on this machine are clang's real output.  GCC
  fixtures are written from GCC's documented format and marked
  `"recorded": "synthetic"`; `tools/record_compiler_fixtures.sh` re-records them
  on a machine with real GCC and **refuses to overwrite a GCC fixture with clang
  output**.  GCC fixtures are never fabricated by copying clang output.

**Rationale.** Honest provenance beats a plausible-looking file.  CI (which has
gcc 12) is the environment where the GCC dialect is exercised for real, and the
script turns that into a one-command refresh.

---

## ADR-014 — Platform and build policy

**Context.** `-Wall -Wextra -Werror` on gcc 12 and clang 15 is a hard gate.

**Decision.** The flags apply to `fixit`, `fixit-cli` and every test target, but
never to third-party code (separate targets, `SYSTEM` includes).  Warnings are
errors for our code; upstream's are invisible.  C++20, `CMAKE_CXX_EXTENSIONS OFF`,
and a single `enable_testing()`/`ctest` entry point.  Windows is out of scope, so
`fork`/`exec` is used directly rather than a portable process wrapper.

**Rationale.** `-Werror` on someone else's code makes the build hostage to
upstream releases; `-Werror` on ours is the point.

---

## ADR-015 — `--metrics` shape

**Context.** The brief fixes the JSON keys but not the values' meaning.

**Decision.** `patch_success` counts hunks by outcome (`exact` for status
`Applied`, `drift` for `FuzzyApplied`, `whitespace`/`missing_ctx` for failures
classified by their structured reason).  `fix_rate` is `1.0`/`0.0` for the run,
`avg_iterations` is the run's iteration count, `total_time_ms` is wall-clock and
the *only* intentionally non-deterministic field — it is never part of the trace.

**Rationale.** The Wiki's success-rate chapter needs stable counts, and keeping
the clock out of the trace is what makes the determinism test meaningful.

---

## ADR-016 — How the effectiveness numbers are produced

**Context.** The W3 milestone asks for a success-rate matrix and a heat map. A
hand-made chart would be unfalsifiable.

**Decision.** `tools/matrix.cpp` builds the corpus, the diffs and the runs, and
prints both the table and `docs/patch_success_matrix.json`;
`tools/render_heatmap.py` renders that JSON into three dependency-free SVGs.
Every diff the tool generates is *well formed* (the `@@` counts equal the lines
actually quoted); the impairment is always in the context content or shape, never
in the header arithmetic, because a malformed header is a different and
uninteresting failure. A trial counts only when the hunk applied **and** the
repaired line replaced the intended line: a repair that lands on the wrong line
is a miss.

**Rationale.** The numbers in the README and the Wiki must be reproducible by
anyone with the repository, and they must not flatter the engine. The measured
result — 100% to ±20 lines of drift on realistic corpora, 0% past ±2 once every
candidate is byte-identical — is the honest shape of the technique.

**Consequence for the window ladder.** A perfect score inside a window no longer
ends the search: when several positions match equally well, the hunk belongs to
the one nearest the declared position, and a nearer candidate may still be found
by widening. The whole-file fallback is skipped once a perfect candidate exists,
so the common case stays cheap. This is the spec's "track the global best plus
distance tie-break" intent, made explicit.

---

## ADR-017 — "Clean" requires a compiler that actually ran

**Context.** `CompileResult::clean()` was "no error diagnostics".  A compiler that
could not be executed (`fork`/`exec` failure, exit code `127`) or one killed on
timeout produces *no diagnostics at all*, so it looked clean.  `clean()` is the
loop's only ground truth, which made "the toolchain is missing" indistinguishable
from "the code is fixed" — the worst possible confusion for this tool.

**Decision.** `clean()` is true only when the process exited `0` **and** no
error-level diagnostic was parsed.  A non-zero exit, a timeout, or a signal all
report not-clean.  To keep the diagnostics visible anyway, `exit_code`,
`timed_out` and `raw_output` are always populated, and the `compile` tool returns
them alongside `clean`.

**Consequences.** The mock model detects "not clean, zero errors" and answers
`FINAL` with an explanation instead of guessing or looping, so a broken
environment fails loudly in one round.  `agent_mock.test` covers it, including
that the source file is left untouched and the trace records `exit_code: 127`.

---

## ADR-018 — The standard tools use the loop's compiler

**Context.** `make_standard_tools(workdir)` built its `compile` tool with a
default-constructed `Compiler`, i.e. `g++`, regardless of `--compiler`.  The
agent's own verification used the configured compiler.  With the mock backend the
mismatch is invisible (the mock never calls `compile`), but any real model that
re-checks its work through the tool would be told about a different compiler's
diagnostics than the loop acts on — the model could "fix" something the agent
still sees as broken, or vice versa.

**Decision.** `make_standard_tools(workdir, compiler)` takes the compiler; the
single-argument overload remains for convenience and uses the documented
default.  The CLI and every test pass the same compiler object to both the tools
and the agent, so the two can never disagree.

**Rationale.** One loop, one compiler.  The bug was found by exercising the tools
directly (a deliberately non-existent compiler binary proved the override was
being ignored) rather than by reading the code.

---

## ADR-019 — The demo transcript is byte-reproducible

**Context.** §10 requires the demo evidence to be committed, but the summary line
prints wall-clock time, so `docs/demo_output.txt` changed on every regeneration
and a reader could not tell a real behavioural change from a slow machine.

**Decision.** `--no-timing` omits the elapsed time, and `tools/record_demo.sh`
uses it.  The transcript is now byte-identical across runs (verified by hashing
it twice), so it can be committed as evidence that never drifts.  Wall-clock time
remains available by default and is still the *only* intentionally
non-deterministic field in `--metrics`.

**Rationale.** Determinism is a stated quality gate; an artefact that churns on
every run is not evidence.  `expected_artifacts.test` checks that the committed
file has no timing values and mentions all three examples.
