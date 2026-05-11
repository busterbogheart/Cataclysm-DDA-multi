# MP Performance Audit

Every function listed here runs at least once per game turn (host side, client side, or both).

---

## CRITICAL

### #1 — `g_tile_baseline` grows unbounded (memory leak)

**File:** `src/mp_gamestate.cpp` — `build_tile_changes()`

`g_tile_baseline` is an `unordered_map<tripoint_abs_ms, mp_tile_state>` that records the last-seen
terrain/furniture/items/fields for every tile the server has ever scanned. Entries are added but
never removed. As the host moves around the world the map grows without bound. After a long session
it can hold tens of thousands of entries, each storing four strings.

**Fix (not yet implemented):** Evict entries whose absolute coordinates fall outside the current
sync radius by ~20% (a generous margin) whenever the remote player's position changes significantly.
Or replace with a fixed-size LRU cache (e.g. `boost::multi_index` or a hand-rolled ring buffer of
coordinate → state). A simple version: after each `build_tile_changes()` call, erase any entry
where `abs(entry.x - center.x) > EVICT_RADIUS || abs(entry.y - center.y) > EVICT_RADIUS`.

---

### #2 — Host worn JSON rebuilt from scratch every broadcast ✅ FIXED

**File:** `src/mp_gamestate.cpp` — `serialize_remote_player_state()`

Previously, every call to `serialize_remote_player_state()` would:
- Call `host.worn.inv_dump()` to collect all worn item pointers
- Rebuild the entire `host_worn_json` string from scratch
- Call `mp_log("[cdda-mp] host_worn: ...")` — hitting stdout AND the log file unconditionally

The host's clothing rarely changes (only when they wear/take off an item), yet this ran on every
single state broadcast, which happens multiple times per turn.

**Fix applied:** A static signature `g_host_worn_sig_cache` (type IDs + variants + skin_tone +
hair_trait joined as a string) is compared each call. The JSON string `g_host_worn_json_cache` is
only rebuilt when the signature changes. The `mp_log` also only fires on change. The `inv_dump()`
still runs to build the sig, but that is a pointer-only collection (no per-item allocation) and is
O(n) over a small set (typically 5–15 items). Net result: on the vast majority of ticks, the worn
block costs only one `inv_dump()` + one string comparison instead of a full JSON rebuild + log I/O.

---

## MODERATE

### #3 — `mp_log` flushes to stdout + file on every broadcast

**File:** `src/mp_gamestate.cpp` — `mp_log()`

`mp_log()` calls `std::cout << msg << std::endl` (which flushes) and `logfile.flush()` on every
call. Several call sites trigger every tick:
- `build_tile_changes()` logs every tile with changed items
- `serialize_remote_player_state()` used to log `host_worn` every tick (fixed in #2)
- `apply_one_state_message()` logs `"parsing state (N bytes)..."` and `"state applied ok"` every
  incoming packet — on the client this is several times per turn

**Fix:** Wrap high-frequency call sites in a debug flag (`#ifdef CDDA_MP_DEBUG_VERBOSE` or a
runtime bool). Keep error/connect/disconnect logs unconditional. The `endl` flushes are unnecessary
for non-error paths; switching to `'\n'` would also help if verbose logging is kept.

---

### #4 — `build_viewport()` scans 861 tiles per broadcast even in tiles mode

**File:** `src/mp_gamestate.cpp` — `serialize_remote_player_state()`

`build_viewport()` renders a 41×21 ASCII viewport by reading terrain symbols from the map. This
runs unconditionally every broadcast. In SDL tiles mode (the normal play mode) the client already
renders its own tile view and ignores the `"map"` field. The viewport is only useful for a
hypothetical headless/ncurses client.

**Fix:** Guard the call with `if( is_server_mode() )` (headless only) or add a per-session flag
when the client negotiates tiles capability. Short-term: check whether the client ever reads the
`"map"` field; if not, remove it entirely.

---

### #5 — `build_monster_list()` iterates ALL monsters in the tracker

**File:** `src/mp_gamestate.cpp` — `build_monster_list()`

`get_creature_tracker().get_monsters_list()` returns a flat vector of all active monsters in the
reality bubble. `build_monster_list()` iterates the entire list and range-filters inside the loop.
In a populated area this can be hundreds of monsters.

**Fix:** The creature_tracker already supports `get_creatures_in_zone()` / spatial queries in some
CDDA versions. Alternatively, maintain a server-side set of net-ID → last-known-position so only
dirty (moved/damaged) monsters are included in the diff, analogous to the tile baseline approach.

---

### #6 — `update_client_host_npc()` re-applies ally status every position update

**File:** `src/mp_gamestate.cpp` — `update_client_host_npc()`

The `is_player_ally()` check makes the branch cheap on the common path, but `set_fac()`,
`add_npc_follower()`, etc. are called whenever `is_player_ally()` returns false — which can happen
after a load cycle. This is mostly a correctness guard and not a hot path, but it could be made
explicit (only on first spawn and after load) rather than re-checked every tick.

---

## LOW / FUTURE

### #7 — JSON built by string concatenation throughout

All JSON in the MP code is hand-built with `+` concatenation (many small `std::string`
allocations). Switching to `std::ostringstream` with pre-allocated buffers, or a minimal JSON
builder, would reduce allocator pressure on the hot broadcast path. This is a refactor, not a
quick fix — leave for later.

### #8 — `client_resync_worn()` triggers a full worn JSON build on client side

`client_resync_worn()` is called once at join and after every wear/remove, so it is not a hot path.
No change needed.

### #9 — `g_last_monster_hp` and `g_last_bodypart_hp` grow without eviction

Similar to `g_tile_baseline` but bounded in practice by the number of unique monsters ever seen in
a session and the fixed number of body parts (~10). Low priority.

---

## Summary Table

| # | Location | Frequency | Severity | Status |
|---|----------|-----------|----------|--------|
| 1 | `g_tile_baseline` unbounded growth | every broadcast | CRITICAL | open |
| 2 | Host worn JSON rebuilt every tick | every broadcast | CRITICAL | **FIXED** |
| 3 | `mp_log` flushes stdout+file unconditionally | every broadcast | MODERATE | open |
| 4 | `build_viewport()` runs in tiles mode | every broadcast | MODERATE | open |
| 5 | Monster list full scan | every broadcast | MODERATE | open |
| 6 | Ally status re-checked every position update | every state msg | LOW | open |
| 7 | String concat JSON building | every broadcast | LOW | future |
| 8 | `client_resync_worn` on join/change | rare | LOW | fine |
| 9 | `g_last_monster_hp` no eviction | session-length | LOW | fine |
