#!/bin/sh
# ForceMazeVoice AddOn Manager (MockbaMod convention).
#   sh manage.sh ENABLE | DISABLE | UNINSTALL
#
# Ported Schwung "Maze Voice" DSP synth (Moog Labyrinth-style oscillator/
# wavefolder/filter). maze_host renders it via its native v2 plugin API and
# writes audio into a shared-memory ring that the separate ForceAudioIn
# addon's forceAudioIn.so (LD_PRELOAD'd into MPC) mixes into what MPC reads
# from its capture device.
#
# This addon does NOT touch LD_PRELOAD or restart acvs - ForceAudioIn owns
# arming the shared tap exclusively (enable it separately, once; see its own
# README.md). maze_host itself is started/stopped entirely from the
# nodeServer Modules page (/moduler) - never from this script, and never at
# boot (see NSMODULE.json - Autoload is deliberately unavailable). ENABLE/
# DISABLE here only control whether the addon's files are present.

appname=maze_host
appTitle="Force Maze Voice"
appDir=ForceMazeVoice

mmPath=$(cat /dev/shm/.mmPath)
. $mmPath/MockbaMod/env.sh

runDir="$mmPath/AddOns"
installroot="$runDir/$appDir"
mode=$1

echo "
***********************************************************
*   $appTitle AddOn Manager for MockbaMod
***********************************************************
"

STOP() {
    for p in $(ps 2>/dev/null | grep "[m]aze_host" | awk '{print $1}'); do
        kill -9 $p 2>/dev/null
    done
}

if [ "$mode" = "UNINSTALL" ]; then
    STOP
    rm -rf "$installroot" 2>/dev/null
    echo "<<<< $appTitle uninstalled."
    exit 0
fi

if [ "$mode" = "DISABLE" ]; then
    STOP
    echo "$appTitle's maze_host stopped (files kept - this addon has no boot-time footprint to remove)."
    exit 0
fi

if [ "$mode" = "ENABLE" ]; then
    echo "$appTitle enabled. Make sure the separate ForceAudioIn addon is"
    echo "also enabled (its own manage.sh ENABLE) - it arms the shared tap"
    echo "this addon needs. Start the voice itself from the nodeServer"
    echo "Modules page (/moduler), not from here."
    exit 0
fi

echo "Usage: sh manage.sh ENABLE | DISABLE | UNINSTALL"
echo
echo "Status:"
ps 2>/dev/null | grep -q "[m]aze_host" && echo "  voice: RUNNING" || echo "  voice: stopped"
echo "  start/stop from the nodeServer Modules page (/moduler) - requires"
echo "  the separate ForceAudioIn addon to be enabled first."
echo "  web UI is a separate addon - see web/manage.sh (survives this being disabled)"
echo "  logs: /tmp/forceAudioIn.log (mix tap, in the ForceAudioIn addon), /tmp/maze_host.log (synth)"
echo "  control socket: /tmp/maze_ctrl.sock"
