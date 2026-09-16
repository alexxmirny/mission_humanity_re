#!/usr/bin/env python3
"""gen_notify_readback.py -- the R3b gate: what each notify scope WRITES, and who reads it back.

  python tools/gen_notify_readback.py            # regenerate tools/data/notify_readback.json
  python tools/gen_notify_readback.py --check    # the lint_repo gate
  python tools/gen_notify_readback.py --md       # the human-readable worklist

WHAT R3b IS (docs/libmh-abi.md section 2). A notify record is void and fire-and-forget. R3 covers an
entry whose RETURN the sim consumes. R3b covers the other half: an entry that returns nothing but
whose *writes libmh reads back in the same frame*. The hosted config dispatches synchronously at
emit, so the readback happens to work today; a NULL-callback host drains at frame edge and libmh
would read the PREVIOUS frame's value. That is a divergence no oracle we own would catch, because
the hosted arm is the only arm the wall is ever tested in.

WHY IT IS A GENERATOR AND NOT A CHECKLIST. R3b was written at LIFT-TACT slice 0, 49-71 minutes
AFTER the on_screen scopes had already landed, and nothing ran it backwards over them. The
2026-09-04 sweep was that missed check, done by hand across 42 scopes. This is the same check made
mechanical so the gap cannot recur: a scope added tomorrow is measured the moment it is added.

------------------------------------------------------------------------------------------------
THE MEASUREMENT, and the three things it cannot see
------------------------------------------------------------------------------------------------

For each `case LIBMH_EVK_*` in mh.dll's sink (seams/host_event_sink.cpp -- the routing table that
turns a record back into the original call), the tool walks the ORIGINAL call graph from the
dispatch targets, unions the regions those functions STORE to, and intersects that with the regions
translated libmh READS. A non-empty intersection is a candidate readback.

  * THE FRAME-DRIVER CUT. Several screen scopes reach `llm_strat_frame_sim_only` -- the game's own
    frame driver -- and past it the closure saturates at 775 functions, i.e. the whole program.
    Those writes are the FRAME's, not the scope's, so the walk stops at a cut node and records that
    it crossed one. The cut set is DATA (`_frame_cuts` in the dispositions file) with a reason per
    entry, because a cut is exactly the kind of thing that gets added to make a number look better.
    `game_SetEvent` was MEASURED as a candidate (it collapses bldg_panel_open 50 -> 1 and
    progress_notify 50 -> 1) and DELIBERATELY REJECTED: it is the game's event dispatcher, not a
    frame loop, so a scope that fires a game event really does cause that event's writes. Tuning
    the cut set until the report is short is how a gate gets rigged; only a re-entry into the frame
    loop qualifies.

  * POINTER-MEDIATED WRITES ARE INVISIBLE (the blind bucket). state_write_index.json is projected
    from an instruction scan over DIRECT memory operands. A `mov [edx+eax], 2` whose EDX came from
    `_G_LLM_TILE_VIS_MAP_PTR` is attributed to nothing, so `llm_tact_vis_map_fill_default`,
    `_clear_right_margin` and `llm_tact_view_shift` all measure as writing NOTHING AT ALL. That is
    the sweep's inv 3/inv 4 pair, and a checker that reported them clean would be reporting silence
    as safety. Scopes whose closure yields zero write symbols are therefore classed BLIND, not
    clean, and the gate REQUIRES a disposition for each -- the hole is tracked, not hidden.

  * NO FRAME ORDERING. The tool cannot tell whether a reader runs before or after the emit within
    one frame. inv 3's reader provably always precedes its emit; that is a justified exemption
    carrying its reason, not something the intersection can derive.

So the intersection is a WORKLIST, and tools/data/notify_readback_dispositions.json is the ledger
that answers it. The gate is: nothing unlisted, and nothing listed without a reason.

------------------------------------------------------------------------------------------------
BUCKETS, and what each owes the ledger
------------------------------------------------------------------------------------------------

  clean    closure measured, no intersection                      -- owes nothing
  readback closure measured, intersects                           -- owes a PER-PAIR disposition
  crossed  closure crossed a frame cut, so it over-approximates    -- owes a PER-SCOPE disposition
  blind    closure yielded no write data at all                    -- owes a PER-SCOPE disposition

A `crossed` scope's pairs are listed but not individually dispositioned: the closure past a frame
driver is not a faithful subtree, so demanding 66 per-pair reasons for 7 scopes would buy rubber
stamps, not analysis. The scope-level entry says what is true about the whole scope instead.

Exit 0 on success; --check exits 1 on any gate failure.
"""

