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

# boot.sh calls addon scripts with "kill" on shutdown/restart - full teardown.
if [ "$1" = "kill" ]; then
    for p in $(ps 2>/dev/null | grep "[m]aze_host" | awk '{print $1}'); do
        kill -9 $p 2>/dev/null
    done
    for p in $(ps 2>/dev/null | grep "[s]erver.py" | awk '{print $1}'); do
        kill -9 $p 2>/dev/null
    done
    if [ -f "$mmLD_PRELOAD_VAR" ]; then
        cat "$mmLD_PRELOAD_VAR" | tr " " "\n" | grep -v forceAudioIn | tr "\n" " " > /tmp/.p
        mv /tmp/.p "$mmLD_PRELOAD_VAR"
    fi
    exit 0
fi

# ── ARM THE TAP FIRST ──────────────────────────────────────
if [ -f "$mmLD_PRELOAD_VAR" ]; then
    FC=$(cat "$mmLD_PRELOAD_VAR" | tr " " "\n" | grep -v forceAudioIn | tr "\n" " ")
    echo "$LIB $FC" > "$mmLD_PRELOAD_VAR"
else
    echo "$LIB" > "$mmLD_PRELOAD_VAR"
fi

# ── start the synth NOW, not after MPC comes up ────────────
# See ForceAudioIn's run script for why: forceAudioIn.so's constructor needs
# the /forceAudioInject shared-memory ring to already exist the moment MPC's
# process is exec'd, and maze_host creates it at its own startup.
"$APPDIR/maze_host" --module-dir "$APPDIR" --ctrl-sock /tmp/maze_ctrl.sock \
    > /tmp/maze_host.log 2>&1 &

# ── web control panel ───────────────────────────────────────
# No ordering constraint (unlike maze_host above): it only needs maze_host's
# control socket to exist by the time a browser actually asks it for
# something, and it fails soft (503) until then. Pure stdlib server, no
# LD_LIBRARY_PATH/mido dance needed (that's only for the bundled
# python-rtmidi wheel, which this doesn't use).
python3 "$APPDIR/web/server.py" --port 8304 --ctrl-sock /tmp/maze_ctrl.sock \
    > /tmp/maze_web.log 2>&1 &
