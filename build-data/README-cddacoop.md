<!-- ============================================================
     SECTION SYNC MAP — this file is the single source of truth.
     Surfaces each `## ` section is published to:
       [zip]  bundled verbatim in the release readme (EVERY section)
       [git]  injected into GitHub README.md  (scripts/sync-readme.mjs, SYNC: markers)
       [site] rendered on cddacoop.com         (build-time fetch of master)

       First launch ...................... [zip][site]
       Playing co-op / Host / Join ....... [zip]
       Connecting ........................ [zip][git] + site (NOT auto-synced —
                                           the site's version is hand-written in
                                           catacoop-site/src/pages/index.astro
                                           with its own accordion/codeblocks;
                                           update BOTH when adding a method)
       Where things live ................. [zip]
       Sound ............................. [zip]
       What works ........................ [zip][git][site]
       Known limits ...................... [zip][git][site]
       FAQ ............................... [zip][git][site]
       Mods .............................. [zip]
       Changelog ......................... [zip][site]
       Getting help / reporting bugs ..... [zip][git]
       Donate ............................ [zip][git]
       Credits ........................... [zip]

     Editing a [git]/[site] section here propagates on the next
     README sync (run scripts/sync-readme.mjs) / site build.
     ============================================================ -->
# CDDA CO-OP

__ Why die alone when you can die together? __

