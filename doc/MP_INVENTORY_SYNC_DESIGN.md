# MP Inventory Sync — Design Notes

Status: design draft, not implemented. Created 2026-05-18.

## Why this matters

Several near-term and medium-term MP features all wait on this:

- **Picking up items from the world** — today the client has no way to acquire an arbitrary world item. The host's proxy NPC can't carry whatever the client just grabbed.
- **Authoritative host activities** — `drop_activity_actor`, `craft_activity_actor`, `read_activity_actor`, etc. all move items between inventory and world over multiple turns. To run them on the host's proxy (SP-mirror), the proxy must have the same inventory the client does.
- **Crafting** — recipes consume components from inventory and produce results. Both sides need a consistent view of what's in the pack.
- **Item use / transformation** — drinking water reduces a charge; applying bandages consumes them. Has to mutate the right side's truth.
- **Trading between players** — eventual feature; literally is inventory sync.
- **Death/respawn item recovery** — the survivor needs to find the dead partner's gear.

Today's partial sync (worn + wielded only, full re-snapshot when fingerprint changes) is sufficient for combat/movement parity but not for any of the above.

---

## Authority model

Three viable models, ranked by my recommendation:

### A. Host authoritative on the proxy NPC (recommended)

- The host's `remote_player_npc_id` is the source of truth for the client's inventory.
- Client mutates its **own** avatar's inventory locally for snappy UI (pick up, drop dialog, etc.) but every mutation is also dispatched to the host as an action.
- Host applies the same mutation to the proxy NPC.
- Host periodically broadcasts a delta of proxy inventory changes back to the client.
- On conflict (host rejects a pickup because the item was already taken), host's view wins; client reconciles.

**Why this model:**
- Matches existing MP architecture (host runs the simulation; client predicts and sends actions).
- Works naturally with activities — host's activity ticks against host's proxy inventory.
- Makes trading and theft mechanics easy — the host arbitrates.

**Costs:**
- Every inventory action is a round-trip to host. Pickup of a backpack full of items could be 50+ messages.
- Client UI must handle "host rejected, please reconcile" gracefully.

### B. Client authoritative on own avatar, host mirrors

- Client's avatar inventory is truth.
- Client broadcasts inventory snapshot or delta to host periodically.
- Host's proxy is a read-only mirror — never mutated independently.

**Why not this:**
- Activities running on the host can't take items out of inventory (proxy is read-only). Defeats the SP-mirror goal.
- Trading would require both parties to manipulate each other's inventories, which violates the authority model.

### C. Lock-step ownership (worst)

- Each item has a single owner-side. Items can only mutate on their owner-side.
- Trading is a handoff of ownership.

**Why not:**
- Complex to implement, doesn't compose with crafting (recipe consumes items from your inventory; if some have different "owners" the model breaks).

---

## Protocol

Assuming model A.

### Delta format (client → host)

Every inventory mutation the client performs locally emits an action:

```json
{
  "type": "action",
  "action": "inv_op",
  "op": "pickup",                  // pickup | drop_to | use | wield | unwield | wear | takeoff | move | charge
  "target_abs": {"x": ..., "y": ..., "z": ...},   // tile, when relevant
  "item_uid": 12345,               // existing item's stable uid, when mutating an item the host already knows
  "item_payload": { ... },         // full serialized item, when introducing a NEW item to the proxy's view
  "container_uid": 678,            // pocket target, when nesting
  "charges": 3                     // for divisible items (water, ammo)
}
```

Each item carries a **stable UID** that's assigned on first introduction to the network. Item references in subsequent messages use the UID; full payloads are only sent on introduction.

UID lifecycle:
- Mapgen items: assigned by the host when first observed (via the existing tile sync).
- Items created by player action: client generates a UID, host accepts.
- Items consumed (eaten, ammo spent, etc.): UID retired on both sides.
- Items split (a stack of 5 nails → 3 + 2): new UIDs for the split halves; old UID retired.

### Delta format (host → client)

The host periodically sends an inventory diff for the proxy NPC, included in the existing `remote_player_state` packet:

```json
"client_inv_delta": [
  {"op": "add", "uid": ..., "payload": {...}, "loc": "worn|wielded|pocket:UID|hand"},
  {"op": "remove", "uid": ...},
  {"op": "charge", "uid": ..., "charges": 4},
  {"op": "move", "uid": ..., "loc": "..."}
]
```

The client applies these to its avatar's inventory. The client's local prediction is correctable here.

### Periodicity

