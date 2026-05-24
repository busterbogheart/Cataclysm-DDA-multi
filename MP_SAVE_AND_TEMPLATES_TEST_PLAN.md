# MP test plan — main-menu Co-op, templates sync, save handshake

Three independent features. Run them in this order.

---

## Setup

- **HOST (M4):** binary at `/Users/ethankemp/Cataclysm-DDA-multiplayer/cataclysm-tiles`.
- **CLIENT:** Intel mini (192.168.1.25), same binary after Intel rebuild.
- Logs land at `/tmp/cdda-mp-server.log` and `/tmp/cdda-mp-client.log`.
- `grep SESSION-END /tmp/cdda-mp-*.log` and `grep TEMPLATES /tmp/cdda-mp-*.log` are your friends.

---

## Test 1 — Main-menu Co-op submenu

### 1a. Host from menu (no CLI flags)

1. Launch `./cataclysm-tiles` on the M4 (no `--host`, no `--client`, no `start-mp.sh`).
2. Main menu shows a single new item `Co-op` between `Load` and `World`.
3. Hit `O` (or click `Co-op`) → chooser pops up with `Host a session` / `Join a session` / `Cancel`.
4. Pick `Host a session` (hotkey `h`).
5. Expected: popup "Hosting on port 8080…", footer banner turns green: *"Co-op: hosting on port 8080 — waiting for partner"*.
6. Dismiss the popup. Pick `New Game` → custom character → enter the world.

### 1b. Join from menu

1. On the Intel client, launch `./cataclysm-tiles` (no flags).
2. Hit `O` for `Co-op` → chooser pops up.
3. Pick `Join a session` (hotkey `j`).
4. Enter `192.168.1.<host-lan-ip>` (or `100.x.y.z` over Tailscale). No port = 8080.
5. Expected: "Connected to …" popup, banner becomes *"Co-op: connected to host — pick New Game or Load to enter"*.
6. Pick `Load` or `New Game` to enter the session.

### 1c. Connection failure path

1. Pick `Co-op` → `Join a session`. Enter a bogus IP (e.g. `1.2.3.4`).
2. Expected: ~5 s wait, then "Could not connect to 1.2.3.4:8080." popup. Banner stays empty (client mode reverts).
3. Cancel button on the IP prompt also exits cleanly.
4. Picking `Cancel` from the Co-op chooser dismisses it with no side effects.

### 1d. Pre-existing CLI flags still work

1. Launch with `--host` → banner shows host status as before (sanity check that CLI path wasn't broken).
2. Launch with `--client 192.168.1.x:8080` → banner shows client status.

---

## Test 2 — Templates wire-sync on join

### 2a. Pre-test

- On host machine, create 2 custom-character templates with distinct names: `host-only-A.template`, `shared-name.template`. Both live in `~/Library/Application Support/Cataclysm/templates/`.
- On client machine, create 2 with: `client-only-B.template`, `shared-name.template` (intentionally same name as host).
- Confirm the four files exist before the session.

### 2b. Sync on join

1. Start an MP session (use Test 1's menu path or CLI flags — doesn't matter).
2. Once both players are in-game, check `~/Library/.../templates/` on each side:
   - **Host** now has: `host-only-A`, `shared-name` (original), and **`client-only-B`** (received from client).
   - **Client** now has: `client-only-B`, `shared-name` (original), and **`host-only-A`** (received).
   - **Neither side's `shared-name` was overwritten** — confirm via `diff` or `stat -f "%Sm" *.template`. Local always wins.
3. `grep TEMPLATES /tmp/cdda-mp-*.log` shows:
   - "sent list, n=2" on each side.
   - "requested 1" on each side (the missing one).
   - "sent 1 requested" + "wrote received 'client-only-B'" (host log) / "wrote received 'host-only-A'" (client log).
   - "skip overwrite of existing: shared-name" on at least one side.

### 2c. Use received templates

1. Either player goes to `New Game → Custom Character → Load Template`.
2. The just-received template name appears in the list. Loading it should produce the same character its original creator built.

### 2d. Edge: no templates on one side

1. Host has 0 templates, client has 1. Join.
2. Expected: host's "sent list, n=0", client's "nothing to request", but client receives host's request and sends data. After: host has the 1 template the client had.

---

## Test 3 — Save+quit handshake (v1)

### 3a. Host initiates quit

1. Start MP session, both players in-game.
2. Client should be doing something (idle or in an activity — either is fine).
3. On host, hit `Esc → Save and quit`, confirm `Y`.
4. **On client side:** an in-game message appears: *"Your partner is leaving. The session will end shortly."* (warning color). Shortly after, the existing "lost connection" / disconnect handling fires.
5. `grep SESSION-END /tmp/cdda-mp-*.log`:
   - host log: `SESSION-END: host notified client`
   - client log: `SESSION-END RECV: host is leaving`

### 3b. Client initiates quit

1. Restart session. Both in-game.
2. On client, hit `Esc → Save and quit` (renders as "Disconnect and quit?" for client mode). Confirm `Y`.
3. **On host side:** message appears: *"Your partner is leaving. The game has been saved."* The host's world is quicksaved before the TCP disconnect lands. Host can keep playing solo (proxy NPC body remains as a corpse-shaped scaffold).
4. `grep SESSION-END /tmp/cdda-mp-*.log`:
   - client log: `SESSION-END: client notified host`
   - host log: `SESSION-END RECV: client is leaving — auto-saving host`

### 3c. Confirm the host-side auto-save actually happened

1. After 3b, host's `~/Library/Application Support/Cataclysm/save/<world>/` should have an updated mtime on the world files (compare `stat -f "%Sm" *` before the client quit and after).
2. Host loads the same save in a later session — the proxy NPC of the client's last-known position should be there.

### 3d. "Unable to save" fallback path still notifies

1. Manufacture a save failure if you can (or trust the code path). The third call to `mp_notify_session_ending()` in the `save_is_dirty && query_yn(...)` branch fires the same notification.

---

## Regression checks

- Single-player mode unaffected: launch without `--host` / `--client`, pick `New Game`, play a few turns, save+quit. No co-op banner. No SESSION-END / TEMPLATES log lines emitted.
- Existing `start-mp.sh host` / `client` paths still work — they just call the same binary with the same flags.
- Death screen path on host (proxy dies on host) still fires `notify_client_host_died` unchanged.

---

## Known limitations (documented, not bugs)

- **Client's own `.sav` isn't written on quit** under the current architecture. Session-end notification is messaging-only on the client side. Address in a follow-up if/when the client save model changes.
- **Templates sync is one-shot at join.** Creating a template mid-session does not propagate — quit and rejoin to sync.
- **`shared-name` collision**: local always wins. No conflict UI. If you need the partner's version, rename your local copy first.
