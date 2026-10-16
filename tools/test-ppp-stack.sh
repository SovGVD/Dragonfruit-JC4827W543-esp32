#!/usr/bin/env bash
# Test the SDK's actual PPP server/client in memory without a device or ROM.
set -euo pipefail
project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
idf_image="${DRAGONFRUIT_IDF_IMAGE:-espressif/idf:release-v6.0}"
docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp \
    -v "$project_dir:/project:ro" -w /project "$idf_image" bash -c '
set -euo pipefail
lwip_dir="$IDF_PATH/components/lwip/lwip/src"
cc -std=c11 -O1 -g -UNDEBUG \
    -Isrc/host/ppp_test_support -Isrc/components/wifi_gateway/include \
    -I"$lwip_dir/include" \
    src/host/test_ppp_stack.c \
    "$lwip_dir"/core/*.c "$lwip_dir"/core/ipv4/*.c \
    "$lwip_dir"/netif/ethernet.c "$lwip_dir"/netif/ppp/*.c -o /tmp/test-ppp-stack
/tmp/test-ppp-stack
'