- Client → host: one message per discrete user action (mostly already true).
- Host → client: rolled into the existing `remote_player_state` broadcast, only when the proxy inventory actually changed since last broadcast (fingerprint check, like the current worn-list sync).

### Bandwidth

A modest sweep of a zombie corpse pile might pick up 20–40 items at once. At ~200 bytes per item payload + 50 bytes envelope per message, that's ~4–10 KB total for a big pickup batch. Acceptable for LAN and most internet links. The host→client delta is smaller (it's a diff after the action settles, not per item).

For ongoing-state churn (crafting that consumes 5 components per turn for 100 turns), bandwidth is dominated by the delta size — likely <500 B/turn after the first batch. Also fine.

---

## Edge cases

### Pickup races

Both players reach for the same item.

- Host arbitrates. The first action it processes wins. The loser's action returns `inv_op_rejected` with a reason ("item already taken by partner"). Client reconciles by removing the optimistically-added item.

### Container nesting

CDDA items can contain items (pockets), recursively. The protocol must handle a backpack-full pickup atomically. Solution: when introducing a container, the `item_payload` is the full serialized container tree (CDDA's existing item serialization handles this).

### Item charges (water, ammo, batteries)

Charges are mutated frequently (every drink, every shot). Use the `charge` op rather than re-sending the full item.

### Freshness / deterioration

CDDA items have `bday`, `last_temp_check`, `last_rot_check`, etc. These are computed locally from the calendar. Both sides have the same calendar (lockstep), so they should stay in sync without per-item updates — but if they drift, the host's view is canonical.

### Saved games

The host's save is canonical. On load:

- Host loads world + proxy NPC (proxy NPC is persisted as a real NPC).
- Client connects, requests a full inventory snapshot from host.
- Host sends entire proxy inventory as `inv_op` messages (or a single bulk `inv_snapshot` packet, which we add).
- Client clears its avatar inventory and applies the snapshot.

This gives load-time correctness without per-session UID stability.

---

## Migration path

Recommended order so each step ships independently:

1. **Stable item UIDs** — already partially present (`item_uid` field in serialized items). Verify uniqueness across host/client, ensure they survive save/load. No new wire format yet.
2. **`inv_op` action infrastructure** — add the action type and host handler. Initially handles only `pickup` from a tile — proves the model end-to-end against the simplest case.
3. **`drop` and `drop_to` via `inv_op`** — replace the current `drop` action with the unified `inv_op` model. Now drops are SP-mirror on the host. *This is the point where the activity_id work we shipped today gets backed by real authority.*
4. **`use`, `charge`, `wield`/`unwield`, `wear`/`takeoff`** — round out the basic single-item ops.
5. **Container ops + bulk pickups** — atomic multi-item moves, pocket nesting.
6. **Host → client `client_inv_delta`** — currently the host's proxy inventory is opaque to the client. Add the back-channel.
7. **Crafting** — by this point most components exist; crafting becomes a thin wrapper over `inv_op` consume/produce.
8. **Trading UI** — the cherry on top.

Each step is independently testable: you can pickup items (step 2) without crafting (step 7) working.

---

## Open questions

- **Pocket UIDs vs slot indices** — the protocol uses pocket UIDs, but pockets are normally referenced by index within a container. Need to decide whether to add UIDs to pockets or keep slot indices and accept the mild fragility.
- **NPC inventories** — when an NPC dies and drops their inventory, does the proxy NPC's inventory go through `inv_op` too? Probably yes for consistency, but it bypasses the "client action triggers it" pattern. May need a `host_initiated` flag on `inv_op` for these cases.
- **Replay log size** — if the host crashes and reloads, all the per-action `inv_op` history is lost. The host should reconstruct state from the persisted proxy NPC's inventory, not replay. So the protocol is fire-and-forget, not a replicated log.
- **Cheating** — this design trusts the client. A malicious client could send `inv_op pickup` for items they aren't standing next to. Host should validate proximity and possession. Worth scoping but not blocking — this is a co-op game, not adversarial.

---

## Estimated effort

Rough sizing (sessions of focused work):

- Step 1 (UID stability): 1 session
- Step 2 (pickup): 2 sessions
- Step 3 (drop unification): 1 session (replaces existing code rather than adding)
- Steps 4–5 (basic ops + containers): 2–3 sessions
- Step 6 (host→client delta): 1 session
- Step 7 (crafting): 2–4 sessions
- Step 8 (trading): 1–2 sessions

Total: ~10–15 sessions to full completion. Steps 1–3 are the highest-value chunk — they unlock authoritative-host drops and pave the way for everything else.
