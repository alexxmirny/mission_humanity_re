#!/usr/bin/env python3
"""gen_cap_units_manifests.py -- PT-CAP9-U1: the units + soldiers relocation / limit manifests, as GENERATED data.

Two manifests, applied in memory by mh.dll next to the v0.2.2 pair (nothing here edits cap_v022_*):

  src/patcher/cap_v023_units_reloc_EN.mh.patch.json    class "caps_units"         RELOCATION layer, always applied
  src/patcher/cap_v023_units_limits_EN.mh.patch.json   class "caps_units_limits"  LIMIT layer, `toggle: true`

units [8][100] x 233 B (0xdd8c48) and soldiers [8][100] x 29 B (0xe0e5a8) are ALWAYS relocated into `.mhcapu`
(units [8][500], soldiers [8][500], one 64-byte aligned layout) and the stock .bss extents become the canonical
mirror (spec D1), so the planet save/load block sites (the 8 `save_layer` sites of the census) stay vanilla and are
not in these files. The limit layer holds the 27 sites the census VERDICTS call `raise`: the per-player unit /
soldier bounds (100 -> 500) and the recruit caps (91 -> 491), each as an imm32-in-place edit or a cave.

Inputs, all committed: tools/data/cap_units_census.json (every move site with its value, the verdict on every
limit row, and the exe window of every raise row) and the July grand manifest (a CROSS-CHECK only: the same
arithmetic under the July layout must land every site on a constant delta, and July's own widened-bound caves
must carry the same immediates). The clean EN exe is needed only by --write and --selftest: its proofs (P1
expect==exe, P3 flags/register liveness at every cave exit) are cached in each output's `_proofs.exe` block and
--check verifies the cache is complete and current.

THE U4 HOOK. A site is (va, vanilla bytes, relocated bytes, optional RAISED bytes). The 58 direct
`units.path_slot_id` (+0xaa) sites are ordinary relocation sites today and are tagged `u4` in the proofs; U4 will
rewrite the same instructions to a 16-bit read at +0xda by giving a site a `raised` value. Such a site lands in
the LIMIT layer with `expect` = the RELOCATED bytes (the reloc layer is applied first) and `bytes` = the raised
ones, and the layers may overlap only on that terms (proof P2b). Nothing sets `raised` yet; --selftest plants one.

  python tools/gen_cap_units_manifests.py --write [--exe PATH]   # derive, prove against the exe, write both
  python tools/gen_cap_units_manifests.py --check                # hermetic: re-derive, byte-compare, re-prove
  python tools/gen_cap_units_manifests.py --selftest             # planted mutations must go red (needs the exe)
  python tools/gen_cap_units_manifests.py --report

Proofs (each a refusal, each with a --selftest mutation): P1 expect==exe; P2 no overlap inside a layer, none with a
cap_v022 site, and across layers only the composed U4 shape; P2b a cross-layer overlap's `expect` is the relocated
bytes; P3 flags/register liveness at every cave exit; P5 no limit site in the entry window of a claimed body; P6
sites sorted by VA; P7 the July cross-check; plus array extents, field-inside-record, census coverage, save-layer
absence, and gim refs_complete.
"""

import argparse
import copy
import hashlib
import json
import os
import re
import struct
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import gen_cap_path_sites as gp  # noqa: E402  (PT-CAP9-U4b: the path-slot pool sites)
import gen_cap_v022_manifests as gcv  # noqa: E402
import gen_inmem_manifest as gim  # noqa: E402
import scan_cap_reloc_sites as scan  # noqa: E402
import scan_cap_units_census as cen  # noqa: E402

REPO = scan.REPO
PATCHER = scan.PATCHER
RELOC_NAME = "cap_v023_units_reloc_EN"
LIMITS_NAME = "cap_v023_units_limits_EN"
RELOC_PATH = os.path.join(PATCHER, RELOC_NAME + ".mh.patch.json")
LIMITS_PATH = os.path.join(PATCHER, LIMITS_NAME + ".mh.patch.json")
PATH_LAYOUT_H = os.path.join(REPO, "src", "mh_dll", "mh", "caps", "path_pool_layout.gen.h")
JULY = "grand_all_caphike_storagecap_EN"


# VAs: the static pipeline (mhpatch.add_bss_section, and so the offline parity oracle) lays sections down at the NEXT FREE VA of the chain
# reloc22 -> limits22 -> aiscratch22 -> reloc23 -> limits23, so they are derived from the v0.2.2 manifests' sections rather than chosen: the units host starts
# where .mhcapa ends (0x13d0000 today) and the limit caves where the units host ends. A section added to the chain ahead of these moves
# both: re-run `gen_cap_units_manifests.py --write`.
def _chain_end():
    end = 0
    for n in ("cap_v022_reloc_EN", "cap_v022_limits_EN", "cap_v022_aiscratch_EN"):
        doc = json.load(open(os.path.join(PATCHER, n + ".mh.patch.json"), encoding="utf-8"))
        for sec in doc["sections"]:
            end = max(end, int(sec["vaddr"], 16) + int(sec["vsize"], 16))
    return (end + 0xFFFF) & ~0xFFFF  # the image's section alignment (mhpatch next_free_va)


_U_VA = _chain_end()
SEC_U = (".mhcapu", _U_VA, 0x100000, 0xC0000040)  # zero-init units + soldiers host
# PT-CAP9-U4b: the path-slot pool host (buffers [8][501] slots + flags, one zero-init section) and its always-on cave section sit
# right after the units host; the limit caves (.mhcapv) follow them. Section order = the static pipeline's next-free-VA order.
SEC_P = (".mhcapp", _U_VA + 0x100000, gp.HOST_VSIZE, 0xC0000040)


def _al64k(v):
    return (v + 0xFFFF) & ~0xFFFF  # the image's section alignment (mhpatch next_free_va)


SEC_Q = (
    ".mhcapq",
    _al64k(SEC_P[1] + SEC_P[2]),
    gp.CAVE_SECTION_SIZE,
    0x60000020,
)  # path caves (always-on AND raise-switched)
SEC_V = (".mhcapv", _al64k(SEC_Q[1] + SEC_Q[2]), 0x1000, 0x60000020)  # limit caves (widened bounds)

RAISED_COUNT = 500
PLAYERS = 8
ARRAY_ORDER = ("units", "soldiers")
SOLDIER_OFF = 0xE38C0  # units end at 8 * 500 * 233 = 932000 = 0xe38a0, rounded up to 64
SAVE_FNS = cen.SAVE_FNS
SAVE_ROLES = cen.SAVE_ROLES
U4_FIELD_OFFSET = 0xAA  # units.path_slot_id
U4_DIRECT_SITES = 58  # tmp/capraise/u4 design: the direct path_slot_id sites that U4 rewrites
ROLE_KIND = {
    "disp": "base",
    "abs": "base",
    "imm_addr": "base",
    "row_stride": "stride",
    "row_multiple": "stride",
    "block_size": "clear",
}


class Refuse(Exception):
    pass


hx = gcv.hx
unhx = gcv.unhx
u32 = gcv.u32
p32 = gcv.p32