import argparse
import io
import json
import os
import re
import sys
from collections import deque

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from call_graph import load_graph  # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SINK = os.path.join(REPO, "src", "mh_dll", "mh", "seams", "host_event_sink.cpp")
EVENTS_H = os.path.join(REPO, "src", "mh_dll", "libmh", "include", "libmh_host_events.h")
WRITE_INDEX = os.path.join(REPO, "tools", "data", "state_write_index.json")
ACCESSORS = os.path.join(REPO, "tools", "data", "region_accessors.json")
DISPOSITIONS = os.path.join(REPO, "tools", "data", "notify_readback_dispositions.json")
OUT_PATH = os.path.join(REPO, "tools", "data", "notify_readback.json")
MD_PATH = os.path.join(REPO, "tmp", "libmh_abi_lift", "notify_readback.md")

# The sink's own local helpers are expanded into the case that calls them: a case that routes
# through draw_active_count_hud() dispatches everything that helper dispatches.
NOT_A_HELPER = ("route", "route_text", "log_first_dispatch")


def _load(path, encoding="utf-8"):
    return json.load(io.open(path, encoding=encoding))


def _strip_comments(text):
    return re.sub(r"//[^\n]*", "", re.sub(r"/\*.*?\*/", "", text, flags=re.S))


def parse_sink():
    """[(channel, kind_id, [dispatch targets])] from mh.dll's routing table.

    Case bodies are bounded by the NEXT case marker, so the last case of one channel's switch
    stops at the first case of the next -- the text between them is switch scaffolding with no
    dispatch in it. The trailing `namespace mh::hook` half of the file is cut first so the final
    case does not absorb the binder.
    """
    src = _strip_comments(io.open(SINK, encoding="utf-8").read()).split("namespace mh::hook")[0]

    helpers = {}
    helper_src = {}
    for m in re.finditer(r"\n(?:\w[\w:*&<>\s]*?)\b(\w+)\s*\([^;{]*\)\s*\{", src):
        name = m.group(1)
        if name in NOT_A_HELPER:
            continue
        start = src.index("{", m.end() - 1)
        depth = 0
        end = start
        for i in range(start, len(src)):
            if src[i] == "{":
                depth += 1
            elif src[i] == "}":
                depth -= 1
                if depth == 0:
                    end = i
                    break
        helpers[name] = sorted(set(re.findall(r"mh::call::(\w+)", src[start:end])))
        helper_src[name] = src[start:end]

    # HELPER EXPANSION IS TRANSITIVE, to a fixed point (LIFT-TACT slice A close, 2026-09-09).
    #
    # It used to be one level, and the comment here recorded that as a deliberate conservative
    # choice: a case routing through draw_group_panel -> blit_panel_icon -> mh::call resolved to
    # NO dispatch target, so the scope filed BLIND and the gate demanded a disposition. That was
    # sound as far as it went, and the note claimed a transitive expansion "did not resolve those
    # chains in practice". It does -- the chain that failed is case -> helper -> helper, i.e. two
    # levels from the CASE, so the fix is to close the HELPER MAP over itself rather than to
    # expand the case body twice.
    #
    # WHY THIS HAD TO BE FIXED RATHER THAN LIVED WITH, which is the part worth keeping. Blind is
    # conservative only when the scope resolves NOTHING. A scope that routes some targets directly
    # and one more through a second helper resolves a NON-EMPTY target set, so it files CLEAN --
    # with a subtree the tool never looked at. That is not conservatism, it is a silent hole, and
    # it is the exact shape the scope added in the same slice would have had: draw_sel_panel_init
    # dispatches seven targets directly and llm_ui_set_draw_surface through blit_icon_own_header.
    # A gate whose silence is only sometimes meaningful is worse than one that says it cannot see,
    # because nobody re-reads the clean rows.
    for _ in range(len(helpers) + 1):
        changed = False
        for name, targets in helpers.items():
            for called in re.findall(r"\b(\w+)\s*\(", helper_src.get(name, "")):
                if called == name:
                    continue
                for t in helpers.get(called, ()):
                    if t not in targets:
                        targets.append(t)
                        changed = True
        if not changed:
            break
    for name in helpers:
        helpers[name] = sorted(set(helpers[name]))

    # channel markers, so each kind case is attributed to the switch it sits in
    chans = [(m.start(), m.group(1)) for m in re.finditer(r"case\s+LIBMH_EVC_(\w+)\s*:", src)]

    scopes = []
    parts = re.split(r"\bcase\s+(LIBMH_EV[T]?K_\w+)\s*:", src)
    offset = len(parts[0])
    for i in range(1, len(parts), 2):
        kind_id, body = parts[i], parts[i + 1]
        channel = "text" if kind_id.startswith("LIBMH_EVTK_") else "unknown"
        if channel == "unknown":
            for pos, name in chans:
                if pos < offset:
                    channel = name.lower()
        targets = list(dict.fromkeys(re.findall(r"mh::call::(\w+)", body)))
        for called in re.findall(r"\b(\w+)\s*\(", body):
            for t in helpers.get(called, ()):
                if t not in targets:
                    targets.append(t)
        scopes.append([channel, kind_id, targets, body.strip() == ""])
        offset += len("case " + kind_id + ":") + len(body)

    # C fallthrough: `case A: case B: { shared body }` gives A an EMPTY body and B the shared one.
    # A dispatches exactly what B dispatches -- LIBMH_EVK_SCR_WGTL_CENTER/_DRAW are that pair, and
    # reading CENTER as "dispatches nothing" would file it BLIND and ask for a disposition of a
    # hole that is not there.
    for i, row in enumerate(scopes):
        if row[3]:
            for nxt in scopes[i + 1 :]:
                if not nxt[3]:
                    row[2] = list(nxt[2])
                    break
    return [(c, k, t) for c, k, t, _empty in scopes]


