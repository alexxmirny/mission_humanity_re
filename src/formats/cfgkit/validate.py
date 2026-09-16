"""Static validator (Phase 3): catch edits the game would choke on or silently
mangle, before they ship. Dependency-free and table-driven off
:mod:`cfgkit.grammar` (the plan named pydantic; the validation *logic* — caps,
ranges, refs, cross-refs — is identical, and driving it from the grammar tables
avoids a second copy of the type info and a heavy dependency).

Two tiers, over a parsed :class:`~cfgkit.parse.Model`:
  1. per-object: name/TEXT length, slot & count caps, probability range;
  2. registry cross-refs: every sprite/anim/icon/define/invention/weapon/unit/
     building/planet/progress reference resolves; DEF_BANK id-space arithmetic;
     each PLANET loads exactly one 50–56 tree bank; PROTECTED_NAMES exist;
     class-count / anim-frame-budget growth guards; duplicate names.

Every reference field resolves 100% on retail, so dangling refs are ERRORs; the
softer heuristics (probability range, tree-bank count, DEF_BANK overlap,
duplicates, asset files) are WARNINGs. Checkpoint: retail yields **zero errors**.
"""
from __future__ import annotations

import os
from collections import Counter, namedtuple

from . import grammar as G
from ._paths import INIT_DEFAULT, LANG_DEFAULT

Finding = namedtuple("Finding", "level cls obj field message")


def build_registry(model) -> dict:
    sprites = {s for b in model.banks for s, _ in b["sprites"]}
    anims = {r["__name__"] for r in model.classes["ANIM"]}
    icons = {n for n, _ in model.icons}
    defines = {n for n, _ in model.defines}
    def nm(c):
        return {r["__name__"] for r in model.classes[c]}
    units, builds, weaps, plans, progs = (nm(c) for c in
        ("UNIT", "BUILDING", "WEAPON", "PLANET", "PROGRESS"))
    return {
        "art": sprites | anims, "sprite": sprites, "icon": icons,
        "define": defines, "invention": defines | progs,
        "weapon": weaps, "building": builds, "unit_or_bldg": units | builds,
        "planet": plans, "obj": units | weaps | builds, "progress": progs,
    }


def _ref_values(rec, field):
    v = rec.get(field)
    if isinstance(v, str):
        return [v] if v else []
    if isinstance(v, dict):                       # slot_str array
        return [x for x in v.values() if isinstance(x, str) and x]
    if isinstance(v, list):                       # DEPEND list
        return [x for x in v if isinstance(x, str) and x]
    return []


