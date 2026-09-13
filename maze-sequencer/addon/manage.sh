#!/bin/sh
appname=maze_seq
appTitle="Force Maze Seq"
appDir=ForceMazeSeq

################ NO NEED TO EDIT BELOW THIS LINE ###############

mmPath=$(cat /dev/shm/.mmPath)
. $mmPath/MockbaMod/env.sh

runDir="$mmPath/AddOns/"
installroot="$mmPath/AddOns/$appDir/"
runScript="$runDir/run_$appname.sh"
mode=$1

echo "
***********************************************************
*   $appTitle AddOn Manager for Mockba Mod
***********************************************************
"
if [ "$mode" = "UNINSTALL" ]; then
    if [ -e "$installroot" ]; then
        killall maze_seq_host 2>/dev/null
        rm -rf "$installroot"
        rm -f "$runScript"
        echo "<<<< $appTitle has been UnInstalled."
    fi
fi

if [ "$mode" = "DISABLE" ]; then
    killall maze_seq_host 2>/dev/null
    rm -f "$runScript"
    echo "$appTitle disabled from Auto Launch"
fi

if [ "$mode" = "ENABLE" ]; then
    cp -f "$installroot/run_$appname.sh" "$runScript"
    "$runScript"
    echo "$appTitle enabled for Auto Launch"
fi
