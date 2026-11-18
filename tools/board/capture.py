"""Records the ESP32's output through the SAMD51 bridge, for example the self-test.

    capture.py OUTPUT.txt SECONDS [--no-reset]

Requires a bridge on the SAMD51 (build/petri-esp32-passthrough.uf2, which
tools/board/flash_esp32.sh leaves there). Unless --no-reset is given it first
restarts the ESP32 into its firmware through the bridge's RTS line. Every line
goes to OUTPUT.txt; lines starting with BOOT, HEAP, FP, TIMING, RENDER, BRAIN,
SELFTEST, DONE, or BLE, and crash reports, are also printed. It stops at DONE,
or a few seconds after a crash, or after SECONDS. PETRI_PORT overrides the
serial port (default /dev/ttyACM0).
"""
import os
import sys
import time

import serial

SHOWN = ('BOOT', 'HEAP', 'FP', 'TIMING', 'RENDER', 'BRAIN', 'SELFTEST', 'DONE', 'BLE', 'Guru', 'abort', 'assert')


def main():
    if len(sys.argv) not in (3, 4):
        sys.exit(__doc__)
    output, limit = sys.argv[1], float(sys.argv[2])
    port = serial.Serial(os.environ.get('PETRI_PORT', '/dev/ttyACM0'), 115200, timeout=0.5)
    if '--no-reset' not in sys.argv[3:]:
        port.dtr = False
        port.rts = True
        time.sleep(0.1)
        port.rts = False
    start = time.time()
    buffer = b''
    with open(output, 'w') as log:
        while time.time() - start < limit:
            buffer += port.read(4096)
            *lines, buffer = buffer.split(b'\n')
            for raw in lines:
                line = raw.decode(errors='replace').rstrip('\r')
                log.write(line + '\n')
                log.flush()
                if line.startswith(SHOWN):
                    print(f'{time.time() - start:6.1f}s {line}', flush=True)
                if line.startswith('DONE'):
                    return 0
                if 'Guru Meditation' in line:
                    limit = min(limit, time.time() - start + 3)
    print('stopped without DONE')
    return 1


if __name__ == '__main__':
    sys.exit(main())
