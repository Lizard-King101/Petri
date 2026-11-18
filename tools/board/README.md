# Board tools

Scripts for flashing and driving the MatrixPortal M4 from a Linux host. The
toolchain is described in the main README: arduino-cli with the Adafruit
SAMD core and Protomatter, ESP-IDF v5.5.6, esptool, and `uf2conv.py`. The
serial port needs the udev rule from the README's ESP32 bridge section.

All scripts default to `/dev/ttyACM0`; set `PETRI_PORT` to use another.
`flash_esp32.sh` uses ESP-IDF if the shell has already loaded it (with
`export.sh`); otherwise it loads it from `IDF_PATH` or `~/esp/esp-idf-v5.5.6`.
If ESP-IDF was installed with a Python other than the default `python3`, set
`PETRI_IDF_PYTHON_DIR` to the directory holding that interpreter. The
Python scripts need pyserial. `python.sh` finds an interpreter with it: from
`PETRI_PY`, then the one esptool runs under, then `python3`.

| Script | Does |
| --- | --- |
| `flash_uf2.sh FILE.uf2 [--in-bootloader]` | Restarts the SAMD51 into MATRIXBOOT (1200-baud touch), copies the UF2, and waits for the port |
| `flash_esp32.sh BUILD_DIR [AFTER.uf2]` | Flashes an ESP-IDF build in `esp32/` through the USB-to-UART bridge, then optionally a SAMD51 firmware |
| `drive.py SCRIPT` or `drive.py -e "STEPS"` | Sends display commands and prints status lines |
| `capture.py OUT.txt SECONDS [--no-reset]` | Records ESP32 output through the bridge until `DONE` (the self-test) |
| `soak.py SECONDS LOG.txt` | Runs the display at 8 ticks/s and summarizes its health |

## Typical runs

```sh
py=$(tools/board/python.sh)
```

Rebuild and flash the whole dual-processor firmware: the ESP32 world app,
then the SAMD51 display.

```sh
(cd esp32 && . ~/esp/esp-idf-v5.5.6/export.sh && idf.py -B build-world -DSDKCONFIG=build-world/sdkconfig build)
firmware/build_petri_uf2.sh ~/.local/share/uf2/uf2conv.py build/petri-display.uf2 --display
tools/board/flash_esp32.sh build-world build/petri-display.uf2
```

The world resumes from the newest checkpoint in QSPI, which the display
saves every 10 minutes. A different world configuration in new firmware
applies only after a fresh start. To start fresh, corrupt every slot and
restart:

```sh
$py tools/board/drive.py -e "send X; wait 2; send X; wait 2; send X; wait 2; send X; wait 2"
tools/board/flash_uf2.sh build/petri-display.uf2
```

Check the display after a change: boot, run at 8 ticks/s, save once.

```sh
$py tools/board/drive.py -e "label boot; wait 8; send >>>; label 8tps; wait 10; send S; label saved; wait 6"
```

Run the ESP32 self-test and compare its fingerprints with native:

```sh
(cd esp32 && . ~/esp/esp-idf-v5.5.6/export.sh && idf.py -B build -DSDKCONFIG=build/sdkconfig build)
tools/board/flash_esp32.sh build
$py tools/board/capture.py esp32-selftest.txt 600
g++ -std=c++17 -O2 -DPETRI_WIDE_WORLD=1 -Icore -Irender -Iesp32/selftest esp32/selftest/*.cpp core/*.cpp render/*.cpp -o build/selftest-native
build/selftest-native | grep '^FP' | diff - <(grep '^FP' esp32-selftest.txt) && echo match
```

The self-test leaves the bridge on the SAMD51. Flash the world app and the
display again afterwards.

`drive.py` and `soak.py` read the display's status lines. Those rotate once
a second through `DISPLAY` (tick, population, fps, fingerprint, draw time),
`STREAM` (updates, errors, ESP32 heap), and `STORE` (checkpoint saves and
restores).
