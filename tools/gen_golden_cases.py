#!/usr/bin/env python3
"""Generates tests/patch_golden_cases.inc from a deterministic template.

60 positive golden patches (10 exact, 10 line drift, 10 whitespace, 10 missing
context, 10 extra context, 10 multi-hunk) and 10 negative cases.  The generated
file is committed, so the test suite itself never needs Python.

Run from the repo root:  python3 tools/gen_golden_cases.py
"""

import os

# The base file every case patches.  Lines are 1-based in all comments.
BASE = [
    "#include <cstdio>",                             # 1
    "#include <string>",                             # 2
    "",                                              # 3
    "namespace demo {",                              # 4
    "",                                              # 5
    "int helper_a(int value) {",                     # 6
    "  int total_a = value;",                        # 7
    "  return total_a;",                             # 8
    "}",                                             # 9
    "",                                              # 10
    "int helper_b(int value) {",                     # 11
    "  int total_b = value * 3;",                    # 12
    "  return total_b + 1;",                         # 13
    "}",                                             # 14
    "",                                              # 15
    "std::string name_of(int id) {",                 # 16
    '  if (id == 1) return "one";',                  # 17
    '  if (id == 2) return "two";',                  # 18
    '  return "many";',                              # 19
    "}",                                             # 20
    "",                                              # 21
    "int run(int id) {",                             # 22
    "  const int value = id * 2;",                   # 23
    "  return helper_a(value) + helper_b(value);",   # 24
    "}",                                             # 25
    "",                                              # 26
    "}  // namespace demo",                          # 27
]


def render(lines):
    return "\n".join(lines) + "\n"


def cpp(text):
    return text.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n")


def make_case(name, old_lines, new_lines, hunk, content=None, extra_target=None,
              expected_content=None):
    """Builds one golden case.

    hunk: the literal unified-diff body (context/± lines), without the @@ header.
    extra_target: (old_tuple, new_tuple, hunk) for a second hunk in the same diff.
    """
    source = list(BASE) if content is None else list(content)
    expected = list(expected_content) if expected_content is not None else list(source)
    if expected_content is None:
        index = expected.index(old_lines[0])
        expected[index:index + len(old_lines)] = new_lines

    diff = "--- a/golden.cpp\n+++ b/golden.cpp\n"
    diff += hunk
    if extra_target is not None:
        diff += extra_target[2]
        second_old, second_new = extra_target[0], extra_target[1]
        index = expected.index(second_old[0])
        expected[index:index + len(second_old)] = second_new

    return {
        "name": name,
        "content": render(source),
        "diff": diff,
        "expected": render(expected),
    }


POSITIVE = []

# ---------------------------------------------------------------------------
# 1. 10 exact patches: declared position is right, context is complete.
# ---------------------------------------------------------------------------
for i in range(10):
    old = [BASE[6]]                      # line 7: "  int total_a = value;"
    new = [f"  int total = value + {i};"]
    pos = 7
    body = (f" {BASE[5]}\n"              # line 6 context
            f"-{old[0]}\n"
            f"+{new[0]}\n"
            f" {BASE[7]}\n")             # line 8 context
    POSITIVE.append(make_case(
        f"exact_{i}", old, new,
        f"@@ -{pos},3 +{pos},3 @@\n{body}"))

# ---------------------------------------------------------------------------
# 2. 10 line-drift patches: same edit, but @@ declares a wrong position.
# ---------------------------------------------------------------------------
for i in range(10):
    old = [BASE[6]]
    new = [f"  int drifted_{i} = {i};"]
    offset = (i % 5) + 1                 # +1 .. +5 off
    declared = 7 + offset
    body = (f" {BASE[5]}\n-{old[0]}\n+{new[0]}\n {BASE[7]}\n")
    POSITIVE.append(make_case(
        f"drift_{i}", old, new,
        f"@@ -{declared},3 +{declared},3 @@\n{body}"))

# ---------------------------------------------------------------------------
# 3. 10 whitespace-difference patches: trailing spaces differ on every line.
# ---------------------------------------------------------------------------
for i in range(10):
    old = [BASE[6]]
    new = [f"  int spaced_{i} = {i};"]
    pos = 7
    # Every diff line carries one extra trailing space compared with the file
    # (a stripped copy is the realistic failure mode); the engine must still
    # match and must still replace the right line.
    body = (f" {BASE[5]} \n"
            f"-{old[0]} \n"
            f"+{new[0]} \n"
            f" {BASE[7]} \n")
    expected_source = list(BASE)
    expected_source[6:7] = [new[0] + " "]
    POSITIVE.append(make_case(
        f"whitespace_{i}", old, new,
        f"@@ -{pos},3 +{pos},3 @@\n{body}", expected_content=expected_source))

# ---------------------------------------------------------------------------
# 4. 10 missing-context patches: the hunk only quotes the changed line.
# ---------------------------------------------------------------------------
for i in range(10):
    old = [BASE[6]]
    new = [f"  int ctx_{i} = {i};"]
    pos = 7
    body = f"-{old[0]}\n+{new[0]}\n"
    POSITIVE.append(make_case(
        f"missing_context_{i}", old, new,
        f"@@ -{pos},1 +{pos},1 @@\n{body}"))

