# CDDA CO-OP

Why die alone when you can die together?

**Project home:** [cddacoop.com](https://cddacoop.com) &middot;
**Repo:** [busterbogheart/Cataclysm-DDA-multi](https://github.com/busterbogheart/Cataclysm-DDA-multi) &middot;
**Upstream:** [CleverRaven/Cataclysm-DDA](https://github.com/CleverRaven/Cataclysm-DDA)

Hey its a fan-made fork... issues belong on this fork's GitHub or in the cddacoop.com Discord.

---

## First launch

1. Unzip wherever you like
2. **macOS:** Open `Cddacoop.app` — first launch needs **right-click → Open**
   to bypass Gatekeeper (unsigned app). If nothing happens, run
   `xattr -cr Cddacoop.app` in Terminal first.
3. **Windows:** Run `cddacoop.bat` (or `cataclysm-tiles.exe` directly).
   SmartScreen may warn — click **More info → Run anyway**.

The main menu is straight experimental CDDA, kept up to date, pretty much.
The **CO-OP** menu item is new.  Otherwise this plays as single-player CDDA.

---

## Playing co-op

Pick **CO-OP** from the main menu, then:

### Host a session

The host runs the full world simulation — put the faster machine on
hosting duty if your hardware differs.

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

- **macOS:** `~/Library/Application Support/Cataclysm/`
- **Windows:** `.\save\`, `.\config\`, etc. next to the exe (portable)

Safe to delete the whole folder for a clean slate.

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
- **Host activity & health broadcast** — the host's current activity,
  progress, HP, and movement mode stream to the client every turn so
  the co-op HUD always reflects what the partner is actually doing

---

## Known limits

- **Same reality bubble**, centered on the host. A client more than
  about 65 tiles from the host falls outside the simulated area; entities
  there don't tick.
- **Sleep** runs but two-player sleep dynamics aren't fully validated.
  Coordinate with your partner or expect rough edges around
  partner-status messages.
- **Multi-hour crafts / reading** not yet validated across real
  network latency.

---

## Changelog

### 2026-05-27

- **Trading between players** — totally works
- **Pass item** -- new feature for co-op!  bump into your partner and see a new
option called "Pass item" (g) to hand over one item without the full trade menu.  Like
throwing your man a loaded Saiga.  Bindable in options menu.  
- **Overmap note sync** — any map notes and custom markers show on both players' maps

### 2026-05-25

- **Grab & haul** — client can grab furniture and push/pull/shift it;
  hauling items works for both players
- **Turn indicators** — red/green co-op panel border shows whose turn
  it is at a glance
- **Vehicle construction** — client can install and remove vehicle parts

### 2026-05-23 — 2026-05-24

- **CO-OP main menu** — self-contained Host/Join flow replaces the old
  shell-script launcher; worlds tagged as co-op get a badge
- **Partner help** —  new menu item "Help with task" in the partner menu (when
you bump into them) lets you assist with crafting, construction, and vehicle work
- **Fast-forward** — if both players are in passive activities (sleep, wait) or
longer activities turns skip ahead quickly
- **Host driving sync** — vehicles driven by the host broadcast
  per-tile position updates to the client; both players can drive now, 
  even SEPARATE VEHICLES.   
- **Recent hosts** — join screen remembers the last few IPs
- **Partner activity and health status bar** -- see your buddy's health (calculated as worst bodypart) and
what they're doing in the new co-op panel (also move mode so you can run when they do)


### 2026-05-16 — 2026-05-18

- **Lockstep turn system** — grant/wait cycle keeps both players
  synchronized; host waits for client before advancing monsters
- **Activity sync** — long actions (eating, dropping, reloading) work
  for both players with proper lockstep integration
- **Partner menu** — new partner menu item (when you bump into them)
called 'Tap on shoulder' which interrupts their waiting ('wait for several minutes` action)

### 2026-05-07 — 2026-05-12

- **Initial multiplayer** — TCP server/client, NPC proxy used as framework
for remote/client player, tile/monster/field sync, movement dispatch
- **Combat** — both players can attack in turn against the same enemy... or 
multiple.  melee and ranged damage forwarded and applied server-side w/ proper kill attribution
- **Vehicles** — client can drive, turn, toggle engine; vehicle state
  (part HP, fuel) synced to client
- **Traps & graffiti** — synced in tile broadcast; client triggers
  traps server-side
- **Appearance sync** — skin tone, hair, clothing, wielded weapon, and
  sprite facing all mirrored between players

---

## Getting help / reporting bugs

- Discord: <https://discord.gg/MzBD4v3xAU>
- GitHub issues: <https://github.com/busterbogheart/Cataclysm-DDA-multi/issues>

Logs:

- **macOS:** `~/Library/Application Support/Cataclysm/cata.log` and `debug.log`
- **Windows:** `cata.log` and `debug.log` next to the exe

When reporting a bug, attach both logs and mention which preview
build you're on (the version is in the window title bar).

---

## Credits

Forked from
[CleverRaven/Cataclysm-DDA](https://github.com/CleverRaven/Cataclysm-DDA)
experimental. All upstream contributors retain credit — see the
[contributors graph](https://github.com/CleverRaven/Cataclysm-DDA/graphs/contributors).

Site hero art by my guy [Delicadeath](https://reddit.com/u/Delicadeath).
Code and content under [CC-BY-SA 3.0](https://creativecommons.org/licenses/by-sa/3.0/).


