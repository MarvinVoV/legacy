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

echo "== motor emergency stop =="
"$CC" -std=c11 -Wall -Wextra -O2 \
  -I "$DIR/tests/host/stubs" \
  -I "$DIR/Application/Modules/Motor/Inc" \
  -I "$DIR/Application/Services/Motor/Inc" \
  "$DIR/tests/host/motor_estop_test.c" \
  "$DIR/Application/Modules/Motor/Src/motor_driver.c" \
  "$DIR/Application/Services/Motor/Src/pid_controller.c" \
  -lm -o "${TMPDIR:-/tmp}/motor_estop_test"
"${TMPDIR:-/tmp}/motor_estop_test"

echo "== runtime safety =="
"$CC" -std=c11 -Wall -Wextra -O2 \
  -I "$DIR/Application/System/Utils/Inc" \
  "$DIR/tests/host/runtime_safety_test.c" \
  "$DIR/Application/System/Utils/Src/control_lease.c" \
  "$DIR/Application/System/Utils/Src/task_health.c" \
  -o "${TMPDIR:-/tmp}/runtime_safety_test"
"${TMPDIR:-/tmp}/runtime_safety_test"

echo "== task timing =="
"$CC" -std=c11 -Wall -Wextra -O2 \
  -I "$DIR/Application/System/Utils/Inc" \
  "$DIR/tests/host/task_timing_test.c" \
  "$DIR/Application/System/Utils/Src/task_timing.c" \
  -o "${TMPDIR:-/tmp}/task_timing_test"
"${TMPDIR:-/tmp}/task_timing_test"

echo "== encoder fault monitor =="
"$CC" -std=c11 -Wall -Wextra -O2 \
  -I "$DIR/Application/System/Utils/Inc" \
  "$DIR/tests/host/encoder_fault_monitor_test.c" \
  "$DIR/Application/System/Utils/Src/encoder_fault_monitor.c" \
  -lm -o "${TMPDIR:-/tmp}/encoder_fault_monitor_test"
"${TMPDIR:-/tmp}/encoder_fault_monitor_test"

echo "== servo service =="
"$CC" -std=c11 -Wall -Wextra -O2 \
  -I "$DIR/tests/host/stubs" \
  -I "$DIR/Application/Services/Servo/Inc" \
  -I "$DIR/Application/System/Utils/Inc" \
  "$DIR/tests/host/servo_service_test.c" \
  "$DIR/Application/Services/Servo/Src/servo_service.c" \
  -lm -o "${TMPDIR:-/tmp}/servo_service_test"
"${TMPDIR:-/tmp}/servo_service_test"

echo "== steering LUT =="
"$CC" -std=c11 -Wall -Wextra -O2 \
  -I "$DIR/Application/System/Utils/Inc" \
  "$DIR/tests/host/steering_lut_test.c" \
  "$DIR/Application/System/Utils/Src/steering_lut.c" \
  -lm -o "${TMPDIR:-/tmp}/steering_lut_test"
"${TMPDIR:-/tmp}/steering_lut_test"

echo "== heading control =="
"$CC" -std=c11 -Wall -Wextra -O2 \
  -I "$DIR/tests/host/stubs" \
  -I "$DIR/Application/Services/Ctrl/Inc" \
  "$DIR/tests/host/heading_control_test.c" \
  "$DIR/Application/Services/Ctrl/Src/heading_control.c" \
  -lm -o "${TMPDIR:-/tmp}/heading_control_test"
"${TMPDIR:-/tmp}/heading_control_test"

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

echo "== motor inhibit =="
"$CC" -std=c11 -Wall -Wextra -O2 \
  -I "$DIR/tests/host/stubs" -I "$DIR/Application/Modules/Motor/Inc" \
  -I "$DIR/Application/System/Utils/Inc" \
  "$DIR/tests/host/motor_inhibit_test.c" "$DIR/Application/Modules/Motor/Src/motor_hal.c" \
  -o "${TMPDIR:-/tmp}/motor_inhibit_test"
"${TMPDIR:-/tmp}/motor_inhibit_test"

echo "== imu service =="
"$CC" -std=c11 -Wall -Wextra -O2 \
  -I "$DIR/tests/host/stubs" -I "$DIR/Application/Services/IMU/Inc" \
  -I "$DIR/Application/Modules/IMU/ThirdParty" -I "$DIR/Application/System/Utils/Inc" \
  "$DIR/tests/host/imu_service_test.c" "$DIR/Application/Services/IMU/Src/imu_service.c" \
  -lm -o "${TMPDIR:-/tmp}/imu_service_test"
"${TMPDIR:-/tmp}/imu_service_test"

echo "== motion transaction =="
"$CC" -std=c11 -Wall -Wextra -O2 \
  -I "$DIR/tests/host/stubs" -I "$DIR/Application/Services/Ctrl/Inc" \
  -I "$DIR/Application/Services/Motor/Inc" -I "$DIR/Application/Modules/Motor/Inc" \
  -I "$DIR/Application/Services/Servo/Inc" -I "$DIR/Application/Services/IMU/Inc" \
  -I "$DIR/Application/Services/Safety/Inc" -I "$DIR/Application/System/Utils/Inc" \
  -I "$DIR/Application/System/Common/Inc" -I "$DIR/Application/System/Protocol/Inc" \
  -I "$DIR/Application/Tasks/MotionTask/Inc" -I "$DIR/Application/Tasks/ImuTask/Inc" \
  -I "$DIR/Application/Tasks/SonarTask/Inc" \
  "$DIR/tests/host/motion_transaction_test.c" \
  "$DIR/Application/Services/Ctrl/Src/motion_service.c" \
  "$DIR/Application/Services/Ctrl/Src/heading_control.c" \
  "$DIR/Application/Services/Safety/Src/safety_service.c" \
  "$DIR/Application/System/Utils/Src/motion_kinematics.c" \
  "$DIR/Application/System/Utils/Src/motion_goal.c" \
  "$DIR/Application/System/Utils/Src/steering_lut.c" \
  "$DIR/Application/System/Utils/Src/control_lease.c" \
  "$DIR/Application/System/Utils/Src/encoder_fault_monitor.c" \
  -lm -o "${TMPDIR:-/tmp}/motion_transaction_test"
"${TMPDIR:-/tmp}/motion_transaction_test"

echo "== motion task asynchronous telemetry =="
"$CC" -std=c11 -Wall -Wextra -O2 \
  -I "$DIR/tests/host/motion_task_stubs" -I "$DIR/Application/System/Protocol/Inc" \
  "$DIR/tests/host/motion_task_test.c" "$DIR/Application/System/Protocol/Src/motion_status.c" \
  -o "${TMPDIR:-/tmp}/motion_task_test"
"${TMPDIR:-/tmp}/motion_task_test"
