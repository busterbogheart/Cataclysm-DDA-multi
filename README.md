# Cataclysm: Dark Days Ahead CO-OP?!

Why die alone when you can die together?

**Project home:** [cddacoop.com](https://cddacoop.com) &middot;
**Upstream:** [CleverRaven/Cataclysm-DDA](https://github.com/CleverRaven/Cataclysm-DDA)

## Contents

- [Multiplayer (This co-op fork)](#multiplayer-this-co-op-fork)
  - [Download a release](#download-a-release)
  - [Start a session](#start-a-session)
  - [Connecting](#connecting)
    - [Same network (LAN)](#same-network-lan)
    - [Across the internet](#across-the-internet)
  - [What works](#what-works)
  - [Current limitations](#current-limitations)
  - [Reporting bugs](#reporting-bugs)
- [Building from source](#building-from-source)
  - [macOS](#macos)
  - [Windows](#windows)
  - [Linux](#linux)
- [Frequently Asked Questions](#frequently-asked-questions)
- [Contribute](#contribute)

## Multiplayer (This co-op fork)

This fork adds experimental co-op multiplayer. One player hosts; a second player connects as a client.

---

### Download a release

Prebuilt binaries are on the [Releases page](https://github.com/busterbogheart/Cataclysm-DDA-multi/releases): **macOS (universal — Apple Silicon + Intel)**, **Windows x64**, and **Linux x64**.

**macOS first launch:** unzip, then right-click `Cddacoop.app` → **Open**. Gatekeeper blocks unsigned apps on a normal double-click, so this dance is required once per machine.

**Windows first launch:** SmartScreen will warn — click **More info** → **Run anyway**.

The host listens on **TCP port 8080**; allow it through your firewall when prompted. For play across the internet see below.

---

### Start a session

From the main menu, pick **CO-OP**.

- **Host:** CO-OP > Host > New character (or Load existing character). Pick or create a world. Share your IP with your partner.
- **Join:** CO-OP > Join > paste the host's IP (recent hosts are remembered). New character or Load existing character.

---

### Connecting

<!-- SYNC:connecting section="Connecting" -->
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
<!-- /SYNC:connecting -->

---

### What works

<!-- SYNC:what-works section="What works" -->
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
<!-- /SYNC:what-works -->

### Current limitations

<!-- SYNC:known-limits section="Known limits" -->
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
<!-- /SYNC:known-limits -->

---

### Reporting bugs

<!-- SYNC:reporting-bugs section="Getting help / reporting bugs" -->
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
<!-- /SYNC:reporting-bugs -->

---

<!-- SYNC:donate section="Donate" -->
If you'd like to support development: [GitHub Sponsors](https://github.com/sponsors/busterbogheart) &middot; [Ko-fi](https://ko-fi.com/cddacoop) &middot; [Boosty](https://boosty.to/cddacoop).  Thank you!  Sponsor reward ideas welcome...
<!-- /SYNC:donate -->

---

## Building from source

> **`SDL3=0` is required.** The released binaries (and the commands below) build against **SDL2**. The Makefile otherwise defaults to SDL3, which this fork doesn't ship yet — and the SDL2 prerequisites listed here won't satisfy an SDL3 build — so pass `SDL3=0` exactly as shown. (Switching SDL major version requires a `make clean` first.)

### macOS

Prerequisites (via Homebrew): `sdl2 sdl2_image sdl2_mixer sdl2_ttf freetype gettext ccache`

**macOS 12+ (Monterey and newer):**
```sh
make -j$(sysctl -n hw.logicalcpu) TILES=1 SOUND=1 SDL3=0 LINTJSON=0 PCH=0 cataclysm-tiles
```

**macOS 11 (Big Sur) and older / Intel** — Homebrew bottles on newer macOS may reference symbols not available on 11.x, so build natively on the target machine. Apple clang 12 doesn't recognize some GCC warning flags in the Makefile; silence them with `CXXFLAGS="-Wno-unknown-warning-option"`:
```sh
brew install sdl2 sdl2_image sdl2_mixer sdl2_ttf freetype gettext ccache
CXXFLAGS="-Wno-unknown-warning-option" make -j$(sysctl -n hw.logicalcpu) TILES=1 SOUND=1 SDL3=0 LINTJSON=0 PCH=0 cataclysm-tiles
```

Or, with MacPorts SDL via `pkg-config` and Clang:

```sh
export PKG_CONFIG_PATH="/opt/local/lib/pkgconfig"
make -j$(sysctl -n hw.logicalcpu) NATIVE=osx CLANG=1 TILES=1 SOUND=1 SDL3=0 PCH=0 LINTJSON=0 cataclysm-tiles
```

### Windows

Builds via MSYS2 / MinGW-w64. See [`.github/actions/build-windows/action.yml`](./.github/actions/build-windows/action.yml) for the full toolchain list used by the release workflow. From an MSYS2 MINGW64 shell with those packages installed:

```sh
make -j$(nproc) MSYS2=1 STATIC=1 TILES=1 SOUND=1 SDL3=0 LINTJSON=0 PCH=0 RELEASE=1 cataclysm-tiles.exe
```

### Linux

CDDA's native platform — the simplest build. On Debian/Ubuntu:

```sh
sudo apt install g++ make git pkg-config ccache \
  libsdl2-dev libsdl2-image-dev libsdl2-mixer-dev libsdl2-ttf-dev \
  libfreetype6-dev gettext libncursesw5-dev zlib1g-dev
make -j$(nproc) TILES=1 SOUND=1 SDL3=0 LINTJSON=0 PCH=0 RELEASE=1 cataclysm-tiles
./cataclysm-tiles
```

(Fedora: `sudo dnf install gcc-c++ make git SDL2-devel SDL2_image-devel SDL2_mixer-devel SDL2_ttf-devel freetype-devel gettext ncurses-devel zlib-devel ccache`.)

**For co-op, build from the same commit as your partner** — the version handshake compares commit identity, so a Linux client joins a Mac/Windows host fine as long as the commits match (`git checkout <release-sha>` before building). Prebuilt Linux x64 tarballs are also published on the [releases page](https://github.com/busterbogheart/Cataclysm-DDA-multi/releases) alongside the Mac/Windows builds.

---

## Frequently Asked Questions

<!-- SYNC:faq section="FAQ" -->
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
Password protection exists but there's no UI yet for it.  A host launched from the command line can pass `--host --password <string>` and the server will reject any join attempt with the wrong one.  On Windows you can alternatively make a shortcut and add `--host --password <string>`.  For custom port use `--port <portnumber>`.
<!-- /SYNC:faq -->

---


## Contribute

The original upstream repo for Cataclysm: Dark Days Ahead is the result of contributions from over 1000 volunteers under the Creative Commons Attribution ShareAlike 3.0 license. The code and content of the game is free to use, modify, and redistribute for any purpose whatsoever. See https://creativecommons.org/licenses/by-sa/3.0/ for details.
Some code distributed with the project is not part of the project and is released under different software licenses; the files covered by different software licenses have their own license notices.

Please see [CONTRIBUTING.md](./CONTRIBUTING.md) for details.

Special thanks to the contributors, including but not limited to, people below:
<a href="https://github.com/cleverraven/cataclysm-dda/graphs/contributors">
  <img src="https://contrib.rocks/image?repo=cleverraven/cataclysm-dda" />
</a>

Made with [contrib.rocks](https://contrib.rocks).

---
