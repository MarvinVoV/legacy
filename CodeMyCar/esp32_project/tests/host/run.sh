#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
flags=(-Wl,--gc-sections)
if [[ $(uname) == Darwin ]]; then flags=(-Wl,-dead_strip); fi
stop_barrier() {
  cc -std=c11 -O2 -g -fsanitize=address,undefined -ffunction-sections -fdata-sections \
    -Istubs -I../../components/uart_service/include -I../../components/pkt_codec/include \
    -I../../components/ack_manager/include \
    test_stop_barrier.c ../../components/ack_manager/src/ack_manager.c \
    "${flags[@]}" -o "$out/test_stop_barrier"
  "$out/test_stop_barrier"
}

motion_json() {
  cc -std=c11 -O2 -g -fsanitize=address,undefined \
    -Istubs -I../../components/bridge/include -I../../components/pkt_codec/include \
    -I../../managed_components/espressif__cjson/cJSON \
    test_motion_json.c ../../components/bridge/src/bridge_codec.c \
    ../../components/pkt_codec/src/motion_status.c \
    ../../managed_components/espressif__cjson/cJSON/cJSON.c -o "$out/test_motion_json"
  "$out/test_motion_json"
}

if [[ $# == 0 ]]; then set -- stop_barrier motion_json network; fi
for name in "$@"; do
  case $name in
    stop_barrier|motion_json) "$name" ;;
    network) bash ./run_network.sh ;;
    *) echo "Expected stop_barrier, motion_json or network, got: $name" >&2; exit 2 ;;
  esac
done
