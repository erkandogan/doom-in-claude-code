#!/usr/bin/env bash
# Download the latest Freedoom WADs to ./assets/. Freedoom is a GPL-licensed
# free Doom game content replacement — it is legal to redistribute.

set -eu

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ASSETS="$REPO_ROOT/assets"
mkdir -p "$ASSETS"

URL="${FREEDOOM_URL:-https://github.com/freedoom/freedoom/releases/download/v0.13.0/freedoom-0.13.0.zip}"
ZIP="$ASSETS/freedoom.zip"

if [ -f "$ASSETS/freedoom1.wad" ] && [ -f "$ASSETS/freedoom2.wad" ]; then
  echo "Freedoom already present in $ASSETS"
  exit 0
fi

echo "Downloading Freedoom from $URL ..."
if command -v curl >/dev/null 2>&1; then
  curl -L -o "$ZIP" "$URL"
elif command -v wget >/dev/null 2>&1; then
  wget -O "$ZIP" "$URL"
else
  echo "Error: need curl or wget." >&2
  exit 1
fi

echo "Unpacking..."
(cd "$ASSETS" && unzip -o "$ZIP" >/dev/null)

# Freedoom zips into freedoom-<version>/; flatten the WADs.
find "$ASSETS" -name 'freedoom*.wad' -type f -exec mv {} "$ASSETS"/ \;
rm -f "$ZIP"

echo "Done. WADs in $ASSETS"
ls -1 "$ASSETS"/*.wad 2>/dev/null || true