def parse_kind_numbers():
    """{macro name: int value} from the hand-authored contract header."""
    text = io.open(EVENTS_H, encoding="utf-8").read()
    return {
        m.group(1): int(m.group(2))
        for m in re.finditer(r"#define\s+(LIBMH_EV[T]?K_\w+)\s+(\d+)u", text)
    }


def closure(targets, ids_by_name, name_by_id, succ, cut_ids):
    """(reachable function names, cut nodes crossed, targets absent from the graph)."""
    missing = [t for t in targets if t not in ids_by_name]
    roots = [i for t in targets for i in ids_by_name.get(t, ())]
    seen, queue, crossed = set(roots), deque(roots), set()
    while queue:
        cur = queue.popleft()
        for nxt in succ.get(cur, ()):
            if nxt in cut_ids:
                crossed.add(name_by_id.get(nxt, nxt))
                continue
            if nxt not in seen:
                seen.add(nxt)
                queue.append(nxt)
    return {name_by_id[i] for i in seen if i in name_by_id}, sorted(crossed), missing


def build():
    disp = _load(DISPOSITIONS)
    cuts = disp.get("_frame_cuts", {})
    name_by_id, ids_by_name, succ = load_graph()
    cut_ids = set()
    for fn in cuts:
        cut_ids |= set(ids_by_name.get(fn, ()))

    writers = _load(WRITE_INDEX)["writers"]
    regions = _load(ACCESSORS)["regions"]
    sym_to_rid = {}
    for rid, meta in regions.items():
        sym_to_rid.setdefault(meta["name"], []).append(rid)
    kind_no = parse_kind_numbers()

    rows = []
    for channel, kind_id, targets in parse_sink():
        fns, crossed, missing = closure(targets, ids_by_name, name_by_id, succ, cut_ids)
        write_syms = sorted(s for s, ws in writers.items() if set(ws) & fns)
        readback = []
        for sym in write_syms:
            for rid in sym_to_rid.get(sym, ()):
                readers = regions[rid].get("ours_readers") or []
                if readers:
                    readback.append({"region": rid, "symbol": sym, "readers": sorted(readers)})
        if not write_syms:
            bucket = "blind"
        elif crossed:
            bucket = "crossed"
        elif readback:
            bucket = "readback"
        else:
            bucket = "clean"
        rows.append(
            {
                "channel": channel,
                "kind": kind_id,
                "kind_no": kind_no.get(kind_id),
                "dispatch": targets,
                "targets_absent_from_graph": missing,
                "reachable_fns": len(fns),
                "write_symbols": len(write_syms),
                "frame_cuts_crossed": crossed,
                "bucket": bucket,
                "readback": sorted(readback, key=lambda r: r["region"]),
            }
        )

    rows.sort(key=lambda r: (r["channel"], r["kind_no"] if r["kind_no"] is not None else 0))
    counts = {}
    for r in rows:
        counts[r["bucket"]] = counts.get(r["bucket"], 0) + 1
    return {
        "_generated_by": "tools/gen_notify_readback.py -- DO NOT HAND-EDIT",
        "_what": "Per notify scope: what its host-side subtree writes, and which of those regions "
        "translated libmh reads back (R3b). The ledger that answers it is "
        "tools/data/notify_readback_dispositions.json.",
        "_provenance": {
            "sink": os.path.relpath(SINK, REPO).replace("\\", "/"),
            "frame_cuts": sorted(cuts),
            "scopes": len(rows),
            "buckets": counts,
            "readback_pairs": sum(len(r["readback"]) for r in rows if r["bucket"] == "readback"),
            "crossed_pairs": sum(len(r["readback"]) for r in rows if r["bucket"] == "crossed"),
        },
        "scopes": rows,
    }


