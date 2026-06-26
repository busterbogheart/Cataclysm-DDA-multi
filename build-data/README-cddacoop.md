<!-- ============================================================
     SECTION SYNC MAP — this file is the single source of truth.
     Surfaces each `## ` section is published to:
       [zip]  bundled verbatim in the release readme (EVERY section)
       [git]  injected into GitHub README.md  (scripts/sync-readme.mjs, SYNC: markers)
       [site] rendered on cddacoop.com         (build-time fetch of master)

       First launch ...................... [zip][site]
       Playing co-op / Host / Join ....... [zip]
       Connecting ........................ [zip][git]
       Where things live ................. [zip]
       Sound ............................. [zip]
       What works ........................ [zip][git][site]
       Known limits ...................... [zip][git][site]
       FAQ ............................... [zip][git][site]
       Mods .............................. [zip]
       Changelog ......................... [zip][site]
       Getting help / reporting bugs ..... [zip][git]
       Credits ........................... [zip]

     Editing a [git]/[site] section here propagates on the next
     README sync (run scripts/sync-readme.mjs) / site build.
     ============================================================ -->
# CDDA CO-OP

Why die alone when you can die together?

**Project home:** [cddacoop.com](https://cddacoop.com) &middot;
**Repo:** [busterbogheart/Cataclysm-DDA-multi](https://github.com/busterbogheart/Cataclysm-DDA-multi) &middot;
**Upstream:** [CleverRaven/Cataclysm-DDA](https://github.com/CleverRaven/Cataclysm-DDA)

This is a fan-made fork... issues belong on this fork's GitHub or in the cddacoop.com Discord (<https://discord.gg/MzBD4v3xAU>).

---

## First launch

1. Unzip anywhere
2. **macOS:** Open `Cddacoop.app` — first launch needs **right-click → Open**
   to bypass Gatekeeper (unsigned app). If nothing happens, run
   `xattr -cr Cddacoop.app` in Terminal first.
3. **Windows:** Run `cataclysm-tiles.exe`. SmartScreen may warn — click
   **More info → Run anyway**.
4. **Linux:** Extract the tarball and run `./cddacoop.sh` from inside the
   `Cddacoop/` folder — the launcher loads the bundled libraries, so don't run
   `cataclysm-tiles` directly. Built on Ubuntu 22.04, so you need glibc 2.35+
   (most current distros). If it won't start, make it executable first:
   `chmod +x cddacoop.sh cataclysm-tiles`.

The main menu is straight experimental CDDA, kept up to date, pretty much.
The **CO-OP** menu item is new.  Otherwise this plays as single-player CDDA.

---

## Playing co-op

Pick **CO-OP** from the main menu, then:

### Host a session

The host runs the full world simulation, so put the better machine on
hosting duty if your hardware differs.

`CO-OP > Host > New character` (or `Load existing character`). Pick
or create a world; co-op worlds get a badge in the menu so you can
tell them apart from solo/SP worlds.

Your partner needs your IP — see **Connecting** below for same-network (LAN)
and across-the-internet options.

### Join a session

`CO-OP > Join`, then paste the host's IP. Recent hosts are remembered
between launches. Pick `New character` or `Load existing character`; 
the client auto-creates a scratch world and teleports to the host on
connect.

---

## Connecting

The host always listens on **TCP port 8080**; only the route between the two
machines changes. Allow it through the host's firewall when prompted.

### Same network (LAN)

If both players are on the same Wi-Fi/LAN, no VPN is needed — the host just
shares its local IP:

- **macOS host:** `ipconfig getifaddr en0` (try `en1` if that's empty)
- **Windows host:** `ipconfig` — use the IPv4 Address (looks like `192.168.x.y`)

The client pastes that into `CO-OP > Join`. Port defaults to 8080.

### Across the internet

You need one of these to route your partner's traffic to your machine:

- **[playit.gg](https://playit.gg)** *(easiest)* — free, no port forwarding, no
  account needed for the client. Install the playit agent on the host machine,
  add a TCP tunnel on port 8080, and share the address it gives you. Your partner
  pastes it straight into the Join screen.
- **[Tailscale](https://tailscale.com)** *(what I use)* — free VPN that makes both
  machines appear on the same LAN. By default you
  and your friend are on **separate networks** and won't see each other. You have
  to put both machines on the same network first:
    1. Both players install Tailscale and sign in (any login works, and email can be the same actually).
    2. **Get on the same network-- pick one:**
       - **Same account (simplest):** both sign in to the *same* Tailscale
         account. Both machines then show up together automatically.
       - **Share the device:** if you each have your own account, the host opens
         the [Tailscale admin console](https://login.tailscale.com/admin/machines),
         clicks the **⋯** next to their machine → **Share…**, and sends the invite
         link to their friend. The friend opens the link and accepts. (Repeat the
         other way if needed.)
    3. **Verify before launching the game:** in the admin console / Tailscale app,
       each player should see the *other* person's machine listed and marked
       **Connected**. If you don't see their machine here, the game can't connect
       either so fix this first.
    4. The host shares their Tailscale IP (the `100.x.y.z` address next to their
       machine name). The friend pastes it into the Join screen as `100.x.y.z:8080`. (You can save the IP in the
       game with a name like `Joe's Tailscale`)
- **Router port forwarding** — forward TCP 8080 to the host machine and share
  your public IP. Works without any extra software but requires router access.
- **ZeroTier / ngrok** — similar to Tailscale; see
  [cddacoop.com](https://cddacoop.com) for walkthroughs.

---

## Where things live

Saves, worlds, config, screenshots, memorials:

- **macOS:** `~/Library/Application Support/Cddacoop/` — kept **separate** from
  single-player CDDA (which uses `.../Cataclysm/`), so co-op and SP saves, fonts,
  and options never mix
- **Windows:** `.\save\`, `.\config\`, etc. next to the exe (portable)
- **Linux:** `save/`, `config/`, etc. inside the `Cddacoop/` folder, next to the
  binary (portable — the launcher runs the game from there)

Safe to delete the whole folder for a clean slate.

---

## Sound

The CC-Sounds pack (~135 MB) is **not bundled** in this zip to keep
the download small. On **macOS** first launch the app offers to download
it for you, or you can skip and play silently; it lands at
`~/Library/Application Support/Cddacoop/sound/CC-Sounds/`. On **Windows**,
download `cc-sounds.zip` from the
[releases page](https://github.com/busterbogheart/Cataclysm-DDA-multi/releases/latest)
and extract it into `%APPDATA%\Cataclysm\sound\` (so you have
`...\sound\CC-Sounds\`). On **Linux**, download `cc-sounds.zip` from the
releases page and extract it into `Cddacoop/data/sound/` (so you have
`Cddacoop/data/sound/CC-Sounds/`).

---

## What works

- Movement, melee combat, smashing terrain and furniture
- Vehicle driving by client and host (including being a passenger)
- Item pickup, drop, wear, wield, use (single-tile and adjacent)
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
- Vehicle state sync — part HP, fuel, name messages
- Vehicle construction — install and remove parts
- Drop-into-vehicle (drop items into the storage of a vehicle you're
  standing on)
- In-game text chat: bind `Co-op chat` to a key to message your
  partner... yelling still works too
- Trading: full trade menu between players (in addition to the new "Pass item" action, below)
- Different z-levels — ground and overmap stay in sync when players are
  on different levels; ramps and bridges work now as expected
- Separate vehicles — both players can drive their own vehicles
- Fast-forward — turns skip ahead when both players are in long waits or long activities
- Co-op HUD — bottom-left panel showing partner name, movement mode, mood, worst-body-part HP bar, 
  current activity + progress, and ping in ms
- Partner menu co-op special actions — bump into your partner to open i: "*Tap on shoulder*"
 interrupts their wait, "*Help with task*" works like single-player NPC help, 
 "*Pass item*" quickly tosses them one thing and "*High five*" gives a small morale bonus (just like real life)

---

## Known limits

- **Same reality bubble** (for now), centered on the host. There's only one simulated
  area (not one per player), so you have to stay near each other, within
  about 60 tiles. You'll get escalating warnings as you drift apart, and past
  about 68 tiles you leave the host's simulated zone and things break (vehicle
  physics especially). The world does **not** auto-pause... so try and close the gap when
  the warning shows. 
- **The host has to stay running.** It's a listen server, not a dedicated one:
  if the host quits or loses connection, the session ends for both players.
- **Sleep** should work, but isn't fully developed yet. Coordinate sleep times 
  with your partner or expect the occasional issue.

---

## FAQ

### Does CO-OP change hardware requirements?

It's about the same as single-player CDDA. The simulation is CPU-bound and single-threaded, RAM is modest (~0.5 to 2 GB), and the tiles renderer barely touches the GPU.

Co-op does not double the CPU cost. There is one shared simulation area centered on the host, not one per player, so the host runs roughly single-player plus a little overhead, not 2x.

Obviously the new requirement is the network; the host streams world updates to the client every turn, so a faster connection and hardware helps here.

### So the better computer/connection should be the host?

Yes. The host runs the full world simulation, serializes the changed state every turn, and uploads it to the client, so it does the heavy lifting.

The client mostly renders what the host sends and waits its turn, so it is lighter on CPU; and memory use is similar to single-player.

### How does it work under the hood?

One machine is the host: it runs the real game. The client sends actions over a TCP connection and gets back the world state to draw, tiles, monsters, the other player.

The second player is wired in as a special NPC on the host (like a proxy), so existing systems like combat, driving and melee already treat them as a real character. The client's input is intercepted at the same point the game already routes keypresses, then run on the host.

There is one shared simulation bubble centered on the host, and a per-turn grant/wait handshake keeps both players in lockstep so the world does not desync.

### Is it free?

Free and open source. It's a fork of the experimental branch of CDDA.

### What version of CDDA is this based on?

It's a fork of CDDA experimental, kept current with the upstream code. This means everything in the 0.I release plus current experimental (June 2026 and any future updates).

---

## Mods

Most content mods work fine.  This co-op fork is built on standard CDDA. 
**When you build a co-op world, mods are color-coded
in the list and the info panel explains why**.

- **Red = won't work in co-op.** These can't be selected, and the host can't create a world that contains one.
- **Orange = may break.** You can still enable these, after a heads-up dialog;
  some features just won't sync cleanly to the other player.  Or maybe it'll work perfectly. 
- **Everything else works** as far as we know. If you find a mod that misbehaves
  in co-op, please report it (see below) and we'll add it here.

**Won't work (Red):** Sky Island, Isolation Protocol, Magiclysm, Mind Over
Matter (and its Knacks-only variant), Xedra Evolved.  These rely on travel to
separate map layers and/or sweeping scripted powers that can't be kept in sync.

**May break (Orange):** Xedra Evolved: Innawoods, Bombastic Perks,
Perk Melee System, Bionic Professions, the extra mutation scenarios, Sorcerer,
Crazy Cataclysm, Deadly Bites, Tamable Wildlife, Hunvre, MMA. Typically the
affected feature still works for whoever *triggers* it, but its status/UI or
progression may not show up correctly for the other player.

Why some mods don't fit: the second player is simulated as a special NPC inside
the host's world, and only concrete results like tiles, damage, fields, stat changes etc
 flow back to them. Mods built on dimension-hopping, instanced/pocket maps, or
large scripted-effect/teleport power sets fall outside that model.

---

## Changelog


### 2026-06-26

- **Better partner stats + ping** — the co-op panel now
  shows your partner's **mood**, **health** (worst-hurt body part), and **ping** in ms.
- **Killing monsters as the client is fixed** — monsters you kill as the joining
  player no longer resurrect or drop duplicate corpses, hopefully.
- **Monsters aggro on both players fairly** — monsters used to prefer the
  client instead of whoever's actually closer... they should no longer pile onto the
  client player.
- **Construction works in co-op** — small changes to make construction mostly usable.  In-progress
  tiles now sync both directions, and build progress stays in sync.
- **Fewer co-op crashes & lockups** — fixed a crash when a mutated character
  (e.g. with a tail) joined; the host can now save & quit and toggle safe mode
  even while waiting on a locked turn; the host survives the client disconnecting.
- **Movement & stamina match the host** — client move costs and the stamina
  burned from smashing now behave the same as on the host.


### 2026-06-21

- **Aiming and firing fixed for clients** — the joining player can aim and shoot
  again: no more missing aim panel, dropped keys or the client
  locking up when entering aim mode. 
- **Joining drops you more reliably into your host's world...** — a joining player could spawn in
  a separate, mismatched world instead of next to the host. Hopefully this is vanquished.

### 2026-06-17

- **Z-level fixes** — when you and your partner are on different z-levels, the ground should sync correctly and
  the overmap should match up.  Ramps/bridges should not impede the client anymore. 
- **In-game text chat!** — Bind `Co-op chat` to a key and chat w/ your partner.  Kept in messages
and a new 3-line area above the co-op panel bottom left.  (Alternately you can yell a sentence if you're close enough)

### 2026-06-13

- **Co-op warns you about untested mods** — any enabled mod that isn't bundled with the game (third-party /
  downloaded mods) now shows a co-op warning in the mod menu instead of silently passing. A mod mismatch
  between host and client is a common cause of the joining player crashing, so both players should run the
  exact same mods.
- **Merge with upstream CDDA** — 371 commits of upstream changes: new content, balance and fixes
- **Now on itch.io** — releases now mirror to itch.io alongside the downloads here

### 2026-06-09

- **macOS: co-op keeps its own save folder now** — (Windows and Linux were already separate).  Co-op no longer shares its
  saves, fonts, and options with single-player CDDA (both used the same folder).
  If your menus showed the wrong sans-serif font, that was single-player’s
  config bleeding in, now fixed. Also: co-op reads from this new folder, so existing co-op worlds won’t
  appear automatically.  Copy them over yourself from
  `~/Library/Application Support/Cataclysm/save/<world>` into
  `~/Library/Application Support/Cddacoop/save/`. (The app never touches your
  single-player folder on its own.)

### 2026-06-08

- **Debug: new random monster spawn** — good luck!
- **Better recovery from players deadlocking** — a client whose action got stuck mid-turn now
  recovers on its own instead of forcing a quit.
- **More visibility on game version** — your build's commit hash now shows in the title
  bar and the in-game bug report (debug menu), so you can confirm you're on the same version as your partner  

### 2026-06-05

- **Linux build** — native Linux x64 download now ready...  Untested so please give feedback. 
- **Stamina fix for the joining player (client)** — the client's stamina now recovers at the proper rate instead of crawling, and you no longer rack up extra bleeding, pain, or thirst while it's not your turn.  Hopefully. 

### 2026-06-04

- **Maps now match, finally** — the host's overmap is streamed to the client on join so both players reliably see the 
same towns, roads, and regions on the minimap and overmap.
- **Debug: new random item spawn** — debug spawn menu drops a random item, for fun

### 2026-06-01

- **Combat messages full synced** — Your partner's hit/kill/event messages now correctly
show up, either host or client.  

### 2026-05-31

- **One macOS download for every Mac** — the mac build is now a universal binary that runs on both Silicon and Intel Macs
  (macOS 10.15+)
- **High-five your partner** — new co-op "High five" option!  Slap skin for a
  quick mutual morale boost.  Bindable in options and added to the bump partner menu
  alongside trade, push, pass item, etc. 

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
- **The Dream Begins**

---

## Getting help / reporting bugs

- Discord: <https://discord.gg/MzBD4v3xAU>
- GitHub issues & feature requests: <https://github.com/busterbogheart/Cataclysm-DDA-multi/issues>

**Co-op bugs need logs from both players.** Most sync issues (desync,
resurrecting monsters, connection failures) only make sense when the host's and
the client's logs are lined up side by side, one is rarely enough. 
The co-op logs are the important ones:

- **Host player:** `cdda-mp-server.log`
- **Joining player:** `cdda-mp-client.log`
- Locations:
  - **macOS/Linux:** `/tmp/cdda-mp-server.log` or `/tmp/cdda-mp-client.log`
  - **Windows:** in your user folder — `C:\Users\<you>\cdda-mp-server.log` or
    `cdda-mp-client.log` (paste `%USERPROFILE%` into Explorer's address bar)

The standard CDDA logs help too especially for crashes:

- **macOS:** `~/Library/Application Support/Cddacoop/cata.log` and `debug.log`
- **Windows:** `cata.log` and `debug.log` next to the exe
- **Linux:** `cata.log` and `debug.log` inside the `Cddacoop/` folder

When reporting a bug, please attach **both players'** `cdda-mp-*.log` files (plus
`cata.log`/`debug.log` if a crash was involved). 

---

## Credits

Forked from
[CleverRaven/Cataclysm-DDA](https://github.com/CleverRaven/Cataclysm-DDA)
experimental. All upstream contributors retain credit — see the
[contributors graph](https://github.com/CleverRaven/Cataclysm-DDA/graphs/contributors).

Site hero art by my hero [Delicadeath](https://reddit.com/u/Delicadeath).
Code and content under [CC-BY-SA 3.0](https://creativecommons.org/licenses/by-sa/3.0/).


