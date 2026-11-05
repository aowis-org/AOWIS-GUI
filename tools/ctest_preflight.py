#!/usr/bin/env python3
"""Concise preflight for the AOWIS-labelled CTest suite.

Use JSON instead of `ctest -N`: textual discovery prints dozens of search
paths for each unbuilt test executable, obscuring the actual problem.
"""

import json
from pathlib import Path
import subprocess
import sys


def check_registration(document: dict) -> tuple[int, list[str]]:
    tests = document.get("tests", [])
    if not isinstance(tests, list):
        raise ValueError("CTest JSON has no usable tests array")
    # CTest leaves 'command' out when it could not resolve the test executable.
    missing = [test.get("name", "<unnamed>") for test in tests
               if not test.get("command")]
    return len(tests), missing


def main() -> int:
    if len(sys.argv) != 2:
        print("Usage: ctest_preflight.py BUILD_DIRECTORY", file=sys.stderr)
        return 2

    build_dir = Path(sys.argv[1])
    completed = subprocess.run(
        ["ctest", "--test-dir", str(build_dir), "--show-only=json-v1", "-L", "^aowis$"],
        text=True, capture_output=True, check=False,
    )
    if completed.returncode != 0:
        print("ERROR: Could not inspect registered AOWIS tests:", file=sys.stderr)
        print(completed.stderr.strip() or completed.stdout.strip(), file=sys.stderr)
        return 1
    try:
        count, missing = check_registration(json.loads(completed.stdout))
    except (ValueError, TypeError) as exc:
        print(f"ERROR: Invalid CTest registration data: {exc}", file=sys.stderr)
        return 1

    if count == 0:
        print("ERROR: No AOWIS tests registered in this build.", file=sys.stderr)
        return 1
    if missing:
        print(f"ERROR: {len(missing)} of {count} AOWIS tests have no built executable:",
              file=sys.stderr)
        for name in missing:
            print(f"  - {name}", file=sys.stderr)
        print(f"Build them with: cmake --build '{build_dir}' --target aowis-tests",
              file=sys.stderr)
        return 1
    print(f"AOWIS tests: {count} registered; all executables available.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
