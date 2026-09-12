#!/bin/sh
############################################################
# Copy this file to $mmPath/AddOns to launch automatically
# at boot (manage.sh ENABLE does that for you).
############################################################
#
# Runs the Force Maze Voice web control panel (server.py). Independent of
# the engine's own addon/manage.sh/run_maze_host.sh - that one stays
# disabled pending the boot-race fix (see DESIGN.md), but the *panel* can
# still be up and reachable at all times, same as force-acid's
# web/run_forceacidweb.sh (this is a direct copy of that pattern).
#
# PID-file based kill, not `killall python3` or a `pgrep -f` name match:
# this device runs other python3 processes (nodeServer's tooling, force-
# acid's own web panel), and a name-based kill would take those down too -
# confirmed the hard way once already this project (see DESIGN.md's
# SCHED_FIFO incident write-up for the general lesson on blast radius).

mmPath=$(cat /dev/shm/.mmPath)
. $mmPath/MockbaMod/env.sh

APPDIR="$mmPath/AddOns/ForceMazeVoice/web"
PIDFILE="$APPDIR/.maze_web.pid"

if [ "$1" = "kill" ]; then
    if [ -f "$PIDFILE" ]; then
        kill "$(cat "$PIDFILE")" 2>/dev/null
        rm -f "$PIDFILE"
    fi
else
    cd "$APPDIR" || exit 1
    python3 server.py --port 8304 --ctrl-sock /tmp/maze_ctrl.sock >/tmp/maze_web.log 2>&1 &
    echo $! > "$PIDFILE"
fi