**Project home:** [cddacoop.com](https://cddacoop.com) &middot;
**Repo:** [busterbogheart/Cataclysm-DDA-multi](https://github.com/busterbogheart/Cataclysm-DDA-multi) &middot;
**Upstream:** [CleverRaven/Cataclysm-DDA](https://github.com/CleverRaven/Cataclysm-DDA)

This is a fan-made fork... issues belong on this fork's GitHub or in the cddacoop.com Discord (<https://cddacoop.com/discord>).

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
- **[Radmin VPN](https://www.radmin-vpn.com)** *(Windows only!)* free, no account, no email. Connects the two machines.  If either player is on macOS or Linux, use Tailscale or ZeroTier instead.  Steps:
    1. Both players install Radmin VPN from [radmin-vpn.com](https://www.radmin-vpn.com).
    2. **Host:** *Network* → *Create network* — pick a network name and password then send both to your partner.
    3. **Client:** *Network* → *Join network* — enter the same name and password.
    4. **Verify before launching the game:** you should each see the other machine
       listed with a green dot and a `26.x.y.z` address. If you don't see each other there, the game can't connect either so fix this first.
    5. The host shares their `26.x.y.z` address; the client pastes it into the Join screen as `26.x.y.z:8080`.

- **Router port forwarding** — forward TCP 8080 to the host machine and share
  your public IP. Works without any extra software but requires router access.
- **[playit.gg](https://playit.gg)** *(easiest)* — free, no port forwarding, no
  account needed for the client. Install the playit agent on the host machine,
  add a TCP tunnel on port 8080, and share the address it gives you. Your partner
  pastes it straight into the Join screen.
- **ZeroTier / ngrok** — similar to Tailscale; see
  [cddacoop.com](https://cddacoop.com) for walkthroughs.

### If it just won't connect

Two causes account for nearly every report:

1. **The host isn't listening yet.** The host doesn't open the port until they
   are actually in the world; arming co-op in the menu isn't enough. World
   generation plus character creation can easily take 5–20 minutes, and for that
   whole time your partner gets "connection refused" even though the address and
   the port forwarding are perfectly correct. **Host: get into the world first,
   then tell your partner to join.**
2. **One player is running a commercial VPN.** NordVPN, ExpressVPN, Proton, etc can take over your machine's routing and send the co-op traffic out through
   the VPN exit instead of to your partner, then they drop connections that sit
   idle. This is not the same thing as Tailscale/Radmin, which exist to connect
   two players. **So, turn the commercial VPN off on both machines.** 

If it's neither: check that both of you are on the same build (the game refuses
mismatched versions), and that you can see each other's machine in
your VPN/Tailscale app before launching the game.

---

## Where things live

Saves, worlds, config, screenshots, memorials:

- **Windows:** `.\save\`, `.\config\`, etc. next to the exe (portable)
- **macOS:** `~/Library/Application Support/Cddacoop/` — kept **separate** from
  single-player CDDA (which uses `.../Cataclysm/`), so co-op and SP saves, fonts,
  and options never mix
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
and extract it into `Cddacoop\data\sound\` (so you have
`Cddacoop\data\sound\CC-Sounds\`) — the Windows build is portable, so it all
lives next to the exe. On **Linux**, download `cc-sounds.zip` from the
releases page and extract it into `Cddacoop/data/sound/` (so you have
`Cddacoop/data/sound/CC-Sounds/`).

---

## What works

- Movement, melee combat, smashing terrain and furniture
- Vehicle driving by client and host (including being a passenger)
- Item pickup, drop, wear, wield, use (single-tile and adjacent)
- Eating, drinking, short consumption activities (both players
  simultaneously)
- Long activities for either player — crafting (including batches),
  butchering, field dressing, skinning and dissecting, construction and
  building, reading, disassembly, foraging, fishing, workouts and vehicle
  repair
- Host and client appear as NPC proxies in each other's world with
  correct clothing and skin tone
- Monster sync with damage messages
- Client ranged / thrown / spell damage forwarded and applied
  server-side
- Field sync (blood, fire, acid)
- Tile sync (terrain, furniture, items, graffiti)
- Trap sync — client triggers traps server-side
- Shared overmap notes — a note either player adds or deletes shows up on the
  other's overmap
- Vehicle state sync — part HP, fuel, name messages
- Vehicle construction — install and remove parts
- Drop-into-vehicle (drop items into the storage of a vehicle you're
  standing on)
- Vehicle extras — honking, cruise control, the handbrake and hotwiring
- Grabbing and dragging furniture or vehicles, and toggling hauling
- In-game text chat: bind `Co-op chat` to a key to message your
  partner... yelling still works too
- Trading: full trade menu between players (in addition to the new "Pass item" action, below)
- Different z-levels — ground and overmap stay in sync when players are
  on different levels; ramps and bridges work now as expected
- Separate vehicles — both players can drive their own vehicles
- Loot zones — the joining player's `Loot/sort` command runs on the host, so
  it sorts into the zones the host actually drew
- Quicksave — either player can trigger it, and both get an on-screen
  confirmation once the host's copy is up to date
- Fast-forward — turns skip ahead when both players are in long waits or long activities
- Co-op HUD — bottom-left panel showing partner name, movement mode, mood, worst-body-part HP bar,
  what your partner is actually doing (recipe name, batch count and progress, like
  `crafting bandage x5 12% ▶▶`, where the arrows mean fast-forward is engaged), and ping in ms
- Partner menu co-op special actions — bump into your partner to open it: "*Tap on shoulder*"
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
- **The joining player's bionics don't reach the host.** Stats, skills,
  proficiencies, mutations, worn gear and martial-arts style all sync across to
  your partner's copy of you — bionics don't. Passive CBM bonuses and active
  bionic powers won't apply to anything the host resolves on your behalf, so a
  heavily bionic character will underperform in shared combat.
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

### How does saving work?

The host's save holds the shared world (map, your partner's inventory, kill count, everything simulated), and each
player's own local save holds their character (stats, skills, appearance + mutations).  That's what "Load existing character" restores when
you join.

Quicksave: the most reliable option, both players get an on-screen confirmation once the host's copy is up to date, so you know it worked.
Save & quit: also saves everything on both sides, just without the round-trip confirmation. 
If you're the one joining save & quit periodically so your character carries over.  Ideally you quicksave together right before you stop for the day, so both saves are fresh and in sync.

### What about keeping save files with new releases?

Your saves carry over, the save format is the same across every co-op release, so there's nothing 
to convert:

- **Windows:** The build is portable, so each download is a new folder. Just copy
  your old `save\` (and `config\` if you want your settings) into the new folder next to the new `.exe`.
- **macOS:** Nothing actually. Saves live in `~/Library/Application Support/Cddacoop/`,
  outside the app, so a new `.app` finds them automatically; just replace the app.
- **Linux:** Same as Windows — copy `save/` (and `config/`) folders into the new `Cddacoop/` folder next to the binary.

### Is it free?

Free and open source. It's a fork of the experimental branch of CDDA.

### What version of CDDA is this based on?

It's a fork of CDDA experimental, including all changes from 0.I (Ito).  Any specific vetted fixes or cool features are pulled in from experimental manually, instead of weekly upstream pulls which inherited potential issues and partial features.  

### How does the network layer work?

It's open source. The whole networking layer is public, anyone can read exactly what a connection can and can't do.  The protocol only carries game actions and state like moves, chat, tile/monster deltas, and saves. 

The connection itself isn't encrypted. It's plain TCP/JSON without TLS. On a LAN this is a non-issue (traffic never leaves your network). For internet play, running it over Tailscale or playit.gg (see `Connecting`) wraps it in an encrypted tunnel, which I'd recommend over basic router port-forwarding if that matters to you.
Password protection exists, but there's no UI for it yet, so **both** players have to launch from the command line for it to work.  The host passes `--host --password <string>`; the joining player passes `--client <address>:<port> --password <string>`.  The server rejects any join attempt with the wrong one.  Joining from the in-game `CO-OP > Join` menu always sends an empty password, so don't set a password unless both of you are starting the game from the command line.  On Windows you can make a shortcut and add the flags to it.  For a custom port use `--port <portnumber>`.


---

## Mods

Most content mods work fine.  This co-op fork is built on standard CDDA. 
**When you build a co-op world, mods are color-coded
in the list and the info panel explains why**.

- **Red = won't work in co-op.** These can't be selected, and the host can't create a world that contains one.
- **Orange = may break.** You can still enable these, after a heads-up dialog;
  some features just won't sync cleanly to the other player.  Or maybe it'll work perfectly. 
- **Everything else bundled with the game works** as far as we know. If you find
  one that misbehaves in co-op, please report it (see below) and we'll add it here.
- **Mods you installed yourself are always orange**, whatever they are. That's
  not a verdict on the mod — we simply haven't tested it, and the game says so
  rather than pretending it knows. Plenty of them will be perfectly fine.

**Won't work (Red):** Sky Island, Isolation Protocol, Magiclysm, Mind Over
Matter (and its Knacks-only variant), Xedra Evolved, Aftershock.  Most of these
rely on travel to separate map layers and/or sweeping scripted powers that can't
be kept in sync.  Aftershock is the odd one out: its distinctive map specials
(exoplanet start pads, alien biomes) aren't streamed host to client, so the
joining player's world generates its own terrain at the same coordinates and the
two of you end up standing on completely different maps.

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

<!-- Stage the NEXT release's notes under "### Unreleased". The site filters out any
     non-dated heading, so Unreleased notes do NOT publish. At release, rename the
     heading to "### YYYY-MM-DD" and it publishes to the site + in-zip notes at once. -->

### 2026-08-17

- **Long actions are now about !!30X FASTER!! when you do them together.** When both
  players are busy with something long (crafting, sleeping, reading, waiting, etc) the
  game now skips ahead at about 135 game turns per second instead of 4 or 5. A one-hour craft
  ingame should now only take about 20 seconds. A few reasons:
    - The host was rebuilding and re-sending the entire world state just to tell your partner
      "nothing to do this turn." 
    - Every tile AND every vehicle in view was being scanned twice per turn.
    - Turns are now granted in batches (shoulda thought of this before).
- **The co-op panel now tells you what your partner is actually doing.** Instead of just
  "crafting", it names the item, how many they're batching, and whether the game is
  currently in FF mode like: `crafting bandage x5 12% ▶▶`.
- **Ten more activities no longer slow down the game:** Hotwiring, picking a lock,
  mining, chopping a tree, boltcutting, hacksawing, oxytorching, prying, clearing
  rubble and milking now count toward the fast forward changes above.
- **Log now reports why a fast-forward didn't happen.** If co-op feels slow, the
  log now names which player and which activity is holding it back. 
- **A couple deadlock and freeze fixes.** 1) Opening your message log while crafting, as
  the joining player, froze you completely and 2) sometimes if the host finished crafting first
  the game would deadlock (red/red).
- **Pulping progress finally makes sense.** It used to show a stuck 0% while you pulped,
  and the co-op panel said "pulping" while the progress bar above it said "smashing" —
  now both agree, and it counts corpses as you go (like `2/2`), starting at 1 instead of
  sitting at 0 until the first one's already dead.
- **Logs got a lot smaller.** The host used to write one line per changed tile — a
  normal session was 3.6MB of just that. Gone now; logs stay verbose everywhere else,
  just not there.
- **First real Windows ↔ macOS test.** A Windows host and a Mac client played together
  over the internet with no issues — first time we've confirmed cross-platform co-op
  actually works.

### 2026-08-02

- **A few fixes for joining issues** Several separate problems:
    - **Your partner's join could quietly die while they made their character.** Between the
      moment the client connects and the moment it actually joins, the player is in character
      creation and neither machine sent a single byte during that time. Routers, 
      firewalls and VPNs treat a connection quiet like that as dead
      and silently cut it off, so the join was sent into a connection that no longer existed. The
      host now sends a tiny keepalive to connecting players, and the client re-dials if it
      finds the link dead at the moment it needs it.
    - **If the join fails, you're now told.** Previously the client finished character
      creation and dropped into its own empty world about seventy tiles from the host, with
      no message at all. Now it says so plainly.
    - **The host warns you that your partner can't connect yet.** The game doesn't open the
      port until you're actually in the world, so from the moment you set up hosting through
      worldgen, character creation and the intro/flavor text, anyone trying to join just gets
      "connection refused."
- **New Advice: Turn off your commercial VPN.** NordVPN, ExpressVPN, Proton and friends hijack your
  machine's routing and send co-op traffic out through the VPN exit instead of to your
  partner. This is not the same as Tailscale or Radmin, which exist to connect the two players.
- **Vehicle cargo duplication is fixed (for real this time?)** — loot inside a car, bus, or cart would quietly clone itself...
 Two separate causes were found and fixed.
- **An update from upstream experimental CDDA** — this build pulls in roughly six weeks of changes from
  the main fork (early June through late July).  The parts most worth knowing about:
    - **Three fixes that protect your save.** Containers can no longer be nested more than 12
      deep and putting an item inside itself no longer crashes the game. Deeply-nested containers were a known way
      to corrupt a save file.
    - **Various annoyances now gone.** Car and motorbike batteries lying on the ground no longer
      act as obstacles you have to walk around. Elevators that led nowhere are fixed. Labyrinth
      treasure-room terminals work again. Cold-blooded characters get their heat speed bonus in
      hot weather like they're supposed to. You can no longer target-practice in pitch darkness.
      Mines no longer spawn absurd numbers of sacks holding one unit of sand each. The overmap
      map item covers twice the range.
    - **New !!content!! (SPOILER ALERT)** A large batch for Xedra, the new
      XedraWood mod filling out (giant serpents and their nests, magical tattoos, etc), Mind Over Matter, Aftershock, Magiclysm and Bombastic Perks, and a pile of new items; manga and manga-themed bookshelves, wyrmhound mutant dogs, a dragonfly head mutation, and an Obsolete Academic profession.
    - Some upstream changes were left out on purpose because they broke content
      checks when tested including some chemical and container recipe changes, and a mutant limb update.  If they're fixed
      later we can pull those in. 

### 2026-07-26

- **Fixed a crash when both players reach/wield the same item** — picking up, examining, or opening the advanced inventory manager while your partner had one of those same screens open on their end could crash their game.  Now it just errors, and only sometimes. 
- **Indoor lighting mis-match fixes** — sometimes the client would see a different lighting in a room than the host, especially in taller buildings. The client now gets the host's real ceiling data instead of guessing at its own.
- **Finally fixed the stamina bug** — even with matching speed/cost, the client could visibly lag further and further behind over a long walk or run.  This should be fixed and you can move together as expected. 

### 2026-07-21

- **Item duplication fixes** — closed several ways items could clone themselves when moving them between a vehicle and your inv
- **More vehicle sync stability** — spawning or parking several vehicles near each other no longer makes an earlier one vanish or collapse into nothingness on the client's screen
- **More accurate ping reporting in co-op HUD** — the latency readout now measures true round-trip time so idle players no longer see a fake multi-second 'ping'.  A good ping will render in green (0 - 180ms).

### 2026-07-18

- **Safer rejoining for client** — rejoining after a crash or a save-and-quit is much more solid now. No more instantly fast-forwarding 17 million turns, and your partner correctly reappears on your screen — previously the host could come back invisible with a blank `[?]` / `----` status in the co-op panel.
- **Combat race condition fix** — players hitting the same monster in the same instant could mis-count the damage, should be more predictable now. 
- **Streamer IP privacy** — hosts who stream can now hide their join address and the game copies it to your clipboard so you can send it privately to your partner.
- **Co-op chat in the Interact menu** — you can now reach co-op chat straight from the Actions Menu (`G` by default).
- **Co-op HUD is movable** — the co-op partner panel can be repositioned to the top or bottom of the game screen
- **Faster, quieter loading** — squashed a bug that spewed ~110,000 lines of debug noise into the log on every world load, and reworded the client join popup so it no longer looks frozen while it's connecting.
- **More new loading screen art** — additional pieces from fans & sharper (linear filter) rendering.  Thanks to Anton, Maslin and Torvie!!  Also an incredibly sick antlered horror co-op battle by deviantart.com/epsilon-shadow (and one more from them)

### 2026-07-10

- **Sleep enabled** — at your own risk... but testing shows it works for the most part.
- **More vehicle controls enabled for the joining player** — hotwiring a locked car and triggering/smashing the alarm now actually work for the client player (previously silently did nothing). Everything else in the vehicle menus that still isn't synced (headlights and other electronics, doors/curtains, camera, bike rack) now tells you clearly it is not supported instead of pretending to work.
- **Better vehicle sync** — potential fix for a case where a vehicle would be invisible to one player, and a case where an unrelated vehicle sitting in the same spot could get swapped in by mistake.
- **New loading screen art!!** finally

### 2026-07-06

- **Tilesets included now** — all the tilesets ship in every release, like the upstream CDDA download
- **Custom hosting port** — hosts can now set a custom listen port from the host
  menu (or `CDDA_MP_PORT` env var for the nerds) instead of defaulting to 8080.
- **Better reconnect logic** — Connection hiccups of different sorts should now be noticed
  better and reconnect, eventually.  Testing shows the game surviving 5–15 second drops in both directions (client losing the
  host, host losing the client).
- **A 'busy' host no longer blocks client from joining** — Previously a client couldn't join
  if the host in that moment had their inventory or morale dialog up, (other panels too caused this). 
- **Co-op kill tally in the HUD☠** — a running count of monsters killed for both players
- **Non-US build crash fix?** — fixed a crash in non-US builds related to float number parsing (commas vs periods)
- **TCP optimizations** — host now compresses world updates that it sends every turn; packs its messages into fewer packets 
  and also sends them off immediately instead of waiting around to send a batch. Should be noticeable moreso over VPNs and long 
  distance play.
- **Rejoining after a disconnect follows you properly now** — if the host moved around while you
  were disconnected, rejoining used to leave you stuck at your old spot, unable to move, with your
  partner's info blank on the HUD. Reconnecting now properly catches you back up to where they are.


### 2026-06-29

- **Killing monsters as the client is fixed** — monsters you kill as the joining
  player no longer resurrect or drop duplicate corpses, hopefully.
- **Better partner stats + ping** — the co-op panel now
  shows your partner's **mood**, **health** (worst-hurt body part), and **ping** in ms.
- **Monsters aggro on both players fairly** — monsters used to prefer the
  client instead of whoever's actually closer... they should no longer pile onto the
  client player.
- **Construction works in co-op** — small changes to make construction mostly usable.  In-progress
  tiles now sync both directions, and build progress stays in sync.
- **Fewer co-op crashes & lockups** — fixed a crash when a mutated character
  (e.g. with a tail) joined; the host can now save & quit and toggle Safe Mode
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

- Discord: <https://cddacoop.com/discord>
- GitHub issues & feature requests: <https://github.com/busterbogheart/Cataclysm-DDA-multi/issues>

**Co-op bugs need logs from both players.** Most sync issues (desync,
resurrecting monsters, connection failures) only make sense when the host's and
the client's logs are lined up side by side, one is rarely enough. 
The co-op logs are the important ones:

- **Host player:** `cdda-mp-server.log`
- **Joining player:** `cdda-mp-client.log`
- Locations (all in your home folder):
  - **Windows:** in your user folder — `C:\Users\<you>\cdda-mp-server.log` or
    `cdda-mp-client.log` (paste `%USERPROFILE%` into Explorer's address bar)
  - **macOS/Linux:** `~/cdda-mp-server.log` or `~/cdda-mp-client.log` (your home
    directory — e.g. `/home/<you>/` on Linux, `/Users/<you>/` on macOS)

The standard CDDA logs help too especially for crashes:

- **Windows:** `cata.log` and `debug.log` next to the exe
- **macOS:** `~/Library/Application Support/Cddacoop/cata.log` and `debug.log`
- **Linux:** `cata.log` and `debug.log` inside the `Cddacoop/` folder

When reporting a bug, please attach **both players'** `cdda-mp-*.log` files (plus
`cata.log`/`debug.log` if a crash was involved). 

---

## Donate

If you'd like to support development: [GitHub Sponsors](https://github.com/sponsors/busterbogheart) &middot; [Ko-fi](https://ko-fi.com/cddacoop) &middot; [Boosty](https://boosty.to/cddacoop).  Thank you!  Sponsor reward ideas welcome...

---

## Credits

Forked from
[CleverRaven/Cataclysm-DDA](https://github.com/CleverRaven/Cataclysm-DDA)
experimental. All upstream contributors retain credit — see the
[contributors graph](https://github.com/CleverRaven/Cataclysm-DDA/graphs/contributors).

Site hero art by my hero [Delicadeath](https://reddit.com/u/Delicadeath).
Code and content under [CC-BY-SA 3.0](https://creativecommons.org/licenses/by-sa/3.0/).



