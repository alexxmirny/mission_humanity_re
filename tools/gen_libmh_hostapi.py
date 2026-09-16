#!/usr/bin/env python3
"""gen_libmh_hostapi.py -- generate the TWO flat C host-callback tables (LIB-ABI stage A;
split sim/tact at LIB-IFACE-SPLIT, 2026-09-10).

The residual outward calls a standalone libmh may legitimately make -- the
`host-callback:*` classes of tools/data/libmh_call_ledger.json -- become versioned C structs
of function pointers the host fills at init (the endgame plan D-E3). Signatures come
from tools/data/dll_call_protos.json (the same source the mh::call:: wrappers are generated
from), so the mh.dll thin impl can forward each entry to its thunk without adaptation.
C++-typed struct-pointer parameters degrade to `void *` -- the surface is C89-includable on
purpose (Godot GDExtension, the LIB-REF headless host, and mh.dll all bind it without C++
ABI coupling).

RESHAPED ENTRIES (a row's `abi` block, SIMABI-VFS 2026-09-10). Proto-derived means the table
mirrors the original thunk, dead parameters and all -- an interface nobody designed. A row may
instead carry `abi` = {name, ret, params, bind?}: the entry is emitted under a DESIGNED name and
signature, and where `bind` is set the mh.dll slot is filled by a HAND-WRITTEN binder that absorbs
the original's quirks rather than by the generated one-line forward. The ledger key stays the
original callee, so the census / adjudication / caller evidence keep resolving; `tact_frozen` may
set `"abi": null` to hold the frozen tact table on the original shape.

TWO TABLES, MEMBERSHIP DERIVED, DUPLICATION ALLOWED (LIB-IFACE-SPLIT). The sim table
(`libmh_host_api`) holds every entry referenced from a non-tact libmh module; the tact table
(`libmh_tact_host_api`) holds every entry referenced from tact/. An entry both call is
emitted into BOTH -- nothing is assigned exclusively, so a translation-frontier accident
cannot be frozen into the ABI (the defect that killed the earlier exclusive
direct_by_domain split; docs/libmh-abi.md sec 3). Membership is a SOURCE SCAN, not a hand
list: module code reaches the sim table through mh::host() and the tact table through
mh::tact_host(), and this generator FAILS on an accessor/module disagreement, an entry with
no sites, or a scanned name with no ledger row -- so adding a site is the only way to grow a
table, and removing the last site shrinks it on the next regen (the --check gate goes red
until then). The point of the split: the sim table is the follow-on abstraction target;
tact's stays frozen.

Outputs (one --check covers all four):
  src/mh_dll/libmh/include/libmh_host_api.gen.h        -- the SIM table
  src/mh_dll/libmh/include/libmh_tact_host_api.gen.h   -- the TACT table
  * each: `struct <name>` grouped by category (ledger reason as the comment), its own
    LIBMH[_TACT]_HOST_API_VERSION (FNV-1a over the ABI-relevant text; reason-comment edits
    do NOT bump it, any signature/membership change does; the set call refuses a mismatch),
    and its own FOR_EACH X-macro -- X(name, category, notify) -- so consumers derive the
    name table / offsetof table / notify flags without a second generated file. notify=1
    means a headless host may bind a no-op; notify=0 entries return data the sim consumes --
    a headless host must implement them or bind a named trap, never a silent no-op.
  src/mh_dll/mh/addr/mh_hostapi_bind.gen.cpp -- the mh.dll THIN IMPL (stage B): one
    forwarder per entry onto its mh::call:: thunk (void* params cast back to the real
    struct-pointer types); BOTH tables filled from the same forwarders. Harness-side TU:
    in mh.vcxproj, never in libmh.vcxproj.
  src/mh_dll/mh_nettest/mh_hostapi_selftest.gen.cpp -- the SELFTEST HOST (stage D's
    no-op-host arm), both tables: notify entries return-default no-ops, required entries
    call mh_hostapi_trap(name) first -- a silent no-op of a required entry is impossible by
    construction; the trap names the entry.

  --check   regenerate all four in memory and fail (exit 1) on any difference from the
            committed files -- the drift gate (wired into lint_repo). Adding/reclassing a
            ledger row, changing a committed prototype, or adding/removing an accessor site
            turns it red until regenerated.
"""

