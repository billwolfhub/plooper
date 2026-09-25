#!/bin/sh
# Builds Plooper's audio code for the Mac against stub hardware and prints
# output levels over time: EMPTY, then a 4 s recording, then playback.
#
# Usage: sim/run.sh [input_dc] [reverb_mode 0-5] [stress]
#   reverb modes: 0 Plate, 1 Shimmer, 2 Sub, 3 Backwards, 4 Ghost, 5 Freeze
#   stress: all four heads full, DECAY and ODD max, 30 s of playback
#   env: ONEHEAD=1 (stress with head A only), DECAY=0..1 (stress decay)
set -e
cd "$(dirname "$0")"
D=../../DaisySP
# The newest macOS SDK may not match the installed linker; use the newest that works
SDK=$(ls -d /Library/Developer/CommandLineTools/SDKs/MacOSX26*.sdk 2>/dev/null | head -1)
c++ ${SDK:+-isysroot "$SDK"} -std=gnu++14 -O2 -DUSE_DAISYSP_LGPL -I. -I.. \
    -I$D/Source -I$D/Source/Utility -I$D/DaisySP-LGPL/Source \
    harness.cpp $D/DaisySP-LGPL/Source/Effects/reverbsc.cpp $D/Source/Control/phasor.cpp \
    -o harness
./harness "$@"
