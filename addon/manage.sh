#!/bin/sh
# ForceMazeVoice AddOn Manager (MockbaMod convention).
#   sh manage.sh ENABLE | DISABLE | UNINSTALL
#
# Ported Schwung "Maze Voice" DSP synth (Moog Labyrinth-style oscillator/
# wavefolder/filter). maze_host renders it via its native v2 plugin API and
# writes audio into ForceAudioIn's shared-memory ring; forceAudioIn.so
# (bundled here, LD_PRELOAD'd into MPC) mixes it into what MPC reads from
# its capture device. Restarting `acvs` (the "InMusic MPC Application"
# service) is required for a changed LD_PRELOAD to take effect.
#
# Only one of ForceAudioIn / ForceMazeVoice should be enabled at a time.

appname=maze_host
appTitle="Force Maze Voice"
appDir=ForceMazeVoice

mmPath=$(cat /dev/shm/.mmPath)
. $mmPath/MockbaMod/env.sh

runDir="$mmPath/AddOns"
installroot="$runDir/$appDir"
runScript="$runDir/run_$appname.sh"
mode=$1

echo "
***********************************************************
*   $appTitle AddOn Manager for MockbaMod
***********************************************************
"

# See run_maze_host.sh for why this lock exists: mockbaMagic's and
# MidiLoop's own scripts read-modify-write this same file with no locking,
# concurrently at boot - a confirmed, observed lost-update race. This can't
# fix their side of it, only make ours safe.
PRELOAD_LOCK="/dev/shm/.LD_PRELOAD.lock"
lock_preload() {
    i=0
    while ! mkdir "$PRELOAD_LOCK" 2>/dev/null; do
        i=$((i + 1))
        [ $i -ge 50 ] && return 1
        sleep 0.1
    done
    return 0
}
unlock_preload() { rmdir "$PRELOAD_LOCK" 2>/dev/null; }

STOP() {
    for p in $(ps 2>/dev/null | grep "[m]aze_host" | awk '{print $1}'); do
        kill -9 $p 2>/dev/null
    done
    lock_preload
    if [ -f "$mmLD_PRELOAD_VAR" ]; then
        cat "$mmLD_PRELOAD_VAR" | tr " " "\n" | grep -v forceAudioIn | tr "\n" " " > /tmp/.p.$$
        mv /tmp/.p.$$ "$mmLD_PRELOAD_VAR"
    fi
    unlock_preload
}

if [ "$mode" = "UNINSTALL" ]; then
    STOP
    rm -f "$runScript" 2>/dev/null
    rm -rf "$installroot" 2>/dev/null
    echo "<<<< $appTitle uninstalled. Restarting the Force app."
    systemctl restart acvs
    exit 0
fi

if [ "$mode" = "DISABLE" ]; then
    STOP
    rm -f "$runScript" 2>/dev/null
    echo "$appTitle disabled. Restarting the Force app."
    systemctl restart acvs
    exit 0
fi

if [ "$mode" = "ENABLE" ]; then
    cp "$installroot/run_$appname.sh" "$runScript" 2>/dev/null
    chmod 755 "$runScript" 2>/dev/null
    echo "$appTitle enabled. Restarting the Force app."
    systemctl restart acvs
    exit 0
fi

echo "Usage: sh manage.sh ENABLE | DISABLE | UNINSTALL"
echo
echo "Status:"
[ -f "$runScript" ] && echo "  autostart: ENABLED" || echo "  autostart: disabled"
ps 2>/dev/null | grep -q "[m]aze_host" && echo "  voice: RUNNING" || echo "  voice: stopped"
echo "  web UI is a separate addon now - see web/manage.sh (survives this being disabled)"
echo "  logs: /tmp/forceAudioIn.log (mix tap), /tmp/maze_host.log (synth)"
echo "  control socket: /tmp/maze_ctrl.sock"