import argparse
import io
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _dllsrc  # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LEDGER_PATH = os.path.join(REPO, "tools", "data", "libmh_call_ledger.json")
PROTOS_PATH = os.path.join(REPO, "tools", "data", "dll_call_protos.json")
MH_SRC = os.path.join(REPO, "src", "mh_dll", "mh")
OUT_PATH = os.path.join(REPO, "src", "mh_dll", "libmh", "include", "libmh_host_api.gen.h")
TACT_OUT_PATH = os.path.join(REPO, "src", "mh_dll", "libmh", "include", "libmh_tact_host_api.gen.h")
BIND_PATH = os.path.join(REPO, "src", "mh_dll", "mh", "addr", "mh_hostapi_bind.gen.cpp")
SELFHOST_PATH = os.path.join(REPO, "src", "mh_dll", "mh_nettest", "mh_hostapi_selftest.gen.cpp")

# The two accessors module code reaches the tables through (state/host_api.h). The spelling
# is table-selecting, and the scan below refuses a spelling that disagrees with the TU's
# module -- so the derivation cannot be defeated by calling the wrong accessor.
SIM_HOST_RE = re.compile(r"\bmh::host\(\)\.(\w+)")
TACT_HOST_RE = re.compile(r"\bmh::tact_host\(\)\.(\w+)")
LINE_C = re.compile(r"//[^\n]*")
BLOCK_C = re.compile(r"/\*.*?\*/", re.S)

# Category order is part of the ABI (struct layout follows it). notify flag per category.
#
# NAMED GROUPS, not a four-bucket split (LIFT-TABLE S7, 2026-09-09). The table used to carry four
# categories -- render-notify / sound / platform / map-io -- and `platform` had become the place
# every required entry went regardless of what it was for: files, the input pump, the clock, process
# death, the network, string codecs and a CD diagnostic, thirty of them in one undifferentiated
# list. A host implementor reading that list learned nothing about which entries it could stub, which
# it had to get bit-exact, and which it already had an implementation of.
#
# The groups below are what a host READS, so they are cut by what the host must supply, never by who
# calls the entry. The sim/tact TABLE split (LIB-IFACE-SPLIT) is orthogonal to the groups and is
# derived from the accessor sites, not from here.
CATEGORIES = (
    # ---- notify: a headless host may bind a no-op ------------------------------------------------
    ("host-callback:render-notify", "render-notify", 1),
    ("host-callback:sound", "sound", 1),
    ("host-callback:hook", "hook", 1),
    # ---- required: implement, or bind a NAMED trap ------------------------------------------------
    ("host-callback:display", "display", 0),
    ("host-callback:input", "input", 0),
    ("host-callback:chat", "chat", 0),
    ("host-callback:io", "io", 0),
    ("host-callback:map-io", "map-io", 0),
    ("host-callback:net", "net", 0),
    ("host-callback:string", "string", 0),
    ("host-callback:time", "time", 0),
    ("host-callback:fatal", "fatal", 0),
    ("host-callback:platform", "platform", 0),
)

CATEGORY_BLURB = {
    "render-notify": "visual/UI notifications; a headless host binds no-ops",
    "sound": "audio playback/control; a headless host binds no-ops",
    "hook": "the game's own EMPTY hook points -- retail's bodies are a stack probe and a return, so "
    "a no-op is not a degradation, it is the original behaviour; mh.dll binds them anyway to keep "
    "R5 exact",
    "display": "REQUIRED despite being visual: each of these either returns a value the sim reads "
    "or reconfigures state libmh reads back in the same call (R3/R3b), so a no-op DIVERGES",
    "input": "the input pump -- REQUIRED per R10: the host owns it and pushes, libmh never polls",
    "chat": "player chat-target bookkeeping -- REQUIRED per R3b (translated lockstep code reads the "
    "mask back in the same frame); libmh owns these in the end state, they are not host concerns",
    "io": "files and packed resources -- REQUIRED: implement or bind a named trap",
    "map-io": "map/save file access -- REQUIRED: implement or bind a named trap",
    "net": "the lockstep transport -- REQUIRED: a no-op is a session that never advances",
    "string": "locale-dependent text codecs -- REQUIRED, and one of them feeds HASHED sim state",
    "time": "the millisecond clock -- REQUIRED: pacing only, never hashed (R8)",
    "fatal": "process death and its cleanup -- REQUIRED: these must not return",
    "platform": "host services that fit no narrower group -- REQUIRED",
}

