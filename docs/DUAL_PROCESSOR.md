# Petri dual-processor architecture

**Status:** Design decision, November 18, 2026. Stages 1 through 5 are done;
the rest is not implemented yet. Values marked *verify* are unconfirmed on
this board.

Petri moves its authoritative world from the SAMD51 to the MatrixPortal M4's
onboard ESP32. The SAMD51 becomes the board's display, input, storage, and USB
controller. Roadmap P7 originally gated this move on benchmarks. The project
now targets it directly because the ESP32 adds a dedicated simulation core
and Bluetooth LE for a phone controller app. Benchmarks remain required at each stage. The
SAMD51-only firmware stays buildable as a fallback.

## Hardware facts

| | SAMD51J19 | ESP32 coprocessor |
| --- | --- | --- |
| Chip | ATSAMD51J19A | ESP32-D0WD-V3, revision v3.0, 40 MHz crystal (read with esptool) |
| CPU | 1× Cortex-M4F, 120 MHz, DSP MAC instructions | 2× Xtensa LX6, up to 240 MHz |
| RAM | 192 KiB | 520 KiB on chip. Measured: 217,824 bytes of byte-addressable heap free at boot without a radio, or 121,788 with NimBLE advertising. The largest block was 110,592 bytes. Static DRAM has 180,736 bytes without Bluetooth and 124,580 with it |
| Flash | 512 KiB internal + 2 MiB QSPI | 4 MiB, 3.3 V (read with esptool) |
| PSRAM | None | The D0WD-V3 has none built in. esptool cannot detect external PSRAM, which WROOM modules lack |
| Shipped firmware | UF2 bootloader 3.16.0 | Adafruit nina-fw 1.2.2, backed up privately in `backups/` |
| Other | USB, HUB75 matrix, UP/DOWN buttons, LIS3DH at I2C 0x19, NeoPixel | Wi-Fi, Bluetooth |

Board links between the chips, from the Adafruit SAMD 1.7.17 variant:

| Signal | SAMD51 pin | Variant name |
| --- | --- | --- |
| SPI SCK, MISO, MOSI | 34, 35, 36 | `SPIWIFI` (`SPI`) |
| SPI chip select | 33 | `SPIWIFI_SS` |
| Busy/ready handshake | 31 | `SPIWIFI_ACK` |
| ESP32 reset | 30 | `ESP32_RESETN` |
| ESP32 GPIO0 (boot mode) | 29 | `ESP32_GPIO0` |
| ESP32 UART RX, TX | 27, 28 | `SerialESP32` (`Serial2`) |

The ESP32 has no USB port of its own, so every ESP32 flash goes through the
SAMD51. `firmware/esp32_passthrough/esp32_bridge.hpp` bridges USB to
`SerialESP32`. The host's DTR and RTS lines drive the ESP32's GPIO0 and reset
lines like a development board's auto-reset circuit. The bridge holds reset
for 10 ms after the lines release it, because hosts change DTR and RTS in
separate steps. It also writes to the TinyUSB buffer directly, because
`Serial.write()` discards data while DTR is off. With these changes,
esptool's default reset and hard reset work. Reads matched the backup at
921600 baud (668 kbit/s) through the standalone bridge and at 460800
(348 kbit/s) through Petri's bridge mode. There, panel refresh likely
explains one byte lost at 921600. An earlier bridge without reset lines
connected only at 115200 and read 4 MiB in 372 seconds.

The ESP32 does not add usable RAM to the SAMD51. The two chips have separate
memories, and a single world cannot be split across them efficiently.
Each chip gets one coherent job.

## Ownership

```text
                ESP32 core 1                      ESP32 core 0
          ┌──────────────────────┐   ┌──────────────────────────────────┐
          │ World::step()        │──▶│ Panel render (existing panel.cpp)│
          │ simulation only;     │   │ link task (SPI slave, DMA)       │
          │ no flash, no radio   │   │ BLE control service (NimBLE)     │
          └──────────────────────┘   │ checkpoint serializer            │
                                     └───────────────┬──────────────────┘
                                                     │ SPI + BUSY handshake
                    SAMD51 ┌─────────────────────────▼──────────────────┐
                           │ Protomatter scanout (double buffered)      │
                           │ buttons, LIS3DH, NeoPixel                  │
                           │ QSPI: rotating checkpoints, history log    │
                           │ USB: companion relay, ESP32 passthrough    │
                           └────────────────────────────────────────────┘
```

- **ESP32 core 1** runs only the simulation. It never touches flash or the
  radio, so Bluetooth traffic and flash stalls cannot change the biology.
