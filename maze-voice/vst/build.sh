#!/usr/bin/env bash
# Build Maze Voice as a VST2 plugin for the Force's built-in MPC plugin host.
#   vst/build/maze_voice.so     -> /sdcard/vst/ on the Force
#   vst/build/skin/<folder>/    -> /sdcard/Synths/ on the Force
#   vst/build/pluginlist-entry.xml   the <PLUGIN> line for MPC.settings' pluginList-arm
set -euo pipefail
cd "$(dirname "$0")/.."

# Skin artwork is drawn by force-shadow's own renderer (layout.conf -> shadow_art).
FORCE_SHADOW="${FORCE_SHADOW:-$PWD/../../force-shadow}"
mkdir -p vst/build
if [ -f vst/layout.conf ]; then
  docker run --rm -u "$(id -u):$(id -g)" -v "$PWD":/w -v "$FORCE_SHADOW":/fs:ro -w /w gcc:12 \
    gcc -O2 -I/fs/tools -o vst/build/shadow_art vst/shadow_art.c -lm
fi

docker run --rm -u "$(id -u):$(id -g)" -v "$PWD":/w -v /usr/share/fonts/truetype/dejavu:/fonts:ro -w /w python:3.11-slim sh -c "pip install -q --no-warn-script-location --target /tmp/p pillow >/dev/null 2>&1; PYTHONPATH=/tmp/p python3 vst/gen_vst.py"

docker run --rm --platform linux/arm/v7 -u "$(id -u):$(id -g)" -v "$PWD":/b -w /b arm32v7/gcc:12 bash -euxc '
  gcc -O2 -Wall -Wextra -Wno-unused-parameter -fPIC -shared -fvisibility=hidden \
      -DMAZE_LFO=1 -DMAZE_VST=1 -std=gnu11 -Isrc -Ivst/build \
      src/maze_voice.c vst/vst2_wrap.c -lm -o vst/build/maze_voice.so
  strip vst/build/maze_voice.so
  echo "-- exported --"; readelf --dyn-syms -W vst/build/maze_voice.so | grep -E " GLOBAL .* [0-9]+ [A-Za-z]" | grep -v UND
  echo "-- highest glibc (device has 2.39) --"; readelf -V vst/build/maze_voice.so | grep -o "GLIBC_[0-9.]*" | sort -uV | tail -1
'
md5sum vst/build/maze_voice.so