# Per-table identity: (struct name, macro prefix, short label, banner line).
TABLES = {
    "sim": (
        "libmh_host_api",
        "LIBMH_HOST_API",
        "SIM",
        "the SIM table -- every entry a non-tact libmh module reaches through mh::host(). "
        "This is the follow-on abstraction target (LIB-IFACE-SPLIT step 3).",
    ),
    "tact": (
        "libmh_tact_host_api",
        "LIBMH_TACT_HOST_API",
        "TACT",
        "the TACT table -- every entry tact/ reaches through mh::tact_host(). FROZEN by design: "
        "the sim table is the abstraction target; this one changes only when tact/ code does.",
    ),
}

# C keywords a Ghidra parameter name could collide with (C89/C99 core set).
C_RESERVED = frozenset(
    "auto break case char const continue default do double else enum extern float for goto "
    "if inline int long register restrict return short signed sizeof static struct switch "
    "typedef union unsigned void volatile while".split()
)


def _c_type(ctype):
    """Degrade a proto ctype to the C facade type. Returns (c_type, note-or-None)."""
    if "::" in ctype:
        # A C++-namespaced struct pointer -- opaque at the C boundary.
        return "void *", ctype
    return ctype, None


def _c_param_name(name):
    return name + "_" if name in C_RESERVED else name


def _fnv1a32(text):
    h = 0x811C9DC5
    for b in text.encode("utf-8"):
        h = ((h ^ b) * 0x01000193) & 0xFFFFFFFF
    return h


# A row's `abi` block: a DESIGNED entry shape, replacing the proto-derived one (SIMABI-VFS,
# 2026-09-10). Every other row's member name and signature are the ORIGINAL thunk's -- which is what
# made the mechanical table a mirror of the translation frontier rather than an interface. A reshaped
# entry names itself (`vfs_open`, not `utils_open_file`), drops the original's dead parameters, and
# takes the types a host implementor would choose; the ledger key stays the original callee so the
# census, the adjudication ledger and the caller evidence keep resolving.
#
#   name    the struct member / X-macro / trap name -- what a host binds
#   ret     C return type;  params  [[ctype, name], ...]
#   bind    OPTIONAL fully-qualified mh.dll function that fills the slot. Present when the shape
#           differs enough that no one-line forward exists: the binder absorbs the original's quirks
#           (synthesising a mode string, the constant 1s, the doubled filename, ptr -> copy -> free).
#           Absent means the generator still emits the one-line forward onto mh::call::<ledger key>.
#
# `tact_frozen` may carry `"abi": null` to keep the FROZEN tact table on the original shape while the
# sim side reshapes -- the per-side mechanism SIMABI-DISPLAY added, used here for its second case.
def _abi_proto(abi, key):
    """-> a dll_call_protos-shaped row synthesised from an `abi` block. `bind`/`origin` ride
    along so render_bind knows what fills the slot without a second ledger lookup."""
    return {
        "name": abi["name"],
        "status": "ok",
        "ret": {"ctype": abi["ret"]},
        "params": [{"ctype": t, "name": n} for t, n in abi["params"]],
        "bind": abi.get("bind"),
        "origin": key,
    }


# A row's `tact_frozen` block: the FROZEN tact table's own view of the same original, used when
# the two tables disagree about what an entry IS. That happens exactly once so far and the split
# was designed for it (SIMABI-DISPLAY, 2026-09-10): llm_view_set_size_mode CONVERTED sim-side to a
# screen record, while tact/ still binds it as a `display` host callback. One ledger row cannot say
# both, and the alternative -- reclassing tact's copy too -- would have re-frozen a tact decision
# into a sim-side reshape, which is the exclusivity defect LIB-IFACE-SPLIT exists to prevent.
#
# It carries `class` and `reason` because the tact HEADER prints both, and a converted entry's new
# reason describes a record tact does not send. A row without the block reads identically to both
# tables, which is still the normal case.
def _row_for(row, table):
    if table == "tact" and isinstance(row.get("tact_frozen"), dict):
        merged = dict(row)
        merged.update(row["tact_frozen"])
        return merged
    return row


