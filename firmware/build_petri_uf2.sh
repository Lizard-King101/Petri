#!/bin/sh
set -eu

if [ "$#" -lt 1 ] || [ "$#" -gt 3 ]; then
    echo "Usage: $0 /path/to/uf2conv.py [output.uf2] [checkpoint.petri|--wide|--connected|--observatory|--selftest|--linktest|--display]" >&2
    exit 2
fi

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
output=${2:-"$repo/build/petri-matrixportal-m4.uf2"}
stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT

if [ "${3:-}" = --display ]; then
    sketch="$stage/petri_display"
    mkdir -p "$sketch/src" "$(dirname -- "$output")"
    cp "$repo/firmware/petri_display/petri_display.ino" "$sketch/"
    cp "$repo/link/link.cpp" "$repo/link/link.hpp" "$repo/link/messages.cpp" "$repo/link/messages.hpp" \
        "$repo/firmware/link_master.hpp" "$repo/firmware/checkpoint_store.hpp" \
        "$repo/render/render_stream.cpp" "$repo/render/render_stream.hpp" \
        "$repo/render/observation.cpp" "$repo/render/observation.hpp" "$sketch/src/"
elif [ "${3:-}" = --linktest ]; then
    sketch="$stage/linktest"
    mkdir -p "$sketch/src" "$(dirname -- "$output")"
    cp "$repo/firmware/linktest/linktest.ino" "$sketch/"
    cp "$repo/link/link.cpp" "$repo/link/link.hpp" "$sketch/src/"
elif [ "${3:-}" = --selftest ]; then
    sketch="$stage/selftest"
    mkdir -p "$sketch/src" "$(dirname -- "$output")"
    cp "$repo/firmware/selftest/selftest.ino" "$sketch/"
    cp "$repo/esp32/selftest/selftest.cpp" "$repo/esp32/selftest/selftest.hpp" "$repo/core/checkpoint.cpp" \
        "$repo/render/observation.cpp" "$repo/render/observation.hpp" "$sketch/src/"
else
    sketch="$stage/petri_matrix"
    mkdir -p "$sketch/src" "$(dirname -- "$output")"
    cp "$repo/firmware/petri_matrix/petri_matrix.ino" "$sketch/"
fi
if [ "${3:-}" != --linktest ]; then
    cp "$repo/core/world.cpp" "$repo/core/world.hpp" "$repo/core/nests.cpp" "$repo/core/hunting.cpp" "$repo/core/brain.cpp" "$repo/core/brain.hpp" "$repo/core/brains_world.cpp" "$repo/core/founder_brains.hpp" "$repo/core/founder_brains_trophic.hpp" "$repo/core/founder_brains_social.hpp" "$repo/core/watershed_table.hpp" "$repo/render/life.cpp" "$repo/render/life.hpp" "$sketch/src/"
    cp "$repo/render/render_state.cpp" "$repo/render/render_state.hpp" "$sketch/src/"
    cp "$repo/render/panel.cpp" "$repo/render/panel.hpp" "$repo/render/terrain.hpp" "$sketch/src/"
    cp "$repo/firmware/esp32_passthrough/esp32_bridge.hpp" "$sketch/src/"
fi

cpp_flags=-std=gnu++17
if [ "${3:-}" = --wide ]; then
    cpp_flags="$cpp_flags -DPETRI_WIDE_WORLD=1"
elif [ "${3:-}" = --connected ]; then
    cpp_flags="$cpp_flags -DPETRI_WIDE_WORLD=1 -DPETRI_CONNECTED_BIOMES=1"
elif [ "${3:-}" = --selftest ]; then
    cpp_flags="$cpp_flags -DPETRI_WIDE_WORLD=1"
elif [ "${3:-}" = --linktest ]; then
    :
elif [ "${3:-}" = --display ]; then
    cpp_flags="$cpp_flags -DPETRI_WIDE_WORLD=1"
elif [ "${3:-}" = --observatory ]; then
    cp "$repo/render/observation.cpp" "$repo/render/observation.hpp" "$sketch/src/"
    cpp_flags="$cpp_flags -DPETRI_WIDE_WORLD=1 -DPETRI_CONNECTED_BIOMES=1 -DPETRI_OBSERVATORY=1"
elif [ "$#" -eq 3 ]; then
    cp "$repo/core/checkpoint.cpp" "$sketch/src/"
    python3 - "$3" "$sketch/src/demo_checkpoint.hpp" <<'PYTHON'
from pathlib import Path
import sys

checkpoint = Path(sys.argv[1]).read_bytes()
checkpoint_types = {4: "FoodWebCheckpoint", 5: "ConnectedCheckpoint", 6: "IrregularCheckpoint",
                    7: "SexualCheckpoint", 8: "NestCheckpoint", 9: "AdaptiveCheckpoint",
                    10: "HuntTuningCheckpoint", 11: "KinAvoidanceCheckpoint", 12: "KinTuningCheckpoint",
                    13: "FactionCheckpoint", 14: "FloraCheckpoint", 15: "ClimateCheckpoint",
                    16: "StormCheckpoint", 17: "CapacityCheckpoint", 18: "Checkpoint"}
if checkpoint[:5] != b"PETRI" or len(checkpoint) < 6 or checkpoint[5] not in checkpoint_types:
    raise SystemExit("The demo requires a Petri checkpoint from versions 4 through 18")
with Path(sys.argv[2]).open("w") as header:
    checkpoint_type = checkpoint_types[checkpoint[5]]
    header.write(f"static constexpr petri::{checkpoint_type} demoCheckpoint{{{{\n")
    for start in range(0, len(checkpoint), 16):
        row = checkpoint[start:start + 16]
        header.write("    " + ", ".join(f"0x{byte:02x}" for byte in row) + ",\n")
    header.write("}};\n")
    header.write(f"static_assert(demoCheckpoint.size() == {len(checkpoint)});\n")
PYTHON
    cpp_flags="$cpp_flags -DPETRI_DEMO_CHECKPOINT"
fi

# Extra compiler flags for experiments, for example PETRI_EXTRA_FLAGS=-DPETRI_COLOR_DEPTH=6.
cpp_flags="$cpp_flags ${PETRI_EXTRA_FLAGS:-}"
arduino-cli compile --fqbn adafruit:samd:adafruit_matrixportal_m4:usbstack=tinyusb \
    --build-property "compiler.cpp.extra_flags=$cpp_flags" --output-dir "$stage/output" "$sketch"
python3 "$1" -c -b 0x4000 -f SAMD51 \
    -o "$stage/petri-matrixportal-m4.uf2" "$stage/output/$(basename -- "$sketch").ino.bin"
install -m 644 "$stage/petri-matrixportal-m4.uf2" "$output"
