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

| Component | Snapshot and FetchContent pin | Notes |
|---|---|---|
| tree-sitter runtime | 0.28.0, pinned by commit `752c612a` | 0.28.0 is unreleased (newest tag is v0.27.0); the snapshot declares that version and master matches it, so a tag cannot identify it |
| tree-sitter-cpp | v0.23.4 | grammar ABI 15 |
| nlohmann/json | v3.12.0 | |
| cpp-httplib | v0.59.0 | the snapshot carries a local patch (see ADR-027) |
| Catch2 | v3.16.0 | |

The pins and the snapshot must move together.  They had drifted — the snapshot held
cpp-httplib 0.59.0 and Catch2 3.16.0 while these lines still named v0.28.0 and
v3.8.1 — which meant the offline path and the `FetchContent` path would have built
*different* libraries, and the vendored cpp-httplib is patched, so a different
upstream revision need not even compile.  Both paths have since been built and run.

**Rationale.** The acceptance criterion is "one command from a fresh clone", and
a snapshot cannot be broken by an upstream force-push or a network outage at the
wrong moment.  The fallback keeps the repo usable if the snapshot is stripped.

**Note.** Vendored third-party sources are never compiled with the project's
`-Wall -Wextra -Werror`: they are their own targets with `SYSTEM` includes, so
upstream warnings cannot break our build and our warnings cannot be hidden.

---

## ADR-007 — Clang JSON dialect is probed, never assumed

**Context.** The brief specifies `-fjson-diagnostics`.  A build that blindly passes
the documented flag silently loses every diagnostic — the worst possible failure
mode for a tool whose ground truth is compiler output.

This ADR originally claimed Apple clang 17 *wants* `-fdiagnostics-format=json`.
Measured on the machine this was developed on, clang 17 rejects **both**:
`-fjson-diagnostics` is an unknown argument and `-fdiagnostics-format=json` fails
with `invalid value 'json'`.  That is precisely why the flag is probed rather than
selected by compiler name, and the README was right where this paragraph was
wrong.

**Decision.** `Compiler` probes the configured compiler once (against an empty
translation unit), preferring `-fjson-diagnostics`, then
`-fdiagnostics-format=json`, and finally falls back to the text parser.  Forcing
`Text` also stops the probed JSON flag from being sent: passing a JSON flag while
parsing text yields zero diagnostics from a failed compile, which looks like a
clean file.  Only `uses_json` decides whether the flag is emitted.  The
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

---

## ADR-020 — CodeMap is reachable from the agent and the CLI

**Context.** `CodeMap` was a library class used only by the CLI's verbose context
line and by its own tests.  A model in the loop had no way to ask "what does this
file look like?" — it could only read numbered lines and guess at structure.

**Decision.** Two additive integrations, neither of which changes the documented
shapes:

* the `read` tool keeps its contract (`{content}` with numbered lines) and adds an
  optional `outline` (default on) carrying functions with their line ranges,
  includes, and syntax-error lines.  `outline: false` returns the minimal answer.
* `fixit <file> --outline` prints the same map for a human and exits.

**Rationale.** The brief defines the three tools, so no fourth tool is added; the
structure rides along with the read the model already does.  This also gives
`--outline` a natural CI use: it fails visibly on a syntax-broken tree without
running a compiler.

---

## ADR-021 — The CLI's contract is tested through the binary

**Context.** Every library module had tests, but nothing checked the wiring a user
actually touches: exit codes, argument handling, and the two flags added for
reproducibility.  A first smoke pass immediately found that passing a *directory*
as the source file exited `0` and printed "clean" — a false green build.

**Decision.** `cli_contract.test` runs the built binary with `popen` and asserts
the observable contract: usage errors (including that a directory is rejected
with the usage text), compile mode's `0`/`1`, `--outline` content, `--no-write`
leaving the source byte-identical, `--no-timing` producing no wall-clock value,
and `--llm openai` without a key exiting `2`.  Usage mistakes now print the usage
text to stderr; `--help` still prints it to stdout and exits `0`.

**Rationale.** The library can be perfectly correct while the executable still
misreports success, and "exit 0" is the one output a CI bot trusts.

---

## ADR-022 — Unknown tool calls are visible in the trace, and the key is guarded by a test

**Context.** Two failure modes had no evidence trail.  If a model asks for a tool
this build does not provide, the only trace of it was the error string inside an
observation — easy to miss when diagnosing a stuck loop.  And the API-key rule in
§10 was a policy statement with no test behind it.