def load_entries(table="sim"):
    """-> [(category, notify, name, proto)] in ABI order; hard-fails on a gap.

    `table` selects which view of a split row is read (see _row_for). `name` is the EMITTED
    name -- the ledger key unless the row's `abi` block renames it -- and the returned ledger
    map is rekeyed to match, so every downstream lookup is by what the table actually calls
    the entry."""
    ledger = json.load(io.open(LEDGER_PATH, encoding="utf-8"))["callees"]
    protos = {f["name"]: f for f in json.load(io.open(PROTOS_PATH, encoding="utf-8"))["functions"]}
    entries = []
    problems = []
    emitted_ledger = {}
    for key, row in ledger.items():
        view = _row_for(row, table)
        abi = view.get("abi")
        emitted_ledger[abi["name"] if abi else key] = view
    for cls, cat, notify in CATEGORIES:
        rows = []
        for key, row in ledger.items():
            view = _row_for(row, table)
            if view["class"] != cls:
                continue
            abi = view.get("abi")
            rows.append((abi["name"] if abi else key, key, abi))
        for name, key, abi in sorted(rows):
            if abi:
                entries.append((cat, notify, name, _abi_proto(abi, key)))
                continue
            p = protos.get(key)
            if p is None:
                problems.append("%s: no row in dll_call_protos.json" % key)
            elif p["status"] != "ok":
                problems.append("%s: proto status %r, not callable" % (key, p["status"]))
            else:
                entries.append((cat, notify, name, p))
    if problems:
        for msg in problems:
            print("[gen_libmh_hostapi] FAIL: %s" % msg, file=sys.stderr)
        sys.exit(1)
    return entries, emitted_ledger


def scan_membership(sim_names, tact_names):
    """-> ({name: sorted sim modules}, {name: sorted tact modules}); hard-fails on:
    an accessor/module disagreement, a scanned name with no ledger row IN THE TABLE THAT
    ACCESSOR SELECTS, or a host-callback entry with no site at all (membership underivable).

    The two name sets differ only for a `tact_frozen` split row, and the per-accessor check is
    what makes such a row safe: a sim module reaching a sim-side-CONVERTED entry through
    mh::host() fails here by name rather than quietly resurrecting it in the sim table."""
    sim_names, tact_names = set(sim_names), set(tact_names)
    names = sim_names | tact_names
    sim_sites, tact_sites = {}, {}
    problems = []
    for tree, root, dirs, files in _dllsrc.walk():
        rel_root = _dllsrc.rel_dir(tree, root)
        top = "" if rel_root == "." else rel_root.replace("\\", "/").split("/", 1)[0]
        if top in ("addr", "attic"):
            dirs[:] = []
            continue
        for fn in files:
            if not fn.endswith((".cpp", ".h")):
                continue
            rel = _dllsrc.rel_or_raise(os.path.join(root, fn))
            mod = rel.split("/", 1)[0] if "/" in rel else "(root)"
            text = LINE_C.sub(
                "",
                BLOCK_C.sub(
                    "", io.open(os.path.join(root, fn), encoding="utf-8", errors="replace").read()
                ),
            )
            for m in set(SIM_HOST_RE.findall(text)):
                if m not in sim_names:
                    problems.append(
                        "%s: mh::host().%s has no host-callback ledger row%s"
                        % (
                            rel,
                            m,
                            " for the SIM table (its sim-side class is not host-callback:* -- it "
                            "was converted; reach it through its record, not the table)"
                            if m in tact_names
                            else "",
                        )
                    )
                elif mod == "tact":
                    problems.append(
                        "%s: mh::host().%s -- tact/ must use mh::tact_host() (the tact table)"
                        % (rel, m)
                    )
                else:
                    sim_sites.setdefault(m, set()).add(mod)
            for m in set(TACT_HOST_RE.findall(text)):
                if m not in tact_names:
                    problems.append(
                        "%s: mh::tact_host().%s has no host-callback ledger row" % (rel, m)
                    )
                elif mod != "tact":
                    problems.append(
                        "%s: mh::tact_host().%s -- only tact/ may use the tact accessor" % (rel, m)
                    )
                else:
                    tact_sites.setdefault(m, set()).add(mod)
    for n in sorted(names):
        if n not in sim_sites and n not in tact_sites:
            problems.append(
                "%s: no mh::host()/mh::tact_host() site anywhere -- membership underivable "
                "(convert the entry or drop its ledger row)" % n
            )
    if problems:
        for msg in problems:
            print("[gen_libmh_hostapi] FAIL: %s" % msg, file=sys.stderr)
        sys.exit(1)
    return (
        {k: sorted(v) for k, v in sim_sites.items()},
        {k: sorted(v) for k, v in tact_sites.items()},
    )


