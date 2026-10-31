#!/usr/bin/env bash
# PID + 运动学 host 仿真/测试
set -euo pipefail
DIR="$(cd "$(dirname "$0")/../.." && pwd)"
CC="${CC:-clang}"

echo "== motion kinematics =="
"$CC" -std=c11 -Wall -Wextra -O2 \
  -I "$DIR/Application/System/Utils/Inc" \
  "$DIR/tests/host/motion_kinematics_test.c" \
  "$DIR/Application/System/Utils/Src/motion_kinematics.c" \
  -lm -o "${TMPDIR:-/tmp}/motion_kinematics_test"
"${TMPDIR:-/tmp}/motion_kinematics_test"

echo "== motion goal =="
"$CC" -std=c11 -Wall -Wextra -O2 \
  -I "$DIR/Application/System/Utils/Inc" \
  "$DIR/tests/host/motion_goal_test.c" \
  "$DIR/Application/System/Utils/Src/motion_goal.c" \
  "$DIR/Application/System/Utils/Src/motion_kinematics.c" \
  -lm -o "${TMPDIR:-/tmp}/motion_goal_test"
"${TMPDIR:-/tmp}/motion_goal_test"

echo "== seq dedup =="
"$CC" -std=c11 -Wall -Wextra -O2 \
  -I "$DIR/Application/System/Utils/Inc" \
  "$DIR/tests/host/seq_dedup_test.c" \
  "$DIR/Application/System/Utils/Src/seq_dedup.c" \
  -lm -o "${TMPDIR:-/tmp}/seq_dedup_test"
"${TMPDIR:-/tmp}/seq_dedup_test"

echo "== PID simulation =="
"$CC" -std=c11 -Wall -Wextra -O2 \
  -I "$DIR/tests/host/stubs" \
  -I "$DIR/Application/Services/Motor/Inc" \
  "$DIR/tests/host/pid_sim_test.c" \
  "$DIR/Application/Services/Motor/Src/pid_controller.c" \
  -lm -o "${TMPDIR:-/tmp}/pid_sim_test"
"${TMPDIR:-/tmp}/pid_sim_test"

echo "== ultrasonic safety =="
"$CC" -std=c11 -Wall -Wextra -O2 \
  -I "$DIR/Application/Services/Safety/Inc" \
  "$DIR/tests/host/ultrasonic_safety_test.c" \
  "$DIR/Application/Services/Safety/Src/safety_service.c" \
  -lm -o "${TMPDIR:-/tmp}/ultrasonic_safety_test"
"${TMPDIR:-/tmp}/ultrasonic_safety_test"
