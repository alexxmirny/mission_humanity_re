#!/usr/bin/env python3
"""scan_cap_units_census.py -- PT-CAP9-U0: the units / soldiers relocation census, every site classified.

WHY. PT-CAP8's scanner (scan_cap_reloc_sites.py) already decodes the whole clean EN image and lists every
field that encodes an address inside a cataloged array, units 0xdd8c48 and soldiers 0xe0e5a8 included --
but it only REPORTS those two ("PT-CAP9's list, reported not gated"), and nothing says what each field IS.
Before U1 relocates the arrays, each site needs a verdict, so a site that is neither moved nor consciously
left alone cannot exist:

  move     a relocation site the U1 manifest must rewrite (disp / abs / imm_addr / row_stride / row_multiple /
           block_size / shift_chain). Carries liveness (live / dead / nofn = in no recorded function) and
           whether the July grand manifest already patched it (july=1).
  exclude  a field the scan lists but that is not moved, with a recorded REASON:
             next_global   an `end` value (array end) that is really the base of the NEXT global
             not_pointer   an initialised-data dword that is a byte-pattern coincidence, not a pointer
             save_layer    the base / block size of the planet-save writer and reader (map_SavePlanetToDisk /
                           map_LoadPlanetFromDisk): they keep the STOCK address and size, because the stock
                           .bss extent is the canonical mirror (PT-CAP8 D1) -- the same design as buildings
LIMITS (the separate list). Constants 91 / 100 / 200 / 300 in decoded instructions -- the per-player unit
cap, the recruit caps, selection and order-queue sizes -- derived from the exe:
  raise_july    the July grand manifest patched this exact instruction (NOT proof the constant was the cap: the
                July patch may only have moved a field of the same instruction -- 14 of its 29 units rows were)
  triage        the function touches units/soldiers (or the old design scan listed it)
  incidental    other function, constant not tied to units (heuristic; recorded, not silent)
PT-CAP9-U1 DECIDES every triage / raise_july row that no cap_v022 manifest already owns (VERDICTS below, one
row each with a reason): `raise` (to 500 / 491, carried into the limits manifest) or `keep` (a code naming
WHY it is not a unit cap, or which later stage -- U4 path slots, U5 orders, U6 selection -- owns it). The exe
bytes the raise rows need are recorded under `raise_windows` so the manifest generator is hermetic.
The "every move site is covered" arm of --check reads src/patcher/cap_v023_units_reloc_EN.mh.patch.json.
This tool applies NOTHING; tools/gen_cap_units_manifests.py reads the doc and writes the manifests.

DATA. tools/data/cap_units_census.json. `--check` is hermetic: it re-derives the site table from
tools/data/cap_reloc_hits.json (+ the July manifest's byte extents) and byte-compares, then validates every
invariant. `--selftest` plants mutations that must go red. `--gen` rewrites the file; the limit list needs the
clean EN exe (md5 b3b389e8...) via `--gen --exe PATH` (otherwise the committed list is kept).

usage:
  python tools/scan_cap_units_census.py --check
  python tools/scan_cap_units_census.py --selftest
  python tools/scan_cap_units_census.py --gen [--exe PATH]
  python tools/scan_cap_units_census.py --report
"""

import argparse
import copy
import hashlib
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import scan_cap_reloc_sites as scan  # noqa: E402

REPO = scan.REPO
CENSUS = os.path.join(scan.DATA, "cap_units_census.json")
CAP9 = ("units", "soldiers")
JULY_MANIFEST = "grand_all_caphike_storagecap_EN"

# `end` hits: the scan lists any value equal to an array's end that is nobody's base. For units and soldiers
# that value is the base of the next global (verified in Ghidra, /eng/mh.exe, 2026-10-21).
NEXT_GLOBALS = {
    0xE06468: {
        "array": "units",
        "next": "_G_LLM_STRAT_MINIMAP_VIGNETTE_MASK",
        "evidence": "units end = 0xdd8c48 + 8*23300 = 0xe06468; Ghidra symbol at 0xe06468 is "
        "_G_LLM_STRAT_MINIMAP_VIGNETTE_MASK; the three sites are llm_strat_minimap_init addressing that mask",
    },
    0xE14048: {
        "array": "soldiers",
        "next": "Tree",
        "evidence": "soldiers end = 0xe0e5a8 + 8*2900 = 0xe14048; Ghidra data at 0xe14048 is "
        "cfg_final_struct_Tree[1] (44 B, symbol Tree); sites are the final-snapshot save/load and "
        "cfg_final_tree_Construct addressing Tree",
    },
}
# `data_ptr` hits, keyed by the address of the dword.
NOT_POINTERS = {
    0x511180: {
        "array": "units",
        "value": 0xDE0400,
        "evidence": "undefined data in DGROUP, no references in or out (ReVA find-cross-references 0/0). The "
        "neighbouring dwords read 0xdb040000 0x04000000 0xdc 0xdd04 0xde0400 0xdf040000 ...: a table of "
        "records with an incrementing id byte (db dc dd de df e0 e1 e2 e3), so 0x00de0400 is the unaligned "
        "bytes 00 04 de 00 of record n. 0xde0400 is not read as an address by any decoded instruction",
    },
    0x656948: {
        "array": "soldiers",
        "value": 0xE10000,
        "evidence": "inside _G_LLM_GFX_UI_COLORS_RGB (int[4][3] at 0x656946; the table starts at an odd "
        "address) -- bytes 00 00 e1 00 are the high half of colour 0x61 and the low half of 0xe1 read "
        "unaligned. No references in or out",
    },
}
LIMIT_VALUES = (91, 100, 200, 300)
LIMIT_MNEMONICS = ("cmp", "imul", "add", "sub", "mov", "push")
WEAK_MNEMONICS = ("cmp", "imul")  # outside the unit/soldier functions only these are listed
EXCLUDE_REASONS = ("next_global", "not_pointer", "save_layer")
SAVE_FNS = scan.SAVE_FNS  # ("map_SavePlanetToDisk", "map_LoadPlanetFromDisk")
SAVE_ROLES = ("block_size", "disp", "abs", "imm_addr")
RELOC_MANIFEST = "cap_v023_units_reloc_EN"
V022_GLOB = "cap_v022_*.mh.patch.json"