def _signature(p):
    """-> (ret_c, [(type_c, pname, note)]) for one proto row."""
    ret_c, _ = _c_type(p["ret"]["ctype"])
    params = []
    for prm in p["params"]:
        t, note = _c_type(prm["ctype"])
        params.append((t, _c_param_name(prm["name"]), note))
    return ret_c, params


def _member_decl(name, p):
    ret_c, params = _signature(p)
    if params:
        parts = []
        for t, pname, note in params:
            decl = "%s%s%s" % (t, "" if t.endswith("*") else " ", pname)
            if note:
                decl += " /* %s */" % note
            parts.append(decl)
        arglist = ", ".join(parts)
    else:
        arglist = "void"
    return "%s%s(*%s)(%s);" % (ret_c, "" if ret_c.endswith("*") else " ", name, arglist)


def _abi_descriptor(entries):
    """The version input: everything layout/signature-relevant, nothing cosmetic."""
    lines = []
    for cat, notify, name, p in entries:
        ret_c, params = _signature(p)
        lines.append(
            "%s|%d|%s|%s|%s" % (cat, notify, name, ret_c, ",".join(t for t, _n, _o in params))
        )
    return "\n".join(lines)


def _wrap_comment(text, width=96):
    words = text.replace("*/", "* /").split()
    out, line = [], ""
    for w in words:
        if line and len(line) + 1 + len(w) > width:
            out.append(line)
            line = w
        else:
            line = (line + " " + w) if line else w
    if line:
        out.append(line)
    return out


def render(entries, ledger, table, membership):
    struct_name, prefix, label, banner = TABLES[table]
    version = _fnv1a32(_abi_descriptor(entries))
    L = []
    L.append("/* GENERATED by tools/gen_libmh_hostapi.py -- DO NOT EDIT; regenerate instead.")
    L.append(" * Sources: tools/data/libmh_call_ledger.json (host-callback:* rows) x")
    L.append(" * tools/data/dll_call_protos.json (signatures) x the accessor-site scan of")
    L.append(" * src/mh_dll/mh/** (membership). `--check` is the drift gate.")
    L.append(" *")
    for ln in _wrap_comment(banner):
        L.append(" * %s" % ln)
    L.append(" *")
    L.append(" * ONE OF TWO TABLES (LIB-IFACE-SPLIT): sim (libmh_host_api, via mh::host()) and")
    L.append(" * tact (libmh_tact_host_api, via mh::tact_host()), bound separately, each with its")
    L.append(" * own version handshake and unbound walk. An entry both sides call appears in BOTH")
    L.append(" * -- duplication is the design; nothing is assigned exclusively. Membership is")
    L.append(" * DERIVED from the accessor sites, so this file changes only when source sites do.")
    L.append(" *")
    L.append(" * mh.dll binds every entry over its mh::call:: thunks; a headless host binds no-ops")
    L.append(
        " * for the NOTIFY groups and real impls or NAMED traps for the REQUIRED ones -- each group's"
    )
    L.append(
        " * header line says which it is. C++ struct-pointer params are void* here on purpose --"
    )
    L.append(" * the true type is in the trailing comment and in addr/mh_structs.gen.h. */")
    L.append("#ifndef %s_GEN_H" % prefix)
    L.append("#define %s_GEN_H" % prefix)
    L.append("")
    L.append("#include <stdint.h>")
    L.append("")
    L.append("/* FNV-1a over the ABI-relevant text (categories, names, signatures). Pass to")
    L.append(
        " * libmh_set_%shost_api(); the implementation refuses a value it was not compiled with. */"
        % ("tact_" if table == "tact" else "")
    )
    L.append("#define %s_VERSION 0x%08Xu" % (prefix, version))
    L.append("#define %s_ENTRY_COUNT %du" % (prefix, len(entries)))
    L.append("")
    L.append("#ifdef __cplusplus")
    L.append("extern \"C\" {")
    L.append("#endif")
    L.append("")
    L.append("typedef struct %s {" % struct_name)
    cur_cat = None
    for cat, notify, name, p in entries:
        if cat != cur_cat:
            cur_cat = cat
            n = sum(1 for c, _f, _n, _p in entries if c == cat)
            L.append("")
            L.append("    /* ---- %s (%d): %s ---- */" % (cat, n, CATEGORY_BLURB[cat]))
        mods = membership.get(name)
        if mods:
            L.append("    /* [%s modules: %s] */" % (label.lower(), ", ".join(mods)))
        reason = ledger[name].get("reason", "")
        if reason:
            for ln in _wrap_comment(reason):
                L.append("    /* %s */" % ln)
        L.append("    " + _member_decl(name, p))
    L.append("} %s;" % struct_name)
    L.append("")
    L.append("#ifdef __cplusplus")
    L.append("} /* extern \"C\" */")
    L.append("#endif")
    L.append("")
    L.append("/* X-macro over every entry, in struct order: X(name, category, notify).")
    L.append(" * category is the GROUP NAME as a string literal; notify is 1 where a headless host")
    L.append(" * may bind a no-op. Group membership is the ledger's class, one place, not a rule.")
    L.append(
        " * Derive the name table / offsetof table / notify flags from this -- do not hand-list. */"
    )
    L.append("#define %s_FOR_EACH(X) \\" % prefix)
    for i, (cat, notify, name, _p) in enumerate(entries):
        tail = " \\" if i + 1 < len(entries) else ""
        L.append("    X(%s, \"%s\", %d)%s" % (name, cat, notify, tail))
    L.append("")
    L.append("#endif /* %s_GEN_H */" % prefix)
    return "\n".join(L) + "\n"