def arrays():
    out = {}
    off = 0
    for n, a in ((x["name"], x) for x in scan.arrays() if x["name"] in ARRAY_ORDER):
        out[n] = dict(
            name=n, base=a["base"], elem=a["elem"], vrow=a["row"], vsize=a["size"],
            players=PLAYERS, rrow=a["elem"] * RAISED_COUNT,
            rsize=PLAYERS * a["elem"] * RAISED_COUNT,
        )  # fmt: skip
    out["units"]["off"] = 0
    out["soldiers"]["off"] = SOLDIER_OFF
    if (out["units"]["rsize"] + 63) & ~63 != SOLDIER_OFF:
        raise Refuse(
            "units end at %#x; the soldier offset %#x is not their 64-aligned end"
            % (out["units"]["rsize"], SOLDIER_OFF)
        )
    end = SOLDIER_OFF + out["soldiers"]["rsize"]
    if end > SEC_U[2]:
        raise Refuse("arrays need %#x bytes, the section is %#x" % (end, SEC_U[2]))
    for a in out.values():
        a["new_base"] = SEC_U[1] + a["off"]
    return out


ARRS = None


def A():
    global ARRS
    if ARRS is None:
        ARRS = arrays()
    return ARRS


def load_census():
    return json.load(open(cen.CENSUS, encoding="utf-8"))


# ---- the site model ---------------------------------------------------------------------------------------------


def site(va, exp, new, kind, array, note, **kw):
    s = dict(
        va=va,
        exp=exp,
        new=new,
        kind=kind,
        array=array,
        note=note,
        raised=None,
        u4=False,
        p=None,
        q=None,
        jval=None,
    )
    s.update(kw)
    return s


def rebake(a, value):
    r = value - a["base"]
    if not 0 <= r < a["vsize"]:
        raise Refuse("value %#x is outside stock array %s" % (value, a["name"]))
    p, q = divmod(r, a["vrow"])
    return a["new_base"] + p * a["rrow"] + q, p, q


def reloc_sites(doc):
    """-> (sites, save_layer). One site per unique VA; census rows whose function overlaps another (63 units
    fields sit in two bodies) are listed once."""
    arrs = A()
    by_va = {}
    save = []
    for d in cen.site_dicts(doc["sites"]):
        if d["class"] == "exclude":
            if d["reason"] == "save_layer":
                save.append(d["va"])
            continue
        if d["class"] != "move":
            raise Refuse("census row %#x has class %r" % (d["va"], d["class"]))
        if d["role"] == "shift_chain":
            raise Refuse(
                "a shift_chain use at %#x: the units generator has no splice for it" % d["va"]
            )
        a = arrs[d["array"]]
        v = d["value"]
        kind = ROLE_KIND.get(d["role"])
        if kind is None:
            raise Refuse("census role %r at %#x has no relocation rule" % (d["role"], d["va"]))
        if d["w"] != 4:
            raise Refuse("census field at %#x is %d bytes wide" % (d["va"], d["w"]))
        if kind == "base":
            new, p, q = rebake(a, v)
            s = site(
                d["va"],
                p32(v),
                p32(new),
                "base",
                a["name"],
                "base %s [%d]+%d" % (a["name"], p, q),
                p=p,
                q=q,
            )
            if (
                a["name"] == "units"
                and q % a["elem"] == U4_FIELD_OFFSET
                and d["role"] in ("disp", "abs")
            ):
                s["u4"] = True
        elif d["role"] == "row_stride":
            if v != a["vrow"]:
                raise Refuse("row_stride %d at %#x is not %s's row" % (v, d["va"], a["name"]))
            s = site(
                d["va"],
                p32(v),
                p32(a["rrow"]),
                "stride",
                a["name"],
                "row stride %s %d->%d" % (a["name"], v, a["rrow"]),
            )
        elif d["role"] == "row_multiple":
            k, rem = divmod(v, a["vrow"])
            if rem or not 2 <= k <= 7:
                raise Refuse("row_multiple %d at %#x" % (v, d["va"]))
            s = site(
                d["va"],
                p32(v),
                p32(k * a["rrow"]),
                "stride",
                a["name"],
                "row multiple %s %dx" % (a["name"], k),
            )
        else:  # block_size
            if v != a["vsize"]:
                raise Refuse("block_size %d at %#x is not %s's extent" % (v, d["va"], a["name"]))
            s = site(
                d["va"],
                p32(v),
                p32(a["rsize"]),
                "clear",
                a["name"],
                "clear size %s %d->%d" % (a["name"], v, a["rsize"]),
            )
        old = by_va.get(d["va"])
        if old is not None:
            if (old["exp"], old["new"], old["array"]) != (s["exp"], s["new"], s["array"]):
                raise Refuse("two census rows at %#x disagree" % d["va"])
            continue
        by_va[d["va"]] = s
    return [by_va[k] for k in sorted(by_va)], sorted(save)


# ---- the limit layer ----------------------------------------------------------------------------------------------


def raise_rows(doc):
    """-> [(va, verdict, window, imm_old)] sorted by VA."""
    rows = {r[0]: r for r in doc["limits"]["rows"]}
    out = []
    for k, v in sorted(doc["verdicts"].items()):
        if v["verdict"] != cen.RAISE:
            continue
        va = int(k, 16)
        out.append((va, v, doc["raise_windows"][k], rows[va][3]))
    return out


def _ks():
    return gcv._ks()


def build_cave(va, v, win, imm_old, cave_va):
    """-> (code, exits, intended, name). The window with the immediate widened, any trailing jcc re-encoded
    to rel32, and a jump back to the first byte after the window."""
    raw = unhx(win["hex"])
    md = gcv._md()
    ins = []
    pos = 0
    while pos < len(raw):
        i = next(md.disasm(raw[pos:], va + pos, 1))
        ins.append(i)
        pos += i.size
    first = ins[0]
    to = v["to"]
    text = "%s %s" % (first.mnemonic, first.op_str)
    m = re.search(r"(0x[0-9a-fA-F]+)$", text)
    if not m or int(m.group(1), 16) != imm_old:
        raise Refuse(
            "window %#x: first instruction %r does not end in immediate %d" % (va, text, imm_old)
        )
    new_text = text[: m.start()] + "0x%x" % to
    first_b = gcv._asm(_ks(), new_text, 0)
    if len(first_b) <= first.size and to > 127:
        raise Refuse("window %#x: %r did not widen" % (va, new_text))
    body = bytearray(first_b)
    exits = []
    after = va + win["n"]
    intended = []
    last_jcc = False
    for i in ins[1:]:
        b0 = i.bytes[0]
        if i.group(1) and (b0 in cen.JCC_REL8 or b0 == 0x0F):
            tgt = i.operands[0].imm & 0xFFFFFFFF
            cc = (b0 - 0x70) if b0 in cen.JCC_REL8 else (i.bytes[1] - 0x80)
            at = cave_va + len(body)
            body += b"\x0f" + bytes([0x80 + cc]) + struct.pack("<i", tgt - (at + 6))
            exits.append(tgt)
            last_jcc = i is ins[-1]
        else:
            body += bytes(i.bytes)
    at = cave_va + len(body)
    body += b"\xe9" + struct.pack("<i", after - (at + 5))
    exits.append(after)
    if first.mnemonic in ("sub", "add", "mov", "and", "or", "xor") and first.op_str.split(",")[
        0
    ].strip() in ("eax", "ebx", "ecx", "edx", "esi", "edi"):
        intended.append(first.op_str.split(",")[0].strip())
    del last_jcc
    return bytes(body), exits, intended, "raise@%#x" % va