# ---- PT-CAP9-U1: the verdict on every limit row no cap_v022 manifest owns (2026-10-21) -------------------
# Each row was read in the decompile AND the disassembly (tmp/capraise/u1/decomp2). raise = the constant is a
# per-player unit / soldier / recruit bound that must become 500 (units, soldiers) or 491 (recruit units: the
# stock 91 leaves slots 91..99 reserved, the same nine at 491..499) when [caps] raise is ON. keep = it is not.
RAISE = "raise"
KEEP = "keep"
KEEP_CODES = {
    "sound_volume": "llm_snd_play(id, 100): the volume argument",
    "stack_frame": "utils_assert_stack_capacity(0x64) / sub esp: a stack-frame size",
    "weapon_sentinel": "units.selected_weapon == 100: the 'no weapon' sentinel, not an index bound",
    "cheat_penalty": "debug-console cheat penalty score (+100, locks out above 200)",
    "shuttle_status": "PROD_SHUTTLE_SLOTS[].status code 200 (cargo ready)",
    "path_slot": "path-slot pool geometry (100 slots per player, flags [8][100]): U4 owns it",
    "path_waypoints": "path buffer waypoint count 300 per slot: U4 owns it",
    "group_scratch": "GROUP_MOVE_SCRATCH holds 100 members (2000 B, never relocated): the cap stays",
    "notify_code": "llm_strat_unit_notify_status(.., 100): a status code",
    "retry_counter": "unit +0xab blocked-move retry byte compared with 200",
    "rand_range": "llm_rand_below(100): a percentage roll",
    "elevation": "unit-proto elevation + 300",
    "ai_distance": "squared-distance threshold 100 after llm_strat_toroidal_dist",
    "ai_pct": "AI home-guard / arrival percentage arithmetic (/100, *100)",
    "minimap_radius": "minimap vignette radius^2 100",
    "tutorial_units": "tutorial step scan over units[0..99]: the scripted mission never owns 100 units",
    "tutorial_buildings": "tutorial step scan over buildings[..]: scripted mission, buildings stay stock-sized",
    "tutorial_state": "tutorial compares a building state (99 / 100)",
}


def _r(to, arr, why):
    return {"verdict": RAISE, "to": to, "array": arr, "why": why}


def _k(code, why=""):
    return {"verdict": KEEP, "code": code, "why": why or KEEP_CODES[code]}


VERDICTS = {
    0x43FB21: _k("shuttle_status"),
    0x440E2E: _k("stack_frame"),
    0x4420D7: _k("sound_volume"),
    0x442E38: _k("sound_volume"),
    0x442E9B: _k("sound_volume"),
    0x443AA8: _k("sound_volume"),
    0x4449A1: _k("weapon_sentinel"),
    0x444B47: _k("weapon_sentinel"),
    0x444C54: _k("weapon_sentinel"),
    0x44968E: _k(
        "weapon_sentinel",
        "July touched this instruction only to move the disp32; the 100 is the sentinel",
    ),
    0x4498C0: _k(
        "weapon_sentinel",
        "July touched this instruction only to move the disp32; the 100 is the sentinel",
    ),
    0x44A6CB: _r(
        500, "units", "alive-count scan over units[player][1..99] bounded by the cached count"
    ),
    0x44ACB7: _r(500, "units", "find_next_alive scans units[player][i]"),
    0x44AD2A: _r(500, "units", "find_nth_alive scans units[player][i]"),
    0x44AE13: _r(500, "units", "find_nth_alive_excluding_ctrl_group scans units[player][i]"),
    0x44AEB5: _r(500, "units", "find_nth_alive_unassigned scans units[player][i]"),
    0x44B721: _k("cheat_penalty"),
    0x44C16D: _k("cheat_penalty"),
    0x44C90A: _k("sound_volume"),
    0x44CD0D: _k("sound_volume"),
    0x44EDCA: _k("stack_frame"),
    0x44FCC1: _k("stack_frame"),
    0x45622B: _k("path_slot", "free-slot count initialised to 100 per player"),
    0x45623C: _k(
        "path_slot",
        "path-slot flags clear loop i < 100 (July raised it; the flags array stays [8][100] until U4)",
    ),
    0x456249: _k("path_slot", "flags index player*100 + slot"),
    0x4563C8: _r(
        500,
        "soldiers",
        "soldier-record clear loop (redundant after the block clear, kept in step with the bound)",
    ),
    0x461C53: _r(500, "soldiers", "free soldier slot search: soldiers[player][i].owner_unit == 0"),
    0x46389E: _r(500, "units", "free unit slot search bound (100 - reserved), imm32 in place"),
    0x463B2B: _r(
        500, "soldiers", "soldier housing: soldiers[p][0].owner_unit + soldier_count + 1 < 100"
    ),
    0x463BEB: _r(
        500, "units", "unit slot bound for the soldier's carrier (100 - reserved), imm32 in place"
    ),
    0x463EF7: _r(491, "units", "recruit cap: units[p][0].order + 1 < 91"),
    0x464041: _r(500, "soldiers", "recruit soldier housing < 100"),
    0x464164: _r(491, "units", "spawn_on_tile free slot search 1..0x5a"),
    0x464398: _r(500, "soldiers", "spawn_docked soldier housing < 100"),
    0x4643B0: _r(491, "units", "spawn_docked free slot search 1..0x5a"),
    0x464460: _r(491, "units", "add_docked free slot search 1..0x5a"),
    0x4674D0: _k("sound_volume"),
    0x467522: _k("sound_volume"),
    0x46ADF4: _k("sound_volume"),
    0x46AF08: _k("sound_volume"),
    0x47B7FB: _r(
        500, "units", "unmap_footprint clears home_storage_slot over units[player][1..99]"
    ),
    0x47D52E: _k("retry_counter"),
    0x47E656: _k("rand_range"),
    0x48266D: _r(
        500,
        "units",
        "group_marshal peer scan; the member list stays capped at 100 by the scratch, so a larger group is taken in waves",
    ),
    0x482833: _k("group_scratch", "register_member count == 100 ends the gather"),
    0x482EA7: _k("notify_code"),
    0x4833ED: _r(500, "units", "group_step_ground peer scan over units[player][i]"),
    0x48358F: _k("group_scratch", "scratch registration count == 100"),
    0x483C0F: _k("notify_code"),
    0x484332: _k("notify_code"),
    0x4849D3: _k("notify_code"),
    0x484DEE: _k("notify_code"),
    0x485384: _k("weapon_sentinel"),
    0x48541F: _k("weapon_sentinel"),
    0x486296: _k("shuttle_status"),
    0x487B03: _k("sound_volume"),
    0x48BB5D: _k("weapon_sentinel"),
    0x48C6A2: _k(
        "weapon_sentinel",
        "stores the 'no weapon' sentinel; July touched the instruction only to move the disp32",
    ),
    0x48F438: _k("elevation"),
    0x4906AF: _r(
        491,
        "units",
        "shuttle capacity remaining = units + cargo - 91; the recruit cap (July did not patch it)",
    ),
    0x4928A4: _r(491, "units", "try_start_unit recruit cap: units[p][0].order + 1 < 91"),
    0x492A9F: _r(500, "soldiers", "try_start_unit soldier housing < 100"),
    0x4950EF: _k("path_slot"),
    0x495106: _k("path_slot"),
    0x49573C: _k("path_slot"),
    0x495753: _k("path_slot"),
    0x495B10: _k("path_waypoints"),
    0x495C76: _k("path_waypoints"),
    0x495E82: _k("path_waypoints"),
    0x495FBE: _k("path_slot"),
    0x495FD6: _k("path_slot"),
    0x496075: _k("path_waypoints"),
    0x496A3F: _k("path_slot"),
    0x496A9A: _k("path_slot"),
    0x498B1D: _r(
        500, "units", "mother_reelect_primary scans units[player][1..99] for a living unit"
    ),
    0x4992B3: _r(500, "units", "planet-conquest credit: first living unit of the player"),
    0x49936A: _r(500, "units", "planet-conquest credit: kill-credit loop over the loser's units"),
    0x499BF2: _r(500, "units", "purge_unregistered scans units[player][1..99]"),
    0x49ADC4: _k("minimap_radius"),
    0x49F42A: _k("path_waypoints"),
    0x4A01FF: _r(
        500, "units", "alive_count_resync scans units[player][1..99] (dead code, kept in step)"
    ),
    0x4BA79B: _k("weapon_sentinel"),
    0x4BA8EB: _k("tutorial_buildings"),
    0x4BAA1A: _k("tutorial_buildings"),
    0x4BAA7D: _k(
        "tutorial_state", "caseD_8 sets the building-state threshold 100 (caseD_7 sets 99)"
    ),
    0x4BAA92: _k("tutorial_buildings"),
    0x4BAADE: _k("tutorial_state"),
    0x4BABEC: _k("tutorial_units"),
    0x4BAC48: _k("tutorial_units"),
    0x4D66BD: _k(
        "ai_distance", "July widened this to 500 (a radius, not a roster bound); not carried"
    ),
    0x4D6756: _k(
        "ai_distance", "July widened this to 500 (a radius, not a roster bound); not carried"
    ),
    0x4D67F7: _k("ai_distance"),
    0x4D68A4: _k("ai_distance"),
    0x4D697F: _k(
        "ai_distance", "July widened this to 500 (a radius, not a roster bound); not carried"
    ),
    0x4DAE4F: _k("notify_code"),
    0x4E6F6D: _k("ai_pct"),
    0x4E9529: _k(
        "ai_distance", "July widened this to 500 (a radius, not a roster bound); not carried"
    ),
    0x4E9591: _k("ai_pct"),
}
LIVE = {"live": 0, "dead": 1, "nofn": 2}
LIVE_NAMES = {v: k for k, v in LIVE.items()}
SITE_COLS = ["va", "w", "array", "role", "value", "fn", "class", "reason", "live", "july"]
LIMIT_COLS = ["va", "fn", "ins", "imm", "class", "live", "july", "strong", "seed"]