def check(data, disp):
    """[failure strings]. Empty means green."""
    fails = []
    pair_d = {(e["kind"], e["region"]): e for e in disp.get("pairs", [])}
    scope_d = {e["kind"]: e for e in disp.get("scopes", [])}
    seen_pairs, seen_scopes = set(), set()

    for row in data["scopes"]:
        kind = row["kind"]
        if row["bucket"] == "readback":
            for hit in row["readback"]:
                key = (kind, hit["region"])
                seen_pairs.add(key)
                entry = pair_d.get(key)
                if entry is None:
                    fails.append(
                        f"UNDISPOSITIONED READBACK {kind} writes {hit['region']} "
                        f"({hit['symbol']}), read by {', '.join(hit['readers'][:3])} -- "
                        f"add a pairs[] entry with a reason, or fix the readback"
                    )
                elif not (entry.get("reason") or "").strip():
                    fails.append(f"EMPTY REASON on pair {kind} / {hit['region']}")
                elif entry.get("verdict") not in disp.get("_verdicts", {}):
                    fails.append(
                        f"UNKNOWN VERDICT {entry.get('verdict')!r} on pair {kind} / {hit['region']}"
                    )
        elif row["bucket"] in ("crossed", "blind"):
            seen_scopes.add(kind)
            entry = scope_d.get(kind)
            if entry is None:
                fails.append(
                    f"UNDISPOSITIONED {row['bucket'].upper()} SCOPE {kind} -- its closure is "
                    f"{'not a faithful subtree (crossed ' + ', '.join(row['frame_cuts_crossed']) + ')' if row['bucket'] == 'crossed' else 'unmeasurable (no direct-operand writes; pointer-mediated)'}"
                    f", so it owes a scopes[] entry with a reason"
                )
            elif not (entry.get("reason") or "").strip():
                fails.append(f"EMPTY REASON on scope {kind}")
            elif entry.get("verdict") not in disp.get("_verdicts", {}):
                fails.append(f"UNKNOWN VERDICT {entry.get('verdict')!r} on scope {kind}")

    for key in sorted(pair_d):
        if key not in seen_pairs:
            fails.append(
                f"STALE PAIR DISPOSITION {key[0]} / {key[1]} -- no such readback any more; "
                f"delete it (a ledger that keeps answers to questions nobody asks stops being read)"
            )
    for kind in sorted(scope_d):
        if kind not in seen_scopes:
            fails.append(f"STALE SCOPE DISPOSITION {kind} -- it is no longer crossed/blind")
    return fails


def render_md(data, disp):
    pair_d = {(e["kind"], e["region"]): e for e in disp.get("pairs", [])}
    scope_d = {e["kind"]: e for e in disp.get("scopes", [])}
    out = ["# R3b readback worklist (generated -- do not hand-edit)", ""]
    p = data["_provenance"]
    out.append(
        f"{p['scopes']} scopes; buckets {p['buckets']}; {p['readback_pairs']} pairs to disposition "
        f"({p['crossed_pairs']} more under crossed scopes, dispositioned per scope)."
    )
    out.append("")
    for bucket in ("readback", "crossed", "blind", "clean"):
        rows = [r for r in data["scopes"] if r["bucket"] == bucket]
        if not rows:
            continue
        out.append(f"## {bucket} ({len(rows)})")
        out.append("")
        for r in rows:
            head = f"- **{r['kind']}** ({r['channel']}) reach={r['reachable_fns']}"
            if r["frame_cuts_crossed"]:
                head += f" CUT@{','.join(r['frame_cuts_crossed'])}"
            sd = scope_d.get(r["kind"])
            if sd:
                head += f" -- `{sd['verdict']}`: {sd['reason']}"
            out.append(head)
            for hit in r["readback"]:
                d = pair_d.get((r["kind"], hit["region"]))
                tag = f"`{d['verdict']}`: {d['reason']}" if d else "**UNDISPOSITIONED**"
                out.append(f"    - {hit['region']} <- {', '.join(hit['readers'][:4])} -- {tag}")
        out.append("")
    return "\n".join(out) + "\n"