def _forward_args(p):
    """-> (table-typed arg decl list, wrapper-call arg list with casts where degraded)."""
    decls, calls = [], []
    for i, prm in enumerate(p["params"]):
        table_t, note = _c_type(prm["ctype"])
        an = "a%d" % i
        decls.append("%s%s%s" % (table_t, "" if table_t.endswith("*") else " ", an))
        calls.append("(%s)%s" % (prm["ctype"], an) if note else an)
    return ", ".join(decls), ", ".join(calls)


def render_bind(sim_entries, tact_entries):
    L = []
    L.append("// GENERATED by tools/gen_libmh_hostapi.py -- DO NOT EDIT; regenerate instead.")
    L.append("// The mh.dll THIN IMPLEMENTATION of the host-callback ABI (LIB-ABI stage B),")
    L.append("// BOTH tables (LIB-IFACE-SPLIT): every entry forwards to its mh::call:: thunk, so")
    L.append("// the hosted configuration executes the ORIGINAL bodies bit-exactly through the")
    L.append("// tables. An entry in both tables shares one forwarder -- in this host the sim and")
    L.append("// tact bindings of a shared entry are the same code on purpose; a standalone host")
    L.append("// may bind them differently. HARNESS-side TU (mh.vcxproj only) -- it names fixed")
    L.append("// VAs via mh::call and can never join libmh.")
    L.append("#include \"../../libmh/include/libmh_host_api.gen.h\"")
    L.append("#include \"../../libmh/include/libmh_tact_host_api.gen.h\"")
    L.append("#include \"addr/mh_calls.gen.h\"")
    L.append("#include \"include/mh_hostapi_bind.h\"")
    L.append("")
    L.append("namespace {")
    emitted = set()
    for _cat, _notify, name, p in sim_entries + tact_entries:
        if name in emitted or p.get("bind"):
            # A reshaped entry with a `bind` has no one-line forward: mh.dll's HAND-WRITTEN binder
            # fills the slot directly (seams/hostapi_io_bind.cpp), because absorbing the original
            # thunk's quirks is what the reshape moved out of libmh.
            continue
        emitted.add(name)
        ret_c, _params = _signature(p)
        decls, calls = _forward_args(p)
        call = "mh::call::%s(%s)" % (p.get("origin", name), calls)
        body = ("%s;" % call) if ret_c == "void" else ("return %s;" % call)
        L.append(
            "%s%sf_%s(%s) { %s }" % (ret_c, "" if ret_c.endswith("*") else " ", name, decls, body)
        )
    L.append("}  // namespace")
    L.append("")
    L.append("namespace mh::hostapi {")
    L.append("")
    L.append("const libmh_host_api &mhdll_table() {")
    L.append("    static const libmh_host_api t = {")
    for _cat, _notify, name, p in sim_entries:
        L.append("        %s," % (p.get("bind") or ("f_%s" % name)))
    L.append("    };")
    L.append("    return t;")
    L.append("}")
    L.append("")
    L.append("const libmh_tact_host_api &mhdll_tact_table() {")
    L.append("    static const libmh_tact_host_api t = {")
    for _cat, _notify, name, p in tact_entries:
        L.append("        %s," % (p.get("bind") or ("f_%s" % name)))
    L.append("    };")
    L.append("    return t;")
    L.append("}")
    L.append("")
    L.append("}  // namespace mh::hostapi")
    return "\n".join(L) + "\n"


