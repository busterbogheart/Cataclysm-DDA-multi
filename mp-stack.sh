#!/bin/bash
# Where is each running CDDA instance's main thread, right now?
#
# Replaces the add-a-log-line / rebuild / re-test / read-the-log loop for any
# HANG or "it went silent" bug: sample the live process instead.  The build
# carries -g, so frames come back symbolized with file:line.  Takes ~3s and
# needs no rebuild, no code change, and no reproduction after the fact --
# just run it WHILE it is stuck.
#
# usage: ./mp-stack.sh [seconds]   (default 2)
SECS="${1:-2}"
PIDS=$(pgrep cataclysm-tiles)
[ -z "$PIDS" ] && { echo "no cataclysm-tiles running"; exit 1; }
for P in $PIDS; do
    OUT=$(mktemp /tmp/mpstack.XXXXXX)
    sample "$P" "$SECS" -f "$OUT" >/dev/null 2>&1
    echo "═══ pid $P ═══"
    # Main thread only, our own frames only, de-duplicated to the call chain.
    awk '/Call graph:/,0' "$OUT" \
        | awk '/com.apple.main-thread/,/^ *$/' \
        | grep -E "cataclysm-tiles\)| in libSDL| in libsystem" \
        | sed -E 's/^[ +!:|]*[0-9]+ //; s/ \(in .*\) \+ [0-9]+ +\[[^]]*\]//' \
        | head -30
    rm -f "$OUT"
    echo
done