- **ESP32 core 0** runs the existing `Panel` renderer from `const World&` and
  the step observer. ESP-IDF runs the radio stack on core 0 by default.
  Native rendering costs about 0.05 ms per frame, so rendering at 20 fps is a
  small share of this core.
- **SAMD51** displays finished 64×32 RGB565 frames. It sends button,
  accelerometer, and camera commands, and it stores checkpoints. Rendering on
  the ESP32 keeps `panel.cpp` unchanged and avoids mirroring world state on
  the SAMD51. A 4 KiB frame at 20 fps is 80 KiB/s on the link.
- **Checkpoints live on the SAMD51 QSPI flash**, so the living world survives
  ESP32 reflashing. ESP32 flash writes would also stall both ESP32 cores.
  The plan uses rotating slots with the existing FNV-1a checksum and keeps the
  last known good slot. Four slots of a roughly 90 KiB wide checkpoint use
  about 360 KiB, leaving most of the 2 MiB for a bounded event and lineage log.
  At boot, the ESP32 asks the SAMD51 for the newest valid slot.
- **One radio: Bluetooth LE.** Wi-Fi is never initialized, and the memory
  reserved for classic Bluetooth is released at boot. The NimBLE stack is
  used because it is smaller than Bluedroid. The real RAM cost is measured
  in stage 4.

## Link protocol

