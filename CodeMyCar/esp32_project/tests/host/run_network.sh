#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
for service in WIFI MQTT; do
 cc -std=c11 -O1 -g -fsanitize=address,undefined -DTEST_${service} \
  -Inetwork_stubs -I../../components/common/include \
  -I../../components/wifi_service/include -I../../components/mqtt_service/include \
  test_network_recovery.c -o "$out/test_$service"
 "$out/test_$service"
done

cc -std=c11 -O1 -g -fsanitize=address,undefined -DTEST_WIFI -DCONFIG_CMCAR_WIFI_MAX_RETRY=2 \
 -Inetwork_stubs -I../../components/common/include -I../../components/wifi_service/include \
 test_network_recovery.c -o "$out/test_wifi_finite"
"$out/test_wifi_finite"
