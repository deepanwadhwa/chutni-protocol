#!/usr/bin/env python3
"""Check the vendored BLAKE3 against the official test_vectors.json.

The store is content-addressed, so a wrong hash is not a wrong number: it is
every object in every store silently unreachable. This runs the upstream
vectors, including the extended 131-byte output, in all three modes.
"""
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
VECTORS = os.path.join(HERE, "conformance", "blake3_test_vectors.json")


def main() -> int:
    if len(sys.argv) < 2:
        print("usage: run_blake3_vectors.py <blake3_vectors binary>", file=sys.stderr)
        return 2
    binary = sys.argv[1]

    with open(VECTORS) as handle:
        cases = json.load(handle)["cases"]

    failures = 0
    checks = 0
    for case in cases:
        length = case["input_len"]
        result = subprocess.run([binary, str(length)], capture_output=True, text=True)
        if result.returncode != 0:
            print(f"FAIL len={length}: binary exited {result.returncode}")
            failures += 1
            continue
        got = result.stdout.split()
        for index, mode in enumerate(("hash", "keyed_hash", "derive_key")):
            checks += 1
            if got[index] != case[mode]:
                print(f"FAIL len={length} {mode}")
                print(f"  expected {case[mode][:64]}...")
                print(f"  got      {got[index][:64]}...")
                failures += 1

    print(f"BLAKE3: {len(cases)} vectors x 3 modes = {checks} checks, {failures} failures")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