The SAMD51 is the SPI master and the ESP32 is the slave, the same roles the
stock coprocessor firmware uses. `link/link.hpp` defines the frames. Stage 5
measured the link on the board (see the README's SPI link test).

| Signal | SAMD51 | ESP32 |
| --- | --- | --- |
| SCK | 34 (`SPIWIFI`) | GPIO18 |
| MOSI | 36 | GPIO14 |
| MISO | 35 | GPIO23 |
| Chip select | 33 (`SPIWIFI_SS`) | GPIO5 |
| Ready (active low) | 31 (`SPIWIFI_ACK`), input | GPIO33, output |
| ESP32 reset | 30 (`ESP32_RESETN`), output | EN |

The ESP32 pins come from Adafruit nina-fw `boards/esp32/board.h`. Its GPIO15
and GPIO21 connect to the SAMD51's SWD pins and must stay inputs. MOSI and
MISO pass through the ESP32 GPIO matrix rather than IO_MUX, which limits MISO
timing.

- **Handshake.** The ESP32 queues one fixed 4,112-byte transaction (16-byte
  header and up to 4,096 payload bytes) and drives ready low from the SPI
  driver's post-setup callback. The SAMD51 waits for ready, selects the ESP32,
  and clocks the whole frame with DMA. A chip-select interrupt on the ESP32
  drives ready high again. Every transaction moves one frame in each
  direction.
- **Frames.** Each carries a magic number, version, type, sequence number,
  acknowledged sequence, length, flags, and a CRC-32 over the header and
  payload. A sequence that restarts at 0 marks a restarted peer.
- **Fixed settings.** SPI mode 0 at 8 MHz. Mode 1, which ESP-IDF recommends
  with DMA, failed above 6 MHz. Mode 0 failed at 12 MHz, the next step the
  SAMD51 can produce. Both sides use these values from reset. A hardware
  reset clears the ESP32's RTC memory, so no link setting may depend on
  remembered ESP32 state.
- **Bandwidth.** 176 frames/s, 721 KB/s of payload each way. A 4 KiB panel
  frame at 20 frames/s uses about 11% of the link.
- **Recovery.** After an ESP32 hardware reset or software restart, frames
  resume in 291 ms or 310 ms. The SAMD51 keeps showing the last frame and
  sets a status color on the NeoPixel until frames return.
- **The ESP32 does not survive a SAMD51 reset.** The board pulls the ESP32
  reset line low whenever the SAMD51 is not driving it. A released pin read 0
  and the ESP32 rebooted. That happens on every SAMD51 reset, bootloader
  entry, and UF2 flash. The world's only continuity across a SAMD51 restart is
  therefore its latest checkpoint. The SAMD51 firmware should drive reset high
  early, and it should request a checkpoint before any planned restart or
  update.
- Packet types still to add: frame, camera and input command, telemetry,
  checkpoint chunk and acknowledgment, history record, and hello notices.
  Where possible, their bodies reuse the `PTR4` observation and `PanelInput`
  formats, so the same messages travel over USB, BLE, and the link.
- The UART carries boot messages and debugging output, and it remains the
  ESP32 flashing path.

## Determinism

The core stays integer-only. The acceptance test runs the same seed and
command sequence natively and on the ESP32 and compares
`World::fingerprint()` at fixed ticks. Biology may never read a wall clock, a
radio event, or a link timing. Commands that change the world, such as pause,
speed, or a future accelerometer disturbance, enter at tick boundaries and
are logged so a run can be replayed.

## Phone and desktop access

The panel is the only display. The phone is a controller and data viewer,
and it never receives frames.

- A dedicated phone app connects as a BLE central. It offers a touch pad for
  camera panning, pinch to zoom, follow and view selection, pause and speed
  controls, and simulation data: population, births and deaths, lineages,
  trait distributions, and an inspector for the followed creature.
- Camera gestures become the existing camera command. Each one is small and
  sent at a limited rate, so BLE bandwidth is not a concern.
- Data comes from compact telemetry and inspector records, not the `PTR4`
  frame. Long histories come from the SAMD51 QSPI log, paged over the link on
  request, so the ESP32 holds no history.
- Pairing uses BLE bonding with a passkey shown on the panel. Read-only
  telemetry can be offered before bonding. Controls require a bonded phone.
- The desktop companion stays on USB and keeps its mirror view.
- ESP32 firmware updates use USB passthrough. BLE updates are possible later
  but slow, and they are not planned.

## Bring-up stages

Each stage ends with something measured on hardware or checked by a test.

1. **Baseline.** *Done.* The tree is committed. Installed tools are
   arduino-cli 1.5.1, Adafruit SAMD 1.7.17, Protomatter 1.7.1, TinyUSB 3.7.7,
   esptool 5.6.0, and Microsoft's `uf2conv.py`. ESP-IDF is still to install.
2. **ESP32 backup.** *Done.* Two full 4 MiB reads were byte-identical. The
   image, checksum, and restore steps are in the ignored `backups/` directory.
   The restore procedure has not been run. Petri firmware is back on the board.
3. **Passthrough in Petri firmware.** *Done.* Holding UP at boot bridges USB
   to the ESP32 UART and maps DTR and RTS to GPIO0 and reset. Tested on the
   board with esptool's default reset and 1 MiB reads at 460800 baud. This
   removes the UF2 swap from every ESP32 flash.
4. **Core port.** *Done.* `esp32/` builds the core and renderer with ESP-IDF
   v5.5.6 and runs `esp32/selftest` on core 1. All 16 fingerprints matched
   native, with and without NimBLE. The same self-test also matched on the
   SAMD51. The README records the timing and heap tables. Findings:
   - The observatory world runs at about 242 ticks/s on one ESP32 core and
     103 ticks/s on the SAMD51, a 2.35× gain per core. That is 3× the
     panel's 80 ticks/s maximum. Running the core from IRAM did not help.
     The workload is compute-bound, split between full-grid environment
     passes and creature updates.
   - NimBLE advertising costs about 96 KB of byte-addressable heap and 47 KB
     of IRAM heap. With it, about 105 KB of heap remained beside the static
     72.7 KB world. That is close to the SAMD51's budget today, so the
     dual-processor gain is CPU time and BLE, not memory. Tuning NimBLE
     buffers is worth trying before the BLE stage.
   - `World::load_checkpoint` decodes into a full `World` copy on the stack.
     That will overflow an ESP32 task stack, so persistence work must decode
     in place.
   - The ESP32 now holds the self-test app instead of nina-fw. The Petri
     firmware does not use the ESP32 yet.
5. **Link.** *Done.* `link/` holds the frame format and its tests. The link
   test apps are `esp32` (`sdkconfig.defaults.link`) and `firmware/linktest`.
   In a 3-minute soak at 8 MHz in mode 0, 31,639 frames crossed with no
   errors in either direction while the observatory world ran on core 1 at
   207–268 ticks/s. Recovery and the reset-line finding are under Link
   protocol.
6. **Cutover.** Make the ESP32 world authoritative, display streamed frames,
   save checkpoints to QSPI, and relay companion USB traffic. Run a
   multi-hour soak and a power-cut test.
7. **BLE.** Add the GATT control and telemetry service, bonding, and the
   phone app.

World compaction is done (November 18, 2026): the wide `World` is
72,664 bytes, down from 109,008. See the README's memory layout section.
After cutover, the ecology roadmap uses the ESP32 core 1 budget: evolvable
fixed-topology int8 neural controllers, costed body traits, plant genetics,
dormant eggs, and compatibility-based speciation.

## Options kept open

- **Second island world on the SAMD51.** Its freed RAM and CPU could run a
  separate population with rare migration over the link. Geographic
  isolation of this kind is a classic driver of speciation. Consider this
  only after cutover is stable.
- **Simulation on both ESP32 cores.** Environment cells could update on core 0
  while creatures update on core 1, with double buffering to keep the result
  deterministic. Attempt this only if core 1 becomes the bottleneck.
