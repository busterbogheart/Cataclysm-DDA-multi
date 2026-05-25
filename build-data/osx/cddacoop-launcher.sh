#!/bin/sh
# Cddacoop launcher.
#
# Replaces the upstream Cataclysm.sh inside the .app bundle. Runs the
# CC-Sounds first-run check, then launches the binary directly — mode
# selection (Single-player / Host / Join) is handled by the in-game
# CO-OP main menu, not a pre-launch dialog. CFBundleExecutable still
# points to "Cataclysm.sh" so we keep the filename; the build pipeline
# overwrites that file with this content.

set -e

RES="$(cd "$(dirname "$0")/../Resources" && pwd)"
cd "$RES"

USER_SOUND_DIR="$HOME/Library/Application Support/Cataclysm/sound"
BUNDLE_CC="$RES/data/sound/CC-Sounds"
USER_CC="$USER_SOUND_DIR/CC-Sounds"
# /releases/latest/download/<name> is a GitHub redirect to the latest
# non-draft, non-prerelease release's matching asset. Resolves once a
# v* release ships cc-sounds.zip; pre-releases don't satisfy this URL.
CC_URL="https://github.com/busterbogheart/Cataclysm-DDA-multi/releases/latest/download/cc-sounds.zip"

if [ ! -d "$BUNDLE_CC" ] && [ ! -d "$USER_CC" ]; then
  CHOICE=$(osascript -e 'button returned of (display dialog "CC-Sounds pack not installed (~135 MB). Download now? Skip to play silently." buttons {"Skip", "Download"} default button "Download" with title "Cddacoop")' 2>/dev/null || echo Skip)
  if [ "$CHOICE" = "Download" ]; then
    mkdir -p "$USER_SOUND_DIR"
    # Kick off the download in a Terminal window so the user sees curl's
    # progress bar. Launcher exits — user re-launches the app once done.
    osascript -e "tell application \"Terminal\" to do script \"curl -L --fail '$CC_URL' -o /tmp/cc-sounds.zip && unzip -o /tmp/cc-sounds.zip -d '$USER_SOUND_DIR' && rm /tmp/cc-sounds.zip && echo && echo 'Done. Re-launch Cddacoop.'\""
    exit 0
  fi
fi

exec ./cataclysm-tiles