# ---------------------------------------------------------------------------
# 5. 10 extra-context patches: the file has trailing whitespace everywhere
#    that the patch does not mention.
# ---------------------------------------------------------------------------
for i in range(10):
    # Every line in the file ends with blanks the patch does not mention: a
    # realistic copy/paste artefact from a model that stripped whitespace.
    padded = [ln + "   " if ln.strip() else ln for ln in BASE]
    old = [padded[6]]
    new = [f"  int extra_{i} = {i};"]
    pos = 7
    body = (f" {BASE[5]}\n-{BASE[6]}\n+{new[0]}\n {BASE[7]}\n")
    POSITIVE.append(make_case(
        f"extra_context_{i}", old, new,
        f"@@ -{pos},3 +{pos},3 @@\n{body}", content=padded))

# ---------------------------------------------------------------------------
# 6. 10 multi-hunk patches: two edits in one diff, second one drifted too.
# ---------------------------------------------------------------------------
for i in range(10):
    first_old = [BASE[6]]
    first_new = [f"  int first_{i} = {i};"]
    first_pos = 7
    first_body = (f" {BASE[5]}\n-{first_old[0]}\n+{first_new[0]}\n {BASE[7]}\n")

    second_old = [BASE[12]]              # line 13: "  return total_b + 1;"
    second_new = [f"  return total_b + {i + 2};"]
    second_pos = 13 + (i % 3)            # declared 0..2 lines off
    second_body = (f" {BASE[11]}\n-{second_old[0]}\n+{second_new[0]}\n {BASE[13]}\n")

    first_hunk = f"@@ -{first_pos},3 +{first_pos},3 @@\n{first_body}"
    second_hunk = f"@@ -{second_pos},3 +{second_pos},3 @@\n{second_body}"
    POSITIVE.append(make_case(
        f"multi_hunk_{i}", first_old, first_new, first_hunk,
        extra_target=(second_old, second_new, second_hunk)))


# ---------------------------------------------------------------------------
# Negative cases: the quoted context exists nowhere in the file.
# ---------------------------------------------------------------------------
NEGATIVES = []
for i in range(4):
    NEGATIVES.append((f"missing_symbol_{i}", 7,
                      [f"  int never_declared_{i} = 0;"],
                      "never_declared"))
for i in range(3):
    NEGATIVES.append((f"wrong_function_{i}", 12,
                      [f'  std::string wrong_type_{i} = "x";'],
                      "wrong_type"))
for i in range(3):
    NEGATIVES.append((f"nonexistent_region_{i}", 23,
                      [f"  very_different_{i}(alpha, beta, gamma);"],
                      "very_different"))


def emit_positive():
    chunks = []
    for case in POSITIVE:
        chunks.append(f"""  // -- {case['name']}
  {{
    const std::string content = "{cpp(case['content'])}";
    const std::string diff = "{cpp(case['diff'])}";
    const std::string expected = "{cpp(case['expected'])}";
    const fixit::PatchResult result = engine.apply(content, diff, "golden.cpp");
    INFO("case " << case_index << ": " << result.failure_summary("golden.cpp"));
    CHECK(result.all_applied);
    CHECK(result.new_content == expected);
    ++case_index;
  }}""")
    return "\n".join(chunks)


def emit_negative():
    chunks = []
    file_content = render(BASE)
    for name, anchor, body_lines, marker in NEGATIVES:
        body = "".join(" " + line + "\n" for line in body_lines)
        diff = (f"--- a/golden.cpp\n+++ b/golden.cpp\n"
                f"@@ -{anchor},{len(body_lines)} +{anchor},{len(body_lines)} @@\n{body}")
        chunks.append(f"""  // -- negative / {name}
  {{
    const std::string content = "{cpp(file_content)}";
    const std::string diff = "{cpp(diff)}";
    const fixit::PatchResult result = engine.apply(content, diff, "golden.cpp");
    CHECK_FALSE(result.all_applied);
    REQUIRE_FALSE(result.reports.empty());
    const fixit::HunkReport& report = result.reports.front();
    CHECK(report.status == fixit::HunkReport::Status::Failed);
    // The structured failure must name the score, the gate and the window.
    CHECK(report.failure_reason.find("score") != std::string::npos);
    CHECK(report.failure_reason.find("gate") != std::string::npos);
    CHECK(report.failure_reason.find("Window tried") != std::string::npos);
    // ... and quote the offending line by name.
    CHECK(report.failure_reason.find("{marker}") != std::string::npos);
    // At most the three best candidates are reported.
    CHECK(report.top_candidates.size() <= 3);
    // A failed hunk never touches the file.
    CHECK(result.new_content == content);
  }}""")
    return "\n".join(chunks)


HEADER = """// GENERATED by tools/gen_golden_cases.py -- do not edit by hand.
//
// 60 positive golden patches (10 exact, 10 drift, 10 whitespace, 10 missing
// context, 10 extra context, 10 multi-hunk) and 10 negative cases.  Each case
// states the input file, the (deliberately imperfect) unified diff a model
// would produce, and the exact expected file afterwards.
"""


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    target = os.path.join(here, "..", "tests", "patch_golden_cases.inc")
    with open(target, "w") as handle:
        handle.write(HEADER)
        handle.write("\n// ---------------------------------------------------------------\n")
        handle.write("// Positive cases\n")
        handle.write("// ---------------------------------------------------------------\n")
        handle.write(emit_positive())
        handle.write("\n\n// ---------------------------------------------------------------\n")
        handle.write("// Negative cases\n")
        handle.write("// ---------------------------------------------------------------\n")
        handle.write(emit_negative())
        handle.write("\n")
    print(f"wrote {os.path.normpath(target)}: {len(POSITIVE)} positive, {len(NEGATIVES)} negative")


if __name__ == "__main__":
    main()