def canon(o):
    return json.dumps(o, separators=(",", ":"), sort_keys=True)


def load_hits():
    return [
        f
        for f in scan.unpack_hits(json.load(open(scan.HITS, encoding="utf-8")))
        if f["array"] in CAP9
    ]


def july_cov():
    return scan.Coverage(scan.manifest_extents(names=[JULY_MANIFEST], globs=[]))


def scan_liveness():
    """-> {(va, array, role): 'live'|'dead'|'nofn'} for sites the PT-CAP8 scan still lists as uncovered."""
    out = {}
    s = json.load(open(scan.SUMMARY, encoding="utf-8"))
    for u in s["uncovered"]:
        if u["array"] in CAP9:
            lv = "nofn" if u["fn"] is None else u["liveness"]
            out[(int(u["va"], 16), u["array"], u["role"], u["fn"])] = lv
            out.setdefault((int(u["va"], 16), u["array"], u["role"]), lv)
    return out


def exe_liveness(exe_path, hits):
    """-> {(va, array, role, fn): 'live'|'dead'} computed per FUNCTION (the scan lists one function per
    address; 63 units fields sit in two overlapping bodies, scatter_random_worker / scatter_near_point)."""
    raw = open(exe_path, "rb").read()
    img = scan.Image(raw)
    funcs = scan.load_funcs()
    taken = scan.address_taken(img, [f[0] for f in funcs])
    seen, _callers = scan.reachability(funcs, taken, img.entry)
    by_name = {}
    for e, _end, n in funcs:
        by_name.setdefault(n, []).append(e)
    out = {}
    for f in hits:
        ents = by_name.get(f["fn"] or "", [])
        if len(ents) == 1:
            out[(f["va"], f["array"], f["role"], f["fn"])] = "live" if ents[0] in seen else "dead"
    return out


def array_table():
    return {a["name"]: a for a in scan.arrays()}


def derive_sites(hits, prior_live, live_now, cov):
    """The site table, a pure function of the hits, the July extents and the liveness sources."""
    fns = sorted({f["fn"] for f in hits if f["fn"]})
    fidx = {n: i for i, n in enumerate(fns)}
    rows = []
    for f in sorted(hits, key=lambda f: (f["va"], f["array"], f["role"])):
        key = (f["va"], f["array"], f["role"], f["fn"])
        cls, reason = "move", ""
        if f["role"] == "end":
            cls, reason = "exclude", "next_global"
        elif f["role"] == "data_ptr":
            cls, reason = "exclude", "not_pointer"
        elif f["role"] not in scan.GATING:
            raise SystemExit("unclassified role %r at 0x%x" % (f["role"], f["va"]))
        elif f["fn"] in SAVE_FNS and f["role"] in SAVE_ROLES:
            cls, reason = "exclude", "save_layer"
        if f["fn"] is None:
            lv = "nofn"
        else:
            lv = live_now.get(key) or live_now.get(key[:3]) or prior_live.get(key)
        if lv is None:
            raise SystemExit(
                "no liveness for %s (regenerate from a tree where the scan lists it)" % (key,)
            )
        jul = 0
        if cls == "move" and f["role"] != "shift_chain":
            jul = 1 if cov.test(f["va"], f["w"]) == "covered" else 0
        rows.append(
            [
                f["va"],
                f["w"],
                f["array"],
                f["role"],
                f["value"],
                fidx.get(f["fn"], -1),
                cls,
                reason,
                LIVE[lv],
                jul,
            ]
        )
    return {"fns": fns, "cols": SITE_COLS, "rows": rows}


def site_dicts(sites):
    fns = sites["fns"]
    return [
        dict(zip(sites["cols"], r), fn=(fns[r[5]] if r[5] >= 0 else None)) for r in sites["rows"]
    ]


def field_offset(site, arrs):
    a = arrs[site["array"]]
    if site["role"] in ("disp", "abs"):
        return (site["value"] - a["base"]) % a["elem"]
    return None