**Decision.**
* An observation for a tool the registry does not know carries
  `"unknown_tool": true` and the list of tools that do exist, so the trace answers
  "what did it ask for, and what was available?".
* `agent_mock.test` plants a recognisable key in `FIXIT_API_KEY`, runs a real loop,
  and asserts the value appears in no trace, no `PatchResult`, no failure summary
  and no CLI output.  `cli_contract.test` covers the usage-error path for
  `--llm openai` without a key.

**Rationale.** A quality gate that is only asserted in prose is not a gate.  Both
of these are cheap to check and would be expensive to discover in production.

---

## ADR-023 — Cross-compiler parity is asserted on lines, not columns

**Context.** §6 asks for a parity test proving that `e1`–`e3` produce the same
`(line, col)` sets under GCC and clang.  This machine has no real GCC, so the GCC
fixtures are hand-written and passed trivially.  Writing the *live* parity test
(compile with both, compare the parsed output) exposed the flaw: the compilers
really do not agree on columns.  For `e3_type_error.cpp`'s `count_words(value)`
clang anchors the invalid conversion on the identifier (`13:15`) while GCC points
at the argument expression (`13:25`).  A strict `(line, col)` assertion would
therefore fail against the very tools it is meant to validate, and could only ever
pass on synthetic data.

**Decision.** Both parity cases assert the error **line** sets exactly and compare
column usability only (each reported column must be `> 0`); a differing column is
surfaced through `INFO` rather than failing.  The divergence and its cause are
documented in `tests/fixtures/compiler/README.md`.  Individual fixtures still
assert their own exact columns, so the parsers themselves stay tightly pinned.

**Rationale.** The line is the contract that matters: it selects the hunk context
the repair loop works from.  Asserting more than the tools guarantee would make CI
fail for a reason that is not a defect in FixIt, and relaxing it silently would
hide real parser regressions -- hence the explicit `INFO` and the per-fixture
column assertions.

---

## ADR-024 — `-ferror-limit` is probed, because GCC does not have it

**Context.** §3.1 specifies the fixed flag set
`-std=c++20 -fsyntax-only -ferror-limit=<N>`.  That is true of clang and false of
GCC: `g++-12` answers

```
g++-12: error: unrecognized command-line option '-ferror-limit=5'
```

and exits before compiling anything.  The result is *zero diagnostics*, which is
indistinguishable from a clean build — so on the ubuntu/gcc-12 CI job every
compile looked successful while nothing had been compiled.  The whole suite was
green on macOS because `/usr/bin/g++` there is clang.

**Decision.** Capability probing is extended from the diagnostic dialect to the
optional flags.  `-ferror-limit=N` is passed only when the compiler accepts it;
otherwise it is omitted (the 50-diagnostic internal cap still applies).  This
required removing an early `return` that skipped probing entirely for any
compiler whose name did not look like clang — the reason the flag was passed to
GCC even after the first fix attempt.  The probe itself had to change from "exited
0 and emitted parseable JSON" to "did the compiler *reject* this flag?", because
`-ferror-limit` is accepted silently and emits nothing.

**Consequence (accepted deviation).** With GCC, `error_limit` cannot be honoured:
GCC offers no equivalent flag, so it reports every error rather than the first N.
FixIt's own cap of 50 parsed diagnostics still applies.  This is recorded here
rather than emulated, because inventing a limit by truncating output would hide
diagnostics the compiler considered relevant.

**How it was found and verified.** The CI job's log gave the exact refusal; a
GCC-behaviour simulator (rejecting the flag while emitting GCC-format
diagnostics) then reproduced it locally.  Against that simulator the three
examples behave exactly as required: e1 and e2 exit 0, e3 exits 1, and the
repaired e1 compiles.  Keeping the CI job — rather than trusting the local
toolchain — is what surfaced this; the first green run on Linux is the proof.

---

## ADR-025 — Cross-compiler parity asserts the faulty statement, not the error set

**Context.** §6 asks for a gcc/clang parity test that the `(line, col)` sets are
identical.  Running it against real GCC showed that this is not a property any
two compilers have.  For `e1_missing_include.cpp` the same missing semicolon makes
GCC emit **10** diagnostics and clang **5**, because their error recovery differs;
the full line sets differ too ({10, 14, 20, 21} vs {10, 14}).  Columns differ as
well (`13:25` vs `13:15` on e3).  An equality assertion could therefore only ever
pass against hand-written fixtures, and it failed the first time CI used a real
GCC.

