#!/usr/bin/env python3
"""gen_cap_split_plan.py -- PT-CAP8 S0: the v0.2.2 cap-raise manifest SPLIT PLAN, as data.

THE DESIGN (user, 2026-10-19): arrays are ALWAYS relocated to the raised size;
a setting only moves the runtime limits (vanilla limits when OFF). v0.2.2 raises the buildings roster
[8][100] -> [8][500], the five building sub-pools -> 127 per player, and the projectile pool 1500 ->
4000; units and soldiers stay at 100 (PT-CAP9, v0.2.3). The shipped cap manifests mix the two kinds of
edit in one 7,847-site list (grand_all_caphike_storagecap_EN + projectile_pool_caphike_EN). This tool
reads them and assigns EVERY patch to exactly one class, so S1 can write two manifests plus a cave set
without re-deriving anything:

  a  RELOCATION layer (always applied)  base operand, row stride, shift-chain splice, clear size,
                                        walk backstop at the physical row capacity
  b  LIMIT layer (the switch)           loop bounds, widened-bound trampolines, allocator bounds,
                                        the round-robin budget, the projectile sentinel/loop bound
  c  STORAGE-CAP caves                  the 9 call-site redirects into the .mhcap caves (+ the caves)
  d  SAVE layer (stays vanilla)         SavePlanet/LoadPlanet block sizes -- the trailer design keeps
                                        the vanilla block and writes the overflow in a trailer (S3)
  x  EXCLUDED                           units / soldiers (PT-CAP9)
  ?  UNCLASSIFIED / SUSPECT             listed with the reason; S1 must decide each one

HOW A SITE IS CLASSIFIED (rule ids are written into every row):
  F1 4-byte operand that tools/data/cap_reloc_hits.json decodes as disp/abs/imm_addr   -> a / base
  F2 ... as row_stride                                                                  -> a / stride
  F3 ... as block_size in map_SavePlanetToDisk / map_LoadPlanetFromDisk                 -> d / save_block
  F4 ... as block_size elsewhere (FillDefaults clears)                                  -> a / clear_size
  S1 note 'shift row-stride splice'                                                     -> a / shift_splice
  W1 note 'walk-bound'  (a cave that stops a pointer walk at the physical capacity)     -> a / walk_backstop
  B1 note 'loop bound' / 'widen bound' / 'allocator bound' / 'loop BUDGET': the loop the compare
     controls is found in the vanilla code (recursive descent; bottom-tested, `jl BODY; jmp EXIT`
     and `jge EXIT` shapes), the arrays that loop touches are read from the decoded fields, and the
     bound's value selects the family (100 -> rosters, 32 -> turrets/mines, 25 -> storage/labs, 8 ->
     productions). One array family -> b / <class>; units or soldiers -> x; a loop that touches NO
     raised array at all -> SUSPECT (a widened bound over something the raise does not relocate).
  C1 note '-> cave 0x013c....' (the .mhcap redirects)                                   -> c
  P1 projectile sentinel / heuristic loop bound                                         -> b
Anything else is UNCLASSIFIED.

usage:
  python tools/gen_cap_split_plan.py --regen [--exe PATH]   # needs the clean EN exe (md5 b3b389e8...)
  python tools/gen_cap_split_plan.py --check                # hermetic: plan vs the CURRENT manifests
  python tools/gen_cap_split_plan.py --selftest
  python tools/gen_cap_split_plan.py --report
"""

import argparse
import collections
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import scan_cap_reloc_sites as scan  # noqa: E402

REPO = scan.REPO
PATCHER = scan.PATCHER
PLAN = os.path.join(REPO, "tools", "data", "cap_v022_split_plan.json")
SOURCES = ["grand_all_caphike_storagecap_EN", "projectile_pool_caphike_EN"]
EXCLUDED = ("units", "soldiers")
LAYERS = {
    "a": "relocation",
    "b": "limit",
    "c": "storage_cap",
    "d": "save_vanilla",
    "x": "excluded_units_soldiers",
    "?": "unclassified",
}
ELEM = {
    "buildings": 273,
    "units": 233,
    "soldiers": 29,
    "unit_storage": 244,
    "turrets": 55,
    "mines": 56,
    "productions": 405,
    "projectiles": 121,
}
SAVE_FNS = scan.SAVE_FNS


