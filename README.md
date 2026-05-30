# Cataclysm: Dark Days Ahead CO-OP?!

Why die alone when you can die together?

**Project home:** [cddacoop.com](https://cddacoop.com) &middot;
**Upstream:** [CleverRaven/Cataclysm-DDA](https://github.com/CleverRaven/Cataclysm-DDA)

## Contents

- [Multiplayer (This co-op fork)](#multiplayer-this-co-op-fork)
  - [Download a release](#download-a-release)
  - [Start a session](#start-a-session)
  - [Playing across the internet](#playing-across-the-internet)
  - [What works / Current limitations](#what-works)
- [Building from source](#building-from-source)
- [FAQ](#frequently-asked-questions)
- [Contribute](#contribute)

## Multiplayer (This co-op fork)

This fork adds experimental co-op multiplayer. One player hosts; a second player connects as a client.

---

### Download a release

Prebuilt binaries are on the [Releases page](https://github.com/busterbogheart/Cataclysm-DDA-multi/releases): **macOS Apple Silicon**, **macOS Intel**, and **Windows x64**.

**macOS first launch:** unzip, then right-click `Cddacoop.app` → **Open**. Gatekeeper blocks unsigned apps on a normal double-click, so this dance is required once per machine.

**Windows first launch:** SmartScreen will warn — click **More info** → **Run anyway**.

The host listens on **TCP port 8080**; allow it through your firewall when prompted. For play across the internet see below.

---

### Start a session

From the main menu, pick **CO-OP**.

- **Host:** CO-OP > Host > New character (or Load existing character). Pick or create a world. Share your IP with your partner.
- **Join:** CO-OP > Join > paste the host's IP (recent hosts are remembered). New character or Load existing character.

---

### Same network (LAN)

If both players are on the same Wi-Fi or LAN, no VPN is needed — the host just shares its local IP.

- **macOS host:** `ipconfig getifaddr en0` (try `en1` if that's empty)
- **Windows host:** `ipconfig` — use the IPv4 Address (looks like `192.168.x.y`)

The client pastes that IP into CO-OP > Join. Port defaults to 8080.

---

### Playing across the internet

[cddacoop.com](https://cddacoop.com) has step-by-step instructions for Tailscale, ZeroTier, ngrok, and router port forwarding. The host always listens on port 8080; only the route between the two machines changes.

---

### What works

- Movement, melee combat, smashing terrain and furniture
- Item pickup (single tile `g`, all nearby tiles `Q`), drop, wear, wield
- Item use and wielded-item use forwarded to server
- Eating, drinking, and similar short consumption activities (both players can run them simultaneously)
- Host and client appear as NPC proxies in each other's world with correct clothing and skin tone
- Monster sync with damage messages
- Client ranged/thrown/spell damage forwarded to and applied on the server
- Field sync (blood, fire, acid)
- Tile sync (terrain, furniture, items, graffiti)
- Trap sync — client triggers traps on the server
- Vehicle driving by client (turning deducts AP; cruise speed is free)
- Vehicle state sync (part HP, fuel, name messages)
- Vehicle construction — install and remove parts, including parts spawned via debug
- Drop-into-vehicle (drop items into the storage of a vehicle you're standing on)
- **Co-op HUD** — bottom-left panel showing partner name, movement mode, worst-body-part HP bar, current activity + progress, and calendar drift
- **Partner menu** — bump into your partner to open a menu with 2 new options: "Tap on shoulder" which interrupts their 'wait several turns' command and "Help with task" which is similar to single player NPC help

### Current limitations

- Two players only — no third or fourth slot
- **One reality bubble**, centered on the host. A client more than ~66 tiles from the host falls outside the simulated area; entities there don't tick.
- **Sleep** is functional but two-player sleep dynamics are not yet validated — if one player sleeps and the other keeps acting, the world's lockstep is fine but expect rough edges around partner-status messages.
- **Long crafts (multi-hour)** not yet validated across machines with real network latency.
- **No reconnect** — if the TCP connection drops, both players need to quit and re-launch.
- **Save format** is shared with upstream CDDA but the MP fork adds a few fields; saves between the fork and upstream are not interchangeable.

---

## Building from source

### macOS

Prerequisites (via Homebrew): `sdl2 sdl2_image sdl2_mixer sdl2_ttf freetype gettext ccache`

**macOS 12+ (Monterey and newer):**
```sh
make -j$(sysctl -n hw.logicalcpu) TILES=1 SOUND=1 LINTJSON=0 PCH=0 cataclysm-tiles
```

**macOS 11 (Big Sur) and older / Intel** — Homebrew bottles on newer macOS may reference symbols not available on 11.x, so build natively on the target machine. Apple clang 12 doesn't recognize some GCC warning flags in the Makefile; silence them with `CXXFLAGS="-Wno-unknown-warning-option"`:
```sh
brew install sdl2 sdl2_image sdl2_mixer sdl2_ttf freetype gettext ccache
CXXFLAGS="-Wno-unknown-warning-option" make -j$(sysctl -n hw.logicalcpu) TILES=1 SOUND=1 LINTJSON=0 PCH=0 cataclysm-tiles
```

Or, with MacPorts SDL via `pkg-config` and Clang:

```sh
export PKG_CONFIG_PATH="/opt/local/lib/pkgconfig"
make -j$(sysctl -n hw.logicalcpu) NATIVE=osx CLANG=1 TILES=1 SOUND=1 PCH=0 LINTJSON=0 cataclysm-tiles
```

### Windows

Builds via MSYS2 / MinGW-w64. See [`.github/actions/build-windows/action.yml`](./.github/actions/build-windows/action.yml) for the full toolchain list used by the release workflow. From an MSYS2 MINGW64 shell with those packages installed:

```sh
make -j$(nproc) MSYS2=1 STATIC=1 TILES=1 SOUND=1 LINTJSON=0 PCH=0 RELEASE=1 cataclysm-tiles.exe
```

---

## Frequently Asked Questions



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
