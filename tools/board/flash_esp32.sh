#!/bin/bash
# Flashes an ESP-IDF build onto the board's ESP32 through the SAMD51, then
# puts a SAMD51 firmware back.
#
#   tools/board/flash_esp32.sh BUILD_DIR [AFTER.uf2]
#
# BUILD_DIR is an ESP-IDF build directory under esp32/, for example
# build-world (the world app) or build (the self-test). The script uses
# ESP-IDF from the calling shell if it is loaded, or else loads it from
# $IDF_PATH or ~/esp/esp-idf-v5.5.6; if ESP-IDF was installed with a Python
# other than the default, set PETRI_IDF_PYTHON_DIR to the directory holding
# that python3. It then flashes the standalone
# USB-to-UART bridge (build/petri-esp32-passthrough.uf2, built if missing),
# flashes the ESP32 at 460800 baud, and finally flashes AFTER.uf2 if given.
set -euo pipefail
build=${1:?usage: flash_esp32.sh BUILD_DIR [AFTER.uf2]}
after=${2:-}
repo=$(cd "$(dirname "$(readlink -f "$0")")/../.." && pwd)
here="$repo/tools/board"
port=${PETRI_PORT:-/dev/ttyACM0}
idf=${IDF_PATH:-$HOME/esp/esp-idf-v5.5.6}
bridge="$repo/build/petri-esp32-passthrough.uf2"

if [ ! -f "$bridge" ]; then
    uf2conv=${UF2CONV:-$HOME/.local/share/uf2/uf2conv.py}
    arduino-cli compile --fqbn adafruit:samd:adafruit_matrixportal_m4:usbstack=tinyusb \
        --output-dir "$repo/build/esp32-passthrough" "$repo/firmware/esp32_passthrough"
    python3 "$uf2conv" -c -b 0x4000 -f SAMD51 -o "$bridge" \
        "$repo/build/esp32-passthrough/esp32_passthrough.ino.bin"
fi

"$here/flash_uf2.sh" "$bridge"
sleep 2
(
    cd "$repo/esp32"
    # Use ESP-IDF if the shell already loaded it; otherwise load it here.
    if ! command -v idf.py >/dev/null; then
        # ESP-IDF needs the Python it was installed with first on PATH.
        [ -n "${PETRI_IDF_PYTHON_DIR:-}" ] && PATH="$PETRI_IDF_PYTHON_DIR:$PATH"
        # shellcheck disable=SC1091
        . "$idf/export.sh" >/dev/null
    fi
    idf.py -B "$build" -DSDKCONFIG="$build/sdkconfig" -p "$port" -b 460800 flash
)
if [ -n "$after" ]; then
    "$here/flash_uf2.sh" "$after"
fi
