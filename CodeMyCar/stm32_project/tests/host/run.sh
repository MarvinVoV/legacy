#!/usr/bin/env bash
# 纯 C 运动学 host 测试（不依赖 STM32 工具链）
set -euo pipefail
DIR="$(cd "$(dirname "$0")/../.." && pwd)"
CC="${CC:-clang}"
OUT="${TMPDIR:-/tmp}/motion_kinematics_test"
"$CC" -std=c11 -Wall -Wextra -O2 \
  -I "$DIR/Application/System/Utils/Inc" \
  "$DIR/tests/host/motion_kinematics_test.c" \
  "$DIR/Application/System/Utils/Src/motion_kinematics.c" \
  -lm -o "$OUT"
"$OUT"
