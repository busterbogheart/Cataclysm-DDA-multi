# Cataclysm: Dark Days Ahead CO-OP?!

Cataclysm: Dark Days Ahead is a turn-based survival game set in a post-apocalyptic world. While some have described it as a "zombie game", there is far more to Cataclysm than that. Struggle to survive in a harsh, persistent, procedurally generated world. Scavenge the remnants of a dead civilization for food, equipment, or, if you are lucky, a vehicle with a full tank of gas to get you the hell out of Dodge. Fight to defeat or escape from a wide variety of powerful monstrosities, from zombies to giant insects to killer robots and things far stranger and deadlier, and against the others like yourself, who want what you have...

<p align="center">
    <img src="./data/screenshots/ultica-showcase-sep-2021.png" alt="Tileset: Ultica">
</p>

## Downloads

**Releases** - [Stable](https://cataclysmdda.org/releases/) | [Experimental](https://cataclysmdda.org/experimental/)

**Source** - The source can be downloaded as a [.zip archive](https://github.com/CleverRaven/Cataclysm-DDA/archive/master.zip), or cloned from our [GitHub repo](https://github.com/CleverRaven/Cataclysm-DDA/).

[![General build matrix](https://github.com/CleverRaven/Cataclysm-DDA/actions/workflows/matrix.yml/badge.svg)](https://github.com/CleverRaven/Cataclysm-DDA/actions/workflows/matrix.yml)
[![Coverage Status](https://coveralls.io/repos/github/CleverRaven/Cataclysm-DDA/badge.svg?branch=master)](https://coveralls.io/github/CleverRaven/Cataclysm-DDA?branch=master)
[![Open Source Helpers](https://www.codetriage.com/cleverraven/cataclysm-dda/badges/users.svg)](https://www.codetriage.com/cleverraven/cataclysm-dda)
[![Commit Activity](https://img.shields.io/github/commit-activity/m/CleverRaven/Cataclysm-DDA)](https://github.com/CleverRaven/Cataclysm-DDA/graphs/contributors)
[![Lines of Code](https://tokei.rs/b1/github/CleverRaven/Cataclysm-DDA?category=code)](https://github.com/XAMPPRocky/tokei)
[![TODOs](https://badgen.net/https/api.tickgit.com/badgen/github.com/CleverRaven/Cataclysm-DDA)](https://www.tickgit.com/browse?repo=github.com/CleverRaven/Cataclysm-DDA)

### Packaging status

#### Arch Linux

Ncurses and tiles versions are available in the [community repos](https://www.archlinux.org/packages/?q=cataclysm-dda).

```sh
sudo pacman -S cataclysm-dda
sudo pacman -S cataclysm-dda-tiles
```

#### Fedora

Ncurses and tiles versions are available in the [official repos](https://src.fedoraproject.org/rpms/cataclysm-dda).

```sh
sudo dnf install cataclysm-dda
```

#### Debian / Ubuntu

Ncurses and tiles versions are available in the [official repos](https://tracker.debian.org/pkg/cataclysm-dda).

```sh
sudo apt install cataclysm-dda-curses cataclysm-dda-sdl
```

#### Flatpak

Download from [Flathub](https://flathub.org/apps/org.cataclysmdda.CataclysmDDA).

## Multiplayer Fork — Building (macOS)

```sh
make -j$(sysctl -n hw.logicalcpu) TILES=1 SOUND=1 LINTJSON=0 PCH=0 cataclysm-tiles
```

Prerequisites (via Homebrew): `sdl2 sdl2_image sdl2_mixer sdl2_ttf freetype gettext`

## Compile

Please read [COMPILING.md](doc/c++/COMPILING.md) - it covers general information and more specific recipes for Linux, OS X, Windows and BSD. See [COMPILER_SUPPORT.md](doc/c++/COMPILER_SUPPORT.md) for details on which compilers we support. And you can always dig for more information in [doc/](https://github.com/CleverRaven/Cataclysm-DDA/tree/master/doc).

We also have the following build guides:
* Building on Windows with `MSYS2` at [COMPILING-MSYS.md](doc/c++/COMPILING-MSYS.md)
* Building on Windows with `vcpkg` at [COMPILING-VS-VCPKG.md](doc/c++/COMPILING-VS-VCPKG.md)
* Building with `cmake` at [COMPILING-CMAKE.md](doc/c++/COMPILING-CMAKE.md)  (*unofficial guide*)

## Contribute

Cataclysm: Dark Days Ahead is the result of contributions from over 1000 volunteers under the Creative Commons Attribution ShareAlike 3.0 license. The code and content of the game is free to use, modify, and redistribute for any purpose whatsoever. See https://creativecommons.org/licenses/by-sa/3.0/ for details.
Some code distributed with the project is not part of the project and is released under different software licenses; the files covered by different software licenses have their own license notices.

Please see [CONTRIBUTING.md](./CONTRIBUTING.md) for details.

Special thanks to the contributors, including but not limited to, people below:
<a href="https://github.com/cleverraven/cataclysm-dda/graphs/contributors">
  <img src="https://contrib.rocks/image?repo=cleverraven/cataclysm-dda" />
</a>

Made with [contrib.rocks](https://contrib.rocks).

## Community

Forums:
https://discourse.cataclysmdda.org

GitHub repo:
https://github.com/CleverRaven/Cataclysm-DDA

IRC:
`#CataclysmDDA` on [Libera Chat](https://libera.chat), https://web.libera.chat/#CataclysmDDA

Official Discord:
https://discord.gg/jFEc7Yp

## Frequently Asked Questions

#### Is there a tutorial?

Yes, you can find the tutorial in the **Special** menu at the main menu (be aware that due to many code changes the tutorial may not function). You can also access documentation in-game via the `?` key.

#### How can I change the key bindings?

Press the `?` key, followed by the `1` key to see the full list of key commands. Press the `+` key to add a key binding, select which action with the corresponding letter key `a-w`, and then the key you wish to assign to that action.

#### How can I start a new world?

**World** on the main menu will generate a fresh world for you. Select **Create World**.

#### I've found a bug. What should I do?

Please submit an issue on [our GitHub page](https://github.com/CleverRaven/Cataclysm-DDA/issues/) using [bug report template](https://github.com/CleverRaven/Cataclysm-DDA/issues/new?template=bug_report.yaml). If you're not able to, send an email to `kevin.granade@gmail.com`.

#### I would like to make a suggestion. What should I do?

Please submit an issue on [our GitHub page](https://github.com/CleverRaven/Cataclysm-DDA/issues/) using [feature request template](https://github.com/CleverRaven/Cataclysm-DDA/issues/new?template=feature_request.yaml).

---

## Multiplayer (Co-op fork)

This fork adds experimental co-op multiplayer. One player hosts; a second player connects as a client. Both share the same simulated world in real time.

### Prerequisites

Build from source (macOS):

```bash
git clone https://github.com/busterbogheart/Cataclysm-DDA-multiplayer
cd Cataclysm-DDA-multiplayer
brew install sdl2 sdl2_image sdl2_ttf sdl2_mixer freetype
make -j$(sysctl -n hw.logicalcpu) TILES=1 SOUND=1 LINTJSON=0 PCH=0 cataclysm-tiles
```

The Volta world is included in the repo — no save transfer needed.

---

### Quickstart (two machines, same LAN)

**Host** — plays the game and accepts one remote player:

```bash
./cataclysm-tiles --host --world Volta
```

**Client** — connects to the host:

```bash
./cataclysm-tiles --client <host-ip>:8080 --world Volta --client-name YourName
```

Both `--world Volta` arguments are required. They tell each instance to skip the main menu and load straight into the shared world. Without `--world`, the game stops at the main menu and waits for the player to pick a world manually before anything works.

The client teleports to the host's location on the first tick. A debug HUD appears in the bottom-left corner showing connection state, move budget, and queued action.

---

### Dedicated headless server

Runs the simulation with no display. Requires an existing character save in the target world.

```bash
./cataclysm-tiles --server --world Volta
./cataclysm-tiles --server --world Volta --port 9000 --password secret
```

Client connects the same way:

```bash
./cataclysm-tiles --client <host-ip>:9000 --world Volta --client-name YourName --password secret
```

The server ticks the world at 1 game-turn/second and processes player actions at 10 Hz between ticks.

---

### Flag reference

| Flag | Argument | Default | Description |
|---|---|---|---|
| `--host` | — | — | Play normally while hosting a listen server |
| `--server` | — | — | Run as a headless dedicated server (requires `--world`) |
| `--world` | `<name>` | — | World to load on startup — **required for both host and client** |
| `--port` | `<number>` | 8080 | TCP port to listen on (host/server) |
| `--password` | `<string>` | none | Password clients must supply to join |
| `--client` | `<host:port>` | — | Connect to a server as a client |
| `--client-name` | `<name>` | player2 | Your player name shown to the host |

---

### What works

- Movement, melee combat, smashing terrain and furniture
- Item pickup (single tile `g`, all nearby tiles `Q`)
- Host and client appear as NPC proxies in each other's world with correct clothing and skin tone
- Monster sync with damage messages
- Field sync (blood, fire, acid)
- Tile sync (terrain, furniture, items)

### Current limitations

- Dropping items is not yet forwarded to the server
- Both players load from the same character save (unique characters not yet supported)
- Only two players supported