**Decision.** Both parity cases assert what is actually guaranteed and actually
matters to the repair loop:

* each compiler reports at least one error, and neither misses the fault;
* the **first** diagnostic of each names the same line (that line is what selects
  the hunk context);
* every line clang flags is also flagged by GCC — GCC may cascade further, it may
  not cascade less;
* columns must be usable (`> 0`) but are not compared.

Individual fixtures still assert their own exact line, column, level and message,
so the parsers stay tightly pinned; only the cross-compiler comparison is
relaxed, and the divergence is measured and printed through `INFO`.

**Consequence.** The archived corpus keeps hand-aligned GCC fixtures.  They are
marked `"recorded": "synthetic"` and re-recordable with
`tools/record_compiler_fixtures.sh` on a machine with real GCC.  Because the
parity assertions no longer compare counts, re-recording them will not break the
suite.

---

## ADR-026 — `tools/gcc_sim.py` exists because this machine has no GCC

**Context.** Development happens on macOS, where `/usr/bin/g++` is clang.  Two
GCC-specific defects reached CI before anyone could see them locally: the
clang-only `-ferror-limit` flag, and parity assertions that real GCC cannot
satisfy.  Each cost a CI round trip and a log dig.

**Decision.** Ship a small GCC stand-in (`tools/gcc_sim.py`) that rejects the
clang-only flags exactly as g++-12 does and emits GCC-shaped diagnostics, using
clang only as its analysis backend.  It can be symlinked as `g++` on `PATH`, and
the local suite then reproduces the CI conditions: with it, the full test suite
passes 6/6, and before the ADR-024 fix it failed in exactly the way CI did.

**Rationale.** The simulator is honest about being a simulator (the diagnostics
are real, the driver is not), it is not part of the build, and it does not replace
CI: the ubuntu/gcc-12 job remains the authority on real GCC.  It shortens the
feedback loop from one CI round trip to seconds, which is what the two failures
above actually cost.

---

## ADR-027 — TLS is opt-in, and its trust store is explicit

**Context.** The brief allows exactly four dependencies and OpenSSL is not one of
them, so `https://` is not available in a default build.  A user with their own
API key on an `https://` endpoint therefore cannot use the tool without opting in.
Opting in then surfaced two macOS realities that had to be handled rather than
documented away:

1. cpp-httplib loads the trust store from the Keychain on Apple, which requires
   `CoreFoundation` and `Security`.  Passing `"-framework X"` as a plain string in
   an `INTERFACE` link does not survive to the final executable and the symbols go
   missing, so the frameworks are resolved with `find_library()` and attached to
   the targets that actually link.
2. A system OpenSSL does not read the Keychain (that is a SecureTransport
   privilege, which is why the system `curl` works), so certificate verification
   failed against a valid Let's Encrypt certificate.  The Keychain switch also had
   to be normalised, because the vendored httplib tests it with `#ifdef` and
   `-D...=0` therefore still enabled it.

**Decision.** `FIXIT_ENABLE_OPENSSL=ON` provides TLS, with
`FIXIT_MACOS_KEYCHAIN_CERTS` (default `ON`) choosing between the Keychain and a CA
bundle.  In both cases `OpenAiLlm` points OpenSSL at a bundle explicitly:
`$FIXIT_CA_BUNDLE` if set, else `/etc/ssl/cert.pem` on macOS, else the usual Linux
paths, and only if one exists.  Certificate verification is never disabled.
Both build flavours pass the full suite.

**Rationale.** "It failed SSL verification" is the least actionable error a user
can get, and silently disabling verification would be worse.  Making the trust
store explicit keeps the four-dependency rule (TLS stays opt-in) while letting a
user with a key actually use it.

**Key handling.** `--api-key`/`FIXIT_API_KEY` remain the only sources.  The
wrapper `tools/fixit_with_my_key.sh` reads the value and then unsets it before
exec'ing the CLI, so the key is not in `argv` and cannot reach a trace or metrics
file; a test asserts a planted key appears in neither.

---

## ADR-028 — A tracked defect: insertion hunks can drop the following line

**Context.** Driving the engine with a real model (through an
OpenAI-compatible endpoint) produced a hunk shaped like

