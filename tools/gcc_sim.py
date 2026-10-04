#!/usr/bin/env python3
"""A GCC stand-in for machines that only have clang.

Usage:  ln -s "$PWD/tools/gcc_sim.py" /tmp/fakebin/g++ ; PATH=/tmp/fakebin:$PATH ctest --test-dir build

It reproduces the two GCC behaviours that FixIt actually branches on:

  * it rejects clang-only flags (`-ferror-limit`, `-fjson-diagnostics`,
    `-fdiagnostics-format=json`) exactly as g++-12 does -- passing the first one
    to GCC makes it exit before compiling anything, which reads as a clean build;
  * it emits GCC-shaped text diagnostics (`file:line:col: error: message` with a
    caret block) rather than clang's.

The compiler checks themselves come from clang, so the diagnostics are real.  It
is a development aid for catching GCC-specific breakage locally; the CI job with
real g++-12 remains the authority.  See docs/decisions.md ADR-024/ADR-025.
"""
import re, subprocess, sys

args = sys.argv[1:]
if args and args[0] in ("--version", "-dumpversion"):
    print("g++ (Ubuntu 12.4.0-2ubuntu1~24.04.1) 12.4.0")
    print("Copyright (C) 2022 Free Software Foundation, Inc.")
    print("This is free software; see the source for copying conditions.")
    sys.exit(0)

for a in args:
    if a.startswith("-ferror-limit=") or a in ("-fjson-diagnostics", "-fdiagnostics-format=json"):
        sys.stderr.write(f"g++: error: unrecognized command-line option '{a}'\n")
        sys.exit(1)

src = next((a for a in args if not a.startswith("-")), None)
if src is None:
    sys.exit(0)

# 用 clang 取得語意診斷，再以 GCC 的格式重寫（絕不轉印 clang 的字樣）
r = subprocess.run(["clang++", "-std=c++20", "-fsyntax-only", "-fno-caret-diagnostics",
                    "-fno-diagnostics-fixit-info", src], capture_output=True, text=True)
lines = (r.stdout + r.stderr).splitlines()
pat = re.compile(r"^(.+?):(\d+):(\d+): (error|warning|note): (.*)$")
out, seen = [], []
for ln in lines:
    m = pat.match(ln)
    if not m:
        continue
    f, line, col, kind, msg = m.groups()
    if kind == "note":
        continue
    seen.append((f, line, col, kind, msg))
if seen:
    body = []
    for f, line, col, kind, msg in seen:
        body.append(f"{f}:{line}:{col}: {kind}: {msg}")
        try:
            src_line = open(f, encoding="utf-8", errors="replace").read().split("\n")[int(line) - 1]
            body.append(f"{int(line):>5} | {src_line}")
            body.append("      | " + " " * (int(col) - 1) + "^")
        except Exception:
            pass
    sys.stderr.write("\n".join(body) + "\n")
sys.exit(1 if seen else 0)
