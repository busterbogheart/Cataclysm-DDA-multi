#!/bin/sh
# Cddacoop launcher.
#
# Replaces the upstream Cataclysm.sh inside the .app bundle. Drives the
# Host / Join / Single-player menu and the first-run CC-Sounds download.
# CFBundleExecutable still points to "Cataclysm.sh" so we keep the
# filename — the build pipeline overwrites that file with this content.
#
# Source-tree devs continue to use start-mp.sh; this launcher only runs
# for users who downloaded a packaged .app from a release.

set -e

RES="$(cd "$(dirname "$0")/../Resources" && pwd)"
cd "$RES"

# ---- osascript helpers ------------------------------------------------------

ask_button() {
  # $1=message $2=left-button $3=right-button (default). Echoes button text.
  osascript -e "button returned of (display dialog \"$1\" buttons {\"$2\", \"$3\"} default button \"$3\" with title \"Cddacoop\")" 2>/dev/null
}

ask_choice() {
  # $1=quoted-comma-separated-items  $2=prompt
  osascript -e "choose from list {$1} with prompt \"$2\" with title \"Cddacoop\"" 2>/dev/null
}

ask_text() {
  osascript -e "text returned of (display dialog \"$1\" default answer \"\" with title \"Cddacoop\")" 2>/dev/null
}

# ---- First-run sound pack check --------------------------------------------

USER_SOUND_DIR="$HOME/Library/Application Support/Cataclysm/sound"
BUNDLE_CC="$RES/data/sound/CC-Sounds"
USER_CC="$USER_SOUND_DIR/CC-Sounds"
# /releases/latest/download/<name> is a GitHub redirect to the latest
# non-draft release's matching asset. Works once v0.1.0 ships; draft
# test releases won't satisfy this URL until promoted.
CC_URL="https://github.com/busterbogheart/Cataclysm-DDA-multi/releases/latest/download/cc-sounds.zip"

if [ ! -d "$BUNDLE_CC" ] && [ ! -d "$USER_CC" ]; then
  CHOICE=$(ask_button "CC-Sounds pack not installed (~135 MB). Download now? Skip to play silently." "Skip" "Download")
  if [ "$CHOICE" = "Download" ]; then
    mkdir -p "$USER_SOUND_DIR"
    # Kick off the download in a Terminal window so the user sees curl's
    # progress bar. Launcher exits — user re-launches the app once done.
    osascript -e "tell application \"Terminal\" to do script \"curl -L --fail '$CC_URL' -o /tmp/cc-sounds.zip && unzip -o /tmp/cc-sounds.zip -d '$USER_SOUND_DIR' && rm /tmp/cc-sounds.zip && echo && echo 'Done. Re-launch Cddacoop.'\""
    exit 0
  fi
fi

# ---- Main menu --------------------------------------------------------------

MODE=$(ask_choice '"Single-player", "Host a session", "Join a session"' "Cddacoop")
case "$MODE" in
  "Single-player")
    exec ./cataclysm-tiles
    ;;
  "Host a session")
    exec ./cataclysm-tiles --host
    ;;
  "Join a session")
    IP=$(ask_text "Host address (e.g. 100.64.0.5 or host.com:8080)")
    [ -z "$IP" ] && exit 0
    # Default to port 8080 when the user didn't include one.
    case "$IP" in *:*) ;; *) IP="$IP:8080" ;; esac
    exec ./cataclysm-tiles --client "$IP"
    ;;
  *)
    # User cancelled or osascript returned "false".
    exit 0
    ;;
esac