def summarise(sites, limits):
    arrs = array_table()
    ds = site_dicts(sites)
    out = {"arrays": {}}
    for name in CAP9:
        mine = [d for d in ds if d["array"] == name]
        mv = [d for d in mine if d["class"] == "move"]
        a = arrs[name]
        july_miss = [d for d in mv if not d["july"] and d["role"] != "shift_chain"]
        rowbase = [
            d
            for d in mv
            if d["role"] == "imm_addr"
            and d["value"] != a["base"]
            and (d["value"] - a["base"]) % a["row"] == 0
        ]
        fields = sorted({field_offset(d, arrs) for d in mv if field_offset(d, arrs) is not None})
        miss_fields = sorted(
            {field_offset(d, arrs) for d in july_miss if field_offset(d, arrs) is not None}
        )
        out["arrays"][name] = {
            "sites": len(mine),
            "unique_va": len({d["va"] for d in mine}),
            "unique_va_move": len({d["va"] for d in mv}),
            "move": len(mv),
            "exclude": len(mine) - len(mv),
            "save_layer": sum(1 for d in mine if d["reason"] == "save_layer"),
            "by_role": _count(d["role"] for d in mine),
            "move_live": _count(LIVE_NAMES[d["live"]] for d in mv),
            "move_july_patched": sum(d["july"] for d in mv),
            "move_july_missed": len(july_miss),
            "move_july_missed_live": sum(1 for d in july_miss if d["live"] == LIVE["live"]),
            "row_base_imm_addr": [
                "0x%08x player %d" % (d["va"], (d["value"] - a["base"]) // a["row"])
                for d in rowbase
            ],
            "record_fields_referenced": len(fields),
            "record_fields_july_missed": miss_fields,
            "functions": len({d["fn"] for d in mv if d["fn"]}),
        }
    lim = limits["rows"]
    out["limits"] = {
        "rows": len(lim),
        "by_class": _count(r[4] for r in lim),
        "by_value": _count(r[3] for r in lim),
    }
    return out


def _count(it):
    c = {}
    for x in it:
        c[str(x)] = c.get(str(x), 0) + 1
    return dict(sorted(c.items()))


def limits_digest(rows):
    return hashlib.sha256(canon(rows).encode()).hexdigest()


# ---- limits (exe-derived) -------------------------------------------------------------------------------


def derive_limits(exe_path, hits, cov, seed):
    from capstone import CS_ARCH_X86, CS_MODE_32, Cs
    from capstone.x86 import X86_OP_IMM

    raw = open(exe_path, "rb").read()
    import hashlib as _h

    if _h.md5(raw).hexdigest() != scan.EN_MD5:
        raise SystemExit("%s is not the clean EN exe" % exe_path)
    img = scan.Image(raw)
    funcs = scan.load_funcs()
    names = {f[0]: f[2] for f in funcs}
    taken = scan.address_taken(img, [f[0] for f in funcs])
    seen, callers = scan.reachability(funcs, taken, img.entry)
    strong_fns = {f["fn"] for f in hits if f["fn"]}
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    jul_ext = cov.ext
    import bisect

    jstarts = [e[0] for e in jul_ext]

    def july_touch(a, size):
        i = bisect.bisect_left(jstarts, a + size) - 1
        while i >= 0 and cov.maxhi[i] > a:
            if jul_ext[i][0] < a + size and a < jul_ext[i][1]:
                return 1
            i -= 1
        return 0

    rows = []
    for entry, end, name in funcs:
        code = img.read(entry, end - entry + 1)
        rd = scan.recursive_descent(md, code, entry, end, img)
        live = "live" if entry in seen else "dead"
        strong = name in strong_fns
        for a, ins in sorted(rd.items()):
            if ins.mnemonic not in LIMIT_MNEMONICS:
                continue
            for op in ins.operands:
                if op.type != X86_OP_IMM or ins.imm_size not in (1, 2, 4):
                    continue
                v = op.imm & ((1 << (8 * ins.imm_size)) - 1)
                if v not in LIMIT_VALUES:
                    continue
                if not strong and ins.mnemonic not in WEAK_MNEMONICS:
                    continue
                was_seed = 1 if a in seed else 0
                jul = july_touch(a, ins.size)
                if jul:
                    cls = "raise_july"
                elif strong or was_seed:
                    cls = "triage"
                else:
                    cls = "incidental"
                rows.append(
                    [
                        a,
                        name,
                        "%s %s" % (ins.mnemonic, ins.op_str),
                        v,
                        cls,
                        LIVE[live],
                        jul,
                        int(strong),
                        was_seed,
                    ]
                )
                break
    rows.sort(key=lambda r: (r[0], r[2]))
    del names, callers
    return {"cols": LIMIT_COLS, "rows": rows, "digest": limits_digest(rows)}


# ---- verdicts and the raise windows (PT-CAP9-U1) -----------------------------------------------------------

JCC_REL8 = set(range(0x70, 0x80))


def v022_cov():
    return scan.Coverage(scan.manifest_extents(names=[], globs=[V022_GLOB]))


def reloc_cov():
    return scan.Coverage(scan.manifest_extents(names=[RELOC_MANIFEST], globs=[]))


def verdict_doc():
    return {"0x%08x" % k: v for k, v in sorted(VERDICTS.items())}


def _md():
    from capstone import CS_ARCH_X86, CS_MODE_32, Cs

    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    return md


def window_ok(va, win, imm):
    """Hermetic check of one recorded raise window: it decodes to whole instructions covering exactly
    `n` bytes, the first one carries the row's immediate, and nothing in it is a relative transfer the
    cave could not re-encode. -> list of problems."""
    from capstone.x86 import X86_GRP_CALL, X86_GRP_JUMP, X86_GRP_RET, X86_OP_IMM, X86_OP_REG

    errs = []
    b = bytes.fromhex(win["hex"].replace(" ", ""))
    if len(b) != win["n"]:
        return ["window 0x%x: %d bytes recorded as n=%d" % (va, len(b), win["n"])]
    md = _md()
    pos, ins_l = 0, []
    while pos < len(b):
        i = next(md.disasm(b[pos:], va + pos, 1), None)
        if i is None:
            return ["window 0x%x: undecodable at +%d" % (va, pos)]
        ins_l.append(i)
        pos += i.size
    if pos != len(b):
        errs.append("window 0x%x: instructions overrun the window" % va)
    first = ins_l[0]
    imms = [
        o.imm & ((1 << (8 * first.imm_size)) - 1) for o in first.operands if o.type == X86_OP_IMM
    ]
    if not imms or imms[0] != imm:
        errs.append("window 0x%x: first instruction's immediate is not %d" % (va, imm))
    if win["kind"] == "imm32":
        if not (
            len(ins_l) == 1
            and first.mnemonic == "mov"
            and first.operands[0].type == X86_OP_REG
            and first.imm_size == 4
            and first.imm_offset == win["imm_off"]
        ):
            errs.append("window 0x%x: not a `mov r32, imm32` (in-place kind)" % va)
    elif win["kind"] == "cave":
        if win["n"] < 5:
            errs.append("window 0x%x: a cave window must cover >= 5 bytes" % va)
        if first.imm_size not in (1, 2, 4):
            errs.append("window 0x%x: first instruction has no immediate" % va)
        for k, i in enumerate(ins_l):
            if i.group(X86_GRP_CALL) or i.group(X86_GRP_RET):
                errs.append("window 0x%x: contains a call/ret" % va)
            elif i.group(X86_GRP_JUMP) and not (
                i.bytes[0] in JCC_REL8 or (i.bytes[0] == 0x0F and 0x80 <= i.bytes[1] <= 0x8F)
            ):
                errs.append("window 0x%x: contains a jump the cave cannot re-encode" % va)
            elif i.mnemonic.startswith(("loop", "jcxz", "jecxz")):
                errs.append("window 0x%x: contains a loop/jcxz" % va)
            if k < len(ins_l) - 1 and i.group(X86_GRP_JUMP):
                errs.append("window 0x%x: a jump that is not the last displaced instruction" % va)
    else:
        errs.append("window 0x%x: unknown kind %r" % (va, win["kind"]))
    return errs


def derive_windows(exe_path, limit_rows):
    """-> (windows {va: record}, v022_auto {va: ins_len}). Needs the clean EN exe."""
    from capstone.x86 import X86_GRP_JUMP, X86_OP_IMM, X86_OP_REG

    raw = open(exe_path, "rb").read()
    if hashlib.md5(raw).hexdigest() != scan.EN_MD5:
        raise SystemExit("%s is not the clean EN exe" % exe_path)
    img = scan.Image(raw)
    funcs = scan.load_funcs()
    find = scan.body_finder(funcs)
    md = _md()
    cov22 = v022_cov()
    windows, auto = {}, {}
    for r in limit_rows:
        va, cls, imm = r[0], r[4], r[3]
        if cls not in ("triage", "raise_july"):
            continue
        i = next(md.disasm(img.read(va, 16), va, 1))
        if cov22.test(va, i.size) != "none":
            auto[va] = i.size
            continue
        v = VERDICTS.get(va)
        if v is None or v["verdict"] != RAISE:
            continue
        ins_l = [i]
        wl = i.size
        in_place = (
            i.mnemonic == "mov"
            and i.operands[0].type == X86_OP_REG
            and i.imm_size == 4
            and len(i.operands) == 2
            and i.operands[1].type == X86_OP_IMM
        )
        if not in_place:
            while wl < 5:
                nx = next(md.disasm(img.read(va + wl, 16), va + wl, 1))
                ins_l.append(nx)
                wl += nx.size
        win = {
            "hex": " ".join("%02x" % x for x in img.read(va, wl)),
            "n": wl,
            "kind": "imm32" if in_place else "cave",
            "imm_off": i.imm_offset,
            "imm_size": i.imm_size,
            "ins": "; ".join("%s %s" % (x.mnemonic, x.op_str) for x in ins_l),
        }
        fn = find(va)
        if fn is None:
            raise SystemExit("raise row 0x%x is in no function" % va)
        code = img.read(fn[0], fn[1] - fn[0] + 1)
        tabs = {}
        rd = scan.recursive_descent(md, code, fn[0], fn[1], img, tabs)
        tg = set()
        for a, x in rd.items():
            if x.group(X86_GRP_JUMP) and x.operands and x.operands[0].type == X86_OP_IMM:
                tg.add(x.operands[0].imm & 0xFFFFFFFF)
        for lst in tabs.values():
            tg.update(lst)
        inside = sorted(t for t in tg if va < t < va + wl)
        if inside:
            raise SystemExit(
                "raise row 0x%x: a branch targets inside its displaced window (%s)"
                % (va, ", ".join(hex(t) for t in inside))
            )
        win["fn"] = fn[2]
        win["entry_targets_checked"] = 1
        windows[va] = win
        e = window_ok(va, win, imm)
        if e:
            raise SystemExit("; ".join(e))
    return windows, auto


def verdict_summary(doc_verdicts):
    out = {"raise": {}, "keep": {}}
    for v in doc_verdicts.values():
        if v["verdict"] == RAISE:
            k = "%s->%d" % (v["array"], v["to"])
            out["raise"][k] = out["raise"].get(k, 0) + 1
        else:
            out["keep"][v["code"]] = out["keep"].get(v["code"], 0) + 1
    return {k: dict(sorted(v.items())) for k, v in out.items()}


# ---- the document -------------------------------------------------------------------------------------------


def build_doc(exe=None):
    hits = load_hits()
    cov = july_cov()
    prior = {}
    prior_limits = None
    prior_doc = {}
    if os.path.isfile(CENSUS):
        old = json.load(open(CENSUS, encoding="utf-8"))
        prior_doc = old
        for d in site_dicts(old["sites"]):
            prior[(d["va"], d["array"], d["role"], d["fn"])] = LIVE_NAMES[d["live"]]
        prior_limits = old.get("limits")
    live_now = scan_liveness()
    if exe:
        live_now.update(exe_liveness(exe, hits))
    sites = derive_sites(hits, prior, live_now, cov)
    if exe:
        seed_path = os.path.join(REPO, "tools", "data", "cap_units_limit_seed.json")
        seed = set()
        if os.path.isfile(seed_path):
            seed = {int(x, 16) for x in json.load(open(seed_path))["vas"]}
        limits = derive_limits(exe, hits, cov, seed)
        windows, auto = derive_windows(exe, limits["rows"])
        windows = {"0x%08x" % k: v for k, v in sorted(windows.items())}
        auto = {"0x%08x" % k: v for k, v in sorted(auto.items())}
    elif prior_limits is not None:
        limits = prior_limits
        windows = prior_doc.get("raise_windows")
        auto = prior_doc.get("v022_auto")
        if windows is None or auto is None:
            raise SystemExit("the committed census has no raise windows: --gen needs --exe once")
    else:
        raise SystemExit("no committed limit list and no --exe: cannot build the limit section")
    vd = verdict_doc()
    doc = {
        "_generated_by": "tools/scan_cap_units_census.py --gen",
        "_what": __doc__.split("DATA.")[0].strip(),
        "exe_md5": scan.EN_MD5,
        "arrays": {n: array_table()[n] for n in CAP9},
        "next_globals": {"0x%08x" % k: v for k, v in sorted(NEXT_GLOBALS.items())},
        "not_pointers": {"0x%08x" % k: v for k, v in sorted(NOT_POINTERS.items())},
        "sites": sites,
        "limits": limits,
        "v022_auto": auto,
        "verdicts": vd,
        "verdict_summary": verdict_summary(vd),
        "raise_windows": windows,
    }
    doc["summary"] = summarise(sites, limits)
    return doc


def validate(doc, hits, cov, cov22=None, cov23=None):
    """-> list of problems. Hermetic: committed doc vs the committed hits + July extents (+ the cap_v022
    manifests for the verdict arm and the cap_v023 units manifest for the 'every move site is covered' arm;
    each arm is skipped when its coverage is not passed)."""
    errs = []
    arrs = array_table()
    sites = doc["sites"]
    if sites["cols"] != SITE_COLS:
        return ["site columns changed"]
    ds = site_dicts(sites)
    want = {(f["va"], f["array"], f["role"], f["fn"]): f for f in hits}
    seenk = {}
    for d in ds:
        k = (d["va"], d["array"], d["role"], d["fn"])
        if k in seenk:
            errs.append("duplicate site 0x%x %s %s %s" % k)
        seenk[k] = d
    for k in want:
        if k not in seenk:
            errs.append("census DROPPED site 0x%x %s %s %s" % k)
    for k in seenk:
        if k not in want:
            errs.append("census has site 0x%x %s %s %s that the scan does not" % k)
    nxt = {int(k, 16): v for k, v in doc["next_globals"].items()}
    nop = {int(k, 16): v for k, v in doc["not_pointers"].items()}
    if nxt != NEXT_GLOBALS or nop != NOT_POINTERS:
        errs.append("recorded verdict tables differ from the tool's")
    bases = {a["base"] for a in arrs.values()}
    for d in ds:
        k = (d["va"], d["array"], d["role"], d["fn"])
        f = want.get(k)
        if f is None:
            continue
        if d["value"] != f["value"] or d["w"] != f["w"] or d["fn"] != f["fn"]:
            errs.append(
                "site 0x%x %s changed vs the scan (value/width/function)" % (d["va"], d["array"])
            )
        a = arrs[d["array"]]
        if d["class"] == "move":
            if d["role"] not in scan.GATING:
                errs.append(
                    "0x%x %s: role %s is not a relocation role but is class move"
                    % (d["va"], d["array"], d["role"])
                )
            if d["reason"]:
                errs.append("0x%x: move site carries an exclusion reason" % d["va"])
            if d["role"] in ("disp", "abs", "imm_addr") and not (
                a["base"] <= d["value"] < a["base"] + a["size"]
            ):
                errs.append("0x%x: move site value outside the array" % d["va"])
            if d["role"] == "imm_addr" and (d["value"] - a["base"]) % a["row"] != 0 and False:
                pass
        elif d["class"] == "exclude":
            if d["reason"] not in EXCLUDE_REASONS:
                errs.append(
                    "0x%x %s: exclude without a recorded reason (%r)"
                    % (d["va"], d["array"], d["reason"])
                )
            elif d["reason"] == "next_global":
                n = nxt.get(d["value"])
                if d["role"] != "end" or n is None or n["array"] != d["array"]:
                    errs.append(
                        "0x%x: next_global exclusion has no verdict for value 0x%x"
                        % (d["va"], d["value"])
                    )
                if d["value"] != a["base"] + a["size"] or d["value"] in bases:
                    errs.append(
                        "0x%x: next_global value is not the array end / is another array's base"
                        % d["va"]
                    )
            elif d["reason"] == "not_pointer":
                n = nop.get(d["va"])
                if (
                    d["role"] != "data_ptr"
                    or n is None
                    or n["value"] != d["value"]
                    or n["array"] != d["array"]
                ):
                    errs.append("0x%x: data_ptr exclusion has no recorded verdict" % d["va"])
            elif d["reason"] == "save_layer":
                if d["fn"] not in SAVE_FNS or d["role"] not in SAVE_ROLES:
                    errs.append(
                        "0x%x %s: save_layer exclusion outside the planet save writer/reader"
                        % (d["va"], d["array"])
                    )
        else:
            errs.append("0x%x: unknown class %r" % (d["va"], d["class"]))
        if d["live"] not in LIVE_NAMES:
            errs.append("0x%x: bad liveness" % d["va"])
        if d["class"] == "move" and d["role"] != "shift_chain":
            j = 1 if cov.test(d["va"], d["w"]) == "covered" else 0
            if j != d["july"]:
                errs.append("0x%x %s: july flag stale" % (d["va"], d["array"]))
        if cov23 is not None:
            st = cov23.test(d["va"], d["w"])
            if d["class"] == "move" and st != "covered":
                errs.append(
                    "MOVE SITE NOT COVERED: 0x%x %s %s (%s) is %s by %s"
                    % (d["va"], d["array"], d["role"], d["fn"], st, RELOC_MANIFEST)
                )
            if d["reason"] == "save_layer" and st != "none":
                errs.append(
                    "save-layer site 0x%x %s is %s by %s -- the save/load blocks keep the stock address"
                    % (d["va"], d["array"], st, RELOC_MANIFEST)
                )
    # every verdict-table entry must be used (a stale verdict is a lie about a site that is gone)
    used_n = {d["value"] for d in ds if d["reason"] == "next_global"}
    used_p = {d["va"] for d in ds if d["reason"] == "not_pointer"}
    if used_n != set(NEXT_GLOBALS):
        errs.append(
            "next_global verdicts do not match the end hits (%s vs %s)"
            % (sorted(map(hex, used_n)), sorted(map(hex, NEXT_GLOBALS)))
        )
    if used_p != set(NOT_POINTERS):
        errs.append("not_pointer verdicts do not match the data_ptr hits")
    # byte-compare with a fresh derivation (liveness taken from the doc itself)
    prior = {(d["va"], d["array"], d["role"], d["fn"]): LIVE_NAMES[d["live"]] for d in ds}
    try:
        fresh = derive_sites(hits, prior, {}, cov)
        if canon(fresh) != canon(sites):
            errs.append("site table differs from a fresh derivation")
    except SystemExit as e:
        errs.append("fresh derivation failed: %s" % e)
    lim = doc["limits"]
    if lim["cols"] != LIMIT_COLS:
        errs.append("limit columns changed")
    else:
        if limits_digest(lim["rows"]) != lim["digest"]:
            errs.append("limit list does not match its digest (a row was dropped or edited)")
        seen_l = set()
        for r in lim["rows"]:
            if r[0] in seen_l and False:
                pass
            seen_l.add((r[0], r[2]))
            if r[4] not in ("raise_july", "triage", "incidental"):
                errs.append("limit row 0x%x: bad class %r" % (r[0], r[4]))
            if r[3] not in LIMIT_VALUES:
                errs.append("limit row 0x%x: constant %r not in the census set" % (r[0], r[3]))
            if r[4] == "incidental" and (r[7] or r[8] or r[6]):
                errs.append("limit row 0x%x: incidental but unit-related/seed/July" % r[0])
            if r[4] == "raise_july" and not r[6]:
                errs.append("limit row 0x%x: raise_july without a July patch" % r[0])
        if len(seen_l) != len(lim["rows"]):
            errs.append("duplicate limit rows")
    if doc.get("summary") != summarise(sites, lim):
        errs.append("summary stale")
    errs += validate_verdicts(doc, lim, cov22)
    return errs


def validate_verdicts(doc, lim, cov22):
    errs = []
    for k in ("verdicts", "raise_windows", "v022_auto", "verdict_summary"):
        if k not in doc:
            return ["census lacks %r (run --gen)" % k]
    if doc["verdicts"] != verdict_doc():
        errs.append("recorded verdict table differs from the tool's VERDICTS")
    if doc["verdict_summary"] != verdict_summary(doc["verdicts"]):
        errs.append("verdict summary stale")
    vd = {int(k, 16): v for k, v in doc["verdicts"].items()}
    auto = {int(k, 16): n for k, n in doc["v022_auto"].items()}
    rows = {r[0]: r for r in lim["rows"]}
    need = {va for va, r in rows.items() if r[4] in ("triage", "raise_july")} - set(auto)
    for va in sorted(need - set(vd)):
        errs.append("limit row 0x%x (%s, %s) has NO verdict" % (va, rows[va][1], rows[va][2]))
    for va in sorted(set(vd) - need):
        errs.append("verdict for 0x%x: no such open limit row (stale verdict)" % va)
    for va in sorted(set(auto) - set(rows)):
        errs.append("v022_auto entry 0x%x is not a limit row" % va)
    if cov22 is not None:
        for va, n in sorted(auto.items()):
            if cov22.test(va, n) == "none":
                errs.append(
                    "limit row 0x%x is auto-assigned to cap_v022 but no cap_v022 patch touches it"
                    % va
                )
    win = {int(k, 16): w for k, w in doc["raise_windows"].items()}
    raise_vas = {va for va, v in vd.items() if v["verdict"] == RAISE}
    for va in sorted(raise_vas - set(win)):
        errs.append("raise row 0x%x has no recorded exe window" % va)
    for va in sorted(set(win) - raise_vas):
        errs.append("exe window for 0x%x, which is not a raise row" % va)
    for va, v in sorted(vd.items()):
        r = rows.get(va)
        if r is None:
            continue
        if v["verdict"] == RAISE:
            want = {91: 491, 100: 500}.get(r[3])
            if v["to"] != want:
                errs.append("raise row 0x%x: constant %d cannot become %r" % (va, r[3], v["to"]))
            if v["array"] not in CAP9:
                errs.append("raise row 0x%x: unknown array %r" % (va, v["array"]))
            if va in win:
                errs += window_ok(va, win[va], r[3])
        elif v["verdict"] == KEEP:
            if v["code"] not in KEEP_CODES:
                errs.append("keep row 0x%x: unknown code %r" % (va, v["code"]))
            if not v["why"]:
                errs.append("keep row 0x%x: no reason" % va)
        else:
            errs.append("row 0x%x: bad verdict %r" % (va, v["verdict"]))
    return errs


def reloc_manifest_path():
    return os.path.join(scan.PATCHER, RELOC_MANIFEST + ".mh.patch.json")


def cmd_check():
    doc = json.load(open(CENSUS, encoding="utf-8"))
    if not os.path.isfile(reloc_manifest_path()):
        print(
            "cap_units_census: MISSING %s -- run tools/gen_cap_units_manifests.py --write"
            % reloc_manifest_path()
        )
        return 1
    errs = validate(doc, load_hits(), july_cov(), v022_cov(), reloc_cov())
    if errs:
        print("cap_units_census: %d problem(s)" % len(errs))
        for e in errs[:40]:
            print("  " + e)
        return 1
    s = doc["summary"]["arrays"]
    print(
        "cap_units_census OK: units %d sites (%d move, %d exclude), soldiers %d (%d move, %d exclude), %d limit rows, "
        "all move sites covered by %s; verdicts %s"
        % (
            s["units"]["sites"],
            s["units"]["move"],
            s["units"]["exclude"],
            s["soldiers"]["sites"],
            s["soldiers"]["move"],
            s["soldiers"]["exclude"],
            doc["summary"]["limits"]["rows"],
            RELOC_MANIFEST,
            json.dumps(
                {
                    "raise": sum(doc["verdict_summary"]["raise"].values()),
                    "keep": sum(doc["verdict_summary"]["keep"].values()),
                }
            ),
        )
    )
    return 0


def cmd_gen(exe):
    doc = build_doc(exe)
    errs = validate(doc, load_hits(), july_cov(), v022_cov(), None)
    if errs:
        print("generated census fails its own check:")
        for e in errs[:20]:
            print("  " + e)
        return 1
    with open(CENSUS, "w", encoding="utf-8", newline="\n") as fh:
        json.dump(doc, fh, indent=1, sort_keys=False)
        fh.write("\n")
    print(json.dumps(doc["summary"], indent=1))
    return 0


def cmd_report():
    doc = json.load(open(CENSUS, encoding="utf-8"))
    print(json.dumps(doc["summary"], indent=1))
    return 0


def selftest():
    hits = load_hits()
    cov = july_cov()
    cov22 = v022_cov()
    cov23 = reloc_cov()
    doc = json.load(open(CENSUS, encoding="utf-8"))
    if validate(doc, hits, cov, cov22, cov23):
        print("selftest: the committed census is not clean; fix that first")
        return 1
    failures = []

    def expect_red(name, mut_doc=None, mut_hits=None, c23=None, c22=None, needle=None):
        d = copy.deepcopy(doc) if mut_doc is None else mut_doc(copy.deepcopy(doc))
        h = hits if mut_hits is None else mut_hits(copy.deepcopy(hits))
        errs = validate(d, h, cov, cov22 if c22 is None else c22, cov23 if c23 is None else c23)
        if errs and needle is not None and not any(needle in e for e in errs):
            failures.append(name)
            print(
                "  RED FOR THE WRONG REASON: %s (wanted %r, got %r)" % (name, needle, errs[0][:80])
            )
        elif errs:
            print("  red as planned: %-52s (%s)" % (name, errs[0][:70]))
        else:
            failures.append(name)
            print("  NOT RED: " + name)

    def rows_of(d):
        return d["sites"]["rows"]

    def idx(d, pred):
        ds = site_dicts(d["sites"])
        return next(i for i, x in enumerate(ds) if pred(x))

    def drop_move(d):
        del rows_of(d)[
            idx(d, lambda x: x["class"] == "move" and x["array"] == "units" and x["role"] == "disp")
        ]
        return d

    def drop_soldier(d):
        del rows_of(d)[idx(d, lambda x: x["class"] == "move" and x["array"] == "soldiers")]
        return d

    def end_to_move(d):
        rows_of(d)[idx(d, lambda x: x["reason"] == "next_global")][6:8] = ["move", ""]
        return d

    def move_to_exclude_no_reason(d):
        rows_of(d)[idx(d, lambda x: x["class"] == "move")][6:8] = ["exclude", ""]
        return d

    def move_to_exclude_bad_reason(d):
        rows_of(d)[idx(d, lambda x: x["class"] == "move")][6:8] = ["exclude", "dead_code"]
        return d

    def wrong_next_global(d):
        k = next(iter(d["next_globals"]))
        d["next_globals"][k]["next"] = "SOMETHING_ELSE"
        return d

    def missing_verdict(d):
        d["not_pointers"].pop(next(iter(d["not_pointers"])))
        return d

    def end_hit_value_changed(d):
        rows_of(d)[idx(d, lambda x: x["reason"] == "next_global")][4] += 4
        return d

    def dup_site(d):
        rows_of(d).append(list(rows_of(d)[0]))
        return d

    def stale_july(d):
        r = rows_of(d)[idx(d, lambda x: x["class"] == "move" and x["july"] == 1)]
        r[9] = 0
        return d

    def drop_limit(d):
        del d["limits"]["rows"][0]
        return d

    def limit_class(d):
        for r in d["limits"]["rows"]:
            if r[4] == "triage":
                r[4] = "incidental"
                break
        d["limits"]["digest"] = limits_digest(d["limits"]["rows"])
        return d

    def limit_bad_value(d):
        d["limits"]["rows"][0][3] = 101
        d["limits"]["digest"] = limits_digest(d["limits"]["rows"])
        return d

    expect_red("a dropped units move site", drop_move)
    expect_red("a dropped soldiers move site", drop_soldier)
    expect_red("a next-global end hit reclassified as move", end_to_move)
    expect_red("a move site excluded with no reason", move_to_exclude_no_reason)
    expect_red("a move site excluded with an unlisted reason", move_to_exclude_bad_reason)
    expect_red("a next-global verdict naming the wrong global", wrong_next_global)
    expect_red("a data pointer verdict removed", missing_verdict)
    expect_red("an end hit whose value no longer equals the array end", end_hit_value_changed)
    expect_red("a duplicated site", dup_site)
    expect_red("a stale July flag", stale_july)
    expect_red("a dropped limit row (digest)", drop_limit)
    expect_red("a limit row reclassified incidental while unit-related", limit_class)
    expect_red("a limit row with a constant outside the set", limit_bad_value)

    def new_scan_site(h):
        extra = dict(next(f for f in h if f["array"] == "units" and f["role"] == "disp"))
        extra["va"] = 0x401000
        h.append(extra)
        return h

    def new_scan_end(h):
        extra = dict(next(f for f in h if f["role"] == "end" and f["array"] == "units"))
        extra["va"] = 0x401001
        h.append(extra)
        return h

    expect_red("a site the scan found that the census lacks", None, new_scan_site)
    expect_red("a new end hit with no recorded next-global verdict", None, new_scan_end)

    # ---- PT-CAP9-U1: the coverage arm and the verdict arm ------------------------------------------------
    ext23 = scan.manifest_extents(names=[RELOC_MANIFEST], globs=[])
    first_move = next(
        d for d in site_dicts(doc["sites"]) if d["class"] == "move" and d["role"] == "disp"
    )
    ext_minus = [e for e in ext23 if e[0] != first_move["va"]]
    expect_red(
        "a move site the units manifest does not cover",
        None,
        None,
        scan.Coverage(ext_minus),
    )
    save_site = next(d for d in site_dicts(doc["sites"]) if d["reason"] == "save_layer")
    ext_plus = sorted(ext23 + [(save_site["va"], save_site["va"] + 4, RELOC_MANIFEST, "field")])
    expect_red("a save-layer site the units manifest patches", None, None, scan.Coverage(ext_plus))

    def drop_verdict(d):
        d["verdicts"].pop(next(iter(d["verdicts"])))
        return d

    def stale_verdict(d):
        d["verdicts"]["0x00401000"] = {"verdict": KEEP, "code": "sound_volume", "why": "x"}
        return d

    def drop_window(d):
        d["raise_windows"].pop(next(iter(d["raise_windows"])))
        return d

    def raise_wrong_to(d):
        k = next(k for k, v in d["verdicts"].items() if v["verdict"] == RAISE and v["to"] == 500)
        d["verdicts"][k]["to"] = 501
        return d

    def keep_bad_code(d):
        k = next(k for k, v in d["verdicts"].items() if v["verdict"] == KEEP)
        d["verdicts"][k]["code"] = "because"
        return d

    def window_wrong_imm(d):
        k = next(iter(d["raise_windows"]))
        w = d["raise_windows"][k]
        b = bytearray(bytes.fromhex(w["hex"].replace(" ", "")))
        b[w["imm_off"]] ^= 0x01
        w["hex"] = " ".join("%02x" % x for x in b)
        return d

    def auto_unowned(d):
        k = next(k for k, v in d["verdicts"].items() if v["verdict"] == KEEP)
        d["v022_auto"][k] = 4
        return d

    expect_red("a limit row with no verdict", drop_verdict, needle="NO verdict")
    expect_red("a verdict for a row that does not exist", stale_verdict, needle="stale verdict")
    expect_red("a raise row with no exe window", drop_window, needle="no recorded exe window")
    expect_red("a raise row that becomes the wrong value", raise_wrong_to, needle="cannot become")
    expect_red("a keep row with an unknown code", keep_bad_code, needle="unknown code")
    expect_red(
        "a raise window whose immediate is not the row's",
        window_wrong_imm,
        needle="immediate is not",
    )
    expect_red(
        "a v022-auto row no cap_v022 patch touches",
        auto_unowned,
        needle="no cap_v022 patch touches",
    )
    exe = scan.default_exe()
    if exe and os.environ.get("MH_CENSUS_SELFTEST_EXE", "1") != "0":
        try:
            lim = derive_limits(exe, hits, cov, {int(x, 16) for x in _seed()})
        except SystemExit as e:
            print("  exe arm skipped:", e)
        else:
            if lim["rows"] != doc["limits"]["rows"]:
                failures.append("the committed limit list differs from a fresh exe derivation")
                print("  NOT CLEAN: committed limit list != exe derivation")
            else:
                d2 = copy.deepcopy(doc)
                del d2["limits"]["rows"][5]
                d2["limits"]["digest"] = limits_digest(d2["limits"]["rows"])
                if d2["limits"]["rows"] != lim["rows"]:
                    print(
                        "  red as planned: %-52s (exe re-derivation differs)"
                        % "a limit row dropped with its digest fixed"
                    )
                else:
                    failures.append("exe arm vacuous")
    else:
        print("  exe arm skipped (no clean EN exe)")
    if failures:
        print("selftest FAILED: %s" % "; ".join(failures))
        return 1
    print("selftest OK")
    return 0


def _seed():
    p = os.path.join(REPO, "tools", "data", "cap_units_limit_seed.json")
    return json.load(open(p))["vas"] if os.path.isfile(p) else []


def default_gen_exe(a):
    return a.exe or scan.default_exe()


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--gen", action="store_true")
    ap.add_argument("--report", action="store_true")
    ap.add_argument("--exe")
    a = ap.parse_args(argv)
    if a.selftest:
        return selftest()
    if a.gen:
        return cmd_gen(a.exe)
    if a.report:
        return cmd_report()
    if a.check:
        return cmd_check()
    ap.print_help()
    return 2


if __name__ == "__main__":
    sys.exit(main())
