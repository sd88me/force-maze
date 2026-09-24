#!/usr/bin/env bash
# Build the SD-card release zip into dist-zip/. Unzip it onto the SD card root.
#   maze-voice-vX.Y.Z      -> ForceMazeVoice-vX.Y.Z.zip  (AddOns/ForceMazeVoice)
#   maze-sequencer-vX.Y.Z  -> ForceMazeSeq-vX.Y.Z.zip    (AddOns/ForceMazeSeq)
#   anything else          -> ForceMaze-<version>.zip    (both folders)
# Each folder is that module's addon/, exactly as its scripts/deploy.sh copies
# it, prebuilt binary included - rebuild and commit it first if sources changed.
# .github/workflows/release.yml runs this for every published release.
set -euo pipefail
cd "${PKG_ROOT:-$(dirname "$0")/..}"
VER="${1:-$(git describe --tags --always)}"
case "$VER" in
  maze-voice-v*)     MODS="maze-voice:ForceMazeVoice";   NAME="ForceMazeVoice-${VER#maze-voice-}" ;;
  maze-sequencer-v*) MODS="maze-sequencer:ForceMazeSeq"; NAME="ForceMazeSeq-${VER#maze-sequencer-}" ;;
  *)                 MODS="maze-voice:ForceMazeVoice maze-sequencer:ForceMazeSeq"; NAME="ForceMaze-$VER" ;;
esac
OUT="$PWD/dist-zip"
STAGE="$(mktemp -d)"; trap 'rm -rf "$STAGE"' EXIT
mkdir -p "$STAGE/AddOns" "$OUT"
for m in $MODS; do
  A="$STAGE/AddOns/${m#*:}"
  cp -r "${m%%:*}/addon" "$A"
  chmod 0755 "$A"/*.sh "$A"/web/*.sh
  find "$A" -maxdepth 1 -type f -name 'maze*_host' -exec chmod 0755 {} +
done
find "$STAGE" \( -name __pycache__ -prune -o -name '*.pyc' -o -name .gitkeep \) -exec rm -rf {} +
rm -f "$OUT/$NAME.zip"
python3 -c "import shutil,sys; shutil.make_archive(sys.argv[1], 'zip', sys.argv[2], 'AddOns')" "$OUT/$NAME" "$STAGE"
python3 -m zipfile -l "$OUT/$NAME.zip"
