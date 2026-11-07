#!/bin/bash
# Usage: ./build.sh [pico_defuse|pico_serial]   (default: pico_defuse)
#   pico_serial = serial-only firmware (no de_Fuse), host reset via baud 1201
set -e
TARGET=${1:-pico_defuse}
# old pico-sdk sub-builds (pioasm/elf2uf2) need this with CMake >= 4
export CMAKE_POLICY_VERSION_MINIMUM=3.5
mkdir -p build
cd build
cmake ..
make -j"$(nproc)" "$TARGET"
sudo picotool load -f "$TARGET.uf2"
echo "Done!"
