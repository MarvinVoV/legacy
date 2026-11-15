#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
build_dir=$(mktemp -d)
trap 'rm -rf "$build_dir"' EXIT

cmake -S "$root" -B "$build_dir" -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build "$build_dir" --parallel
ctest --test-dir "$build_dir" --output-on-failure --no-tests=error
