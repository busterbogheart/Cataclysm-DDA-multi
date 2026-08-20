#!/usr/bin/env bash
#
# Regenerate lang/fork/cddacoop.pot — the translation template for the strings
# THIS FORK adds, as opposed to the ~115k it inherits from upstream — and merge
# the result into every lang/fork/<lang>.po without losing existing work.
#
# Run this whenever fork strings are added, removed, or reworded. Nothing else
# updates the template, and a stale template silently means new strings are
# untranslatable and reworded ones lose their translation.
#
#   ./tools/update_fork_pot.sh
#
# How the fork's strings are isolated: extract a full .pot from HEAD, extract
# another from a worktree at the merge-base with upstream, and keep the entries
# that appear only in the first. A plain `git diff` against the merge-base is
# NOT good enough — this tree carries hundreds of commits from upstream PR
# branches that upstream never landed on master under those SHAs, and a diff
# credits every one of their strings to us.
#
# Requires: gettext (msgmerge, msgattrib), python3 with polib — the same polib
# that lang/update_pot.sh already needs.

set -euo pipefail
cd "$(dirname "$0")/.."

FORK_DIR="lang/fork"
TEMPLATE="${FORK_DIR}/cddacoop.pot"
CACHE_DIR="${FORK_POT_CACHE:-${TMPDIR:-/tmp}/cddacoop-fork-pot}"
mkdir -p "${CACHE_DIR}"

command -v msgmerge >/dev/null || { echo "error: gettext not installed"; exit 1; }
python3 -c 'import polib' 2>/dev/null || {
  echo "error: python3 module 'polib' not found (pip install polib)"; exit 1; }

echo "==> Extracting strings from HEAD"
./lang/update_pot.sh >/dev/null
cp lang/po/cataclysm-dda.pot "${CACHE_DIR}/head.pot"

BASE_SHA="$(git merge-base HEAD upstream/master)"
BASE_POT="${CACHE_DIR}/merge-base-${BASE_SHA}.pot"

if [ -f "${BASE_POT}" ]; then
  echo "==> Baseline .pot for ${BASE_SHA:0:10} already cached"
else
  echo "==> Extracting baseline from a worktree at ${BASE_SHA:0:10} (slow, cached after this)"
  WT="${CACHE_DIR}/worktree"
  rm -rf "${WT}"
  git worktree add --detach "${WT}" "${BASE_SHA}" >/dev/null 2>&1
  # The extractor at that commit predates upstream's fix for the "dimension"
  # JSON type and aborts on it. dummy_parser emits nothing, so patching it here
  # cannot change the msgid set we compare against.
  python3 - "${WT}" <<'PY'
import sys, os
p = os.path.join(sys.argv[1], "lang/string_extractor/parser.py")
s = open(p).read()
if '"dimension"' not in s:
    s = s.replace('    "dream": parse_dream,',
                  '    "dimension": dummy_parser,\n'
                  '    "dimension_region_layout": dummy_parser,\n'
                  '    "dream": parse_dream,')
    open(p, "w").write(s)
PY
  ( cd "${WT}" && ./lang/update_pot.sh >/dev/null )
  cp "${WT}/lang/po/cataclysm-dda.pot" "${BASE_POT}"
  git worktree remove --force "${WT}"
fi

echo "==> Isolating this fork's own strings"
python3 - "${CACHE_DIR}/head.pot" "${BASE_POT}" "${TEMPLATE}" <<'PY'
import json, sys, polib

head, base, out_path = sys.argv[1], sys.argv[2], sys.argv[3]
ours, upstream = polib.pofile(head), polib.pofile(base)
key = lambda e: (e.msgctxt or "", e.msgid, e.msgid_plural or "")
seen = {key(e) for e in upstream}

# Only src/ entries. The data/ differences are dominated by those pulled-in
# upstream PR branches rather than anything we wrote.
fork = [e for e in ours
        if key(e) not in seen
        and any(f.startswith("src/") for f, _ in e.occurrences)]

out = polib.POFile()
out.metadata = {
    "Project-Id-Version": "cddacoop fork strings",
    "MIME-Version": "1.0",
    "Content-Type": "text/plain; charset=UTF-8",
    "Content-Transfer-Encoding": "8bit",
}
for e in fork:
    out.append(e)

# The co-op tips are invisible to the extractor: snippet.py reads the standard
# "text" key and our sibling "cooptext" array is not it. Append them by hand,
# with the same prefix text_snippets.cpp bakes on at load — the runtime lookup
# key includes it, so a template built from the bare tip text would produce
# translations that never match anything.
PREFIX = "<color_pink>CO-OP:</color> "
tips = 0
for obj in json.load(open("data/core/tips.json")):
    for t in obj.get("cooptext", []):
        body = t["text"] if isinstance(t, dict) else t
        out.append(polib.POEntry(
            msgid=PREFIX + body, msgstr="",
            occurrences=[("data/core/tips.json", "")],
            comment=("Co-op tip shown on the loading screen and main menu.\n"
                     "Keep the <color_pink>CO-OP:</color> prefix exactly as-is; "
                     "translate only the sentence after it.")))
        tips += 1

out.save(out_path)
print(f"    {len(fork)} strings from src/ + {tips} co-op tips = {len(out)} entries")
PY

echo "==> Merging into each translation (existing work preserved)"
for f in "${FORK_DIR}"/*.po; do
  [ -e "${f}" ] || continue
  before=$(msgattrib --translated --no-obsolete "${f}" 2>/dev/null | grep -c '^msgid "' || true)
  msgmerge --quiet --update --backup=none "${f}" "${TEMPLATE}"
  after=$(msgattrib --translated --no-obsolete "${f}" 2>/dev/null | grep -c '^msgid "' || true)
  fuzzy=$(grep -c '^#, fuzzy' "${f}" || true)
  printf "    %-28s translated %s -> %s   fuzzy %s\n" \
         "$(basename "${f}")" "$((before>0?before-1:0))" "$((after>0?after-1:0))" "${fuzzy}"
done

echo "==> Validating"
for f in "${TEMPLATE}" "${FORK_DIR}"/*.po; do
  msgfmt -c -o /dev/null "${f}" 2>/dev/null || { echo "    INVALID: ${f}"; exit 1; }
done
echo "    all files valid"
echo
echo "Done.  Entries msgmerge marked 'fuzzy' had their English reworded — a"
echo "translator should re-check those; the old translation is kept meanwhile."