def validate(model, assets_root=None) -> list:
    reg = build_registry(model)
    F = []
    err = lambda *a: F.append(Finding("error", *a))
    warn = lambda *a: F.append(Finding("warn", *a))

    # ---- per-object: caps, name length, probability ----
    # NB: duplicate *object* names are legal in this engine — retail ships many
    # (ANIM A_PUSTY x23 / A_STRZAL0 x46 placeholders, BUILDING N_*_Chatka x8),
    # some even referenced (by-name refs resolve first-match). So we do NOT warn
    # on object-name duplicates; only the global DEFINE/ICON/DEF_BANK registries
    # (unique in retail, first-match shadowing) get a duplicate check below.
    for cls, recs in model.classes.items():
        for rec in recs:
            name = rec["__name__"]
            if len(name) > G.NAME_MAXLEN:
                err(cls, name, "__name__", f"name is {len(name)} chars (>{G.NAME_MAXLEN})")
            for (c, field), (mode, n) in G.CAPS.items():
                if c != cls:
                    continue
                v = rec.get(field)
                if mode == "count":
                    if v and len(v) > n:
                        err(cls, name, field, f"{len(v)} entries (max {n})")
                elif isinstance(v, dict):
                    lo = 1 if mode == "slot1" else 0
                    for slot in v:
                        if slot < lo or slot >= n:
                            err(cls, name, field, f"slot {slot} out of range [{lo}..{n - 1}]")
            for slot, pv in (rec.get("probability") or {}).items():
                if not (0 <= pv <= G.PROB_MAX):
                    warn(cls, name, "probability", f"slot {slot} = {pv} (expected 0..{G.PROB_MAX})")

    # ---- per-object: cross-references ----
    for (cls, field), uni in G.REF.items():
        universe = reg[uni]
        for rec in model.classes.get(cls, []):
            for ref in _ref_values(rec, field):
                if ref not in universe:
                    err(cls, rec["__name__"], field, f"dangling {uni} reference {ref!r}")

    # ---- optional asset files ----
    if assets_root:
        for (cls, field), ext in G.ASSET_FIELDS.items():
            for rec in model.classes.get(cls, []):
                v = rec.get(field)
                if v and not os.path.exists(os.path.join(assets_root, v)):
                    warn(cls, rec["__name__"], field, f"asset file not found: {v}")

    # ---- global registries: DEFINE / ICON / DEF_BANK duplicates ----
    for label, pairs in (("DEFINE", model.defines), ("ICON", model.icons),
                         ("DEF_BANK", model.def_banks)):
        dup = [n for n, c in Counter(n for n, _ in pairs).items() if c > 1]
        for n in dup:
            warn(label, n, "name", f"duplicate {label} name")

    # ---- class-count / anim-budget growth guards ----
    for cls, cap in G.CLASS_CAPS.items():
        got = len(model.classes[cls])
        if got > cap:
            err(cls, "*", "count", f"{got} objects (game array cap {cap})")
    frames = sum(r["length"] for r in model.classes["ANIM"])
    if frames > G.ANIM_FRAME_BUDGET:
        err("ANIM", "*", "length", f"anim frame budget {frames} > {G.ANIM_FRAME_BUDGET}")

    # ---- DEF_BANK id-space arithmetic ----
    base = dict(model.def_banks)
    spans = []
    for b in model.banks:
        nm = b["name"]
        if nm not in base:
            warn("BANK", nm, "name", "BANK section has no DEF_BANK base")
            continue
        end = base[nm] + sum(fr for _, fr in b["sprites"])
        spans.append((base[nm], end, nm))
        if end > G.SPRITE_ID_CAP:
            err("BANK", nm, "sprites", f"sprite ids reach {end} (>{G.SPRITE_ID_CAP})")
    spans.sort()
    for (lo, hi, nm), (nlo, _, nnm) in zip(spans, spans[1:]):
        if hi > nlo:
            warn("BANK", nm, "sprites", f"sprite-id span [{lo},{hi}) overlaps {nnm} (base {nlo})")

    # ---- PLANET tree-bank rule ----
    # Retail: 24 planets load exactly one 50..56 tree bank; planet 104 loads two
    # (50 & 56) — so >1 is legal. Only 0 (no tree catalog) is suspicious.
    for rec in model.classes["PLANET"]:
        trees = [b for b in rec["banks"] if b in G.TREE_BANK_RANGE]
        if len(trees) == 0:
            warn("PLANET", rec["__name__"], "banks",
                 "loads no tree bank (50..56); every retail planet loads at least one")

    # ---- PROTECTED_NAMES ----
    art = reg["art"]
    for pn in sorted(G.PROTECTED_NAMES):
        if pn not in art:
            err("PROTECTED", pn, "name", "required exe-referenced anim/sprite is missing")
    for pref in G.PROTECTED_PREFIXES:
        if not any(a.startswith(pref) for a in art):
            err("PROTECTED", pref + "*", "name", "no anim/sprite with required prefix")

    return F


def summarize(findings):
    errs = [f for f in findings if f.level == "error"]
    warns = [f for f in findings if f.level == "warn"]
    return len(errs), len(warns)


def _print(findings, show_warn=True, limit=60):
    for f in findings:
        if f.level == "warn" and not show_warn:
            continue
        print(f"  [{f.level:5}] {f.cls}.{f.field} {f.obj!r}: {f.message}")


def main(argv=None):
    import argparse
    from .parse import parse_files
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--init", default=INIT_DEFAULT)
    ap.add_argument("--lang", default=LANG_DEFAULT)
    ap.add_argument("--assets-root", help="check PLANET.map etc. exist under this dir")
    args = ap.parse_args(argv)

    model = parse_files(args.init, args.lang)
    findings = validate(model, assets_root=args.assets_root)
    n_err, n_warn = summarize(findings)

    print(f"=== validation: {n_err} errors, {n_warn} warnings ===")
    _print(findings)

    # Discrimination smoke-test: a dangling ref injected into a copy must be
    # caught (guards against a validator that vacuously passes everything).
    import copy
    probe = copy.deepcopy(model)
    probe.classes["UNIT"][0]["sprite"] = "S_DOES_NOT_EXIST"
    caught = any(f.level == "error" and "dangling" in f.message for f in validate(probe))
    print(f"discrimination: {'OK (catches an injected dangling ref)' if caught else 'FAIL (blind!)'}")

    ok = n_err == 0 and caught
    print(f"\nPHASE 3 CHECKPOINT (retail validates clean): {'PASS' if ok else 'FAIL'}")
    return 0 if ok else 1


if __name__ == "__main__":
    import sys
    sys.exit(main())
