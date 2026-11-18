# Petri dual-processor architecture

**Status:** Design decision, November 18, 2026. Stages 1 through 7 are done (the stage 7 soak is deferred);
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

Decided November 18, 2026: the ESP32 runs the biology, and the SAMD51 is the
graphics processor. The ESP32 streams compact render state, not pixels. The
SAMD51 keeps a small mirror of that state and draws every frame itself.

```text
                ESP32 core 1                      ESP32 core 0
          ┌──────────────────────┐   ┌──────────────────────────────────┐
          │ World::step()        │──▶│ render-state encoder             │
          │ simulation only;     │   │ link task (SPI slave, DMA)       │
          │ no flash, no radio   │   │ BLE control service (NimBLE)     │
          └──────────────────────┘   │ checkpoint serializer            │
                                     │ later: share of environment work │
                                     └───────────────┬──────────────────┘
                                                     │ SPI, 8 MHz, mode 0
                    SAMD51 ┌─────────────────────────▼──────────────────┐
                           │ render mirror and decoder                  │
                           │ Panel renderer, camera, animation          │
                           │ Protomatter scanout (double buffered)      │
                           │ buttons, LIS3DH, NeoPixel                  │
                           │ QSPI: rotating checkpoints, history log    │
                           │ USB: companion relay, ESP32 passthrough    │
                           └────────────────────────────────────────────┘
```

- **ESP32 core 1** runs only the simulation. It never touches flash or the
  radio, so Bluetooth traffic and flash stalls cannot change the biology.
- **ESP32 core 0** turns the world into render state at tick boundaries and
  runs the link, BLE, and checkpoint transfers. ESP-IDF runs the radio stack
  here by default. Freed from rendering, this core can later take a
  deterministic share of the per-cell environment work.
- **The SAMD51 renders.** Without the simulation, its full CPU and about
  150 KB of RAM go to graphics. The close view takes 10.5 ms per frame there,
  so 30–60 fps is within reach before any optimization. The camera, auto
  follow, and animation timing live here. They are presentation only and
  never feed back into the biology. Buttons and camera commands still reach
  the ESP32 at tick boundaries when they change the world, such as pause and
  speed.
- **If the ESP32 stalls or restarts,** the SAMD51 keeps animating its last
  mirror.
- **Checkpoints live on the SAMD51 QSPI flash**, so the living world survives
  ESP32 reflashing. ESP32 flash writes would also stall both ESP32 cores.
  The plan uses rotating slots with the existing FNV-1a checksum and keeps the
  last known good slot. Four slots of a roughly 90 KiB wide checkpoint use
  about 360 KiB, leaving most of the 2 MiB for a bounded event and lineage log.
  Every SAMD51 reset also restarts the ESP32, so after a reset the SAMD51
  sends the newest valid slot back over the link: about 22 frames, roughly
  0.13 s.
- **One radio: Bluetooth LE.** Wi-Fi is never initialized, and the memory
  reserved for classic Bluetooth is released at boot. The NimBLE stack is
  used because it is smaller than Bluedroid. Stage 4 measured its cost.

## Render state

Implemented natively in stage 6 (`render/render_state.hpp`,
`render/render_stream.hpp`). The ESP32 and SAMD51 halves join at cutover.

- **The renderer reads only `RenderState`.** `Panel` draws one 3-byte
  `RenderCell` per cell, one `RenderCreature` per slot, and the `RenderEvent`s
  since the last update. `LocalPanel` wraps a `World` on the same chip and keeps
  its own state captured, so the native tools and the SAMD51-only firmware use
  the same renderer.
- **Render cell, 3 bytes.** It holds the biome, moisture in 16 levels, a
  carcass flag, exact plants, detritus capped at 16, and shrub and fruit
  state. Every value the renderer used is exact except moisture. Before
  moisture was quantized, all 16,200 frames of a scripted run matched the old
  World-based renderer byte for byte, across both world sizes and every view.
  After quantization the 64×32 build is still identical. The 128×64 build
  differs in 0.15–0.57% of pixels, at most 38 of 2,048 per frame, all from the
  life view's 2×2 moisture average and the soil tint.
- **Creatures carry phenotype.** `Look` packs diet shape, body color,
  detail-sprite back height, and faction into 32 bits, and `look_of(Genome)`
  computes it; `render/render_state.hpp` documents the bit layout. A creature
  entry is slot (u16), id, cell, and look: 12 bytes.
