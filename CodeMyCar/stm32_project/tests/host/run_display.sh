#!/usr/bin/env bash
# Optional argument: directory for actual-renderer PPM previews.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
clang -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined \
  -I "$ROOT/Application/Modules/Display/Inc" \
  "$ROOT/tests/host/display_face_test.c" \
  "$ROOT/Application/Modules/Display/Src/display_face.c" -lm -o "$OUT/display"
if [[ $# -gt 0 ]]; then mkdir -p "$1"; fi
"$OUT/display" "$@"
clang -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined \
  -I "$ROOT/tests/host/display_stubs" -I "$ROOT/Application/Modules/Display/Inc" \
  -I "$ROOT/Application/System/Common/Inc" \
  "$ROOT/tests/host/display_transfer_test.c" \
  "$ROOT/Application/Modules/Display/Src/display.c" -o "$OUT/transfer"
"$OUT/transfer"
