#!/bin/sh
############################################################
# Copy this file to $mmPath/AddOns to launch automatically
# at boot (manage.sh ENABLE does that for you).
############################################################

mmPath=$(cat /dev/shm/.mmPath)
. $mmPath/MockbaMod/env.sh

APPDIR="$mmPath/AddOns/ForceMazeSeq"

if test "$1" = "kill"; then
    killall maze_seq_host 2>/dev/null
else
    "$APPDIR/maze_seq_host" \
        --module-dir "$APPDIR" \
        --ctrl-sock /tmp/maze_seq_ctrl.sock \
        --control-channel 1 \
        >/tmp/maze_seq_host.log 2>&1 &
fi