- **Events** are hunts (with the prey cell's wetness), meals, attacks, and
  deaths. They are recorded during `World::step` in order, 22 bytes each.
- **The stream.** Each update is a sequence of records in payloads of up to
  4,096 bytes, and no record spans two payloads:
  - an update header;
  - the complete creature list, as records that each cover a range of slots
    and list the living creatures in it (at most 255 per record). The ranges
    cover all 320 slots once per update. A lost record leaves only its range
    as of an earlier update, and the decoder counts the update in
    `creature_gaps` rather than as complete. Even with every slot alive the
    list fits the first payload beside the update and end records (3,879
    bytes), so a one-payload budget still sends it whole;
  - the events;
  - runs of changed cells, merging single unchanged cells;
  - a rolling refresh of 128 cells;
  - an end record with an FNV-1a checksum of the encoder's shadow map.
  The encoder keeps a 24 KB shadow and computes cells on the fly. Cells that
  do not fit the payload budget stay pending. The decoder rejects malformed
  records and counts checksum mismatches.
- **Measured** on the observatory world, 300 updates each, with 32-bit looks
  and 16-bit slots (the 16-bit format's figures were 3,634, 8,668, and 12,068
  mean bytes):

  | Ticks per update | Mean bytes | Max bytes | At 20 updates/s |
  | ---: | ---: | ---: | ---: |
  | 1 | 3,893 | 5,195 | 78 KB/s, 11% of the link |
  | 4 | 9,304 | 10,300 | 186 KB/s, 26% |
  | 12 | 12,705 | 13,837 | 254 KB/s, 35% |

- **Verified** by `tests/render_stream_test.cpp` in both world sizes:
  - Without loss, the mirror equals the world's render state after every
    update, and the decoded events match the recorded ones.
  - 2,700 frames drawn from the mirror match `LocalPanel` exactly.
  - With one payload per update, the 128×64 mirror caught up 6 updates
    after stepping stopped.
  - With every third payload dropped, the checksum flagged damage, and one
    refresh cycle (65 updates) repaired the mirror.
  - Truncated, unknown, and random payloads are rejected without crashing.
  - A synthetic state with all 320 slots alive and looks using all 32 bits
    round-trips in one payload as two creature records. Losing the second
    record leaves slots 255–319 as they were and flags the update, and the
    next update repairs them. Creature records with out-of-order slots,
    slots outside their range, or a range past 320 are rejected.
- **SAMD51-only firmware.** The board runs the LocalPanel build
  (`petri-observatory-mirror.uf2`). Static RAM is 132,888 bytes, up 28 KB for
  the mirror and event log. 50,828 bytes remain free between heap and stack
  after the display allocates. Cutover removes the 72.7 KB world from the
  SAMD51.
- **Still to do at cutover:**
  - Measure encode time on ESP32 core 0 and decode time on the SAMD51.
  - Choose the update rate.
  - Carry companion inspector data (parents, generation, energy, intake),
    which the PTR4 observation still reads from `World`.

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
- **Bandwidth.** 176 frames/s, 721 KB/s of payload each way. The render
  state stream is budgeted under Render state.
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
- Packet types still to add: render state, camera and input command, telemetry,
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
6. **Render-state stream.** *Done.* The formats, encoder, decoder, and
   mirror are in `render/`, and `Panel` draws only from the mirror. Tests and
   measurements are under Render state; on-chip timing is under stage 7.
7. **Cutover.** *Done, except the multi-hour soak, which is deferred.* The
   world, the stream, controls, QSPI checkpoints, restore after a reset, and a
   power cut are verified on the board. A 10-minute run at 8 ticks/s advanced
   from tick 441 to 5,372, made one scheduled save (1,568 ms), and had no new
   stream, link, or checksum errors. ESP32 heap held at 127,976 bytes, and the
   final fingerprint matched native.
   - `esp32` built with `sdkconfig.defaults.world` runs the observatory world
     on core 1. Between steps it encodes render updates, at most one per
     30 ms plus one per second while paused. It sends telemetry and a creature
     census once a second (three messages of up to 128 slots, each with a
     16-bit first slot). Core 0 runs the SPI slave and applies speed
     commands.
   - `firmware/petri_display` (`--display`) is the SAMD51 half. It holds no
     `World`: only the mirror, the `Panel`, the census details, and the
     companion's PTR4 observation built from them.
   - Variable-length transfers: the SAMD51 clocks both 16-byte headers, then
     only the longer payload, so an idle poll costs microseconds.
   - On the board, the ESP32's fingerprints matched native at ticks 266–268,
     342, 356 (paused), and 382, across 1 and 8 ticks/s and a pause. The SAMD51
     drew 20 fps and received 8 updates/s at 8 ticks/s. Pause held the tick
     while the 1-per-second update continued.
   - Measured costs:
     - ESP32 encode: 4.8 ms per update, on core 1, about the same as one step.
     - SAMD51: up to 3.7 ms to decode a payload, and up to 19 ms to draw a
       close-view frame and push it to the panel.
     - Free memory: 105 KB on the SAMD51 (heap to stack); 128 KB of ESP32 heap,
       and never below 119 KB.
   - The SAMD51 counted 56 invalid frames while the ESP32 booted: its ready
     line floats before the SPI slave starts. They stop once the slave runs.
   - `World::save_checkpoint(ChunkSink)` streams a checkpoint in chunks of up
     to 256 bytes, and `World::load_checkpoint_in_place` decodes without a
     staging copy. The array encoding is now built from the stream, and all
     180 baseline checkpoints are unchanged.
   - **Checkpoints** (`firmware/checkpoint_store.hpp`):
     - **Layout.** The QSPI flash holds four rotating 96 KiB slots, and the
       old CIRCUITPY drive was given up for them; its files are in `backups/`.
       A save erases each sector just before writing it, writes the data from
       byte 256, and reads it back. Only then does it program a 32-byte header
       (magic, sequence, size, tick, data CRC, header CRC) into the
       still-erased start. The newest valid slot is never overwritten.
     - **Boot handshake.** The ESP32 flags `awaiting_world` until the SAMD51
       either sends `start_fresh` or restores the newest valid slot. A
       restore is `restore_begin`, about 22 chunks, and `restore_end` with a
       CRC. The ESP32 allocates its 90 KB restore buffer before the payload
       pool, decodes in place, falls back to a fresh world on any failure,
       and then frees the buffer.
     - **Saves.** The SAMD51 asks for one every 10 minutes, or on `S` over
       USB. The ESP32 streams it at the next tick boundary after any due step.
       A save takes 1.5–1.7 s, and the panel drops to 11–15 fps while sectors
       erase.
     - **Verified on the board:**
       - With no valid slot, the first boot started fresh.
       - A reset restored the newest slot.
       - After `X` corrupted the newest slot, a reset fell back to the older
         one.
       - SAMD51 resets 0.3 s and 0.8 s into a save restored the previous slot.
         A reset at 1.2 s came after the save had finished.
       - In a real power cut during back-to-back saves (`C`), the next boot
         restored a complete slot.
       - Every restored world matched native fingerprints, at ticks 265, 268,
         279, 282, 403, 406, and 409.
   - **Static DRAM.** The world app used 115,732 of 180,736 bytes of static
     DRAM. With Bluetooth the segment shrinks to 124,580 bytes, which would
     have left about 9 KB. The `World`, the event log, and the encoder's
     shadow now live on the heap (November 18, 2026), and the world app uses
     14,564 bytes of static DRAM (`.bss` fell from 105,736 to 4,568 bytes).
     At boot the app allocates the restore buffer first and then the world,
     while the heap is unfragmented. After the boot decision it frees the
     restore buffer and allocates the event log, the encoder, and the
     payload pool in that space. A failed allocation prints its size and the
     largest free block and stops the app. The BLE self-test build
     (`build-ble`) still links, at 26,936 of 124,580 bytes. A world-app
     build with the NimBLE defaults layered on (`sdkconfig.defaults.world`
     then `sdkconfig.defaults.ble`) also links, at 27,712 of 124,580 bytes.
     Heap use with Bluetooth running has not been measured
     on the board.
8. **Graphics pass on the SAMD51.** Measure Protomatter color depth (5–6
   bitplanes), achievable frame rate, and interpolation smoothness. Then pick
   among day/night lighting, weather particles, animated water, and cached
   procedural sprites per zoom level.
9. **BLE.** Add the GATT control and telemetry service, bonding, and the
   phone app.
10. **Ecology.** Exact early-outs in `recycle` and `grow_plants` made the
    native observatory world 8% faster and the default 128×64 world 92%
    faster (README, Environment early-outs). A deterministic share of the
    cells could still move to core 0. Then add evolvable fixed-topology int8 neural controllers, costed
    body traits, plant genetics, dormant eggs, and compatibility-based
    speciation.

World compaction is done (November 18, 2026): the wide `World` is
72,664 bytes, down from 109,008. See the README's memory layout section.

## Options kept open

- **Second island world on the SAMD51.** Its freed RAM and CPU could run a
  separate population with rare migration over the link. Geographic
  isolation of this kind is a classic driver of speciation. Consider this
  only after cutover is stable.
- **Simulation on both ESP32 cores.** Environment cells could update on core 0
  while creatures update on core 1, with double buffering to keep the result
  deterministic. This is part of stage 10.
