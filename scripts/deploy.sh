#!/usr/bin/env bash
# Combined installer: deploys and enables BOTH Maze Voice and Maze Sequencer
# on a live MockbaMod Force in one command, by calling each module's own
# scripts/deploy.sh in turn.
#
# Usage: scripts/deploy.sh user@force-ip [voice|sequencer|all]
#   (default: all)
set -euo pipefail
cd "$(dirname "$0")/.."

HOST="${1:?usage: scripts/deploy.sh user@force-ip [voice|sequencer|all]}"
TARGET="${2:-all}"

case "$TARGET" in
  voice|sequencer|all) ;;
  *) echo "unknown target '$TARGET' - expected voice, sequencer, or all" >&2; exit 1 ;;
esac

if [ "$TARGET" = "voice" ] || [ "$TARGET" = "all" ]; then
  echo "### Maze Voice ###"
  ( cd maze-voice && ./scripts/deploy.sh "$HOST" )
  echo
fi

if [ "$TARGET" = "sequencer" ] || [ "$TARGET" = "all" ]; then
  echo "### Maze Sequencer ###"
  ( cd maze-sequencer && ./scripts/deploy.sh "$HOST" )
  echo
fi

cat <<EOF
Done.

Reminder: Maze Voice additionally depends on the separate force-audio-jack
addon (the shared audio-injection tap) - enable it once if not already
installed; see maze-voice/README.md's Requirements section. Maze Sequencer
has no such dependency and auto-launches at boot on its own.
EOF
