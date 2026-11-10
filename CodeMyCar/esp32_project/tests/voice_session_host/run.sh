#!/usr/bin/env bash
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
out="$(mktemp -d "${TMPDIR:-/tmp}/cmcar-session-host.XXXXXX")"
trap 'rm -rf "$out"' EXIT
"${CC:-clang}" -std=c11 -Wall -Wextra -O1 -g -fsanitize=address,undefined \
 -Wno-deprecated-declarations -I"$root/components/voice_service/src" \
 -I"$root/managed_components/espressif__cjson/cJSON" "$here/test_session.c" \
 "$root/components/voice_service/src/voice_protocol.c" \
 "$root/components/voice_service/src/voice_state.c" \
 "$root/managed_components/espressif__cjson/cJSON/cJSON.c" -o "$out/test"
"$out/test"
