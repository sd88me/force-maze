#!/bin/sh
############################################################
# ForceMazeVoice — autostart hook.
# Copy this file into the AddOns FOLDER ROOT to enable.
# MockbaMod's boot.sh runs every *.sh in AddOns/ at startup.
#
# Same shape as ForceAudioIn's run script (its own copy of forceAudioIn.so
# is bundled here so this addon is self-contained) but the producer is
# maze_host — a ported Schwung DSP synth voice — instead of the injectTone
# test tone.
#
# MULTIPLE SIMULTANEOUS VOICES (see forceAudioInject.h): forceAudioIn.so
# itself now attaches to every voice slot 0..AI_MAX_VOICES-1 that has a
# shared-memory segment present and mixes them all in - so several voice
# hosts (each maze_host or similar, each given a distinct --mix-slot) CAN
# run at once. But forceAudioIn.so, the LD_PRELOAD shim, only needs to be
# armed ONCE - it is not per-voice. Only ONE addon (whichever one is
# designated the tap owner - by default this one, at --mix-slot 0) should
# run the "ARM THE TAP FIRST" section below; every additional voice addon
# must skip its own copy of that section entirely and just launch its own
# host binary with a different --mix-slot. Two addons both arming
# forceAudioIn.so would not merely be redundant: run_*.sh's own
# `grep -v forceAudioIn` rewrite of $mmLD_PRELOAD_VAR (below) strips ANY
# existing forceAudioIn entry by substring before adding its own, so two
# such scripts running concurrently at boot race exactly like
# mockbaMagic/MidiLoop already do (see DESIGN.md's "Boot-time LD_PRELOAD
# race") - whichever runs last silently drops the other's line, and if
# THAT script's own library then never got added, forceAudioIn.so may not
# load into MPC at all.
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

# ── deliberately NOT starting maze_host here (2026-09-13) ──
# Every previous version of this script started maze_host right here, at
# boot, alongside arming the tap. That is now KNOWN UNSAFE: see DESIGN.md's
# "Open incident" section - restarting `acvs` while a voice ring is
# attached reliably kills pads (sometimes wifi), on the very first restart,
# every time this has been tested, regardless of forceAudioIn.so's own
# build/version. forceAudioIn.so armed with ZERO voices attached, by
# contrast, has survived every repeated-restart test run against it.
#
# So the split that's actually safe: this script arms the tap (above) with
# nothing attached yet, and maze_host is started SEPARATELY, on demand,
# through the nodeServer Modules page (see NSMODULE.json - its RUNNING
# toggle only starts/stops the maze_host process directly, it does not
# touch $mmLD_PRELOAD_VAR or restart acvs). forceAudioIn.so's lazy re-attach
# (its background thread, added specifically for this) picks up the ring
# maze_host creates within ~2s of it starting, with no restart needed -
# confirmed working via /proc/<MPC-pid>/maps showing the ring mmap'd in
# shortly after a post-boot start.
#
# The hard rule this depends on: once a voice has been started this way,
# do not restart acvs again until it's been stopped (again via the Modules
# page) - every live test of "acvs restart while a voice is attached" has
# failed, with no known safe exception yet. This is a real operational
# constraint, not just a recommendation - see DESIGN.md before changing
# this.
#
# The web control panel is a SEPARATE, independently-enabled addon
# (web/manage.sh + web/run_maze_web.sh, same split force-acid uses for its
# own web panel) - it doesn't need maze_host running to serve the page (it
# just answers 503 on /param et al until maze_host's control socket exists),
# so it can stay always-on regardless of whether a voice is currently
# started. See web/README.md.