def selftest(data, disp):
    """Mutation proof: the four ways this gate must go RED. [failure strings]; empty means armed.

    A gate nobody has watched fail is a gate nobody knows is wired up. These run the real check()
    over deliberately broken inputs, in memory, so the arming is re-proven on every lint run rather
    than once by hand at authoring time.
    """
    import copy

    problems = []

    def expect_red(label, d, p, needle):
        fails = check(d, p)
        if not any(needle in f for f in fails):
            problems.append(
                f"{label}: expected a failure containing {needle!r}, got {fails or 'GREEN'}"
            )

    # 1. A NEW READBACK. The case this whole gate exists for: a scope starts writing something
    #    translated libmh reads, and nobody has said whether that is safe.
    d1 = copy.deepcopy(data)
    victim = next((r for r in d1["scopes"] if r["bucket"] == "clean"), None)
    if victim is None:
        problems.append("1. no clean scope to seed a readback into -- the mutation cannot be run")
    else:
        victim["bucket"] = "readback"
        victim["readback"] = [
            {"region": "__SEEDED__", "symbol": "_G_SEEDED", "readers": ["seeded"]}
        ]
        expect_red("1. seeded readback", d1, disp, "UNDISPOSITIONED READBACK")

    # 2. AN EMPTY REASON. A ledger entry that cites nothing is the failure mode a ledger invites:
    #    the shape of an answer with none of the substance.
    if disp.get("pairs"):
        p2 = copy.deepcopy(disp)
        p2["pairs"][0]["reason"] = "   "
        expect_red("2. blanked reason", data, p2, "EMPTY REASON")

    # 3. A DROPPED SCOPE DISPOSITION -- the crossed/blind arm, where silence reads as safety.
    if disp.get("scopes"):
        p3 = copy.deepcopy(disp)
        p3["scopes"] = p3["scopes"][1:]
        expect_red("3. dropped scope disposition", data, p3, "UNDISPOSITIONED")

    # 4. A STALE ENTRY. An answer to a question nobody asks any more; left in place, it is how a
    #    ledger stops being read.
    p4 = copy.deepcopy(disp)
    p4["pairs"] = p4["pairs"] + [
        {
            "kind": "LIBMH_EVK___GONE__",
            "region": "__GONE__",
            "verdict": "justified-exempt",
            "reason": "stale on purpose",
        }
    ]
    expect_red("4. stale disposition", data, p4, "STALE PAIR DISPOSITION")
    return problems


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--check", action="store_true", help="gate: fail on drift or a missing reason")
    ap.add_argument("--md", action="store_true", help="also render the human worklist")
    ap.add_argument("--selftest", action="store_true", help="prove the gate still goes red")
    args = ap.parse_args()

    data = build()
    disp = _load(DISPOSITIONS)

    if args.selftest:
        problems = selftest(data, disp)
        for pr in problems:
            print(f"  !! {pr}")
        print(
            "[r3b] selftest FAIL"
            if problems
            else "[r3b] selftest ok -- all four negative cases fire"
        )
        return 1 if problems else 0

    if args.check:
        committed = _load(OUT_PATH) if os.path.exists(OUT_PATH) else None
        fails = []
        if committed != data:
            fails.append(
                "notify_readback.json is STALE -- rerun `python tools/gen_notify_readback.py`"
            )
        fails += check(data, disp)
        p = data["_provenance"]
        print(f"[r3b] {p['scopes']} scopes, buckets {p['buckets']}, {p['readback_pairs']} pairs")
        for f in fails:
            print(f"  !! {f}")
        if fails:
            print(f"[r3b] FAIL -- {len(fails)} problem(s)")
            return 1
        print("[r3b] ok -- every readback and every unmeasurable scope carries a reason")
        return 0

    with io.open(OUT_PATH, "w", encoding="utf-8", newline="\n") as fh:
        json.dump(data, fh, indent=1, ensure_ascii=False)
        fh.write("\n")
    print(f"wrote {os.path.relpath(OUT_PATH, REPO)}")
    if args.md:
        os.makedirs(os.path.dirname(MD_PATH), exist_ok=True)
        io.open(MD_PATH, "w", encoding="utf-8", newline="\n").write(render_md(data, disp))
        print(f"wrote {os.path.relpath(MD_PATH, REPO)}")
    p = data["_provenance"]
    print(f"  scopes {p['scopes']}  buckets {p['buckets']}  readback pairs {p['readback_pairs']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
