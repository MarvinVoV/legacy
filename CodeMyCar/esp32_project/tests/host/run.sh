#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
flags=(-Wl,--gc-sections)
if [[ $(uname) == Darwin ]]; then flags=(-Wl,-dead_strip); fi
cc -std=c11 -O2 -g -fsanitize=address,undefined -ffunction-sections -fdata-sections \
  -Istubs -I../../components/uart_service/include -I../../components/pkt_codec/include \
  -I../../components/ack_manager/include \
  test_stop_barrier.c ../../components/ack_manager/src/ack_manager.c \
  "${flags[@]}" -o "$out/test_stop_barrier"
"$out/test_stop_barrier"
