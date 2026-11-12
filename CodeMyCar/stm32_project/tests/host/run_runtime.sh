#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
clang -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
  -I "$ROOT/Application/System/Utils/Inc" \
  "$ROOT/tests/host/cycle_clock_test.c" \
  "$ROOT/Application/System/Utils/Src/cycle_clock.c" -o "$OUT/runtime"
"$OUT/runtime"
clang -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
  -I "$ROOT/tests/host/runtime_stubs" -I "$ROOT/Application/Modules/IMU/Inc" \
  "$ROOT/tests/host/imu_i2c_transport_test.c" \
  "$ROOT/Application/Modules/IMU/Src/imu_i2c_transport.c" -o "$OUT/i2c"
"$OUT/i2c"
clang -std=c11 -Wall -Wextra -Werror -Wno-sign-compare -g -fsanitize=address,undefined \
  -I "$ROOT/tests/host/log_stubs" -I "$ROOT/Application/Modules/Log/Inc" \
  -I "$ROOT/Application/System/Protocol/Inc" -I "$ROOT/Application/Tasks/LogTask/Inc" \
  "$ROOT/tests/host/log_dispatch_test.c" \
  "$ROOT/Application/Modules/Log/Src/log_manager.c" -o "$OUT/log"
"$OUT/log"
