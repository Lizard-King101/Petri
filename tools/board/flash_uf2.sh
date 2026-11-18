#!/bin/bash
# Copies a UF2 to the MatrixPortal M4 and waits for it to come back.
#
#   tools/board/flash_uf2.sh FILE.uf2 [--in-bootloader]
#
# Without --in-bootloader it first sends the 1200-baud touch that restarts the
# board into its UF2 bootloader (the MATRIXBOOT drive). Restarting the SAMD51
# also resets the ESP32, so a running world resumes from its newest checkpoint.
# PETRI_PORT overrides the serial port (default /dev/ttyACM0).
set -euo pipefail
uf2=${1:?usage: flash_uf2.sh FILE.uf2 [--in-bootloader]}
port=${PETRI_PORT:-/dev/ttyACM0}
here=$(dirname "$(readlink -f "$0")")
py=$("$here/python.sh")

if [ "${2:-}" != --in-bootloader ]; then
    "$py" -I -c "import serial, sys, time
s = serial.Serial(sys.argv[1], 1200)
s.dtr = False
time.sleep(0.2)
s.close()" "$port"
fi

dev=
for _ in $(seq 1 20); do
    dev=$(lsblk -rno NAME,LABEL | awk '$2=="MATRIXBOOT"{print "/dev/"$1}')
    [ -n "$dev" ] && break
    sleep 1
done
[ -n "$dev" ] || { echo "MATRIXBOOT did not appear" >&2; exit 1; }
sleep 1
mount=$(findmnt -rn -S "$dev" -o TARGET || true)
if [ -z "$mount" ]; then
    udisksctl mount -b "$dev" >/dev/null
    mount=$(findmnt -rn -S "$dev" -o TARGET)
fi
grep -q 'SAMD51J19A-MatrixPortal' "$mount/INFO_UF2.TXT" || { echo "MATRIXBOOT is not a MatrixPortal M4" >&2; exit 1; }
cp "$uf2" "$mount/" && sync

# Wait for the new firmware's USB serial port.
for _ in $(seq 1 20); do
    lsusb | grep -q '239a:80c9' && [ -w "$port" ] && exit 0
    sleep 1
done
echo "The board did not come back on $port" >&2
exit 1
