#!/usr/bin/env python3
"""Summarises a FixIt trace into examples/buggy/expected/<name>.trajectory.json.

Used by tools/gen_expected.sh.  Reads MOCK_TRACE / EXIT_CODE / COMPILER /
EXPECTED from the environment.

The summary records, per round, what the model was looking at when it chose its
tools (`errors_before`) and what the compiler said once they ran
(`errors_after`).  Those two numbers are the trajectory a reviewer checks.
"""

import json
import os


def error_count(payload):
    """Number of error diagnostics in a serialised CompileResult, or None."""
    if not isinstance(payload, dict):
        return None
    errors = payload.get("errors")
    return len(errors) if isinstance(errors, list) else None


def main():
    with open(os.environ["MOCK_TRACE"]) as handle:
        trace = json.load(handle)

    rounds = []
    for entry in trace:
        rounds.append(
            {
                "round": entry.get("round"),
                "tool_calls": [call.get("name") for call in entry.get("tool_calls", [])],
                "errors_before": error_count(entry.get("compile_before")),
                "errors_after": error_count(entry.get("compile_after")),
            }
        )

    exit_code = int(os.environ["EXIT_CODE"])
    payload = {
        "compiler": os.environ["COMPILER"],
        "exit_code": exit_code,
        "success": exit_code == 0,
        "iterations": len(trace),
        "initial_errors": rounds[0]["errors_before"] if rounds else None,
        "final_errors": rounds[-1]["errors_after"] if rounds else None,
        "rounds": rounds,
    }

    with open(os.environ["EXPECTED"], "w") as handle:
        json.dump(payload, handle, indent=2)
        handle.write("\n")

    print(
        "  {}: iterations={} success={} initial_errors={}".format(
            os.path.basename(os.environ["EXPECTED"]),
            payload["iterations"],
            payload["success"],
            payload["initial_errors"],
        )
    )


if __name__ == "__main__":
    main()
