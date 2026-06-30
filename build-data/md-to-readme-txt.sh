#!/usr/bin/env bash
# Render the canonical markdown README to a readable plaintext README.txt for the
# download zips. build-data/README-cddacoop.md stays the single source of truth;
# the build actions pipe it through this at package time instead of copying it raw
# (which leaked ##, **, [](), ` markup into the .txt).
#
# Dependency-light on purpose — sed + awk only — so it runs identically in the
# Linux/macOS runners and the Windows MSYS2 shell. Not a full markdown engine;
# just the constructs this README uses.
#
# Usage: md-to-readme-txt.sh <input.md>  > README.txt
set -eu

sed -E \
  -e 's@<(https?://[^>]+)>@\1@g' \
  -e 's/\[([^]]+)\]\(([^)]+)\)/\1 (\2)/g' \
  -e 's/\*\*([^*]+)\*\*/\1/g' \
  -e 's/`([^`]+)`/\1/g' \
  -e 's/&middot;/·/g' \
  -e 's/&gt;/>/g' \
  -e 's/&lt;/</g' \
  -e 's/&amp;/\&/g' \
  "$1" | awk '
  # Strip HTML comment blocks (dev-only SECTION SYNC MAP, release-staging notes)
  # so they never leak into the player-facing zip README. Handles single-line and
  # multi-line; the README keeps `<!--`/`-->` on their own lines.
  /<!--/ { in_comment = 1 }
  in_comment { if ( /-->/ ) { in_comment = 0 } next }
  # Drop the staged "### Unreleased" changelog section — those are notes for the
  # NEXT release and must not appear in a shipped zip (mirrors the site, which
  # filters non-dated changelog headings). Skip from the Unreleased heading until
  # the next heading at level 1-3 (the first dated "### YYYY-..." entry, or the
  # following "## " section); deeper "####" lines inside it are skipped too.
  /^###[[:space:]]+[Uu]nreleased[[:space:]]*$/ { in_unreleased = 1; next }
  in_unreleased {
    if ( /^#{1,3}[[:space:]]+/ ) { in_unreleased = 0 }   # next heading ends the skip; fall through to print it
    else { next }
  }
  # Headings: drop the leading #s, UPPERCASE for emphasis, and underline h1/h2.
  /^#{1,6}[[:space:]]+/ {
    hashes = $0
    sub(/[^#].*$/, "", hashes)
    level = length(hashes)
    text = $0
    sub(/^#{1,6}[[:space:]]+/, "", text)
    print toupper(text)
    if (level <= 2) {
      u = ""
      n = length(text)
      for (i = 0; i < n; i++) { u = u "-" }
      print u
    }
    next
  }
  # Horizontal rule -> a visible divider.
  /^[[:space:]]*---+[[:space:]]*$/ {
    print "------------------------------------------------------------"
    next
  }
  { print }
'