def render_selfhost(sim_entries, tact_entries, ledger):
    L = []
    L.append("// GENERATED by tools/gen_libmh_hostapi.py -- DO NOT EDIT; regenerate instead.")
    L.append("// The SELFTEST HOST for the host-callback ABI (LIB-ABI stage D's no-op-host arm),")
    L.append("// BOTH tables (LIB-IFACE-SPLIT): entries in a NOTIFY group are return-default")
    L.append("// no-ops; entries in a REQUIRED group call mh_hostapi_trap(name) before returning")
    L.append("// a default, so a required entry reached without a real host impl is caught BY")
    L.append("// NAME, never silently. An entry in both tables shares one stub.")
    L.append(
        "// A ledger row's `noop_return` overrides the 0 default where the caller consumes the"
    )
    L.append("// value (stage E: overlay_dismiss/wait_player/outcome_dialog return 1;")
    L.append("// sync_overlay_show returns -1 -- the no-button value peer-removal branches on).")
    L.append("#include \"../libmh/include/libmh_host_api.gen.h\"")
    L.append("#include \"../libmh/include/libmh_tact_host_api.gen.h\"")
    L.append("#include \"hostapi_selftest_support.h\"")
    L.append("")
    L.append("namespace {")
    emitted = set()
    for _cat, notify, name, p in sim_entries + tact_entries:
        if name in emitted:
            continue
        emitted.add(name)
        ret_c, params = _signature(p)
        arglist = ", ".join(t for t, _n, _o in params)  # unnamed: no unused-param warnings
        stmts = [] if notify else ["mh_hostapi_trap(\"%s\");" % name]
        if ret_c != "void":
            stmts.append("return (%s)%d;" % (ret_c, ledger[name].get("noop_return", 0)))
        L.append(
            "%s%sh_%s(%s) { %s }"
            % (ret_c, "" if ret_c.endswith("*") else " ", name, arglist, " ".join(stmts))
        )
    L.append("}  // namespace")
    L.append("")
    L.append("const libmh_host_api &mh_hostapi_selftest_table() {")
    L.append("    static const libmh_host_api t = {")
    for _cat, _notify, name, _p in sim_entries:
        L.append("        h_%s," % name)
    L.append("    };")
    L.append("    return t;")
    L.append("}")
    L.append("")
    L.append("const libmh_tact_host_api &mh_hostapi_selftest_tact_table() {")
    L.append("    static const libmh_tact_host_api t = {")
    for _cat, _notify, name, _p in tact_entries:
        L.append("        h_%s," % name)
    L.append("    };")
    L.append("    return t;")
    L.append("}")
    return "\n".join(L) + "\n"


