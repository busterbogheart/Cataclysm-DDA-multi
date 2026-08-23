#!/usr/bin/env node
// Single-source-of-truth sync for the README surfaces.
//
// build-data/README-cddacoop.md (the in-zip / site-fetched readme) is the
// CANONICAL source for the player-facing sections that otherwise drift across
// the three places we publish them (zip readme, GitHub README.md, website).
//
// The GitHub README.md keeps GitHub-only content (badges, build-from-source,
// contribute) by hand, but its shared sections are GENERATED: each is wrapped
// in a marker pair that names the canonical section to pull:
//
//   <!-- SYNC:what-works section="What works" -->
//   ...generated bullets...
//   <!-- /SYNC:what-works -->
//
// Usage:
//   node scripts/sync-readme.mjs           rewrite README.md in place
//   node scripts/sync-readme.mjs --check    exit 1 if README.md is out of sync
//                                           (CI uses this so drift fails the build)
//
// No dependencies — plain Node ESM. Edit the canonical file, run this (or let
// the pre-commit/CI check remind you), commit both.
//
// STATUS: as of 2026-08-22, README.md carries NO marker pairs — it was slimmed
// to a landing page that points at cddacoop.com, and the player-facing sections
// now live only in the canonical file (which ships in-zip as README.txt and is
// fetched by the site at build time). With no markers to replace, this script
// is a no-op that reports "already in sync", so the CI gate stays green. It is
// kept, not deleted, so that re-adding a marker pair to README.md immediately
// starts syncing again with no other setup.

import { readFileSync, writeFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, resolve } from "node:path";

const repoRoot = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const CANONICAL = resolve(repoRoot, "build-data/README-cddacoop.md");
const TARGET = resolve(repoRoot, "README.md");
const checkOnly = process.argv.includes("--check");

// Parse a markdown file into { "Section Title": "body…" } keyed by its `## `
// headings. A section body runs until the next `## ` heading; a trailing `---`
// rule and surrounding blank lines are trimmed so injected blocks stay tidy.
function parseSections(md) {
  const sections = {};
  const lines = md.split("\n");
  let title = null;
  let body = [];
  const flush = () => {
    if (title === null) return;
    let b = body.join("\n").replace(/\n+---\s*$/g, "").trim();
    sections[title] = b;
  };
  for (const line of lines) {
    const m = /^##\s+(.+?)\s*$/.exec(line);
    if (m) {
      flush();
      title = m[1];
      body = [];
    } else if (title !== null) {
      body.push(line);
    }
  }
  flush();
  return sections;
}

const canonical = parseSections(readFileSync(CANONICAL, "utf8"));
let target = readFileSync(TARGET, "utf8");

// Replace the content between each marker pair with the named canonical section.
const markerRe =
  /(<!--\s*SYNC:([\w-]+)\s+section="([^"]+)"\s*-->)([\s\S]*?)(<!--\s*\/SYNC:\2\s*-->)/g;

const missing = [];
const updated = target.replace(markerRe, (full, open, key, section, _old, close) => {
  const body = canonical[section];
  if (body === undefined) {
    missing.push(section);
    return full;
  }
  return `${open}\n${body}\n${close}`;
});

if (missing.length) {
  console.error(
    `[sync-readme] canonical section(s) not found in build-data/README-cddacoop.md: ${missing.join(", ")}`
  );
  process.exit(2);
}

if (updated === target) {
  console.log("[sync-readme] README.md already in sync.");
  process.exit(0);
}

if (checkOnly) {
  console.error(
    "[sync-readme] README.md is OUT OF SYNC with build-data/README-cddacoop.md.\n" +
      "             Run: node scripts/sync-readme.mjs   then commit README.md."
  );
  process.exit(1);
}

writeFileSync(TARGET, updated);
console.log("[sync-readme] README.md regenerated from canonical sections.");
