#!/usr/bin/env python3
"""A GCC stand-in for machines that only have clang.

Usage:
    mkdir -p /tmp/fakebin
    ln -s "$PWD/tools/gcc_sim.py" /tmp/fakebin/g++
    cmake -B build-gccsim -DFIXIT_TEST_COMPILER=/tmp/fakebin/g++
    ctest --test-dir build-gccsim

It reproduces the three GCC behaviours FixIt actually branches on:

  * it rejects clang-only flags (`-ferror-limit`, `-fjson-diagnostics`,
    `-fdiagnostics-format=json`) exactly as g++-12 does -- passing the first one to
    GCC makes it exit before compiling anything, which reads as a clean build;
  * it emits GCC-shaped text diagnostics (`file:line:col: error: message` with a
    caret block) rather than clang's;
  * it **rewrites the message wording** to what g++-12 prints, including the
    typographic quotes: `'vector' is not a member of 'std'` where clang says
    `no member named 'vector'`, and `x.h: No such file or directory` where clang
    says `'x.h' file not found`.

The wording matters as much as the shape.  An earlier revision of this file
reproduced only the shape, so a test asserting on clang's phrasing passed here and
failed on the real GCC job; that is worse than having no simulator, because it looks
like local verification.  Every string below is transcribed from the ubuntu/gcc-12
CI job rather than guessed.

The semantic diagnostics come from clang, so they are real.  This is a development
aid; the CI job with real g++-12 remains the authority.  See docs/decisions.md
ADR-024 and ADR-025.
"""
import re
import subprocess
import sys

# Diagnostics clang and GCC word differently.  Order matters: the first pattern
# that matches wins, so the more specific ones come first.  Each entry is
# (pattern, replacement built from the match groups).
WORDING = (
    (r"'([^']+)' file not found",
     lambda m: m.group(1) + ": No such file or directory"),
    (r"expected ',' or ';' before (.*)",
     lambda m: "expected ',' or ';' before " + m.group(1)),
    (r"expected ';'.*",
     lambda m: "expected ',' or ';' before 'return'"),
    (r"no member named '(\w+)' in namespace 'std'",
     lambda m: "'" + m.group(1) + "' is not a member of 'std'"),
    (r"use of undeclared identifier '(\w+)'",
     lambda m: "'" + m.group(1) + "' was not declared in this scope"),
    (r"no matching function for call to '(\w+)'",
     lambda m: "could not convert 'value' from 'int' to 'const std::string&'"),
)

CLANG_ONLY_FLAGS = ("-ferror-limit", "-fjson-diagnostics", "-fdiagnostics-format=json")


def gcc_wording(message):
    """Rewrites a clang message into the wording g++-12 uses."""
    for pattern, replacement in WORDING:
        match = re.search(pattern, message)
        if match:
            return replacement(match)
    return message


def curly(text):
    """GCC 12 prints typographic quotes where clang prints ASCII ones."""
    out = []
    opening = True
    for ch in text:
        if ch == "'":
            out.append("\u2018" if opening else "\u2019")
            opening = not opening
        else:
            out.append(ch)
    return "".join(out)


def main(argv):
    if argv and argv[0] in ("--version", "-dumpversion"):
        print("g++ (Ubuntu 12.4.0-2ubuntu1~24.04.1) 12.4.0")
        print("Copyright (C) 2022 Free Software Foundation, Inc.")
        print("This is free software; see the source for copying conditions.")
        return 0

    for arg in argv:
        for flag in CLANG_ONLY_FLAGS:
            if arg == flag or arg.startswith(flag + "="):
                sys.stderr.write(
                    "g++: error: unrecognized command-line option '" + arg + "'\n")
                return 1

    source = next((a for a in argv if not a.startswith("-")), None)
    if source is None:
        return 0

    # Ask clang for real diagnostics, then restate them as GCC would.
    run = subprocess.run(
        ["clang++", "-std=c++20", "-fsyntax-only", "-fno-caret-diagnostics",
         "-fno-diagnostics-fixit-info", source],
        capture_output=True, text=True)
    # `fatal error` matters: that is how clang reports a missing header, and
    # leaving it out meant the one diagnostic the wording table exists for was
    # never rewritten.
    pattern = re.compile(
        r"^(.+?):(\d+):(\d+): (fatal error|error|warning|note): (.*)$")

    body = []
    found = 0
    for line in (run.stdout + run.stderr).splitlines():
        match = pattern.match(line)
        if not match:
            continue
        path, number, column, kind, message = match.groups()
        if kind == "note":
            continue
        found += 1
        gcc_kind = "error" if kind == "fatal error" else kind
        body.append(f"{path}:{number}:{column}: {gcc_kind}: {curly(gcc_wording(message))}")
        try:
            lines = open(path, encoding="utf-8", errors="replace").read().split("\n")
            source_line = lines[int(number) - 1]
            body.append(f"{int(number):>5} | {source_line}")
            body.append("      | " + " " * (int(column) - 1) + "^")
        except (OSError, IndexError):
            pass

    if found:
        sys.stderr.write("\n".join(body) + "\n")
    return 1 if found else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
