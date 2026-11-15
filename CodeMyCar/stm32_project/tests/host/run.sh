#!/usr/bin/env bash
set -euo pipefail
DIR="$(cd "$(dirname "$0")/../.." && pwd)"
CC="${CC:-clang}"
control_cases=(
  motion_kinematics motion_goal seq_dedup motor_estop
  runtime_safety task_timing encoder_fault_monitor servo_service
  steering_lut heading_control pid_sim ultrasonic_safety
  motor_inhibit imu_service motion_transaction straight_path
  motion_task steering_pipeline motor_pid_tracking heading_transient
)
all_cases=(power lighting display "${control_cases[@]}" runtime)
if [[ ${1:-} == --list ]]; then
  printf '%s\n' "${all_cases[@]}"
  exit 0
fi
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT

motion_kinematics() {
  "$CC" -std=c11 -Wall -Wextra -O2 \
    -I "$DIR/Application/System/Utils/Inc" \
    "$DIR/tests/host/motion_kinematics_test.c" \
    "$DIR/Application/System/Utils/Src/motion_kinematics.c" \
    -lm -o "$out/motion_kinematics_test"
  "$out/motion_kinematics_test"
}

motion_goal() {
  "$CC" -std=c11 -Wall -Wextra -O2 \
    -I "$DIR/Application/System/Utils/Inc" \
    "$DIR/tests/host/motion_goal_test.c" \
    "$DIR/Application/System/Utils/Src/motion_goal.c" \
    "$DIR/Application/System/Utils/Src/motion_kinematics.c" \
    -lm -o "$out/motion_goal_test"
  "$out/motion_goal_test"
}

seq_dedup() {
  "$CC" -std=c11 -Wall -Wextra -O2 \
    -I "$DIR/Application/System/Utils/Inc" \
    "$DIR/tests/host/seq_dedup_test.c" \
    "$DIR/Application/System/Utils/Src/seq_dedup.c" \
    -lm -o "$out/seq_dedup_test"
  "$out/seq_dedup_test"
}

motor_estop() {
  "$CC" -std=c11 -Wall -Wextra -O2 \
    -I "$DIR/tests/host/stubs" \
    -I "$DIR/Application/Modules/Motor/Inc" \
    -I "$DIR/Application/Services/Motor/Inc" \
    "$DIR/tests/host/motor_estop_test.c" \
    "$DIR/Application/Modules/Motor/Src/motor_driver.c" \
    "$DIR/Application/Services/Motor/Src/pid_controller.c" \
    -lm -o "$out/motor_estop_test"
  "$out/motor_estop_test"
}

runtime_safety() {
  "$CC" -std=c11 -Wall -Wextra -O2 \
    -I "$DIR/Application/System/Utils/Inc" \
    "$DIR/tests/host/runtime_safety_test.c" \
    "$DIR/Application/System/Utils/Src/control_lease.c" \
    "$DIR/Application/System/Utils/Src/task_health.c" \
    -o "$out/runtime_safety_test"
  "$out/runtime_safety_test"
}

task_timing() {
  "$CC" -std=c11 -Wall -Wextra -O2 \
    -I "$DIR/Application/System/Utils/Inc" \
    "$DIR/tests/host/task_timing_test.c" \
    "$DIR/Application/System/Utils/Src/task_timing.c" \
    -o "$out/task_timing_test"
  "$out/task_timing_test"
}

encoder_fault_monitor() {
  "$CC" -std=c11 -Wall -Wextra -O2 \
    -I "$DIR/Application/System/Utils/Inc" \
    "$DIR/tests/host/encoder_fault_monitor_test.c" \
    "$DIR/Application/System/Utils/Src/encoder_fault_monitor.c" \
    -lm -o "$out/encoder_fault_monitor_test"
  "$out/encoder_fault_monitor_test"
}

servo_service() {
  "$CC" -std=c11 -Wall -Wextra -O2 \
    -I "$DIR/tests/host/stubs" \
    -I "$DIR/Application/Services/Servo/Inc" \
    -I "$DIR/Application/System/Utils/Inc" \
    "$DIR/tests/host/servo_service_test.c" \
    "$DIR/Application/Services/Servo/Src/servo_service.c" \
    -lm -o "$out/servo_service_test"
  "$out/servo_service_test"
}

steering_lut() {
  "$CC" -std=c11 -Wall -Wextra -O2 \
    -I "$DIR/Application/System/Utils/Inc" \
    "$DIR/tests/host/steering_lut_test.c" \
    "$DIR/Application/System/Utils/Src/steering_lut.c" \
    -lm -o "$out/steering_lut_test"
  "$out/steering_lut_test"
}

heading_control() {
  "$CC" -std=c11 -Wall -Wextra -O2 \
    -I "$DIR/tests/host/stubs" \
    -I "$DIR/Application/Services/Ctrl/Inc" \
    "$DIR/tests/host/heading_control_test.c" \
    "$DIR/Application/Services/Ctrl/Src/heading_control.c" \
    -lm -o "$out/heading_control_test"
  "$out/heading_control_test"
}

pid_sim() {
  "$CC" -std=c11 -Wall -Wextra -O2 \
    -I "$DIR/tests/host/stubs" \
    -I "$DIR/Application/Services/Motor/Inc" \
    "$DIR/tests/host/pid_sim_test.c" \
    "$DIR/Application/Services/Motor/Src/pid_controller.c" \
    -lm -o "$out/pid_sim_test"
  "$out/pid_sim_test"
}

ultrasonic_safety() {
  "$CC" -std=c11 -Wall -Wextra -O2 \
    -I "$DIR/Application/Services/Safety/Inc" \
    "$DIR/tests/host/ultrasonic_safety_test.c" \
    "$DIR/Application/Services/Safety/Src/safety_service.c" \
    -lm -o "$out/ultrasonic_safety_test"
  "$out/ultrasonic_safety_test"
}