def limit_sites(doc):
    """-> (sites, caves). Caves are laid out in .mhcapv sorted by site VA."""
    rows = raise_rows(doc)
    caves = []
    cur = SEC_V[1]
    pre = []
    for va, v, win, imm in rows:
        if win["kind"] == "imm32":
            pre.append((va, v, win, imm, None))
            continue
        code, exits, intended, name = build_cave(va, v, win, imm, cur)
        if len(gcv._asm(_ks(), "nop", 0)) != 1:
            raise Refuse("keystone self-check")
        c = dict(name=name, kind="raise", site=va, exp=unhx(win["hex"]), code=code, va=cur, exits=exits,
                 flags="dead", intended=intended, to=v["to"], array=v["array"])  # fmt: skip
        caves.append(c)
        cur += len(code)
        pre.append((va, v, win, imm, c))
    if cur > SEC_V[1] + SEC_V[2]:
        raise Refuse("limit caves overflow .mhcapv")
    out = []
    for va, v, win, imm, c in pre:
        exp = unhx(win["hex"])
        if c is None:
            off = win["imm_off"]
            new = exp[:off] + p32(v["to"]) + exp[off + 4 :]
            out.append(
                site(
                    va,
                    exp,
                    new,
                    "limit",
                    v["array"],
                    "%s: imm32 in place %d->%d" % (win["ins"], imm, v["to"]),
                )
            )
        else:
            new = b"\xe9" + struct.pack("<i", c["va"] - (va + 5)) + b"\x90" * (len(exp) - 5)
            out.append(
                site(
                    va,
                    exp,
                    new,
                    "cave",
                    v["array"],
                    "%s -> cave %#x (%d->%d)" % (win["ins"], c["va"], imm, v["to"]),
                )
            )
    return out, caves


# ---- build / render ---------------------------------------------------------------------------------------------


class Built:
    pass


def path_census():
    return json.load(open(sp_census_path(), encoding="utf-8"))


def sp_census_path():
    return gp.sp.CENSUS


def _total_ok(blob):
    """Would the `.mhcapq` section, holding exactly `blob`, classify refs_complete? (gen_inmem_manifest's decode vs value-scan
    totality: a dword inside an operand that happens to read as a rel32 into one of the declared sections is a false positive
    the classifier cannot tell from a missed ref; a NOP of padding moves it.)"""
    secs = [
        (SEC_U[1], SEC_U[2], b""),
        (SEC_P[1], SEC_P[2], b""),
        (SEC_Q[1], SEC_Q[2], blob),
    ]
    found, reason = gim._section_refs(blob, SEC_Q[1], SEC_Q[2], secs, 2)
    if reason:
        return False
    return (
        gim._assert_total(
            blob, SEC_Q[1], secs, found, "path caves", internal=(SEC_Q[1], SEC_Q[1] + SEC_Q[2])
        )
        is None
    )


def merge_path(b, doc):
    """PT-CAP9-U4b: add the path-slot pool sites (tools/gen_cap_path_sites.py) to both layers and allocate their caves.

    EVERY path cave (the always-on ones AND the raise-switched ones) lives in `.mhcapq`, the relocation manifest's code section:
    a raise cave holds an absolute address into the relocated units array, and the classifier of the LIMIT manifest (which
    declares only `.mhcapv`) refuses an absolute naming a section it does not declare. The limit-layer site is then just the
    5-byte JMP into `.mhcapq` (a relative jump the classifier does not ask about); like every limit site's `expect` it already
    presumes the relocation manifest landed at its baked VAs."""
    census = path_census()
    lay = gp.Layout(SEC_P[1])
    pr, pl, defs = gp.derive(census, list(b.reloc), lay)
    b.path_layout = lay
    b.path_census = census
    b.caves_reloc = []
    blob = b""
    for st, build_fn, layer, name, flags, intended in sorted(defs, key=lambda d: d[0]["va"]):
        pad = 0
        while True:
            va = SEC_Q[1] + len(blob) + pad
            code, exits = build_fn(va)
            trial = blob + b"\x90" * pad + code
            if _total_ok(trial):
                break
            pad += 1
            if pad > 24:
                raise Refuse("cave %s: no padding in 0..24 gives a refs_complete section" % name)
        blob = trial
        wlen = len(st["exp"])
        st["new"] = gp.cave_site_bytes(st["va"], wlen, va)
        c = dict(name=name, kind="path", layer=layer, site=st["va"], exp=st["exp"], code=code, va=va, exits=exits,
                 flags=flags, intended=intended, to=None, array="path")  # fmt: skip
        b.caves_reloc.append(c)
    if len(blob) > SEC_Q[2]:
        raise Refuse("path caves overflow .mhcapq (%d > %d)" % (len(blob), SEC_Q[2]))
    b.path_cave_blob = blob
    b.reloc = sorted(b.reloc + pr, key=lambda x: x["va"])
    b.limits = b.limits + pl
    b.n_path_reloc, b.n_path_limits = len(pr), len(pl)


def build(doc, exe_proofs=None, planted_raise=None):
    b = Built()
    b.doc = doc
    b.arrs = A()
    b.reloc, b.save_layer = reloc_sites(doc)
    b.limits, b.caves = limit_sites(doc)
    # U4 hook: a `raised` rewrite on a relocation site becomes a limit-layer site (expect = the relocated bytes)
    if planted_raise:
        for va, raised in planted_raise.items():
            s = next(x for x in b.reloc if x["va"] == va)
            s["raised"] = raised
    for s in b.reloc:
        if s["raised"] is not None:
            b.limits.append(
                site(
                    s["va"],
                    s["new"],
                    s["raised"],
                    "raise",
                    s["array"],
                    "U4 raise-switched rewrite of the relocated %s" % s["note"],
                )
            )
    merge_path(b, doc)
    b.limits.sort(key=lambda x: x["va"])
    b.cave_data = b"".join(c["code"] for c in b.caves)
    b.cave_data_reloc = b.path_cave_blob
    b.exe_proofs = exe_proofs or {}
    return b


def _sites_sha(sites):
    h = hashlib.sha256()
    for s in sites:
        h.update(p32(s["va"]) + s["exp"] + b"|" + s["new"] + b"\n")
    return h.hexdigest()


def _masked_expect(b, s):
    """A limit-layer site's `expect`, with the bytes the reloc layer RE-BASES at run time wildcarded (`xx`). The reloc layer writes
    an absolute address into the units / path / cave sections into the instruction; at run time those sections sit at their ACTUAL
    base (not the baked one), so the baked value would fail the expected-bytes guard of the toggle layer. Only a 4-byte field
    whose value is an address inside the appended sections is wildcarded; strides and constants stay exact."""
    lo, hi = SEC_U[1], SEC_V[1] + SEC_V[2]
    exp = bytearray(s["exp"])
    mask = [False] * len(exp)
    for r in b.reloc:
        if len(r["new"]) != 4:
            continue
        v = int.from_bytes(r["new"], "little")
        if not (lo <= v < hi):
            continue
        a = r["va"] - s["va"]
        if 0 <= a and a + 4 <= len(exp):
            for k in range(a, a + 4):
                mask[k] = True
    return " ".join("xx" if m else "%02x" % x for x, m in zip(exp, mask))


