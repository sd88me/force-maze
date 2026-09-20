#!/usr/bin/env bash
# =============================================================================
# Build maze_host for the Akai Force (armhf) inside a QEMU-emulated armhf
# container (same toolchain as force-acid — see scripts/Dockerfile) and
# assemble the MockbaMod addon folder under dist/.
#
#   dist/maze_host              the armhf binary (RtMidi in + DSP + ring out)
#   dist/module.json            needed at runtime (chain_params/ui_hierarchy)
#   dist/ForceMazeVoice/        the addon folder (drop into AddOns/)
#
# Requires Docker with armhf emulation (see force-acid/scripts/build.sh for
# the one-time qemu-user-static setup on a bare dockerd).
# =============================================================================
set -euo pipefail
cd "$(dirname "$0")/.."

IMG=maze-voice-builder
PLATFORM=linux/arm/v7

echo "== build armhf toolchain image ($PLATFORM) =="
docker build --platform "$PLATFORM" -t "$IMG" scripts

echo "== compile + package (native armhf under QEMU — slow, be patient) =="
docker run --rm --platform "$PLATFORM" \
  -u "$(id -u):$(id -g)" -v "$PWD":/build -w /build "$IMG" bash -euxc '
  COMMON="-O2 -Wall -Wextra -Wno-unused-parameter -Isrc"
  rm -rf dist && mkdir -p dist/ForceMazeVoice obj

  # DSP core — C, upstream logic untouched (no LABYRINTH_STANDALONE defined,
  # so its PC-only main() is compiled out and does not collide with ours)
  gcc $COMMON -DMAZE_LFO=1 -std=c11   -c src/maze_voice.c        -o obj/maze_voice.o

  # vendored RtMidi (ALSA backend) + our host shim — C++
  g++ $COMMON -std=c++14 -D__LINUX_ALSA__ -c src/rtmidi/RtMidi.cpp -o obj/RtMidi.o
  g++ $COMMON -std=c++14 -c src/maze_host.cpp       -o obj/maze_host.o

  g++ obj/maze_voice.o obj/RtMidi.o obj/maze_host.o \
      -lasound -lpthread -lrt \
      -o dist/maze_host

  strip dist/maze_host
  file dist/maze_host
  echo "-- shared libs the Force must provide --"
  readelf -d dist/maze_host | grep NEEDED
  echo "-- highest glibc symbol version required (want <= 2.28) --"
  { readelf -V dist/maze_host | grep -o "GLIBC_[0-9.]*" | sort -uV | tail -3; } || true

  rm -rf obj
  cp dist/maze_host          dist/ForceMazeVoice/maze_host
  cp module.json             dist/ForceMazeVoice/
  chmod 0755 dist/ForceMazeVoice/maze_host

  ls -la dist dist/ForceMazeVoice
'
echo "== done -> dist/ForceMazeVoice/ =="
