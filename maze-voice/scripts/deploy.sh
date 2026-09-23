#!/usr/bin/env bash
# Deploy + enable Force Maze Voice on a live MockbaMod Force in one command.
# Usage: scripts/deploy.sh user@force-ip
#
# Standard MockbaMod deploy.sh pattern - see
# ~/.claude/skills/mockbamod-module-creator/references/deploy-debug.md
# ("Standard deploy.sh pattern") for the template this was generated from.
set -euo pipefail
cd "$(dirname "$0")/.."

APP_DIR="ForceMazeVoice"   # device-side AddOns/<APP_DIR> folder name
HAS_WEB=1                  # 1 if addon/web/manage.sh exists and needs its own ENABLE

HOST="${1:?usage: scripts/deploy.sh user@force-ip}"

if [ ! -d addon ]; then
  echo "addon/ not found - run scripts/build.sh first." >&2
  exit 1
fi

mmPath="$(ssh "$HOST" 'cat /dev/shm/.mmPath')"
echo "== remote mmPath: $mmPath =="

# scp -r into an existing destination NESTS rather than merges - remove first.
ssh "$HOST" "rm -rf '$mmPath/AddOns/$APP_DIR'"
scp -r addon "$HOST:$mmPath/AddOns/$APP_DIR"

ssh "$HOST" "'$mmPath/AddOns/$APP_DIR/manage.sh' ENABLE"
if [ "$HAS_WEB" = 1 ]; then
  ssh "$HOST" "'$mmPath/AddOns/$APP_DIR/web/manage.sh' ENABLE"
fi

cat <<EOF

== $APP_DIR deployed and enabled. ==
Still needed once, if not already installed: the separate force-audio-jack
addon (the shared audio-injection tap Maze Voice depends on) - see
../README.md's Requirements section.

Start maze_host itself from the nodeServer Modules page (/moduler) - it is
never auto-launched at boot by design. Web panel: http://${HOST#*@}:8304

HARD RULE: once a voice has been started from the Modules page, do not
restart acvs (or reboot) until it has been stopped from the same toggle
first - see README.md's own hard-rule section.
EOF