def render_doc(b, which, exe_part):
    reloc = which == "reloc"
    sites = b.reloc if reloc else b.limits
    if reloc:
        secs = [
            {
                "name": SEC_U[0],
                "vaddr": "0x%x" % SEC_U[1],
                "vsize": "0x%x" % SEC_U[2],
                "chars": "0x%X" % SEC_U[3],
            },
            {
                "name": SEC_P[0],
                "vaddr": "0x%x" % SEC_P[1],
                "vsize": "0x%x" % SEC_P[2],
                "chars": "0x%X" % SEC_P[3],
            },
            {
                "name": SEC_Q[0],
                "vaddr": "0x%x" % SEC_Q[1],
                "vsize": "0x%x" % SEC_Q[2],
                "data": hx(b.cave_data_reloc),
                "chars": "0x%X" % SEC_Q[3],
            },
        ]
    else:
        secs = [
            {
                "name": SEC_V[0],
                "vaddr": "0x%x" % SEC_V[1],
                "vsize": "0x%x" % SEC_V[2],
                "data": hx(b.cave_data),
                "chars": "0x%X" % SEC_V[3],
            }
        ]
    kinds = {}
    for s in sites:
        kinds[s["kind"]] = kinds.get(s["kind"], 0) + 1
    proofs = {
        "sites": len(sites),
        "sites_sha256": _sites_sha(sites),
        "by_kind": dict(sorted(kinds.items())),
        "exe": exe_part,
    }
    if reloc:
        proofs["arrays"] = {
            n: {
                "off": "0x%06x" % a["off"],
                "bytes": a["rsize"],
                "row_bytes": a["rrow"],
                "stock_base": "0x%08x" % a["base"],
            }
            for n, a in b.arrs.items()
        }
        proofs["save_layer_vanilla"] = ["0x%08x" % v for v in b.save_layer]
        proofs["u4_path_slot_id_sites"] = ["0x%08x" % s["va"] for s in b.reloc if s["u4"]]
        proofs["path"] = {
            "host_section": SEC_P[0],
            "buffers_off": "0x%06x" % gp.BUF_OFF,
            "buffers_bytes": gp.BUF_TOTAL,
            "buffer_player_stride": gp.BUF_STRIDE_NEW,
            "flags_off": "0x%06x" % gp.FLAGS_OFF,
            "flags_bytes": gp.FLAGS_TOTAL,
            "row_slots": gp.ROW,
            "raised_slots": gp.RAISED,
            "stock_buffers": "0x%08x" % gp.BUF_STOCK,
            "stock_flags": "0x%08x" % gp.FLAGS_STOCK,
            "cave_section": SEC_Q[0],
            "sites": b.n_path_reloc,
            "caves": len(b.caves_reloc),
        }
    else:
        proofs["caves"] = [
            {"name": c["name"], "va": "0x%08x" % c["va"], "len": len(c["code"])} for c in b.caves
        ]
        proofs["raise"] = {"0x%08x" % s["va"]: s["note"] for s in b.limits}
        proofs["path"] = {
            "sites": b.n_path_limits,
            "id_hole": "unit +0xda (16-bit); byte +0xaa kept as min(id,0xfe) / 0xff = no path",
            "no_path_value": gp.RAISED,
        }
    doc = {
        "class": "caps_units" if reloc else "caps_units_limits",
        "_class_why": (
            "v0.2.3 cap raise, units + soldiers (+ the path-slot pool) RELOCATION layer: the arrays are ALWAYS relocated to "
            "[8][500] / [8][501] (applied at boot; the setting never moves an array). Compiled into mh.dll, not in COMPILED[]."
            if reloc
            else "v0.2.3 cap raise, units + soldiers LIMIT layer: `expect` is the vanilla (relocated, for a U4 rewrite) "
            "bytes (OFF), `bytes` the raised bytes (ON). The applier verifies and stages them and writes only on a "
            "toggle at a quiescent point."
        ),
    }
    if not reloc:
        doc["toggle"] = True
    doc.update(
        {
            "target": "mh.exe (English, clean retail)",
            "sha256": json.load(
                open(os.path.join(PATCHER, JULY + ".mh.patch.json"), encoding="utf-8")
            )["sha256"],
            "_purpose": (
                "GENERATED by tools/gen_cap_units_manifests.py -- do not hand-edit. "
                + (
                    "3 sections (units+soldiers host, path-slot pool host, always-on caves); %d sites: base/stride/clear "
                    "fields of units (0xdd8c48, 233 B) and soldiers (0xe0e5a8, 29 B) re-baked into .mhcapu at [8][500], "
                    "and (PT-CAP9-U4b) the path-slot pool (buffers/flags/data pointers, 17 caves) re-baked into .mhcapp at "
                    "[8][501]. The planet save/load block sites stay vanilla (canonical mirror)."
                    % len(sites)
                    if reloc
                    else "%d sites: the per-player unit / soldier bounds (100 -> 500) and the recruit caps (91 -> 491); "
                    "(PT-CAP9-U4b) the path-slot id reads/writes (16-bit id at unit +0xda), the 6 path scanner bounds and "
                    "the free-count start (100 -> 500)." % len(sites)
                )
            ),
            "_proofs": proofs,
            "sections": secs,
            "patches": [
                {
                    "va": "0x%08x" % s["va"],
                    "expect": hx(s["exp"]) if reloc else _masked_expect(b, s),
                    "bytes": hx(s["new"]),
                    "_note": s["note"],
                }
                for s in sites
            ],
        }
    )
    return json.dumps(doc, indent=1) + "\n"


