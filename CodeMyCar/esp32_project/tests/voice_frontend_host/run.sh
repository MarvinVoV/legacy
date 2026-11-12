#!/usr/bin/env bash
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
out="$(mktemp -d "${TMPDIR:-/tmp}/cmcar-afe-host.XXXXXX")"
trap 'rm -rf "$out"' EXIT
for denoise in 0 1; do
for aec in 0 1; do
for perf in 0 1; do
"${CC:-clang}" -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined \
 -DCONFIG_CMCAR_VOICE_DENOISE="$denoise" \
 -DCONFIG_CMCAR_VOICE_AEC="$aec" -DCONFIG_CMCAR_VOICE_AEC_HIGH_PERF="$perf" \
 -I"$here/stubs" -I"$root/tests/voice_wake_host/stubs" \
 -I"$root/components/voice_service/include" -I"$root/components/voice_service/src" "$here/test_frontend.c" \
 "$root/components/voice_service/src/voice_frontend_s3.c" \
 "$root/components/voice_service/src/voice_dtd.c" -o "$out/test"
"$out/test"
done
done
done
