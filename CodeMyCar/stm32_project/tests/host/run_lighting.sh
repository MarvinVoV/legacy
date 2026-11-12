#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
clang -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
  -I "$ROOT/Application/Modules/LedStrip/Inc" -I "$ROOT/Application/Services/Lighting/Inc" \
  "$ROOT/tests/host/ws2812_test.c" \
  "$ROOT/Application/Modules/LedStrip/Src/ws2812_encoder.c" \
  "$ROOT/Application/Services/Lighting/Src/lighting_service.c" -o "$OUT/lighting"
"$OUT/lighting"