def load_patches():
    out = []
    for m in SOURCES:
        doc = json.load(open(os.path.join(PATCHER, m + ".mh.patch.json"), encoding="utf-8"))
        for p in doc["patches"]:
            out.append(
                {
                    "manifest": m,
                    "va": int(str(p["va"]), 16),
                    "len": len(bytes.fromhex(p.get("bytes", "").replace(" ", ""))),
                    "expect": p.get("expect", ""),
                    "note": p.get("_note", ""),
                }
            )
    return out


def family(value, arrs):
    """Arrays a bound constant can belong to: value == count or count -/+ 1 (CMP idx,N / N-1)."""
    fam = [
        a["name"]
        for a in arrs
        if a["players"] > 1 and value in (a["count"], a["count"] - 1, a["count"] + 1)
    ]
    return fam


def bound_value(note, expect):
    m = re.search(r"CMP [^,]+,(0x[0-9a-fA-F]+)", note)
    if m:
        return int(m.group(1), 16)
    m = re.search(r"(\d+)->", note)
    if m:
        return int(m.group(1))
    return None


# ---- loop analysis (needs the exe) ----------------------------------------------------------------------------


class Loops:
    def __init__(self, img, funcs, hits):
        from capstone import CS_ARCH_X86, CS_MODE_32, Cs

        self.img, self.funcs = img, funcs
        self.md = Cs(CS_ARCH_X86, CS_MODE_32)
        self.md.detail = True
        self.find = scan.body_finder(funcs)
        self.cache = {}
        self.fields = collections.defaultdict(list)
        for h in hits:
            if h["fn"] and h["role"] in ("disp", "abs", "imm_addr", "row_stride", "shift_chain"):
                self.fields[h["fn"]].append(h)

    def rd(self, fn):
        if fn[0] not in self.cache:
            code = self.img.read(fn[0], fn[1] - fn[0] + 1)
            tables = {}
            self.cache[fn[0]] = scan.recursive_descent(
                self.md, code, fn[0], fn[1], self.img, tables
            )
            self.cache[("tables", fn[0])] = tables
        return self.cache[fn[0]]

    def insn_containing(self, fn, va):
        for a, i in self.rd(fn).items():
            if a <= va < a + i.size:
                return i
        return None

    def cfg(self, fn):
        """-> (succs, preds, cover) over the recursive-descent instructions of fn."""
        key = ("cfg", fn[0])
        if key in self.cache:
            return self.cache[key]
        from capstone.x86 import X86_GRP_JUMP, X86_GRP_RET, X86_OP_IMM, X86_OP_MEM

        m = self.rd(fn)
        succs = collections.defaultdict(set)
        cover = {}
        for a, i in m.items():
            for k in range(i.size):
                cover[a + k] = a
        for a, i in m.items():
            nxt = a + i.size
            if i.group(X86_GRP_RET):
                continue
            if i.group(X86_GRP_JUMP):
                op = i.operands[0] if i.operands else None
                if op is not None and op.type == X86_OP_IMM:
                    t = op.imm & 0xFFFFFFFF
                    if t in m:
                        succs[a].add(t)
                elif a in self.cache[("tables", fn[0])]:
                    for v in self.cache[("tables", fn[0])][a]:
                        if v in m:
                            succs[a].add(v)
                if i.mnemonic == "jmp":
                    continue
            if nxt in m:
                succs[a].add(nxt)
        preds = collections.defaultdict(set)
        for a, ss in succs.items():
            for t in ss:
                preds[t].add(a)
        self.cache[key] = (succs, preds, cover)
        return self.cache[key]

    def loop_set(self, fn, c):
        """-> (set of instruction addresses of the SMALLEST natural loop containing the instruction at c,
        description). A natural loop is a back edge p->h (h <= p) plus every node that reaches p without
        passing through h; this handles the compound-condition shape
        `L: cmp i,N; jge EXIT; <test>; jle BODY; EXIT: jmp OUT; INC: ..; jmp L; BODY: ..; jmp INC`
        that a plain address range cannot."""
        succs, preds, cover = self.cfg(fn)
        best = None
        for p, ss in succs.items():
            for h in ss:
                if h > p:
                    continue
                body = {h}
                stack = [p]
                while stack:
                    n = stack.pop()
                    if n in body:
                        continue
                    body.add(n)
                    stack.extend(preds.get(n, ()))
                if c in body and (best is None or len(body) < len(best[0])):
                    best = (body, "natural loop head 0x%08x, %d instructions" % (h, len(body)))
        if best is None:
            return None, "no enclosing loop"
        return best

    def foreign_refs(self, fn, addrs):
        """Absolute data addresses the instruction set addresses that belong to NO cataloged array."""
        from capstone.x86 import X86_OP_MEM

        cls = scan.Classifier(scan.arrays())
        out = {}
        m = self.rd(fn)
        for a in addrs:
            i = m.get(a)
            if i is None:
                continue
            for o in i.operands:
                if o.type == X86_OP_MEM and i.disp_size == 4:
                    d = o.mem.disp & 0xFFFFFFFF
                    if d >= 0x500000 and cls.addr(d) is None:
                        out[d] = 1
        return ["0x%08x" % d for d in sorted(out)][:4]

    def arrays_touched(self, fn, addrs):
        """-> Counter of arrays with a decoded field / element-stride arithmetic in the instruction set."""
        from capstone.x86 import X86_OP_IMM

        _s, _p, cover = self.cfg(fn)
        got = collections.Counter()
        for h in self.fields.get(fn[2], ()):
            if cover.get(h["va"]) in addrs:
                got[h["array"]] += 1
        elem = {v: k for k, v in ELEM.items()}
        m = self.rd(fn)
        for a in addrs:
            i = m.get(a)
            if i is not None and i.mnemonic in ("imul", "add", "lea"):
                for o in i.operands:
                    if o.type == X86_OP_IMM and (o.imm & 0xFFFFFFFF) in elem:
                        got[elem[o.imm & 0xFFFFFFFF]] += 1
        return got


