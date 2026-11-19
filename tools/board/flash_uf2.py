"""Copies a UF2 to the MatrixPortal M4 and waits for it to come back.

    flash_uf2.py FILE.uf2 [--in-bootloader]

The portable twin of flash_uf2.sh, for hosts without lsblk and udisks
(Windows, macOS). Without --in-bootloader it first sends the 1200-baud touch
that restarts the board into its UF2 bootloader (the MATRIXBOOT drive).
Restarting the SAMD51 also resets the ESP32. PETRI_PORT names the serial port;
without it the first Adafruit port found is used. Prints the port the board
came back on, which may differ from the one it left.
"""
import os
import shutil
import string
import sys
import time

import serial
from serial.tools import list_ports

ADAFRUIT = 0x239A


def board_port():
    wanted = os.environ.get('PETRI_PORT')
    for port in list_ports.comports():
        if port.device == wanted or (not wanted and port.vid == ADAFRUIT):
            return port.device
    return None


def boot_drive():
    """The mounted MATRIXBOOT volume, identified by its INFO_UF2.TXT."""
    if os.name == 'nt':
        roots = [f'{letter}:\\' for letter in string.ascii_uppercase]
    else:
        roots = [os.path.join(base, name) for base in ('/Volumes', '/media', f'/media/{os.environ.get("USER", "")}',
                                                       f'/run/media/{os.environ.get("USER", "")}')
                 if os.path.isdir(base) for name in os.listdir(base)]
    for root in roots:
        info = os.path.join(root, 'INFO_UF2.TXT')
        try:
            with open(info) as file:
                if 'SAMD51J19A-MatrixPortal' in file.read():
                    return root
        except OSError:
            pass
    return None


def main():
    if len(sys.argv) not in (2, 3) or (len(sys.argv) == 3 and sys.argv[2] != '--in-bootloader'):
        sys.exit(__doc__)
    uf2 = sys.argv[1]
    if not os.path.isfile(uf2):
        sys.exit(f'{uf2} does not exist')
    if len(sys.argv) == 2:
        port = board_port()
        if not port:
            sys.exit('No MatrixPortal serial port found; set PETRI_PORT')
        touch = serial.Serial(port, 1200)
        touch.dtr = False
        time.sleep(0.2)
        touch.close()
    drive = None
    for _ in range(30):
        drive = boot_drive()
        if drive:
            break
        time.sleep(1)
    if not drive:
        sys.exit('MATRIXBOOT did not appear')
    time.sleep(1)
    try:
        shutil.copyfile(uf2, os.path.join(drive, os.path.basename(uf2)))
    except OSError as error:
        # The board restarts as soon as the last block arrives, which can end the copy with an error.
        print(f'copy ended with: {error}')
    # Wait for the bootloader drive to go and the new firmware's serial port to appear.
    for _ in range(30):
        time.sleep(1)
        if not boot_drive():
            port = board_port()
            if port:
                print(port)
                return 0
    sys.exit('The board did not come back')


if __name__ == '__main__':
    sys.exit(main())
