#!/usr/bin/env bash
# Build the JC4827W543 firmware and flash it, optionally opening the serial monitor.
# Usage: ./build-flash.sh [-p PORT] [-b BAUD] [-m] [-n]
#   -p PORT  serial port (default /dev/ttyACM0, or $PORT)
#   -b BAUD  flash baud (default 460800)
#   -m       open serial monitor after flashing (Ctrl+] to exit)
#   -n       skip build, flash existing image only
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"

port="${PORT:-/dev/ttyACM0}"
baud=460800
monitor=0
build=1
while getopts "p:b:mnh" opt; do
    case "$opt" in
        p) port="$OPTARG" ;;
        b) baud="$OPTARG" ;;
        m) monitor=1 ;;
        n) build=0 ;;
        *) sed -n '2,7p' "$0"; exit 1 ;;
    esac
done

image="${DRAGONFRUIT_IDF_IMAGE:-espressif/idf:release-v6.0}"
bin=build-jc4827w543/dragonfruit-jc4827w543.bin

if [ "$build" = 1 ]; then
    echo "==> Building"
    ./tools/build-jc4827w543.sh
fi

[ -f "$bin" ] || { echo "Missing $bin - build first." >&2; exit 1; }
[ -e "$port" ] || { echo "Serial port $port not found." >&2; exit 1; }

echo "==> Flashing $bin to $port"
if python3 -c "import esptool" 2>/dev/null; then
    python3 -m esptool --chip esp32s3 --port "$port" --baud "$baud" write-flash 0x0 "$bin"
else
    docker run --rm --device="$port" -v "$PWD:/project" -w /project "$image" \
        python -m esptool --chip esp32s3 --port "$port" --baud "$baud" write-flash 0x0 "$bin"
fi

if [ "$monitor" = 1 ]; then
    echo "==> Monitor on $port (Ctrl+] to exit)"
    docker run --rm -it --device="$port" -v "$PWD:/project" -w /project "$image" \
        idf.py -B build-jc4827w543 -p "$port" monitor
fi