def render_path_header(b):
    """src/mh_dll/mh/caps/path_pool_layout.gen.h: the path-slot pool's layout and the functions whose promotion (libmh) makes the
    raise unavailable. One source for mh.dll: capstest asserts the manifest's own `_proofs.path` against it."""
    rows = gp.rows_of(b.path_census)
    funcs = {n: (e, end) for e, end, n in scan.load_funcs()}
    names = sorted(
        {r["fn"] for r in rows if r["fn"] and r["fn"] not in gp.sp.SAVE_LOAD},
        key=lambda n: funcs[n][0],
    )
    L = [
        "// GENERATED by tools/gen_cap_units_manifests.py from tools/data/cap_path_slot_census.json -- do not hand-edit.",
        "//",
        "// The path-slot pool of src/patcher/cap_v023_units_reloc_EN.mh.patch.json (patching:PT-CAP9-U4): where the relocated pool lives",
        "// inside its `.mhcapp` section, its shape, and the game functions that read or write it (a promoted body among them makes the",
        "// raise unavailable: the libmh sim pins the pool at 100 slots).",
        "#pragma once",
        "#include <cstdint>",
        "",
        "namespace mh::caps::path_layout {",
        "",
        "inline constexpr uint32_t PLAYERS         = %du;" % gp.PLAYERS,
        "inline constexpr uint32_t STOCK_SLOTS     = %du;   // vanilla slots per player"
        % gp.STOCK_SLOTS,
        "inline constexpr uint32_t ROW_SLOTS       = %du;  // slots per player row now (500 usable + the sink)"
        % gp.ROW,
        "inline constexpr uint32_t RAISED_SLOTS    = %du;  // usable slots when raised; also the 16-bit `no path` value (the sink slot)"
        % gp.RAISED,
        "inline constexpr uint32_t SLOT_BYTES      = %du;  // 300 waypoints x {heading, run}"
        % gp.SLOT_BYTES,
        "inline constexpr uint32_t BUF_PLAYER_BYTES = %du;  // ROW_SLOTS * SLOT_BYTES (the buffer player stride)"
        % gp.BUF_STRIDE_NEW,
        "inline constexpr uint32_t BUF_BYTES       = %du;  // PLAYERS * BUF_PLAYER_BYTES"
        % gp.BUF_TOTAL,
        "inline constexpr uint32_t BUF_OFFSET      = 0x%06xu;  // inside `.mhcapp` (a 64-byte lead-in: the biased base-2 displacements stay inside the section)"
        % gp.BUF_OFF,
        "inline constexpr uint32_t FLAGS_OFFSET    = 0x%06xu;  // inside `.mhcapp`" % gp.FLAGS_OFF,
        "inline constexpr uint32_t FLAGS_BYTES     = %du;  // PLAYERS * ROW_SLOTS" % gp.FLAGS_TOTAL,
        "inline constexpr uint32_t HOST_VSIZE      = 0x%06xu;" % gp.HOST_VSIZE,
        "inline constexpr uint32_t STOCK_BUFFERS   = 0x%08xu;  // _G_LLM_STRAT_PATH_BUFFERS, the canonical mirror (slots 0..99)"
        % gp.BUF_STOCK,
        "inline constexpr uint32_t STOCK_FLAGS     = 0x%08xu;  // _G_LLM_STRAT_PATH_SLOT_FLAGS"
        % gp.FLAGS_STOCK,
        "inline constexpr uint32_t STOCK_COUNT     = 0x00b63bc0u;  // _G_LLM_STRAT_PATH_FREE_SLOT_COUNT, int[8] (stays at its stock address)",
        "inline constexpr uint32_t ID_BYTE_OFF     = 0xaau;  // unit.path_slot_id (a byte: min(id, 0xfe), 0xff = no path)",
        "inline constexpr uint32_t ID_WORD_OFF     = 0xdau;  // the 16-bit id the raise adds (500 = no path)",
        "",
        "// Entry VAs of the game functions that read or write the pool or the unit's path slot id.",
        "inline constexpr uint32_t POOL_FUNCTIONS[] = {",
    ]
    for n in names:
        L.append("    0x%08xu, // %s" % (funcs[n][0], n))
    L += [
        "};",
        "inline constexpr unsigned POOL_FUNCTION_COUNT = %du;" % len(names),
        "",
        "} // namespace mh::caps::path_layout",
        "",
    ]
    return "\n".join(L)


# ---- proofs -------------------------------------------------------------------------------------------------------


def _v022_extents():
    out = []
    for n in ("cap_v022_reloc_EN", "cap_v022_limits_EN", "cap_v022_aiscratch_EN"):
        d = json.load(open(os.path.join(PATCHER, n + ".mh.patch.json"), encoding="utf-8"))
        for p in d["patches"]:
            va = int(p["va"], 16)
            out.append((va, va + len(unhx(p["bytes"])), n))
    return sorted(out)


def prove_hermetic(b):
    f = []
    arrs = b.arrs
    if (
        len(b.limits)
        != sum(1 for _ in raise_rows(b.doc))
        + sum(1 for s in b.reloc if s["raised"] is not None)
        + b.n_path_limits
    ):
        f.append("limit site count differs from the census raise rows")
    # P6 sorted
    for nm, ss in (("reloc", b.reloc), ("limits", b.limits)):
        for x, y in zip(ss, ss[1:]):
            if y["va"] <= x["va"]:
                f.append("P6: %s sites not strictly sorted at %#x" % (nm, y["va"]))
                break
    # P2 overlap inside a layer
    for nm, ss in (("reloc", b.reloc), ("limits", b.limits)):
        ext = sorted((s["va"], s["va"] + len(s["exp"])) for s in ss)
        for x, y in zip(ext, ext[1:]):
            if y[0] < x[1]:
                f.append("P2: %s sites overlap at %#x" % (nm, y[0]))
    # P2 vs the v0.2.2 manifests
    v22 = _v022_extents()
    for nm, ss in (("reloc", b.reloc), ("limits", b.limits)):
        for s in ss:
            lo, hi = s["va"], s["va"] + len(s["exp"])
            for a, z, who in v22:
                if a < hi and lo < z:
                    f.append("P2: %s site %#x overlaps %s at %#x" % (nm, lo, who, a))
                    break
    # P2b a cross-layer overlap is allowed only as the composed rewrite: expect == the relocated bytes
    rel = {s["va"]: s for s in b.reloc}
    for s in b.limits:
        lo, hi = s["va"], s["va"] + len(s["exp"])
        hit = [r for r in b.reloc if r["va"] < hi and lo < r["va"] + len(r["exp"])]
        if not hit:
            continue
        if s["kind"] in ("praise", "pcave"):
            # PT-CAP9-U4b: a path-slot rewrite spans a whole instruction / window; its `expect` must be the VANILLA bytes with every
            # overlapping relocation site applied (the relocation layer goes first).
            try:
                want = gp.overlay(s["vanilla"], s["va"], hit)
            except gp.Refuse as e:
                f.append("P2b: %s" % e)
                continue
            if want != s["exp"]:
                f.append(
                    "P2b: path rewrite at %#x expects %s, vanilla+relocation is %s"
                    % (lo, hx(s["exp"]), hx(want))
                )
            continue
        if s["kind"] != "raise":
            f.append("P2b: limit site %#x (%s) overlaps a relocation site" % (lo, s["kind"]))
            continue
        if s["va"] not in rel or s["exp"] != rel[s["va"]]["new"]:
            f.append("P2b: raise rewrite at %#x does not expect the relocated bytes" % lo)
    # relocation: fields inside the arrays, inside a record, one site per VA
    for s in b.reloc:
        if s["array"] == "path":
            continue  # the path-slot pool is proved by gen_cap_path_sites.prove below
        a = arrs[s["array"]]
        if s["kind"] == "base":
            t = u32(s["new"])
            lo = a["new_base"]
            hi = lo + a["rsize"]
            if not lo <= t < hi:
                f.append(
                    "extent: %#x bakes %#x outside %s [%#x,%#x)" % (s["va"], t, s["array"], lo, hi)
                )
            for n, o in arrs.items():
                if n != s["array"] and o["new_base"] <= t < o["new_base"] + o["rsize"]:
                    f.append("extent: %#x bakes %#x inside the WRONG array %s" % (s["va"], t, n))
            if s["q"] >= a["vrow"]:
                f.append("extent: %#x field offset %d beyond the stock row" % (s["va"], s["q"]))
            if s["p"] >= PLAYERS:
                f.append("extent: %#x player row %d" % (s["va"], s["p"]))
        elif s["kind"] == "stride":
            v = u32(s["exp"])
            k, rem = divmod(v, a["vrow"])
            if rem or u32(s["new"]) != k * a["rrow"]:
                f.append("stride: %#x %d -> %d is not k x row" % (s["va"], v, u32(s["new"])))
        elif s["kind"] == "clear":
            if u32(s["exp"]) != a["vsize"] or u32(s["new"]) != a["rsize"]:
                f.append("clear: %#x is not the whole-array size" % s["va"])
    # census coverage: every move site, once; no save-layer site
    cov = scan.Coverage(
        sorted((s["va"], s["va"] + len(s["exp"]), RELOC_NAME, "field") for s in b.reloc)
    )
    for d in cen.site_dicts(b.doc["sites"]):
        st = cov.test(d["va"], d["w"])
        if d["class"] == "move" and st != "covered":
            f.append("census move site %#x (%s %s) is %s" % (d["va"], d["array"], d["role"], st))
        if d["reason"] == "save_layer" and st != "none":
            f.append("save-layer site %#x is patched" % d["va"])
    # the U4 hook: the tagged sites are exactly the direct path_slot_id references
    n_u4 = sum(1 for s in b.reloc if s["u4"])
    if n_u4 != U4_DIRECT_SITES:
        f.append(
            "%d path_slot_id relocation sites tagged, the U4 design counts %d"
            % (n_u4, U4_DIRECT_SITES)
        )
    # PT-CAP12: no stride-only function (the v0.2.2 false-collision class, here over units/soldiers)
    kinds = {"row_stride": "stride", "row_multiple": "stride", "block_size": "clear"}
    rows = [
        (d["va"], d["array"], kinds.get(d["role"], "base"), d["fn"])
        for d in cen.site_dicts(b.doc["sites"])
        if d["class"] == "move" and d["role"] != "shift_chain"
    ]
    for a_, fn_, vas in gcv.stride_only_violations(rows):
        f.append(
            "stride-only: %s in %s scales by the row stride at %s but has no base/clear site of the array"
            % (a_, fn_, ", ".join("%#x" % v for v in vas))
        )
    # PT-CAP9-U4b: the path-slot pool sites (census coverage, extents, cave counts)
    f += gp.prove(
        b.path_census,
        [x for x in b.reloc if x["array"] == "path"],
        [x for x in b.limits if x["array"] == "path"],
        sum(1 for c in b.caves_reloc if c["layer"] == "reloc"),
        sum(1 for c in b.caves_reloc if c["layer"] == "limits"),
        b.path_layout,
    )
    # July cross-check
    f += july_check(b)
    # P5 + refs_complete
    f += gcv.prove_p5(b)
    f += prove_refs(b)
    return f


