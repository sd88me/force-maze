#!/bin/sh
############################################################
# Copy this file to $mmPath/AddOns to launch automatically
# at boot (manage.sh ENABLE does that for you).
############################################################
#
# Runs the Force Maze Sequencer web control panel (server.py). Independent
# of the engine's own addon/manage.sh/run_maze_seq.sh - same split as
# force-acid's web/run_forceacidweb.sh and ../../maze-voice/web/
# run_maze_web.sh (this is a direct copy of that pattern).
#
# PID-file based kill, not `killall python3` or a `pgrep -f` name match:
# this device runs other python3 processes (nodeServer's tooling, the other
# modules' own web panels), and a name-based kill would take those down too.

mmPath=$(cat /dev/shm/.mmPath)
. $mmPath/MockbaMod/env.sh

APPDIR="$mmPath/AddOns/ForceMazeSeq/web"
PIDFILE="$APPDIR/.maze_seq_web.pid"

if [ "$1" = "kill" ]; then
    if [ -f "$PIDFILE" ]; then
        kill "$(cat "$PIDFILE")" 2>/dev/null
        rm -f "$PIDFILE"
    fi
else
    cd "$APPDIR" || exit 1
    python3 server.py --port 8305 --ctrl-sock /tmp/maze_seq_ctrl.sock >/tmp/maze_seq_web.log 2>&1 &
    echo $! > "$PIDFILE"
fi