```
@@ -7,3 +7,4 @@
 #include <string>
+#include <vector>
 
 int main() {
```

and the applied result deleted the line after the insertion point.  The cause is
in the apply path: it consumes `old_count` file lines while emitting
`context + additions` new lines.  When a hunk both quotes context and adds a line
those counts differ by the number of additions, so the splice resumes one line
too late.

This was **not** found by the golden corpus.  Every golden case inserts a line
*without* quoting trailing context around it, or replaces rather than inserts, so
none of them combine "quoted context" with "an added line" in the way a real model
does.

**Decision (first pass).** Record it rather than ship a third speculative fix.  Two
attempts were reverted: the first segfaulted the suite, the second kept the 60-case
golden corpus green but broke it in other places, because reconciling the header's
declared count with the number of lines a hunk actually quotes changes the
replacement-span calculation for *every* hunk.  It was therefore reproduced by a
test tagged `[!mayfail]` (visibly failing, but unable to turn CI red) and written
up here with its exact input.

That tag no longer exists: the next section records the fix, and the test is an
ordinary `[patch][regression]` case that passes.  The lesson that outlived the
defect is about the corpus, not the code — every generated case *replaced* lines,
so none combined quoted context with an inserted one, and the shape a real model
actually emits went untested.

A related gap was fixed in the same pass: a diff that parses to zero hunks used to
return an empty report with no explanation, which is the failure a real model hits
most often (its `@@` header does not match the expected shape).  The `patch` tool
now returns the reason plus the first 600 bytes of the received diff, and the
model corrected its format on the next round when this was tested live.

**Resolution.** Fixed in the same series.  The apply path now separates the two
counts it had conflated:

* the consumed span is the number of **old-side lines the hunk quotes** — context
  plus removals — widened only towards a *larger* `old_count` from the header (the
  case the header exists to rescue: a model that omits context);
* the written output is the quoted context (taken from the file, so untouched code
  keeps its own bytes), the additions, and any widened context.

A pure deletion (`@@ -2,1 +2,0 @@`, one `-` line and no `+` line) removes its line
and writes nothing, which the previous shape got wrong in the opposite direction.
Three bugs were fixed and each is covered: the deletion case, the "widen too
eagerly" case, and a bounds read when the new side quotes nothing.

The lesson recorded for the test corpus: all 60 generated cases replaced lines, so
none of them combined quoted context with an inserted line.  The regression test
for that shape now exists.

---

## ADR-029 — Compiler flags are pass-through, not guessed

**Context.** `CompilerConfig` always had an `extra_flags` field, but the CLI never
populated it.  Any file that included a project header therefore failed with
`'math_utils.h' file not found` before FixIt could see a real diagnostic, which
made the tool unusable on anything but self-contained files.

**Decision.** Add `-I DIR` / `--include DIR`, `-D NAME[=VALUE]` and a general
`--flag FLAG`, all repeatable, all forwarded verbatim; the joined `-Iinclude` form
is accepted.  The echoed command line includes the flags so the user can see
exactly what ran.

**Alternatives rejected.** Parsing `compile_commands.json` automatically would be
more convenient, but it is a second input format with its own failure modes, and
the brief does not ask for it.  Guessing include paths from the tree would be
worse than either: a silent wrong path produces wrong diagnostics, which is harder
to notice than a missing flag.  Explicit flags keep one source of truth.

**Consequence.** Debugging a real file is now:

    fixit src/parser.cpp -Iinclude --agent --llm openai ...

with `-I` matching whatever the project already uses.
---

## ADR-030 — The clone builds itself; the compiler is discovered, not configured

**Context.** A fresh clone was not runnable.  The CMake flow needs a `cmake` the
machine may not have (this Windows host does not), and the Windows flow required
three prepended environment lines plus a toolchain at a path fixed relative to the
checkout (`<repo parent>\toolchain\mingw\mingw64\bin`), so a clone in any other
layout failed with `compiler not found`.  The guide also assumed the checkout lives
at `C:\FixIt`.  Every one of those is environment work the user has to know about
before the first run, which is exactly the question a clone should not raise.