motor_inhibit() {
  "$CC" -std=c11 -Wall -Wextra -O2 \
    -I "$DIR/tests/host/stubs" -I "$DIR/Application/Modules/Motor/Inc" \
    -I "$DIR/Application/System/Utils/Inc" \
    "$DIR/tests/host/motor_inhibit_test.c" "$DIR/Application/Modules/Motor/Src/motor_hal.c" \
    -o "$out/motor_inhibit_test"
  "$out/motor_inhibit_test"
}

imu_service() {
  for MAG_DIAGNOSTICS in 0 1; do
    "$CC" -DCMCAR_IMU_MAG_DIAGNOSTICS="$MAG_DIAGNOSTICS" -std=c11 -Wall -Wextra -O2 \
      -I "$DIR/tests/host/stubs" -I "$DIR/Application/Services/IMU/Inc" \
      -I "$DIR/Application/Modules/IMU/ThirdParty" -I "$DIR/Application/System/Utils/Inc" \
      "$DIR/tests/host/imu_service_test.c" "$DIR/Application/Services/IMU/Src/imu_service.c" \
      -lm -o "$out/imu_service_test"
    "$out/imu_service_test"
  done
}

motion_transaction() {
  for CONFIG in baseline path_experiment trim_experiment; do
    PATH_CONTROL=0; FORWARD_TRIM=0; REVERSE_TRIM=0
    if [[ "$CONFIG" == path_experiment ]]; then PATH_CONTROL=1; fi
    if [[ "$CONFIG" == trim_experiment ]]; then FORWARD_TRIM=6; REVERSE_TRIM=14; fi
    "$CC" -std=c11 -Wall -Wextra -O2 \
      -DCMCAR_STRAIGHT_PATH_CONTROL="$PATH_CONTROL" \
      -DSTEERING_STRAIGHT_FORWARD_TRIM_US="$FORWARD_TRIM" \
      -DSTEERING_STRAIGHT_REVERSE_TRIM_US="$REVERSE_TRIM" \
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
      "$DIR/Application/System/Utils/Src/straight_path.c" \
      -lm -o "$out/motion_transaction_test"
    "$out/motion_transaction_test"
  done
}

straight_path() {
  "$CC" -std=c11 -Wall -Wextra -O2 \
    -I "$DIR/tests/host/stubs" -I "$DIR/Application/System/Utils/Inc" \
    -I "$DIR/Application/Services/Ctrl/Inc" \
    "$DIR/tests/host/straight_path_test.c" \
    "$DIR/Application/System/Utils/Src/straight_path.c" \
    "$DIR/Application/Services/Ctrl/Src/heading_control.c" \
    -lm -o "$out/straight_path_test"
  "$out/straight_path_test"
}

motion_task() {
  "$CC" -std=c11 -Wall -Wextra -O2 \
    -I "$DIR/tests/host/motion_task_stubs" -I "$DIR/Application/System/Protocol/Inc" \
    "$DIR/tests/host/motion_task_test.c" "$DIR/Application/System/Protocol/Src/motion_status.c" \
    -o "$out/motion_task_test"
  "$out/motion_task_test"
}

steering_pipeline() {
  "$CC" -std=c11 -Wall -Wextra -O2 -include stdbool.h -include string.h -include math.h \
    -I "$DIR/Application/Modules/Servo/Inc" -I "$DIR/Application/System/Utils/Inc" \
    -I "$DIR/tests/host/stubs" -I "$DIR/Application/Services/Servo/Inc" \
    "$DIR/tests/host/steering_pipeline_test.c" \
    "$DIR/Application/Services/Servo/Src/servo_service.c" \
    "$DIR/Application/Modules/Servo/Src/servo_driver.c" \
    "$DIR/Application/System/Utils/Src/steering_lut.c" \
    -lm -o "$out/steering_pipeline_test"
  "$out/steering_pipeline_test"
}

motor_pid_tracking() {
  for FINE_INTEGRATION in 0 1; do
    "$CC" -DCMCAR_MOTOR_PID_FINE_INTEGRATION="$FINE_INTEGRATION" -std=c11 -Wall -Wextra -O2 \
      -I "$DIR/tests/host/stubs" -I "$DIR/Application/Services/Motor/Inc" \
      "$DIR/tests/host/motor_pid_tracking_test.c" \
      "$DIR/Application/Services/Motor/Src/pid_controller.c" \
      -lm -o "$out/motor_pid_tracking_test"
    "$out/motor_pid_tracking_test"
  done
}

heading_transient() {
  "$CC" -std=c11 -Wall -Wextra -O2 \
    -I "$DIR/tests/host/stubs" -I "$DIR/Application/Services/Ctrl/Inc" \
    "$DIR/tests/host/heading_transient_test.c" \
    "$DIR/Application/Services/Ctrl/Src/heading_control.c" \
    -lm -o "$out/heading_transient_test"
  "$out/heading_transient_test"
}

run_case() {
  local name=$1 candidate
  for candidate in "${all_cases[@]}"; do
    if [[ $candidate == "$name" ]]; then
      echo "== $name =="
      case $name in
        power|lighting|display|runtime) bash "$DIR/tests/host/run_$name.sh" ;;
        *) "$name" ;;
      esac
      return
    fi
  done
  echo "Unknown STM32 test: $name (see --list)" >&2
  exit 2
}

if [[ $# == 0 ]]; then set -- "${all_cases[@]}"; fi
for name in "$@"; do
  if [[ $name == control ]]; then
    for test_case in "${control_cases[@]}"; do run_case "$test_case"; done
  else
    run_case "$name"
  fi
done
