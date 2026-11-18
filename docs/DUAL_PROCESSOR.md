# Petri dual-processor architecture

**Status:** Design decision, November 18, 2026. Nothing below is implemented yet.
Values marked *verify* are unconfirmed on this board.

Petri moves its authoritative world from the SAMD51 to the MatrixPortal M4's
onboard ESP32. The SAMD51 becomes the board's display, input, storage, and USB
controller. Roadmap P7 originally gated this move on benchmarks. The project
now targets it directly because the ESP32 adds a dedicated simulation core
and Bluetooth LE for a phone controller app. Benchmarks remain required at each stage. The
SAMD51-only firmware stays buildable as a fallback.

## Hardware facts

| | SAMD51J19 | ESP32 coprocessor |
| --- | --- | --- |
| CPU | 1× Cortex-M4F, 120 MHz, DSP MAC instructions | 2× Xtensa LX6, up to 240 MHz |
| RAM | 192 KiB | 520 KiB on chip; roughly 250 KiB usable with Wi-Fi, fragmented across regions (*verify*) |
| Flash | 512 KiB internal + 2 MiB QSPI | Module flash, likely 4 MiB (*verify*) |
| PSRAM | None | None expected on a WROOM-32-class module (*verify*) |
| Other | USB, HUB75 matrix, UP/DOWN buttons, LIS3DH at I2C 0x19, NeoPixel | Wi-Fi, Bluetooth |

Board links between the chips: SPI with `ESP_CS` (D33), `ESP_BUSY` (D31),
`ESP_RESET` (D30). The UART and GPIO0 lines that Adafruit's passthrough
firmware uses are also wired. The ESP32 has no USB port of its own, so every
ESP32 flash goes through the SAMD51.

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
stock coprocessor firmware uses. The ESP32 drives `ESP_BUSY` to say when a
transaction is ready.
ESP-IDF documents a 10 MHz ceiling for SPI slave mode on the classic ESP32
(*verify*), which still leaves far more than the needed bandwidth.

- Every packet carries a magic number, version, type, sequence number, length,
  and a CRC-32 checksum. A receiver drops bad packets and resynchronizes on
  the next magic number.
- Packet types: frame, camera and input command, telemetry, checkpoint chunk
  and acknowledgment, history record, and hello and reset notices.
- The packet bodies reuse the existing `PTR4` observation and `PanelInput`
  formats where possible. The same command and telemetry messages travel
  over USB, BLE, and the link.
- If either chip resets, the other notices the missing hello or heartbeat
  and returns to a known state. The SAMD51 keeps showing the last frame and
  sets a status color on the NeoPixel until the ESP32 returns.
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

1. **Baseline.** Commit the current tree. Reinstall `arduino-cli` with the
   Adafruit SAMD core, and install `esptool` and ESP-IDF 5.x.
2. **ESP32 backup.** Flash Adafruit's passthrough UF2 to the SAMD51; this step
   can be reversed. Record `chip_id` and `flash_id`, read the full ESP32
   flash into `backups/`, and store its SHA-256. Restoring means writing that
   image back at offset 0. Reflash Petri afterward.
3. **Passthrough in Petri firmware.** Holding a button at boot bridges USB to
   the ESP32 UART and drives GPIO0 and RESET. This removes the UF2 swap from
   every ESP32 flash.
4. **Core port.** Build `core/` and `render/` with ESP-IDF. Match the native
   fingerprint on the device, and measure ticks per second, free heap, and the
   largest free block with no radio and with NimBLE advertising.
5. **Link.** Bring up SPI master and slave with the handshake, checksums,
   and reset recovery. Measure throughput and error rate under load.
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