def july_check(b):
    """The same arithmetic under the July layout must land every site on one constant delta per array, and the
    July widened-bound caves must carry the immediates this layer writes."""
    f = []
    jd = json.load(open(os.path.join(PATCHER, JULY + ".mh.patch.json"), encoding="utf-8"))
    jp = {int(p["va"], 16): p for p in jd["patches"]}
    jsec = [(int(s["vaddr"], 16), unhx(s["data"])) for s in jd["sections"] if s.get("data")]
    deltas = {}
    seen = 0
    for s in b.reloc:
        if s["array"] == "path":
            continue
        p = jp.get(s["va"])
        if p is None:
            continue
        jb = unhx(p["bytes"])
        if len(jb) != 4:
            continue
        seen += 1
        new = u32(s["new"])
        jv = u32(jb)
        if s["kind"] == "base":
            deltas.setdefault(s["array"], set()).add(new - jv)
        elif new != jv:
            f.append("July cross-check: %#x %s %d vs July %d" % (s["va"], s["kind"], new, jv))
    for n, ds in deltas.items():
        if len(ds) != 1:
            f.append(
                "July cross-check: %s bases are not one constant delta from the July layout (%d deltas)"
                % (n, len(ds))
            )
    if seen < 3000:
        f.append("July cross-check compared only %d sites" % seen)
    # widened bounds: July's cave for the same site must compare against the same immediate
    for s in b.limits:
        if s["kind"] != "cave":
            continue
        p = jp.get(s["va"])
        if p is None:
            continue
        jb = unhx(p["bytes"])
        if jb[0] != 0xE9:
            continue
        cave = s["va"] + 5 + struct.unpack_from("<i", jb, 1)[0]
        blob = None
        for sv, d in jsec:
            if sv <= cave < sv + len(d):
                blob = d[cave - sv :]
        if blob is None:
            continue
        i = next(gcv._md().disasm(blob, cave, 1))
        imm = i.operands[-1].imm & 0xFFFFFFFF if i.operands else None
        row = next((r for r in b.limits if r["va"] == s["va"]), None)
        want = int(re.search(r"\((\d+)->(\d+)\)", row["note"]).group(2))
        if imm != want:
            f.append(
                "July cross-check: July's cave for %#x compares against %s, this layer writes %d"
                % (s["va"], imm, want)
            )
    return f


