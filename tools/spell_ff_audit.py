#!/usr/bin/env python3
"""Which spells can fast-forward in co-op?  (per-spell FF, B8)

    tools/spell_ff_audit.py <repo root>

Mirrors mp_note_spell_cast() in src/mp_gamestate.cpp:
- spell::select_target() opens the end-of-cast targeting UI iff range > 0 AND
  "none" not in valid_targets AND no RANDOM_TARGET flag;
- effect=translocate opens a destination menu;
- an effect_on_condition spell whose EOC tree uses u_query, run_eoc_selector,
  u_query_tile (and similar) opens a menu.  SP compiles EOCs to code, so the
  game can't check this at runtime: the ids this prints under "EOC UI" must be
  kept in mp_note_spell_cast()'s effect_menu_spells list.  Re-run after
  pulling upstream content and diff that list.
The YES/NO lists only cover spells that can reach FF_MIN_ACTIVITY_MOVES (3000).
"""
import json, os, sys, collections
root = sys.argv[1]
HARD_UI = ("u_query", "npc_query", "run_eoc_selector", "u_query_tile", "u_query_omt",
           "open_dialogue", "string_input")
spells = {}; eocs = {}
def walk(path):
    for d, _, fs in os.walk(path):
        for f in fs:
            if not f.endswith(".json"): continue
            p = os.path.join(d, f)
            try: data = json.load(open(p, encoding="utf-8"))
            except Exception: continue
            for o in (data if isinstance(data, list) else [data]):
                if not isinstance(o, dict): continue
                t = o.get("type")
                mod = os.path.relpath(p, root).split(os.sep)
                modname = mod[2] if mod[0] == "data" and mod[1] == "mods" and len(mod) > 2 else "vanilla"
                if t == "SPELL" and "id" in o: spells[o["id"]] = (o, modname)
                elif t == "effect_on_condition" and "id" in o: eocs[o["id"]] = o
walk(os.path.join(root, "data"))
def isdyn(*vals): return any(isinstance(v, dict) for v in vals)
def rng(o, lvl):
    mn, mx, inc = o.get("min_range", 0), o.get("max_range", o.get("min_range", 0)), o.get("range_increment", 0)
    if isdyn(mn, mx, inc): return None
    v = mn + inc * lvl
    return min(v, mx) if inc >= 0 else max(v, mx)
def ctime(o, lvl):
    b = o.get("base_casting_time", 0); f = o.get("final_casting_time", b); inc = o.get("casting_time_increment", 0)
    if isdyn(b, f, inc): return None
    v = b + inc * lvl
    return max(v, f) if inc < 0 else (min(v, f) if inc > 0 else v)
def strings_in(x):
    if isinstance(x, str): yield x
    elif isinstance(x, list):
        for v in x: yield from strings_in(v)
    elif isinstance(x, dict):
        for v in x.values(): yield from strings_in(v)
def eoc_ui(eid, seen=None):
    """UI effects anywhere in an EOC, following every string that names an EOC."""
    seen = seen if seen is not None else set()
    if not isinstance(eid, str) or eid in seen or eid not in eocs: return set()
    seen.add(eid); s = json.dumps(eocs[eid]); hits = {k for k in HARD_UI if f'"{k}"' in s}
    for ref in strings_in(eocs[eid]):
        if ref in eocs: hits |= eoc_ui(ref, seen)
    return hits
def spell_menu(sid, seen=None):
    """Menu effects of a spell: its own EOC (effect_str, or inline EOC objects)
    plus every extra_effects sub-spell, recursively."""
    seen = seen if seen is not None else set()
    if sid in seen or sid not in spells: return set()
    seen.add(sid); o = spells[sid][0]; hits = set()
    if o.get("effect") == "effect_on_condition":
        es = o.get("effect_str", "")
        if isinstance(es, str): hits |= eoc_ui(es)
        else: hits |= {k for k in HARD_UI if f'"{k}"' in json.dumps(es)}
    for ex in o.get("extra_effects", []):
        if isinstance(ex, dict): hits |= spell_menu(ex.get("id", ""), seen)
    return hits

# Every spell whose EFFECT opens a menu, at ANY cast time: a cast classified as
# safe becomes passive however short it is, and the client ticks passive
# activities (finish() included) from its network-recv path.  This is the list
# mp_note_spell_cast() must carry (translocate is caught there by effect name).
menu = []
for sid, (o, mod) in spells.items():
    hits = spell_menu(sid)
    if hits:
        menu.append((mod, sid, ",".join(sorted(hits))))
print("=== EFFECT-MENU spells (any cast time) -> effect_menu_spells: %d" % len(menu))
for mod, sid, hits in sorted(menu):
    print(f'  "{sid}",  // {mod}: {hits}')

rows = []
for sid, (o, mod) in spells.items():
    flags = o.get("flags", []); vt = o.get("valid_targets", [])
    ml = o.get("max_level", 0)
    if isinstance(ml, dict): ml = 20
    t0, tm = ctime(o, 0), ctime(o, ml if isinstance(ml, int) else 0)
    dyn_t = t0 is None or tm is None
    if not dyn_t and max(t0, tm) < 3000: continue        # never long enough to FF
    if dyn_t: t0 = tm = -100
    r0, rm = rng(o, 0), rng(o, ml if isinstance(ml, int) else 0)
    ui_target = lambda r: (r is None or r > 0) and "none" not in vt and "RANDOM_TARGET" not in flags
    if ui_target(r0) and ui_target(rm): verdict = "NO: targeting UI"
    elif ui_target(r0) or ui_target(rm): verdict = "DEPENDS on level (range)"
    else: verdict = "YES"
    eff = o.get("effect"); extra = ""
    if eff == "translocate": verdict, extra = "NO: destination menu in effect", ""
    hits = spell_menu(sid)
    if hits: verdict, extra = "NO: EOC UI " + ",".join(sorted(hits)), ""
    name = o.get("name"); name = name.get("str") if isinstance(name, dict) else name
    rows.append((mod, verdict, sid, name, t0, tm, eff))
by = collections.defaultdict(list)
for r in rows: by[r[1].split(":")[0].split(" ")[0]].append(r)
for v in ("YES", "DEPENDS", "NO"):
    lst = sorted(by.get(v, []))
    print(f"\n=== {v}: {len(lst)}")
    for mod, verdict, sid, name, t0, tm, eff in lst:
        ct = "formula" if t0 < 0 else f"{t0//100}-{tm//100} turns"
        print(f"  [{mod}] {name} ({sid})  cast {ct}  effect={eff}" + ("" if v=="YES" else f"  <- {verdict}"))
