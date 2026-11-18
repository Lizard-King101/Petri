#!/bin/bash
# Prints a Python interpreter that has pyserial. PETRI_PY wins; then the
# interpreter esptool runs under (which always has pyserial); then python3.
if [ -n "${PETRI_PY:-}" ]; then
    echo "$PETRI_PY"
    exit 0
fi
if esptool=$(command -v esptool 2>/dev/null) && [ -x "$(dirname "$(readlink -f "$esptool")")/python" ]; then
    echo "$(dirname "$(readlink -f "$esptool")")/python"
    exit 0
fi
if python3 -c 'import serial' 2>/dev/null; then
    echo python3
    exit 0
fi
echo "No Python with pyserial: install pyserial or set PETRI_PY" >&2
exit 1