def classify_bounds(patch, loops, arrs):
    """-> (layer, cls, arrays, rule, extra) for a B1 patch."""
    va = patch["va"]
    fn = loops.find(va)
    note = patch["note"]
    if fn is None:
        return "?", "bound", [], "B1", {"why": "not in any function"}
    if note.startswith("widen bound") or note.startswith("loop BUDGET") and False:
        c = va
    else:
        ins = loops.insn_containing(fn, va)
        c = ins.address if ins else None
    val = bound_value(note, patch["expect"])
    cls = (
        "widen_bound"
        if note.startswith("widen bound")
        else "allocator_bound"
        if note.startswith("allocator bound")
        else "round_robin_budget"
        if note.startswith("loop BUDGET")
        else "loop_bound"
    )
    cand = family(val, arrs) if val is not None else []
    rng, shape = loops.loop_set(fn, c) if c is not None else (None, "no-insn")
    body = loops.arrays_touched(fn, rng) if rng else collections.Counter()
    fnall = loops.arrays_touched(fn, set(loops.rd(fn)))
    extra = {"fn": fn[2], "value": val, "loop": shape, "body_arrays": dict(body)}
    pick = [a for a in body if (not cand or a in cand)]
    if not pick and not body:
        # No cataloged array inside the loop (the access goes through a helper, or the loop is not an array
        # loop at all). Fall back to the function: accept it only when every candidate is on the SAME side of
        # the in-scope / excluded line -- otherwise the bound is not attributable and is listed.
        fb = [a for a in fnall if (not cand or a in cand) and a != "projectiles"]
        sides = {a in EXCLUDED for a in fb}
        if fb and len(sides) == 1:
            pick = fb
            extra["basis"] = "function-level (no cataloged array inside the loop)"
        else:
            foreign = loops.foreign_refs(fn, rng) if rng else []
            return (
                "?",
                cls,
                sorted(fb),
                "B1",
                dict(
                    extra,
                    foreign_refs=foreign,
                    suspect="the loop the bound controls touches no cataloged array"
                    + ("; it addresses %s instead" % ", ".join(foreign) if foreign else "")
                    + (
                        "; function-level candidates span in-scope and excluded arrays: %s"
                        % sorted(fb)
                        if fb
                        else ""
                    ),
                ),
            )
    if not pick:
        return (
            "?",
            cls,
            sorted(body),
            "B1",
            dict(extra, suspect="loop arrays %s do not match bound value %s" % (sorted(body), val)),
        )
    pick = sorted(pick)
    ex = [a for a in pick if a in EXCLUDED]
    if ex and len(ex) == len(pick):
        return "x", cls, pick, "B1", extra
    if ex:
        return (
            "?",
            cls,
            pick,
            "B1",
            dict(extra, suspect="shared with units/soldiers inside one loop"),
        )
    return "b", cls, pick, "B1", extra


