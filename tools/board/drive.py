"""Sends USB commands to the display firmware and prints its status lines.

    drive.py SCRIPT            # steps from a file
    drive.py -e "wait 8; send >>>; label 8tps; wait 10"

Steps, one per line or separated by ';':
    send CHARS   write CHARS to the board ('>' speeds up, '|' pauses,
                 'W' toggles rain, 'S' saves now, 'C' back-to-back saves,
                 'X' corrupts the newest checkpoint slot)
    wait SECONDS print DISPLAY, STREAM, and STORE lines for that long
    label TEXT   prefix later lines with TEXT

Each printed line starts with the current label. PETRI_PORT overrides the
serial port (default /dev/ttyACM0).
"""
import os
import sys
import time

import serial

KINDS = ('DISPLAY', 'STREAM', 'STORE')


def main():
    if len(sys.argv) == 3 and sys.argv[1] == '-e':
        steps = [step.strip() for step in sys.argv[2].replace(';', '\n').splitlines()]
    elif len(sys.argv) == 2:
        with open(sys.argv[1]) as file:
            steps = [step.strip() for step in file.read().splitlines()]
    else:
        sys.exit(__doc__)
    port = serial.Serial(os.environ.get('PETRI_PORT', '/dev/ttyACM0'), 115200, timeout=0.1)
    buffer = b''
    label = 'start'

    def pump(seconds):
        nonlocal buffer
        end = time.time() + seconds
        while time.time() < end:
            buffer += port.read(4096)
            *lines, buffer = buffer.split(b'\n')
            for raw in lines:
                line = raw.decode(errors='replace').strip()
                if line.startswith(KINDS):
                    print(f'{label:>10} {line}', flush=True)

    for step in steps:
        verb, _, argument = step.partition(' ')
        if verb == 'send':
            port.write(argument.encode())
            label = argument or label
        elif verb == 'wait':
            pump(float(argument))
        elif verb == 'label':
            label = argument
        elif verb:
            sys.exit(f'unknown step: {step}')


if __name__ == '__main__':
    main()
