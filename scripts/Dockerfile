# Build environment for force-acid — Akai Force is ARM 32-bit hard-float
# (ELF 32-bit LSB, ARM EABI5, /lib/ld-linux-armhf.so.3, "GNU/Linux 3.2.0").
#
# Rather than cross-compile + wrangle a multiarch armhf ALSA, we build
# *natively for armhf under QEMU emulation* (Docker Desktop ships binfmt; on a
# bare dockerd, run once:
#   docker run --rm --privileged multiarch/qemu-user-static --reset -p yes ).
# Slower to compile, but the toolchain, headers and glibc are exactly the
# device's word-size and ABI family — the same way Euclidier is built on a Pi.
#
# Debian Buster (armhf: glibc 2.28, GCC 8) is EOL, so its apt repos have moved
# to archive.debian.org and the Release files are expired — hence the sed and
# the Check-Valid-Until=false below. If a binary built here still won't run on
# the Force ("GLIBC_2.xx not found"), the fallback is an even older armhf base
# (arm32v7/debian:stretch) or building on the Force itself — see DESIGN.md.
FROM arm32v7/debian:stretch

RUN sed -i 's|deb.debian.org|archive.debian.org|g; /debian-security/d; /-updates/d' /etc/apt/sources.list \
    && apt-get -o Acquire::Check-Valid-Until=false update \
    && apt-get install -y --no-install-recommends \
        g++ \
        libasound2-dev \
        file \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build
