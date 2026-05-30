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

# Tell dyld to find the bundled SDL2*.framework and libintl.dylib next to
# the binary, instead of in the build-time rpath (~/Library/Frameworks).
# Mirrors what upstream Cataclysm.sh does.
export DYLD_FRAMEWORK_PATH="$RES"
export DYLD_LIBRARY_PATH="$RES"

USER_SOUND_DIR="$HOME/Library/Application Support/Cataclysm/sound"
# /releases/latest/download/<name> is a GitHub redirect to the latest
# non-draft, non-prerelease release's matching asset. Resolves once a
# v* release ships cc-sounds.zip; pre-releases don't satisfy this URL.
CC_URL="https://github.com/busterbogheart/Cataclysm-DDA-multi/releases/latest/download/cc-sounds.zip"

# A soundpack is "installed" if any directory in the bundle's or the user's
# sound dir holds a soundpack.txt manifest — the same marker CDDA scans for
# when discovering soundpacks. Matches CC-Sounds, CC-Sounds-sfx-only,
# CO.AG-music-only, or any pack dropped in manually, so we never re-prompt
# someone who already has sound just because their folder isn't named
# exactly "CC-Sounds".
has_soundpack() {
  for d in "$RES/data/sound" "$USER_SOUND_DIR"; do
    [ -d "$d" ] || continue
    for p in "$d"/*/soundpack.txt; do
      [ -f "$p" ] && return 0
    done
  done
  return 1
}

if ! has_soundpack; then
  CHOICE=$(osascript -e 'button returned of (display dialog "CC-Sounds pack not installed (~135 MB). Download now? Skip to play silently." buttons {"Skip", "Download"} default button "Download" with title "Cddacoop")' 2>/dev/null || echo Skip)
  if [ "$CHOICE" = "Download" ]; then
    mkdir -p "$USER_SOUND_DIR"
    # Download inline. We deliberately do NOT use `tell application "Terminal"
    # to do script` — controlling another app trips the macOS Automation
    # permission prompt ("sh wants to control Terminal.app"). The app was
    # launched from Finder with no attached terminal, so curl runs silently;
    # notifications stand in for the progress bar. display notification /
    # display dialog are posted by our own process and need no permission.
    osascript -e 'display notification "Downloading CC-Sounds (~135 MB)… the game will open when it finishes." with title "Cddacoop"' 2>/dev/null || true
    if curl -L --fail --retry 3 -s "$CC_URL" -o /tmp/cc-sounds.zip \
        && unzip -oq /tmp/cc-sounds.zip -d "$USER_SOUND_DIR"; then
      rm -f /tmp/cc-sounds.zip
      osascript -e 'display notification "CC-Sounds installed." with title "Cddacoop"' 2>/dev/null || true
    else
      rm -f /tmp/cc-sounds.zip
      osascript -e 'display dialog "CC-Sounds download failed. Launching without sound — you can retry next start." buttons {"OK"} default button "OK" with title "Cddacoop"' 2>/dev/null || true
    fi
  fi
fi

exec ./cataclysm-tiles
