#!/usr/bin/env bash
# =============================================================================
# Build maze_seq_host for the Akai Force (armhf) inside a QEMU-emulated armhf
# container (same toolchain as force-acid/../../maze-voice — see
# scripts/Dockerfile) and write straight into ../addon/, ready to deploy.
#
# Requires Docker with armhf emulation (see force-acid/scripts/build.sh for
# the one-time qemu-user-static setup on a bare dockerd).
# =============================================================================
set -euo pipefail
cd "$(dirname "$0")/.."

IMG=maze-seq-builder
PLATFORM=linux/arm/v7

echo "== build armhf toolchain image ($PLATFORM) =="
docker build --platform "$PLATFORM" -t "$IMG" scripts

echo "== compile + package (native armhf under QEMU — slow, be patient) =="
docker run --rm --platform "$PLATFORM" \
  -u "$(id -u):$(id -g)" -v "$PWD":/build -w /build "$IMG" bash -euxc '
  COMMON="-O2 -Wall -Wextra -Wno-unused-parameter -D_DEFAULT_SOURCE -Isrc"
  rm -rf obj

  # sequencer core — C, upstream logic untouched but for the FORCE-ONLY
  # header swap + state-path change (see src/maze_seq_core.c header)
  gcc $COMMON -std=c11   -c src/maze_seq_core.c      -o obj/maze_seq_core.o

  # vendored RtMidi (ALSA backend) + our host shim — C++
  g++ $COMMON -std=c++14 -D__LINUX_ALSA__ -c src/rtmidi/RtMidi.cpp -o obj/RtMidi.o
  g++ $COMMON -std=c++14 -c src/host_shim.cpp         -o obj/host_shim.o

  g++ obj/maze_seq_core.o obj/RtMidi.o obj/host_shim.o \
      -lasound -lpthread \
      -o maze_seq_host

  strip maze_seq_host
  file maze_seq_host
  echo "-- shared libs the Force must provide --"
  readelf -d maze_seq_host | grep NEEDED
  echo "-- highest glibc symbol version required (want <= 2.28) --"
  { readelf -V maze_seq_host | grep -o "GLIBC_[0-9.]*" | sort -uV | tail -3; } || true

  rm -rf obj
  mv maze_seq_host addon/maze_seq_host
  chmod 0755 addon/maze_seq_host addon/manage.sh addon/run_maze_seq.sh addon/web/manage.sh addon/web/run_maze_seq_web.sh

  ls -la addon
'
echo "== done -> addon/maze_seq_host =="