def classify_walk(patch, loops):
    fn = loops.find(patch["va"])
    extra = {"fn": fn[2] if fn else None}
    if fn is None:
        return "?", "walk_backstop", [], "W1", dict(extra, suspect="not in any function")
    ins = loops.insn_containing(fn, patch["va"])
    c = ins.address if ins else patch["va"]
    rng, shape = loops.loop_set(fn, c)
    if rng is None:
        rng = {a for a in loops.rd(fn) if c <= a < c + 0x80}
        shape = "fallback window"
    got = loops.arrays_touched(fn, rng)
    extra.update(loop=shape, body_arrays=dict(got))
    if not got:
        return (
            "?",
            "walk_backstop",
            [],
            "W1",
            dict(extra, suspect="walk body touches no cataloged array"),
        )
    arr = sorted(got)
    if all(a in EXCLUDED for a in arr):
        return "x", "walk_backstop", arr, "W1", extra
    if any(a in EXCLUDED for a in arr):
        return "?", "walk_backstop", arr, "W1", dict(extra, suspect="walk shares units/soldiers")
    return "a", "walk_backstop", arr, "W1", extra


def classify(patches, hits, loops, arrs):
    byva = collections.defaultdict(list)
    for h in hits:
        byva[h["va"]].append(h)
    rows = []
    for p in patches:
        note, va, ln = p["note"], p["va"], p["len"]
        h = None
        if ln == 4 and va in byva:
            cand = [
                x
                for x in byva[va]
                if x["role"]
                in ("disp", "abs", "imm_addr", "row_stride", "block_size", "row_multiple")
            ]
            h = cand[0] if cand else None
        extra = {}
        if h is not None:
            role, arr, fn = h["role"], h["array"], h["fn"]
            if role in ("disp", "abs", "imm_addr"):
                layer, cls, rule = "a", "base", "F1"
            elif role in ("row_stride", "row_multiple"):
                layer, cls, rule = "a", "stride", "F2"
            elif fn in SAVE_FNS:
                layer, cls, rule = "d", "save_block", "F3"
            else:
                layer, cls, rule = "a", "clear_size", "F4"
            arrays_ = [arr]
            extra["fn"] = fn
            if arr in EXCLUDED:
                layer = "x"
        elif note.startswith("shift row-stride splice"):
            layer, cls, arrays_, rule = "a", "shift_splice", ["buildings"], "S1"
        elif note.startswith("walk-bound"):
            layer, cls, arrays_, rule, extra = classify_walk(p, loops)
        elif re.match(r"(loop bound|widen bound|allocator bound|loop BUDGET)", note):
            layer, cls, arrays_, rule, extra = classify_bounds(p, loops, arrs)
        elif "-> cave 0x013c" in note:
            layer, cls, arrays_, rule = "c", "cave_redirect", ["unit_storage"], "C1"
            f = loops.find(va)
            extra = {"fn": f[2] if f else None, "cave": note.split("-> cave ")[1].strip()}
        elif note.startswith("cap sentinel") or note.startswith("HEURISTIC loop-bound"):
            layer, cls, arrays_, rule = (
                "b",
                "projectile_sentinel" if note.startswith("cap") else "projectile_loop_bound",
                ["projectiles"],
                "P1",
            )
            if note.startswith("HEURISTIC"):
                extra = {"heuristic": True}
        else:
            layer, cls, arrays_, rule, extra = (
                "?",
                "unknown",
                [],
                "-",
                {"suspect": "no rule matches this note: %r" % note[:80]},
            )
        rows.append(dict(p, layer=layer, cls=cls, arrays=arrays_, rule=rule, extra=extra))
    return rows


def summarize(rows):
    s = collections.OrderedDict()
    for L in "abcdx?":
        sub = [r for r in rows if r["layer"] == L]
        s[L] = {
            "name": LAYERS[L],
            "sites": len(sub),
            "by_class": dict(sorted(collections.Counter(r["cls"] for r in sub).items())),
            "by_array": dict(
                sorted(collections.Counter(a for r in sub for a in (r["arrays"] or ["-"])).items())
            ),
        }
    return s


