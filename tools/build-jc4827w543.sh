#!/usr/bin/env bash
# ESP-IDF 6 build for the JC4827W543 resistive (XPT2046) module.
set -euo pipefail
project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
idf_image="${DRAGONFRUIT_IDF_IMAGE:-espressif/idf:release-v6.0}"
run_idf() {
    docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp -e IDF_CCACHE_ENABLE=0 \
        -e GIT_CONFIG_COUNT=1 -e GIT_CONFIG_KEY_0=safe.directory \
        -e GIT_CONFIG_VALUE_0=/opt/esp/idf/components/openthread/openthread \
        -v "$project_dir:/project" -w /project "$idf_image" "$@"
}
run_idf idf.py -B build-jc4827w543 \
    -D IDF_TARGET=esp32s3 -D SDKCONFIG=sdkconfig.jc4827w543 \
    -D SDKCONFIG_DEFAULTS=sdkconfig.defaults.jc4827w543 build
run_idf python -m esptool --chip esp32s3 merge-bin \
    --flash-mode dio --flash-size 4MB --flash-freq 80m \
    -o build-jc4827w543/dragonfruit-jc4827w543.bin \
    0x0 build-jc4827w543/bootloader/bootloader.bin \
    0x8000 build-jc4827w543/partition_table/partition-table.bin \
    0x10000 build-jc4827w543/dragonfruit.bin