# THE CROSS-HEAP-FREE GATE (SIMABI-VFS, 2026-09-10). An `io` entry that RETURNS A POINTER hands
# libmh an allocation somebody then has to release, and until this slice two sim sites released it
# through the vendored CRT -- correct only while libmh and the host share one heap, which a
# standalone host does not. `asset_read` closed that by copying. The rule is kept here rather than in
# prose because prose is what let it stand: no sim-side `io` entry may return a pointer. A future
# entry that needs to hand back an allocation needs a paired release entry and a deliberate decision,
# and this gate is where that argument has to be made.
#
# SCOPED TO `io` ON PURPOSE. Other groups legitimately return pointers into the host's own static
# storage that libmh only reads and never owns (llm_build_media_diag_report's fixed 0x400 block,
# llm_str_ansi_to_wide_scratch's shared buffer); widening this to the whole table would fail them for
# a hazard they do not have.
def check_no_owned_pointers(sim_entries):
    bad = [
        name
        for cat, _notify, name, p in sim_entries
        if cat == "io" and _c_type(p["ret"]["ctype"])[0].endswith("*")
    ]
    if bad:
        print(
            "[gen_libmh_hostapi] FAIL: sim `io` entr%s returning a POINTER: %s.\n"
            "  An io entry that returns an allocation makes libmh its owner, and the only way to\n"
            "  release it is a free the host also understands -- the cross-heap hazard SIMABI-VFS\n"
            "  closed by making asset_read a COPY. Reshape it, or pair it with a release entry and\n"
            "  record the decision in docs/libmh-sim-abi.md before relaxing this gate."
            % ("ies" if len(bad) > 1 else "y", ", ".join(sorted(bad))),
            file=sys.stderr,
        )
        sys.exit(1)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--check", action="store_true", help="fail if a committed output is stale")
    args = ap.parse_args()

    # TWO VIEWS OF ONE LEDGER. They are the same list unless a row carries `tact_frozen` -- the
    # split-row case the tact table's freeze needs (see _row_for).
    entries, ledger = load_entries("sim")
    tact_all, tact_ledger = load_entries("tact")
    sim_mods, tact_mods = scan_membership(
        [n for _c, _f, n, _p in entries], [n for _c, _f, n, _p in tact_all]
    )
    sim_entries = [e for e in entries if e[2] in sim_mods]
    tact_entries = [e for e in tact_all if e[2] in tact_mods]
    check_no_owned_pointers(sim_entries)
    overlap = sorted(set(sim_mods) & set(tact_mods))
    # No entry silently dropped: every ledger entry is in at least one table (scan_membership
    # already failed any zero-site entry, so this is the belt to that suspender). Counted over the
    # UNION of the two views, so a split row is one entry, not two.
    all_names = {n for _c, _f, n, _p in entries} | {n for _c, _f, n, _p in tact_all}
    union = len(sim_entries) + len(tact_entries) - len(overlap)
    assert union == len(all_names), (union, len(all_names))

    outputs = (
        (OUT_PATH, render(sim_entries, ledger, "sim", sim_mods)),
        (TACT_OUT_PATH, render(tact_entries, tact_ledger, "tact", tact_mods)),
        (BIND_PATH, render_bind(sim_entries, tact_entries)),
        # The stub renderer looks a row up by its EMITTED name, and a split row is emitted under two
        # (asset_read sim-side, GetResourseFilePtr tact-side) -- so it needs both views merged, sim
        # winning where the two agree.
        (SELFHOST_PATH, render_selfhost(sim_entries, tact_entries, {**tact_ledger, **ledger})),
    )

    if args.check:
        for path, text in outputs:
            if not os.path.exists(path):
                print("[gen_libmh_hostapi] FAIL: %s does not exist -- run the generator" % path)
                return 1
            if io.open(path, encoding="utf-8").read() != text:
                print(
                    "[gen_libmh_hostapi] FAIL: %s is stale -- a ledger host-callback row, a "
                    "prototype, or an accessor site changed; rerun tools/gen_libmh_hostapi.py "
                    "and review the diff" % os.path.relpath(path, REPO)
                )
                return 1
        print(
            "[gen_libmh_hostapi] OK: host-api tables + binders current "
            "(sim %d 0x%08X, tact %d 0x%08X, both %d, union %d)"
            % (
                len(sim_entries),
                _fnv1a32(_abi_descriptor(sim_entries)),
                len(tact_entries),
                _fnv1a32(_abi_descriptor(tact_entries)),
                len(overlap),
                union,
            )
        )
        return 0

    for path, text in outputs:
        io.open(path, "w", encoding="utf-8", newline="\n").write(text)

    def by_cat(es):
        d = {}
        for cat, _notify, _name, _p in es:
            d[cat] = d.get(cat, 0) + 1
        return d

    sim_c, tact_c = by_cat(sim_entries), by_cat(tact_entries)
    print(
        "wrote %d files: sim %d (0x%08X), tact %d (0x%08X), both %d, union %d"
        % (
            len(outputs),
            len(sim_entries),
            _fnv1a32(_abi_descriptor(sim_entries)),
            len(tact_entries),
            _fnv1a32(_abi_descriptor(tact_entries)),
            len(overlap),
            union,
        )
    )
    print("  in both tables: %s" % (", ".join(overlap) or "none"))
    print("  %-14s %4s %5s" % ("group", "sim", "tact"))
    for c, _b in CATEGORY_BLURB.items():
        if c in sim_c or c in tact_c:
            print("  %-14s %4d %5d" % (c, sim_c.get(c, 0), tact_c.get(c, 0)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