def pack(rows, overlaps):
    fns = sorted({r["extra"].get("fn") for r in rows if r["extra"].get("fn")})
    fi = {f: i for i, f in enumerate(fns)}
    ms = {m: i for i, m in enumerate(SOURCES)}
    cols = ["va", "len", "manifest", "layer", "class", "arrays", "rule", "fn"]
    flat = []
    ov = {(o["site"]) for o in overlaps}
    for r in sorted(rows, key=lambda r: (r["manifest"], r["va"])):
        flat.append(
            [
                r["va"],
                r["len"],
                ms[r["manifest"]],
                r["layer"],
                r["cls"],
                ",".join(r["arrays"]),
                r["rule"],
                fi.get(r["extra"].get("fn"), -1),
            ]
        )
    return {"cols": cols, "manifests": SOURCES, "fns": fns, "rows": flat}


def regen(exe_path):
    raw = open(exe_path, "rb").read()
    img = scan.Image(raw)
    funcs = scan.load_funcs()
    summary, hits = scan.load_committed()
    arrs = scan.arrays()
    loops = Loops(img, funcs, hits)
    patches = load_patches()
    rows = classify(patches, hits, loops, arrs)
    # liveness of the function each row sits in (the scan's reachability, re-derived)
    taken = scan.address_taken(img, [f[0] for f in funcs])
    seen, callers = scan.reachability(funcs, taken, img.entry)
    names = {f[0]: f[2] for f in funcs}
    find = scan.body_finder(funcs)
    dead_fns = set()
    for r in rows:
        f = find(r["va"])
        if f and f[0] not in seen:
            dead_fns.add(f[2])
            r["extra"]["dead_function"] = True
    comp = json.load(
        open(os.path.join(REPO, "tools", "data", "inmem_composition_census.json"), encoding="utf-8")
    )
    overlaps = [o for o in comp["cap_set_overlaps"]]
    ovsite = {int(o["site"], 16): o for o in overlaps}
    for r in rows:
        if r["va"] in ovsite:
            r["extra"]["dll_patch_overlap"] = ovsite[r["va"]]["patch"]
    unclassified = [
        {
            "va": "0x%08x" % r["va"],
            "len": r["len"],
            "manifest": r["manifest"],
            "class": r["cls"],
            "arrays": r["arrays"],
            "note": r["note"][:160],
            "reason": r["extra"].get("suspect", ""),
            "fn": r["extra"].get("fn"),
        }
        for r in rows
        if r["layer"] == "?"
    ]
    excl = [r for r in rows if r["layer"] == "x"]
    shared = [r for r in rows if r["layer"] != "x" and any(a in EXCLUDED for a in r["arrays"])]
    # per-array counts of the INCLUDED sites by (layer, class)
    per_array = collections.OrderedDict()
    for a in [x["name"] for x in arrs if x["name"] not in EXCLUDED]:
        sub = [r for r in rows if a in r["arrays"] and r["layer"] in "abcd"]
        per_array[a] = {
            "total": len(sub),
            "by_layer_class": {
                "%s/%s" % (L, c): n
                for (L, c), n in sorted(
                    collections.Counter((r["layer"], r["cls"]) for r in sub).items()
                )
            },
        }
    sections = []
    for m in SOURCES:
        doc = json.load(open(os.path.join(PATCHER, m + ".mh.patch.json"), encoding="utf-8"))
        for sec in doc.get("sections", []):
            sections.append(
                {
                    "manifest": m,
                    "name": sec["name"],
                    "vaddr": sec["vaddr"],
                    "vsize": sec["vsize"],
                    "data_bytes": len(sec["data"].split()) if sec.get("data") else 0,
                }
            )
    plan = {
        "_generated_by": "tools/gen_cap_split_plan.py --regen",
        "_what": (
            "PT-CAP8 S0: the v0.2.2 cap-raise manifest split plan. Every patch of "
            "grand_all_caphike_storagecap_EN + projectile_pool_caphike_EN assigned to ONE class "
            "(a relocation, b limit, c storage-cap caves, d save block stays vanilla, x units/soldiers "
            "excluded, ? unclassified). Rules F1-F4/S1/W1/B1/C1/P1: see the tool docstring. NOT wired "
            "into mh.dll -- S1 consumes it."
        ),
        "sources": SOURCES,
        "sections": sections,
        "summary": summarize(rows),
        "per_array_included": per_array,
        "excluded_units_soldiers": {
            "sites": len(excl),
            "by_array_class": {
                "%s/%s" % (a, c): n
                for (a, c), n in sorted(
                    collections.Counter((",".join(r["arrays"]), r["cls"]) for r in excl).items()
                )
            },
            "sites_sharing_an_instruction_with_an_included_array": [
                {"va": "0x%08x" % r["va"], "arrays": r["arrays"], "class": r["cls"]} for r in shared
            ],
        },
        "unclassified_or_suspect": unclassified,
        "dll_patch_overlaps": overlaps,
        "dead_function_sites": sum(1 for r in rows if r["extra"].get("dead_function")),
        "dead_functions": sorted(dead_fns),
        "heuristic_sites": ["0x%08x" % r["va"] for r in rows if r["extra"].get("heuristic")],
        "packed": pack(rows, overlaps),
    }
    write_plan(plan)
    print_report(plan)
    return 0


