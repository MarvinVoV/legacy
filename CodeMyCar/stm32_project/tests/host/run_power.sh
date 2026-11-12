#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
"${CC:-clang}" -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
  -I "$ROOT/Application/Modules/Power/Inc" -I "$ROOT/Application/Services/Power/Inc" \
  "$ROOT/tests/host/power_test.c" "$ROOT/Application/Modules/Power/Src/ina219.c" \
  "$ROOT/Application/Services/Power/Src/battery_service.c" -o "$OUT/power_test"
"$OUT/power_test"
for SHUNT in 0 100; do
"${CC:-clang}" -DCMCAR_POWER_SHUNT_MILLIOHM="$SHUNT" -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
  -I "$ROOT/tests/host/power_stubs" -I "$ROOT/Application/System/Common/Inc" \
  -I "$ROOT/Application/Tasks/PowerTask/Inc" -I "$ROOT/Application/Modules/Power/Inc" \
  -I "$ROOT/Application/Services/Power/Inc" \
  "$ROOT/tests/host/power_task_test.c" "$ROOT/Application/Tasks/PowerTask/Src/power_task.c" \
  "$ROOT/Application/Modules/Power/Src/ina219.c" \
  "$ROOT/Application/Services/Power/Src/battery_service.c" -o "$OUT/power_task_test"
"$OUT/power_task_test"
done
