#!/bin/sh
############################################################
# ForceMazeVoice — autostart hook.
# Copy this file into the AddOns FOLDER ROOT to enable.
# MockbaMod's boot.sh runs every *.sh in AddOns/ at startup.
#
# Same shape as ForceAudioIn's run script (its own copy of forceAudioIn.so
# is bundled here so this addon is self-contained) but the producer is
# maze_host — a ported Schwung DSP synth voice — instead of the injectTone
# test tone. Only one of ForceAudioIn / ForceMazeVoice should be enabled at
# a time: both would otherwise fight over the same LD_PRELOAD entry name and
# the same /forceAudioInject shared-memory ring.
############################################################

mmPath=$(cat /dev/shm/.mmPath)
. $mmPath/MockbaMod/env.sh
APPDIR="$mmPath/AddOns/ForceMazeVoice"
LIB="$APPDIR/forceAudioIn.so"

# ── Locking around $mmLD_PRELOAD_VAR ────────────────────────
# CONFIRMED live (2026-09-13, see DESIGN.md): mockbaMagic's and MidiLoop's
# own run_*.sh scripts both read this same file, check their library isn't
# already in it, and write the whole file back - with NO locking at all -
# and boot.sh backgrounds every top-level addon script concurrently
# ("launches every top-level AddOns/*.sh ($f &)"). That's a real, observed
# lost-update race: whichever write lands last wins, based on whatever it
# read, silently dropping another script's entry. Losing MidiLoop's entry
# looks like dead pads/buttons; losing mockbaMagic's looks like its own
# documented WiFi-breaking bug. Adding a third unlocked writer (this addon)
# made the collision measurably worse in practice.
#
# This can't fix mockbaMagic/MidiLoop's OWN unlocked writes - they don't
# check for or respect this lock. It only makes OUR participation safe.
# mkdir is atomic even on busybox, so it works as a mutex; the retry is
# bounded and fails OPEN (proceeds unlocked) rather than risk hanging boot
# forever on a stale lock from a crashed process.
PRELOAD_LOCK="/dev/shm/.LD_PRELOAD.lock"
lock_preload() {
    i=0
    while ! mkdir "$PRELOAD_LOCK" 2>/dev/null; do
        i=$((i + 1))
        [ $i -ge 50 ] && return 1   # ~5s of retries, then fail open
        sleep 0.1
    done
    return 0
}
unlock_preload() { rmdir "$PRELOAD_LOCK" 2>/dev/null; }

# boot.sh calls addon scripts with "kill" on shutdown/restart - full teardown.
if [ "$1" = "kill" ]; then
    for p in $(ps 2>/dev/null | grep "[m]aze_host" | awk '{print $1}'); do
        kill -9 $p 2>/dev/null
    done
    lock_preload
    if [ -f "$mmLD_PRELOAD_VAR" ]; then
        cat "$mmLD_PRELOAD_VAR" | tr " " "\n" | grep -v forceAudioIn | tr "\n" " " > /tmp/.p.$$
        mv /tmp/.p.$$ "$mmLD_PRELOAD_VAR"
    fi
    unlock_preload
    exit 0
fi

# ── ARM THE TAP FIRST ──────────────────────────────────────
lock_preload
if [ -f "$mmLD_PRELOAD_VAR" ]; then
    FC=$(cat "$mmLD_PRELOAD_VAR" | tr " " "\n" | grep -v forceAudioIn | tr "\n" " ")
    echo "$LIB $FC" > "$mmLD_PRELOAD_VAR"
else
    echo "$LIB" > "$mmLD_PRELOAD_VAR"
fi
unlock_preload

# ── start the synth NOW, not after MPC comes up ────────────
# See ForceAudioIn's run script for why: forceAudioIn.so's constructor needs
# the /forceAudioInject shared-memory ring to already exist the moment MPC's
# process is exec'd, and maze_host creates it at its own startup.
"$APPDIR/maze_host" --module-dir "$APPDIR" --ctrl-sock /tmp/maze_ctrl.sock --control-channel 1 \
    > /tmp/maze_host.log 2>&1 &

# The web control panel is now a SEPARATE, independently-enabled addon
# (web/manage.sh + web/run_maze_web.sh, same split force-acid uses for its
# own web panel) - it doesn't need maze_host running to serve the page (it
# just answers 503 on /param et al until maze_host's control socket exists),
# so it can stay always-on even while this engine is disabled. See
# web/README.md.
