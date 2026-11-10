#!/usr/bin/env bash
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
out="$(mktemp -d "${TMPDIR:-/tmp}/cmcar-wake-host.XXXXXX")"
trap 'rm -rf "$out"' EXIT
"${CC:-clang}" -std=c11 -Wall -Wextra -O2 -I"$here/stubs" \
 -I"$root/components/voice_service/include" "$here/test_voice_wake.c" \
 "$root/components/voice_service/src/voice_wake.c" -o "$out/test"
"$out/test"