def prove_refs(b):
    f = []
    root = tempfile.mkdtemp(prefix="capunits_")
    for which in ("reloc", "limits"):
        path = os.path.join(
            root, (RELOC_NAME if which == "reloc" else LIMITS_NAME) + ".mh.patch.json"
        )
        with open(path, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(render_doc(b, which, {}))
        try:
            c = gim._classify(path)
        except gim.Refused as e:
            f.append("refs: %s REFUSED: %s" % (which, e))
            continue
        if not c["complete"]:
            f.append("refs: %s is not refs_complete: %s" % (which, c["why"]))
    return f


def p3_checks(b):
    out = []
    for c in list(b.caves_reloc) + list(b.caves):
        regs = gcv.clobbered(c["code"], c["va"]) - set(c["intended"])
        if c["flags"] != "equal":
            regs = regs | {"eflags"}
        if not regs:
            continue
        for k, ex in enumerate(c["exits"]):
            out.append(("%s/exit%d" % (c["name"], k), ex, regs))
    return out


def prove_exe(b, img):
    f = []
    for nm, ss in (("reloc", b.reloc), ("limits", b.limits)):
        for s in ss:
            vanilla = s.get("vanilla", s["exp"]) if s["kind"] != "raise" else None
            if vanilla is None:
                continue
            if img.read(s["va"], len(vanilla)) != vanilla:
                f.append(
                    "P1: %s site %#x expect %s != exe %s"
                    % (nm, s["va"], hx(vanilla), hx(img.read(s["va"], len(vanilla))))
                )
    res = []
    for cid, ex, regs in p3_checks(b):
        ok, why = gcv.live(img, ex, regs)
        res.append(
            dict(
                id=cid,
                exit="0x%08x" % ex,
                need=sorted(regs),
                verdict="dead" if ok else "REFUSED",
                why=why,
            )
        )
        if not ok:
            f.append("P3: %s exit %#x: %s" % (cid, ex, why))
    return f, res


def exe_parts(b, p3, md5):
    rel_names = {c["name"] for c in b.caves_reloc}
    rel = [r for r in p3 if r["id"].split("/")[0] in rel_names]
    lim = [r for r in p3 if r["id"].split("/")[0] not in rel_names]
    return (
        {"exe_md5": md5, "p1_sites": len(b.reloc), "p3": rel},
        {"exe_md5": md5, "p1_sites": len(b.limits), "p3": lim},
    )


def derive_with_proofs(doc, img, md5, planted=None):
    b = build(doc, {}, planted)
    f, p3 = prove_exe(b, img)
    b.p3 = p3
    b.exe_rel, b.exe_lim = exe_parts(b, p3, md5)
    return b, f


def render_both(b):
    return render_doc(b, "reloc", b.exe_rel), render_doc(b, "limits", b.exe_lim)


# ---- modes -------------------------------------------------------------------------------------------------------------


def cmd_write(exe):
    doc = load_census()
    img, md5, _raw = gcv.load_exe(exe)
    b, f = derive_with_proofs(doc, img, md5)
    f += prove_hermetic(b)
    if f:
        for x in f[:30]:
            print("  FAIL", x)
        print("gen_cap_units_manifests --write: REFUSED (%d)" % len(f))
        return 1
    r, l = render_both(b)
    for path, text in ((RELOC_PATH, r), (LIMITS_PATH, l), (PATH_LAYOUT_H, render_path_header(b))):
        with open(path, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(text)
        print("wrote %s (%d bytes)" % (os.path.relpath(path, REPO), len(text)))
    print(
        "reloc %d sites, limits %d sites, P3 verdicts %d" % (len(b.reloc), len(b.limits), len(b.p3))
    )
    return 0


def committed_proofs():
    out = []
    for path in (RELOC_PATH, LIMITS_PATH):
        if not os.path.isfile(path):
            raise Refuse("MISSING %s -- run --write" % os.path.relpath(path, REPO))
        out.append(json.load(open(path, encoding="utf-8"))["_proofs"]["exe"])
    return out


def cmd_check():
    doc = load_census()
    try:
        exe_rel, exe_lim = committed_proofs()
    except Refuse as e:
        print("gen_cap_units_manifests --check:", e)
        return 1
    b = build(doc, exe_rel)
    b.exe_rel, b.exe_lim = exe_rel, exe_lim
    f = []
    need = {c[0]: c for c in p3_checks(b)}
    have = {r["id"]: r for r in list(exe_rel.get("p3", [])) + list(exe_lim.get("p3", []))}
    for k in sorted(set(need) - set(have)):
        f.append("exe proof missing for %s -- run --write with the exe" % k)
    for k in sorted(set(have) - set(need)):
        f.append("exe proof for %s is stale (no such question any more)" % k)
    for k, r in have.items():
        if r["verdict"] != "dead":
            f.append("exe proof %s is %s" % (k, r["verdict"]))
        elif k in need and (r["exit"] != "0x%08x" % need[k][1] or r["need"] != sorted(need[k][2])):
            f.append("exe proof %s was run for a different exit/register set" % k)
    for e in (exe_rel, exe_lim):
        if e.get("exe_md5") != scan.EN_MD5:
            f.append("exe proofs were not run against the clean EN exe")
    if exe_rel.get("p1_sites") != len(b.reloc) or exe_lim.get("p1_sites") != len(b.limits):
        f.append(
            "the cached P1 site counts differ from the current sites -- run --write with the exe"
        )
    r, l = render_both(b)
    for path, text in ((RELOC_PATH, r), (LIMITS_PATH, l), (PATH_LAYOUT_H, render_path_header(b))):
        if not os.path.isfile(path):
            f.append("MISSING %s (run --write)" % os.path.relpath(path, REPO))
            continue
        cur = open(path, encoding="utf-8").read()
        if cur != text:
            f.append(
                "STALE: %s differs from the re-derivation (run --write)"
                % os.path.relpath(path, REPO)
            )
    f += prove_hermetic(b)
    if f:
        for x in f[:30]:
            print("  FAIL", x)
        print("gen_cap_units_manifests --check: FAIL (%d)" % len(f))
        return 1
    print(
        "gen_cap_units_manifests --check: ok (reloc %d sites, limits %d sites, %d cached exe proofs)"
        % (len(b.reloc), len(b.limits), len(have))
    )
    return 0


def cmd_report():
    doc = load_census()
    exe_rel, _exe_lim = committed_proofs()
    b = build(doc, exe_rel)
    for n, a in b.arrs.items():
        print(
            "  %-9s off %#08x  row %7d  size %8d  base %#x -> %#x"
            % (n, a["off"], a["rrow"], a["rsize"], a["base"], a["new_base"])
        )
    k = {}
    for s in b.reloc:
        k[(s["array"], s["kind"])] = k.get((s["array"], s["kind"]), 0) + 1
    print("reloc %d %s" % (len(b.reloc), dict(sorted(k.items()))))
    print(
        "save-layer (vanilla) sites: %d; u4-tagged path_slot_id sites: %d"
        % (len(b.save_layer), sum(1 for s in b.reloc if s["u4"]))
    )
    print("limits %d; caves %d bytes" % (len(b.limits), len(b.cave_data)))
    for s in b.limits:
        print("   %#x %s" % (s["va"], s["note"]))
    return 0


# ---- selftest -------------------------------------------------------------------------------------------------------------


def patched_image(raw, img, va, data):
    return gcv.patched_image(raw, img, va, data)


def selftest(exe):
    doc = load_census()
    img, md5, raw = gcv.load_exe(exe)
    fails = []

    def expect(what, ok, detail=None):
        print("  [%s] %s" % ("ok" if ok else "FAIL", what))
        if not ok:
            fails.append(what)
            if detail is not None:
                print("       got: %r" % (detail,))

    b, f = derive_with_proofs(doc, img, md5)
    f += prove_hermetic(b)
    expect("the unmutated derivation is green against the exe", not f, f[:3])
    expect(
        "3800+ reloc sites, 27 + the path-slot limit sites",
        len(b.reloc) > 3800 and len(b.limits) == 27 + b.n_path_limits and b.n_path_limits == 65,
        (len(b.reloc), len(b.limits), b.n_path_limits),
    )
    expect(
        "58 path_slot_id sites are tagged for U4",
        sum(1 for s in b.reloc if s["u4"]) == U4_DIRECT_SITES,
    )

    def mut():
        return copy.deepcopy(b)

    # 1. a site whose expect != exe
    m = mut()
    v1 = next(x for x in m.reloc if x["array"] != "path")
    v1["exp"] = bytes([v1["exp"][0] ^ 0xFF]) + v1["exp"][1:]
    ff, _ = prove_exe(m, img)
    expect(
        "mutation 1: a site whose expect != exe goes RED (P1)",
        any(x.startswith("P1") for x in ff),
        ff[:2],
    )
    # 2. a re-baked target outside its array extent
    m = mut()
    s = next(x for x in m.reloc if x["kind"] == "base" and x["array"] == "soldiers")
    s["new"] = p32(SEC_U[1] + SEC_U[2] - 4)
    ff = prove_hermetic(m)
    expect(
        "mutation 2: a baked target outside its array extent goes RED",
        any(x.startswith("extent") for x in ff),
        ff[:2],
    )
    # 3. a reloc site overlapping a v0.2.2 site
    m = mut()
    v22 = _v022_extents()[100]
    m.reloc.append(site(v22[0], b"\0" * 4, b"\0" * 4, "base", "units", "planted", p=0, q=0))
    m.reloc.sort(key=lambda x: x["va"])
    ff = prove_hermetic(m)
    expect(
        "mutation 3: a site over a cap_v022 site goes RED (P2)",
        any("cap_v022" in x for x in ff),
        ff[:2],
    )
    # 4. a cave exit that reads flags
    m = mut()
    cv = next(c for c in m.caves if len(c["exits"]) == 2)
    bad = patched_image(raw, img, cv["exits"][1], b"\x0f\x84\x00\x00\x00\x00")
    ff, _ = prove_exe(m, bad)
    expect(
        "mutation 4: a cave exit that reads flags goes RED (P3)",
        any(x.startswith("P3") for x in ff),
        ff[:2],
    )
    # 5. a limit site in a claimed body's entry window
    m = mut()
    ent, _nm = gcv.claimed_entry()
    m.limits.append(site(ent + 2, b"\x90", b"\x90", "limit", "units", "planted"))
    m.limits.sort(key=lambda x: x["va"])
    ff = gcv.prove_p5(m)
    expect(
        "mutation 5: a limit site in a claimed body's entry window goes RED (P5)",
        any(x.startswith("P5") for x in ff),
        ff[:2],
    )
    # 6. unsorted
    m = mut()
    m.reloc[3], m.reloc[4] = m.reloc[4], m.reloc[3]
    ff = prove_hermetic(m)
    expect(
        "mutation 6: an unsorted site list goes RED (P6)",
        any(x.startswith("P6") for x in ff),
        ff[:2],
    )
    # 7. the July cross-check
    m = mut()
    s = next(x for x in m.reloc if x["kind"] == "stride")
    s["new"] = p32(u32(s["new"]) + 4)
    ff = july_check(m)
    expect("mutation 7: a perturbed stride goes RED (July cross-check)", bool(ff), ff[:2])
    m = mut()
    for s in m.reloc:
        if s["kind"] == "base" and s["array"] == "units" and s["va"] % 7 == 0:
            s["new"] = p32(u32(s["new"]) + 64)
    ff = july_check(m)
    expect(
        "mutation 7b: a unit base off the July delta goes RED",
        any("constant delta" in x for x in ff),
        ff[:2],
    )
    # 8. a census move site dropped from the manifest
    m = mut()
    m.reloc = [
        x for x in m.reloc if x["va"] != next(y for y in m.reloc if y["kind"] == "base")["va"]
    ]
    ff = prove_hermetic(m)
    expect(
        "mutation 8: a census move site missing from the manifest goes RED",
        any("census move site" in x for x in ff),
        ff[:2],
    )
    # 9. a save-layer site patched
    m = mut()
    sv = b.save_layer[0]
    m.reloc.append(site(sv, b"\0" * 4, b"\0" * 4, "base", "units", "planted", p=0, q=0))
    m.reloc.sort(key=lambda x: x["va"])
    ff = prove_hermetic(m)
    expect(
        "mutation 9: a patched save-layer site goes RED", any("save-layer" in x for x in ff), ff[:2]
    )
    # 10. a planted U4 raise-switched rewrite lands in the limit layer on top of its relocated bytes (the design hook)
    u4 = next(s for s in b.reloc if s["u4"] and len(s["exp"]) == 4)
    m = build(doc, {}, {u4["va"]: p32(u32(u4["new"]) + 0x20)})
    got = [s for s in m.limits if s["kind"] == "raise"]
    ff = prove_hermetic(m)
    expect(
        "mutation 10: a planted U4 rewrite is carried by the limit layer over the RELOCATED bytes and passes P2b",
        len(got) == 1 and got[0]["exp"] == u4["new"] and not [x for x in ff if "P2b" in x],
        (ff[:2], len(got)),
    )
    m = build(doc, {}, {u4["va"]: p32(u32(u4["new"]) + 0x20)})
    for s in m.limits:
        if s["kind"] == "raise":
            s["exp"] = u4[
                "exp"
            ]  # expects the VANILLA bytes: the reloc layer has already changed them
    ff = prove_hermetic(m)
    expect(
        "mutation 10b: a raise rewrite that expects the vanilla bytes goes RED (P2b)",
        any("P2b" in x for x in ff),
        ff[:2],
    )
    # 11. PT-CAP9-U4b: the path-slot pool
    m = mut()
    pv = next(x for x in m.reloc if x["array"] == "path" and x["kind"] == "pbase")
    pv["exp"] = bytes([pv["exp"][0] ^ 0xFF]) + pv["exp"][1:]
    pv["vanilla"] = pv["exp"]
    ff, _ = prove_exe(m, img)
    expect(
        "mutation 11: a path-slot site whose expect != exe goes RED (P1)",
        any(x.startswith("P1") for x in ff),
        ff[:2],
    )
    m = mut()
    m.reloc.remove(next(x for x in m.reloc if x["array"] == "path" and x["kind"] == "pstride"))
    ff = prove_hermetic(m)
    expect(
        "mutation 12: a dropped path-slot stride site (an always_on census row unpatched) goes RED",
        any("always_on row" in x for x in ff),
        ff[:2],
    )
    m = mut()
    pc = next(x for x in m.limits if x["kind"] == "pcave" and x["note"].startswith("idread@"))
    pc["exp"] = pc[
        "vanilla"
    ]  # expects the VANILLA displacement although the relocation layer has moved it
    ff = prove_hermetic(m)
    expect(
        "mutation 13: a path rewrite that expects the vanilla bytes goes RED (P2b)",
        any("P2b" in x for x in ff),
        ff[:2],
    )
    m = mut()
    pb = next(
        x
        for x in m.reloc
        if x["array"] == "path" and x["kind"] == "pbase" and "buffer" in x["note"]
    )
    pb["new"] = p32(SEC_U[1])
    ff = prove_hermetic(m)
    expect(
        "mutation 14: a path buffer base baked outside the pool goes RED",
        any("outside the new buffer" in x for x in ff),
        ff[:2],
    )
    m = mut()
    m.limits = [x for x in m.limits if not (x["kind"] == "plimit")]
    m.n_path_limits -= 1
    ff = prove_hermetic(m)
    expect(
        "mutation 15: a dropped raise-switched path site (the free-count start) goes RED",
        any("raise_switched row" in x for x in ff),
        ff[:2],
    )
    print("gen_cap_units_manifests --selftest: %s" % ("FAIL" if fails else "PASS"))
    return 1 if fails else 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--write", action="store_true")
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--report", action="store_true")
    ap.add_argument("--exe")
    a = ap.parse_args(argv)
    try:
        if a.write:
            return cmd_write(a.exe)
        if a.check:
            return cmd_check()
        if a.selftest:
            return selftest(a.exe)
        if a.report:
            return cmd_report()
    except (Refuse, gcv.Refuse) as e:
        print("gen_cap_units_manifests: REFUSED -- %s" % e)
        return 1
    ap.print_help()
    return 2


if __name__ == "__main__":
    sys.exit(main())
