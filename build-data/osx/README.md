# CDDA CO-OP

Why die alone when you can die together?

**Project home:** [cddacoop.com](https://cddacoop.com) &middot;
**Repo:** [busterbogheart/Cataclysm-DDA-multi](https://github.com/busterbogheart/Cataclysm-DDA-multi) &middot;
**Upstream:** [CleverRaven/Cataclysm-DDA](https://github.com/CleverRaven/Cataclysm-DDA)

Hey its a fan-made fork... issues belong on this fork's GitHub or in the cddacoop.com Discord.

---

## First launch

1. Unzip wherever you like 
2. Open `Cddacoop.app` 
The main menu is straight experimental CDDA, kept up to date, pretty much. The **CO-OP** menu item is new.
Otherwise this plays as single-player CDDA.

---

## Playing co-op

Pick **CO-OP** from the main menu, then:

### Host a session

`CO-OP > Host > New character` (or `Load existing character`). Pick
or create a world; co-op worlds get a badge in the menu so you can
tell them apart from solo/SP worlds.

Your partner needs your IP. On the same LAN, your local IP works. Across
networks, [cddacoop.com](https://cddacoop.com) has step-by-step
walkthroughs for Tailscale, ZeroTier, ngrok, and router port
forwarding. The host always listens on TCP port 8080; only the route
between the two machines changes.  I use Tailscale at the moment. 

### Join a session

`CO-OP > Join`, then paste the host's IP. Recent hosts are remembered
between launches. Pick `New character` or `Load existing character`; 
the client auto-creates a scratch world and teleports to the host on
connect.

---

## Where things live

Saves, worlds, config, screenshots, memorials:

```
~/Library/Application Support/Cataclysm/
```

Each macOS user account gets its own. Safe to delete the whole folder
for a clean slate.

---

## Sound

The CC-Sounds pack (~135 MB) is **not bundled** in this zip to keep
the download small. On first launch the app offers to download it for
you, or you can skip and play silently. The pack lands at
`~/Library/Application Support/Cataclysm/sound/CC-Sounds/` once
installed.

---

## What works

- Movement, melee combat, smashing terrain and furniture
- Item pickup, drop, wear, wield, use (single-tile `g` and area `Q`)
- Eating, drinking, short consumption activities (both players
  simultaneously)
- Host and client appear as NPC proxies in each other's world with
  correct clothing and skin tone
- Monster sync with damage messages
- Client ranged / thrown / spell damage forwarded and applied
  server-side
- Field sync (blood, fire, acid)
- Tile sync (terrain, furniture, items, graffiti)
- Trap sync — client triggers traps server-side
- Vehicle driving by client; turning deducts AP, cruise speed is free
- Vehicle state sync — part HP, fuel, name messages
- Vehicle construction — install and remove parts
- Drop-into-vehicle (drop items into the storage of a vehicle you're
  standing on)
- **Co-op HUD** — bottom-left panel showing partner name, movement
  mode, worst-body-part HP bar, current activity + progress, and
  calendar drift
- **Partner menu** — bump into your partner to open it; "Tap on
  shoulder" interrupts their wait, "Help with task" works like
  single-player NPC help

---

## Known limits

- **Two players only** — third connection is rejected.
- **One reality bubble**, centered on the host. A client more than
  ~66 tiles from the host falls outside the simulated area; entities
  there don't tick.
- **Sleep** runs but two-player sleep dynamics aren't fully validated.
  Coordinate with your partner or expect rough edges around
  partner-status messages.
- **Multi-hour crafts / reading** not yet validated across real
  network latency.
- **No reconnect** — if the TCP connection drops, both players quit
  and re-launch.
- **Save format** is shared with upstream CDDA but the MP fork adds
  fields; fork saves aren't interchangeable with upstream.
- **macOS Apple Silicon only** in this preview. Intel and Windows are
  coming as the CI matrix stabilizes.

---

## Getting help / reporting bugs

- Discord: <https://discord.gg/MzBD4v3xAU>
- Matrix: `#cddacoop:matrix.org`
- GitHub issues: <https://github.com/busterbogheart/Cataclysm-DDA-multi/issues>

Logs land in:

```
~/Library/Application Support/Cataclysm/cata.log
~/Library/Application Support/Cataclysm/debug.log
```

When reporting a bug, attach both logs and mention which preview
build you're on (the version is in the filename of the zip you
downloaded — e.g. `preview-arm-20260524c-1ac4e15`).

---

## Credits

Forked from
[CleverRaven/Cataclysm-DDA](https://github.com/CleverRaven/Cataclysm-DDA)
experimental. All upstream contributors retain credit — see the
[contributors graph](https://github.com/CleverRaven/Cataclysm-DDA/graphs/contributors).

Site hero art by [Delicadeath](https://reddit.com/u/Delicadeath).
Code and content under [CC-BY-SA 3.0](https://creativecommons.org/licenses/by-sa/3.0/).

---

Have fun. Try not to die. (Or die together — that's the point.)
