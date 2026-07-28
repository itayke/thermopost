#!/usr/bin/env bash
# Convenience wrapper: configure (first time only), build, run.
# Use `./run.sh "[hysteresis]"` to filter by tag — anything passed through here
# reaches the test binary as a Catch2 argument.

set -euo pipefail
cd "$(dirname "$0")"

if [[ ! -d build ]]; then
    cmake -S . -B build -GNinja >/dev/null
fi
cmake --build build >/dev/null
./build/test_alert "$@"