**Decision.** Two root launchers (`fixit.ps1` for PowerShell, `fixit.cmd` for
`cmd.exe`) become the documented Windows entry point.  They resolve everything from
`$PSScriptRoot`, build into the git-ignored `build-win\` when the binary is missing
or older than any source, and forward every argument unchanged.  Compiler discovery
lives in `tools\windows\fixit-env.ps1` and is dot-sourced by `build_fixit.ps1` too,
so the launcher and the build cannot disagree; `build_fixit.ps1` also accepts
`-CompilerBin`.  The search order is: `-CompilerBin`, `$env:FIXIT_TOOLCHAIN`,
`<repo parent>\toolchain`, `%TEMP%\fixit-toolchain`, a `g++` already on `PATH`, and
finally `C:\msys64\{ucrt64,mingw64,mingw32}\bin`.

Two behaviours were added to the build script itself:

* it prepends the chosen compiler directory to `PATH` **for its own process only**,
  because MSYS2's `gcc.exe` finds `as.exe`/`ld.exe` through `PATH` and, invoked by
  absolute path with its bin directory absent, exits `1` without printing anything
  (measured; the identical command succeeds once the directory is back on `PATH`);
* the launcher switches the console to UTF-8 and restores the previous code page
  afterwards, because the tool draws its status characters with UTF-8 bytes.

**Alternatives rejected.** Committing a prebuilt `fixit.exe` would make `git clone`
a download rather than a source checkout, and a checked-in binary cannot be reviewed
or diffed; releases are the right home for that and are already on the roadmap.
Downloading the 274 MB portable toolchain automatically on first run was rejected as
a silent network side effect: it is an explicit `--fixit-fetch-toolchain` flag.

**Consequence.** `git clone` → `.\fixit.ps1 <file>` is the entire setup on a machine
that already has a C++ compiler, and the flag above covers one that has none.  The
checkout may live anywhere, and the repaired file may live in any other project
folder, including a name containing spaces — verified from an unrelated working
directory with no `g++` on `PATH`.

**Known limit, recorded rather than hidden.** A file argument containing non-ASCII
characters is refused by the launcher instead of being left to crash: `main` receives
the console code page (GBK, for example), `std::filesystem` then throws `Cannot
convert character sequence: Illegal byte sequence`, and the CLI exits `3`.  Running
from the file's own folder with a relative name works, because the argument is then
ASCII, and the launcher prints those two commands.  Making non-ASCII arguments work
end to end needs UTF-8 `argv` plus UTF-8 file I/O in `CodeMap`/`Agent` — a library
change, not a launcher one.

---

## ADR-031 — A compile failure with zero diagnostics has to explain itself

**Context.** When the compiler exits non-zero without emitting a single diagnostic, the
CLI used to print `✗ 0 errors` and the loop ended with `Not fixed`.  On Windows this
happens whenever MSYS2's `gcc.exe` starts but cannot find `as.exe`, because it resolves
its own sub-tools through `PATH` rather than through its executable location.  A user hit
this and reasonably concluded the program was broken: both lines are individually true
and jointly tell nobody anything, and no diagnostic is shown because the compiler never
got as far as parsing the file.

**Decision.** When a compile is not clean and `error_count() == 0`, the CLI now prints the
exit status, a hint naming the likely cause (`as.exe`/`ld.exe` not runnable), and the
captured `raw_output` whenever there is any.  The three places that print a compile
result -- compile-only mode, a repair round, and the terminal verification -- share one
helper, so they cannot drift apart.  The documentation is corrected accordingly:
`--compiler <full path>` is **not** sufficient on its own for MSYS2, because the
compiler's `bin` directory still has to be on `PATH`; `tools\windows\fixit-env.ps1` plus
the launcher do that automatically (ADR-030).

**Rationale.** The exit code contract already distinguishes "not repaired" (`1`) from
"could not run the compiler" (`127`), but that distinction is invisible in the transcript
a user actually reads.  A message that names the cause converts a dead end into a
one-line fix, which is the same principle as the structured failure report of ADR-002:
the failure case has to carry as much information as the success case.

**Consequence.** Reproduced and verified by removing the compiler from `PATH`: the old
output was `✗ 0 errors`; the new output is `✗ the compiler could not be executed`
(exit `127`) or `✗ the compiler exited 1 without reporting a diagnostic for this file`
(exit `1`), each followed by the hint and, when present, the compiler's own output.

---

## ADR-032 — `--no-write` scratch copies are unique, verified, and fall back

**Context.** `--no-write` copies the target into `%TEMP%\fixit-no-write-<pid>` and the
copy's `error_code` was ignored. When the copy could not be made — a read-only or full
temp folder, a lock, or a directory left behind by an interrupted run — the compiler was
handed a path that did not exist and reported `fatal error: ... No such file or
directory`. After ADR-031 the message at least explained itself, but nothing in it said
"temporary folder", and the exit code was `1` ("not repaired") rather than "could not
prepare the run".

**Decision.** The scratch copy is now created by one function that

* names the directory `fixit-no-write-<pid>-<steady-clock ticks>`, so a directory left
  behind by an interrupted or crashed run can never be reused;
* checks every filesystem call and verifies that the copy is a regular file;
* falls back to a private directory next to the source file when the system temp folder
  cannot be used (that copy is removed when the run ends, like the first one);
* reports the OS error and exits `2` when neither location works, instead of compiling a
  file that is not there.

**Rationale.** The success path stays byte-identical, which the demo transcript and the
`expected_artifacts` test depend on; only the failure path changes. This is the same rule
as ADR-002 and ADR-031: when something cannot be done, say which thing and why.

**Consequence.** Verified on this machine: a normal run is unchanged; with `TMP`/`TEMP`
pointing at an unmounted drive the run succeeds through the fallback and leaves no scratch
directory behind.

---

## ADR-033 — The file argument is resolved, not just opened

**Context.** Users describe files the way they see them (`newtest.cpp`,
`Downloads\newtest.cpp`) or paste an almost-right path (`C:\Users\Downloads\...`
instead of `C:\Users\<name>\Downloads\...`).  The CLI answered `is not a readable
file` with exit `2` — correct, but it forces the caller to build an absolute path by
hand, which is exactly the environment knowledge a tool should not require.

**Decision.** Before validating the argument, the CLI resolves it once:

* strip one layer of surrounding quotes, expand a leading `~` and `%VARS%`;
* use the literal path when it exists;
* otherwise try the path, and then its file name, inside `%USERPROFILE%`/`$HOME` and
  their `Downloads`, `Desktop`, `Documents`, `OneDrive\Desktop`, `OneDrive\Documents`;
* exactly one match → print `fixit: using '…' (resolved from '…')` on stdout and use it;
* several matches → print the candidates and fail with exit `2`, because repairing the
  wrong file silently would be worse than an error;
* no match → keep the existing `is not a readable file` message.

**Rationale.** This is the difference between "the tool is broken" and "type the file
name".  The guess is always announced and ambiguity is refused, so convenience never
turns into a silent wrong-file repair.  The resolved absolute path is computed once and
used by everything downstream, so the compiler, `CodeMap` and the patch tools cannot
disagree about which file is open.

**Consequence.** Verified: `newtest.cpp`, `Downloads\newtest.cpp`, `~/Downloads/newtest.cpp`
and even the wrong-folder `C:\Users\Downloads\newtest.cpp` all repair
`C:\Users\garyw\Downloads\newtest.cpp` (exit `0`), each printing the resolution except
the exact path; a name present in two folders lists both and exits `2`.

---

## ADR-034 — The scratch copy has a ladder of writable homes

**Context.** On a machine with Windows' Controlled Folder Access (or an antivirus)
active, `--no-write` failed twice in a row: the copy under `%TEMP%` was not usable, and
the fallback into the source folder (`%USERPROFILE%\Downloads`) was refused with
`Permission denied`.  FixIt reported both attempts and exited `2` — honest, but still no
repair.

**Decision.** The copy is attempted, in order, in: the system temp directory;
`%LOCALAPPDATA%\fixit` (or `$XDG_CACHE_HOME`/`~/.cache/fixit`); the directory of the
running executable; the source file's directory; the current directory.  Every attempt is
unique per run, verified to be a regular file, and removed afterwards; a failure lists
every path it tried with the OS reason.

**Rationale.** The program's own directory is writable *by definition* (the tool was built
there), which makes it the reliable last resort on a locked-down machine — and it is
placed before the source folder on purpose, because the user's Downloads folder is exactly
what Controlled Folder Access protects.  The success path stays byte-identical (demo
transcript and `expected_artifacts`).

**Consequence.** Verified: with `TEMP` pointing at an unmounted drive the run completes
through the program's directory and leaves no scratch folder behind.  The error now names
every path attempted, which turns "permission denied" into "allow this program in Windows
Security".

---

## ADR-035 — `fixit` is installed as a command, not by copying files

**Context.** Users want to `cd` into a project and type `fixit.cmd main.cpp`.  The launcher
lives in the checkout, so that only works if the checkout is on `PATH`; the docs previously
made people type the full launcher path or edit `PATH` by hand.

**Decision.** `tools\windows\install-command.ps1` adds a **user** `PATH` entry pointing at
the checkout — no files are copied, so `git pull` keeps the command current.  When the
registry cannot be written (locked-down or sandboxed session) it falls back to a small
`fixit.cmd` shim in `%LOCALAPPDATA%\Microsoft\WindowsApps`, a folder Windows already has
on `PATH`.  `-Remove` undoes either form.

**Rationale.** A copied launcher would go stale; a `PATH` entry cannot.  The shim is the
fallback rather than the default because a file buried in `WindowsApps` is harder to find
and remove than a `PATH` entry, while still keeping the feature usable where the registry
is restricted.  Nothing is installed system-wide and no project folder is touched.

**Consequence.** Verified in a sandbox where both the registry and `WindowsApps` are
read-only: the script degrades to the manual instruction instead of failing halfway.  With
the checkout on `PATH`, `cd %USERPROFILE%\Downloads; fixit.cmd newtest.cpp ...` repairs the
file (exit `0`) from a folder that contains neither the launcher nor the repository — the
case that was asked for.

---

## ADR-036 — One command after clone: `setup.cmd`

**Context.** A person on another machine has to (1) have a C++ compiler, (2) build with a
script whose flags they must know, and (3) edit `PATH` before `fixit` works in any folder
— three chances to give up.  The requirement was explicit: whoever clones this repository
on any computer should be able to use fixit from most folders.

**Decision.** A root `setup.cmd` (double-clickable, wrapping `setup.ps1` with
`-ExecutionPolicy Bypass`) performs the whole per-machine setup and is idempotent: locate
a compiler (via `fixit-env.ps1`'s ladder), optionally download the portable one
(`-FetchToolchain`), reuse an up-to-date build or build, register the command through
`install-command.ps1`, then verify that `fixit.cmd` resolves and print the next command.
Source timestamps decide whether a rebuild is needed, so re-running costs seconds.

**Rationale.** Everything the checkout cannot do for itself is *machine* state — a
compiler, a build, a `PATH` entry — so it belongs in one visible step instead of being
scattered through documentation.  It installs nothing system-wide, copies nothing into a
project, and is reversible (`install-command.ps1 -Remove`).  When a step is blocked (for
example a sandboxed session where the registry is read-only) it degrades to the exact
manual instruction instead of failing halfway.

**Consequence.** Verified: `setup.cmd -SkipInstall` reports `build is up to date` and exits
`0`; the full run builds or keeps the build, attempts registration, and in a read-only
sandbox prints the folder to add to `PATH` and still exits `0`.  On a normal user account
the registration lands, and `fixit.cmd <file>` works from any folder (verified separately
with the checkout on `PATH`: run from `Downloads` → exit `0`).

---

## ADR-037 — Superseding ADR-030's non-ASCII limit: paths are UTF-8 now

**Context.** ADR-030 recorded that a non-ASCII file argument had to be refused, because
`main` received the console code page rather than UTF-8.  Between then and this rebase,
upstream landed `fixit::platform` (`to_path`/`from_path`, `captured_text_to_utf8`), UTF-8
argument conversion at startup, `SetConsoleOutputCP(CP_UTF8)` and a UTF-8 filesystem
locale.  The launcher/build guards written against the old behaviour then refused paths
the tool now handles correctly.

**Decision.** Remove those guards — `Test-FixitAsciiPath`, the launcher's non-ASCII
argument refusal, and the checkout-path refusals in `setup.ps1`/`build_fixit.ps1` — and
keep one rule worth stating: the **toolchain** must live on an ASCII path, because gcc
hands its own library directory to the linker in the local code page; the checkout and
the files being repaired may contain Chinese characters.  File reads go through
`std::filesystem::path`, so upstream's UTF-8 conversion applies.

**Rationale.** A guard that rejects a supported input is worse than a bad error message:
it makes the tool look broken on the machine of exactly the user it was meant for.  The
guards existed to stop a crash that no longer exists.

**Consequence.** Verified after rebasing onto `origin/main`: a checkout under
`...\專案資料夾\FixIt` builds (exit `0`) and repairs (exit `0`), and a source file named
`測試.cpp` is repaired normally.  The remaining non-ASCII failure surface is the
toolchain's own install path, which the guides now say explicitly.