def write_plan(plan):
    """indent=1 for the summary, one line per packed row (an indented list-of-lists is 5x the bytes)."""
    packed = plan.pop("packed")
    head = json.dumps(plan, indent=1)
    rows = ",\n".join(json.dumps(r, separators=(",", ":")) for r in packed["rows"])
    meta = json.dumps({k: v for k, v in packed.items() if k != "rows"}, separators=(",", ":"))[:-1]
    text = head[:-2] + ',\n "packed": ' + meta + ',"rows":[\n' + rows + "\n]}\n}\n"
    with open(PLAN, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(text)
    plan["packed"] = packed


def print_report(plan):
    print("split plan: %d sites" % sum(v["sites"] for v in plan["summary"].values()))
    for L, v in plan["summary"].items():
        print("  %s %-24s %5d  %s" % (L, v["name"], v["sites"], v["by_class"]))
    print("per included array:")
    for a, v in plan["per_array_included"].items():
        print("  %-13s %5d  %s" % (a, v["total"], v["by_layer_class"]))
    print(
        "excluded units/soldiers:",
        plan["excluded_units_soldiers"]["sites"],
        plan["excluded_units_soldiers"]["by_array_class"],
    )
    print(
        "sharing an instruction with an included array:",
        plan["excluded_units_soldiers"]["sites_sharing_an_instruction_with_an_included_array"],
    )
    print("unclassified/suspect: %d" % len(plan["unclassified_or_suspect"]))
    for u in plan["unclassified_or_suspect"]:
        print("   ", u["va"], u["class"], u["arrays"], u["fn"], "|", u["reason"])
    print(
        "DLL patch overlaps:",
        len(plan["dll_patch_overlaps"]),
        " dead-function sites:",
        plan["dead_function_sites"],
    )


# ---- hermetic check ------------------------------------------------------------------------------------------------


def check_plan(plan, patches):
    """-> failure strings: plan rows must be exactly the CURRENT source manifests' patches, once each."""
    fails = []
    want = collections.Counter((SOURCES.index(p["manifest"]), p["va"], p["len"]) for p in patches)
    have = collections.Counter((r[2], r[0], r[1]) for r in plan["packed"]["rows"])
    for k in sorted((want - have)):
        fails.append(
            "patch %s@0x%08x len %d is in the manifest but NOT in the plan -- run --regen"
            % (SOURCES[k[0]], k[1], k[2])
        )
    for k in sorted((have - want)):
        fails.append(
            "plan row %s@0x%08x len %d has no patch in the manifest any more -- run --regen"
            % (SOURCES[k[0]], k[1], k[2])
        )
    layers = collections.Counter(r[3] for r in plan["packed"]["rows"])
    for L, v in plan["summary"].items():
        if v["sites"] != layers.get(L, 0):
            fails.append(
                "summary count for layer %s (%d) disagrees with the rows (%d)"
                % (L, v["sites"], layers.get(L, 0))
            )
    if plan["sources"] != SOURCES:
        fails.append("plan sources %r differ from %r" % (plan["sources"], SOURCES))
    n_unc = len(plan["unclassified_or_suspect"])
    if n_unc != layers.get("?", 0):
        fails.append(
            "unclassified list (%d) disagrees with the '?' rows (%d)" % (n_unc, layers.get("?", 0))
        )
    return fails


def cmd_check():
    if not os.path.isfile(PLAN):
        print("gen_cap_split_plan: MISSING %s -- run --regen" % PLAN)
        return 1
    plan = json.load(open(PLAN, encoding="utf-8"))
    fails = check_plan(plan, load_patches())
    if fails:
        for f in fails[:20]:
            print("  FAIL %s" % f)
        print("gen_cap_split_plan --check: FAIL (%d)" % len(fails))
        return 1
    print(
        "gen_cap_split_plan --check: ok (%d sites; %d unclassified/suspect listed)"
        % (len(plan["packed"]["rows"]), len(plan["unclassified_or_suspect"]))
    )
    return 0


def selftest():
    fails = []

    def expect(what, ok, detail=None):
        print("  [%s] %s" % ("ok" if ok else "FAIL", what))
        if not ok:
            fails.append(what)
            if detail is not None:
                print("       got: %r" % (detail,))

    arrs = scan.arrays()
    expect(
        "bound 100 -> the three rosters",
        set(family(100, arrs)) == {"buildings", "units", "soldiers"},
        family(100, arrs),
    )
    expect("bound 0x20 -> turrets and mines", set(family(0x20, arrs)) == {"turrets", "mines"})
    expect(
        "bound 0x19 -> unit_storage and labs", set(family(0x19, arrs)) == {"unit_storage", "labs"}
    )
    expect("bound 8 -> productions", family(8, arrs) == ["productions"])
    expect(
        "bound 16 matches no array (allocator bounds are read off the function)",
        family(16, arrs) == [],
    )
    expect(
        "the CMP imm is parsed out of a loop-bound note",
        bound_value("loop bound (CMP dword ptr [EBP + -0x1c],0x20 in FillDefaults) 32->127", "")
        == 0x20,
    )
    if os.path.isfile(PLAN):
        plan = json.load(open(PLAN, encoding="utf-8"))
        patches = load_patches()
        expect(
            "the committed plan matches the manifests",
            check_plan(plan, patches) == [],
            check_plan(plan, patches)[:2],
        )
        expect(
            "every layer count is positive but the unclassified one is small",
            plan["summary"]["a"]["sites"] > 1000 and plan["summary"]["?"]["sites"] < 80,
        )
        # mutation A: a new patch appears in a manifest
        mp = patches + [dict(patches[0], va=0x401234)]
        f = check_plan(plan, mp)
        expect(
            "mutation A: a manifest patch missing from the plan goes RED",
            any("NOT in the plan" in x for x in f),
            f[:2],
        )
        # mutation B: a patch disappears
        f = check_plan(plan, patches[1:])
        expect(
            "mutation B: a plan row with no manifest patch goes RED",
            any("no patch in the manifest" in x for x in f),
            f[:2],
        )
        # mutation C: a corrupt layer count
        bad = json.loads(json.dumps(plan))
        bad["summary"]["a"]["sites"] += 1
        f = check_plan(bad, patches)
        expect(
            "mutation C: a summary that disagrees with the rows goes RED",
            any("summary count" in x for x in f),
            f[:2],
        )
        # mutation D: a changed length (a splice resized)
        mp = [dict(p) for p in patches]
        mp[5]["len"] += 1
        f = check_plan(plan, mp)
        expect("mutation D: a patch whose length changed goes RED", len(f) >= 2, f[:2])
        # structural truths about the plan
        rows = plan["packed"]["rows"]
        keys = collections.Counter((r[2], r[0]) for r in rows)
        expect(
            "each (manifest, va) is classified once",
            max(keys.values()) == 1,
            [k for k, v in keys.items() if v > 1][:3],
        )
        expect(
            "no units/soldiers site is in an included layer",
            not any(
                ("units" in r[5].split(",") or "soldiers" in r[5].split(",")) and r[3] in "abcd"
                for r in rows
            ),
        )
    else:
        expect("committed plan present", False)
    print("gen_cap_split_plan --selftest: %s" % ("FAIL" if fails else "PASS"))
    return 1 if fails else 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--regen", action="store_true")
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--report", action="store_true")
    ap.add_argument("--exe")
    a = ap.parse_args(argv)
    if a.selftest:
        return selftest()
    if a.report:
        print_report(json.load(open(PLAN, encoding="utf-8")))
        return 0
    if a.regen:
        exe = a.exe or scan.default_exe()
        if not exe:
            print("gen_cap_split_plan: no clean EN exe; pass --exe")
            return 1
        return regen(exe)
    if a.check:
        return cmd_check()
    ap.print_help()
    return 2


if __name__ == "__main__":
    sys.exit(main())
