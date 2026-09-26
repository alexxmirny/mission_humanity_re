#!/usr/bin/env python3
"""lint_fixture_currency.py -- every recorded hash fixture is CURRENT, every excusal is REAL
(tooling TL-GATE-D25FX, 2026-09-20).

THE TRAP THIS CLOSES. A hash-manifest change (D25 appended region 62, player_resources) silently
stales every artifact that carries a `state` hash: the three libref world blobs + their streams, and
the UI-REC A/B/C oracles. Nothing said "stale" -- the next full gate, a day later, read them as
"first mismatch at step 1", the shape of a broken replayer, and the D25 session had run
run_selftests (green: it hashes nothing recorded) rather than the gate. The blobs DID carry the
manifest fingerprint and libref_host DID refuse them by it; the refusal was one line in a 5000-line
log under a "no ALL STEPS IDENTICAL" headline, and the oracles carried no stamp at all.

So this lint makes the rule "a manifest change means a re-capture IN THE SAME SESSION" a gate
rather than a sentence: it computes the manifest fingerprint the DLL is built with
(mp_analyze.hash_manifest_fingerprint, from the generated header) and requires

  (1) every committed libref fixture's manifest.json step0.hash_manifest_fp to equal it;
  (2) every committed UI-REC oracle (tools/uiscripts/journals/*.oracle.gz) to carry
      `; hash_manifest_fp: <fp>` equal to it -- an unstamped oracle is stale by definition;
  (3) every row of tools/data/abc_excusals.json to name a region that EXISTS in
      tools/data/hash_manifest.json and is not excluded there, with every byte range inside the
      region, `legs` within {A-vs-B, A-vs-C}, and a reason / since / tracker -- a region that stops existing fails
      here rather than silently excusing nothing;
  (4) the evidence file each row cites to exist, and every run in it to read PROVED with no region
      an excusal does not name;
  (5) every fixture and oracle to carry the CURRENT hash-input epoch (region_view.h
      HASH_INPUT_EPOCH, tooling TL-GATE8) -- the bytes-fed-in class neither fingerprint sees.
  (6) every libref fixture's manifest.json step0.world_schema_fp to equal the CURRENT world-blob
      schema fingerprint (mp_analyze.world_schema_fingerprint(), a Python mirror of
      mh::state::blob::schema_fingerprint<world_policy>() recomputed from the committed
      tools/data/world_snapshot_schema.json -- tooling TL-FIXTURE-SCHEMA, the 2026-09-22 rc=-3
      incident this item closes) -- a block-table change (a region gaining a measured extent, like
      BROWSER_UI_ROWS did) moves this and every recorded world.bin is then ERR_SCHEMA against the
      new build, which used to reach the libref unit as "rc=-3" 20 minutes into a gate instead of
      here.

Exit 0 = current. 1 = something is stale or malformed (the lines say what and how to re-capture).
`--selftest` plants each failure in a scratch copy and requires it to go red.
"""

from __future__ import annotations

import argparse
import copy
import glob
import gzip
import json
import os
import re
import shutil
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import mp_analyze as _m  # noqa: E402

FIXTURES = os.path.join(REPO, "tools", "data", "fixtures")
JOURNALS = os.path.join(REPO, "tools", "uiscripts", "journals")
EXCUSALS = os.path.join(REPO, "tools", "data", "abc_excusals.json")
HASH_MANIFEST = os.path.join(REPO, "tools", "data", "hash_manifest.json")

RECAPTURE_HINT = (
    "re-capture under the current manifest in THIS session: libref fixtures per "
    "tools/data/fixtures/libref-replay-v1/README.md (record -> pack -> replay -> stream -> "
    "counts-guard -> replay -> verify), oracles via `python tools/test_ui.py --ui-oracle <mode="
    "original arm> --ui-oracle-out <oracle> --ui-oracle-note ...` with the equivalence proof"
)


def oracle_epoch(path):
    with gzip.open(path, "rt", encoding="utf-8", errors="replace") as f:
        for line in f:
            if not line.startswith(";"):
                break
            m = re.match(r";\s*hash_input_epoch:\s*(\d+)\s*$", line)
            if m:
                return int(m.group(1))
    return None


def oracle_fp(path):
    with gzip.open(path, "rt", encoding="utf-8", errors="replace") as f:
        for line in f:
            if not line.startswith(";"):
                break
            m = re.match(r";\s*hash_manifest_fp:\s*([0-9A-Fa-f]{8})", line)
            if m:
                return m.group(1).upper()
    return None


def check(
    fixtures=FIXTURES,
    journals=JOURNALS,
    excusals=EXCUSALS,
    hash_manifest=HASH_MANIFEST,
    header_text=None,
    epoch=None,
    schema_fp=None,
):
    errs = []
    cur = _m.hash_manifest_fingerprint(header_text)
    cur_ep = _m.hash_input_epoch() if epoch is None else epoch
    cur_schema = _m.world_schema_fingerprint() if schema_fp is None else schema_fp

    # (1) the libref fixtures
    for mf in sorted(glob.glob(os.path.join(fixtures, "*", "manifest.json"))):
        name = os.path.basename(os.path.dirname(mf))
        try:
            man = json.load(open(mf, encoding="utf-8"))
        except (OSError, ValueError) as e:
            errs.append("%s: unreadable manifest (%s)" % (name, e))
            continue
        fp = (man.get("step0") or {}).get("hash_manifest_fp")
        if fp is None:
            continue  # not a hash fixture (pacing-frametime has no step0)
        if fp.upper() != cur:
            errs.append(
                "STALE FIXTURE %s: captured under hash manifest %s, current %s -- %s"
                % (name, fp, cur, RECAPTURE_HINT)
            )
        bad = _m.epoch_mismatch(man["step0"].get("hash_input_epoch"), cur_ep)
        if bad:
            errs.append("STALE FIXTURE %s: %s -- %s" % (name, bad, RECAPTURE_HINT))
        sfp = man["step0"].get("world_schema_fp")
        if sfp is None:
            errs.append(
                "STALE FIXTURE %s: no `world_schema_fp` in step0, so it is stale by definition "
                "(current world-blob schema %s) -- %s" % (name, cur_schema, RECAPTURE_HINT)
            )
        elif sfp.upper() != cur_schema:
            errs.append(
                "STALE FIXTURE %s: captured against world-blob schema %s, current %s (a carried "
                "region changed -- ERR_SCHEMA at libref, not merely a lint, until re-recorded) -- %s"
                % (name, sfp, cur_schema, RECAPTURE_HINT)
            )

    # (2) the UI-REC oracles
    for op in sorted(glob.glob(os.path.join(journals, "*.oracle.gz"))):
        name = os.path.basename(op)
        try:
            fp = oracle_fp(op)
        except (OSError, EOFError) as e:
            errs.append("%s: unreadable oracle (%s)" % (name, e))
            continue
        if fp is None:
            errs.append(
                "UNSTAMPED ORACLE %s: no `; hash_manifest_fp:` header, so it is stale by "
                "definition (current %s) -- %s" % (name, cur, RECAPTURE_HINT)
            )
        elif fp != cur:
            errs.append(
                "STALE ORACLE %s: captured under hash manifest %s, current %s -- %s"
                % (name, fp, cur, RECAPTURE_HINT)
            )
        bad = _m.epoch_mismatch(oracle_epoch(op), cur_ep)
        if bad:
            errs.append("STALE ORACLE %s: %s -- %s" % (name, bad, RECAPTURE_HINT))

    # (3) the excusal rows against the hash manifest
    regions = {}
    try:
        for r in json.load(open(hash_manifest, encoding="utf-8"))["regions"]:
            regions[r["name"]] = r
    except (OSError, ValueError, KeyError) as e:
        errs.append("hash_manifest.json unreadable (%s)" % e)
    rows = []
    if os.path.isfile(excusals):
        try:
            rows = json.load(open(excusals, encoding="utf-8")).get("excusals", [])
        except (OSError, ValueError) as e:
            errs.append("abc_excusals.json unreadable (%s)" % e)
    named = set()
    for i, e in enumerate(rows):
        tag = "excusal[%d] %s" % (i, e.get("id", "?"))
        for k in ("id", "legs", "region", "reason", "since", "tracker"):
            if not e.get(k):
                errs.append("%s: missing `%s`" % (tag, k))
        legs = e.get("legs") or []
        if not isinstance(legs, list) or not legs or not set(legs) <= {"A-vs-B", "A-vs-C"}:
            errs.append(
                "%s: legs %r -- only the SHIP legs (A-vs-B, A-vs-C) may carry an excusal; "
                "B-vs-C is the replayer's own claim and compares the recording in full"
                % (tag, legs)
            )
        reg = regions.get(e.get("region"))
        if reg is None:
            errs.append(
                "%s: region %r is not in tools/data/hash_manifest.json -- the excusal names "
                "nothing; drop the row or re-target it" % (tag, e.get("region"))
            )
            continue
        named.add(e["region"])
        if reg.get("excluded"):
            errs.append(
                "%s: region %r is already excluded from `state` by the manifest -- an excusal "
                "on it is vacuous" % (tag, e["region"])
            )
        for b in e.get("bytes") or []:
            off, ln = int(b.get("offset", -1)), int(b.get("len", 0))
            if off < 0 or ln <= 0 or off + ln > int(reg["size"]):
                errs.append(
                    "%s: bytes [%d, +%d) fall outside %s (%d bytes)"
                    % (tag, off, ln, e["region"], int(reg["size"]))
                )
        # (4) the evidence
        ev = e.get("evidence")
        if not ev:
            errs.append("%s: no `evidence` file cited" % tag)
        else:
            evp = os.path.join(REPO, ev) if not os.path.isabs(ev) else ev
            if not os.path.isfile(evp):
                errs.append("%s: evidence file %s does not exist" % (tag, ev))
            else:
                try:
                    runs = json.load(open(evp, encoding="utf-8")).get("runs", [])
                except (OSError, ValueError) as ex:
                    errs.append("%s: evidence %s unreadable (%s)" % (tag, ev, ex))
                    runs = None
                if runs is not None:
                    if not runs:
                        errs.append("%s: evidence %s holds no run" % (tag, ev))
                    for j, run in enumerate(runs):
                        if run.get("verdict") != "PROVED":
                            errs.append(
                                "%s: evidence run %d verdict %r, not PROVED"
                                % (tag, j, run.get("verdict"))
                            )
                        for r in run.get("region_columns_differing", []):
                            if r.get("region") not in {x.get("region") for x in rows}:
                                errs.append(
                                    "%s: evidence run %d shows %s differing and no excusal "
                                    "names it" % (tag, j, r.get("region"))
                                )
    return cur, errs


def report(cur, errs, label=""):
    print(
        "fixture currency%s: hash manifest fingerprint %s, hash-input epoch %s"
        % (label, cur, _m.hash_input_epoch())
    )
    for e in errs:
        print("  RED: %s" % e)
    print("  %s" % ("PASS" if not errs else "%d problem(s)" % len(errs)))
    return 0 if not errs else 1


def selftest():
    """Plant each failure in a scratch copy of the inputs; every one must go red, the clean copy green."""
    tmp = tempfile.mkdtemp(prefix="lint_fixture_currency_")
    try:
        fx = os.path.join(tmp, "fixtures")
        jn = os.path.join(tmp, "journals")
        os.makedirs(fx)
        os.makedirs(jn)
        for d in glob.glob(os.path.join(FIXTURES, "*")):
            mf = os.path.join(d, "manifest.json")
            if os.path.isfile(mf):
                os.makedirs(os.path.join(fx, os.path.basename(d)))
                shutil.copy(mf, os.path.join(fx, os.path.basename(d), "manifest.json"))
        for op in glob.glob(os.path.join(JOURNALS, "*.oracle.gz")):
            shutil.copy(op, jn)
        exc = os.path.join(tmp, "abc_excusals.json")
        shutil.copy(EXCUSALS, exc)
        # NORMALISE THE COPY to the current fingerprint first: the selftest proves the MECHANISM,
        # and must not depend on whether the tree happens to be mid-re-capture (it was, the first
        # time this ran -- two fixtures were still being re-recorded, and "clean copy is green"
        # would have reported the tree's state rather than the lint's).
        cur = _m.hash_manifest_fingerprint()
        cur_schema = _m.world_schema_fingerprint()
        for mf in glob.glob(os.path.join(fx, "*", "manifest.json")):
            man = json.load(open(mf, encoding="utf-8"))
            if "step0" in man:
                man["step0"]["hash_manifest_fp"] = cur
                man["step0"]["hash_input_epoch"] = _m.hash_input_epoch()
                man["step0"]["world_schema_fp"] = cur_schema
                json.dump(man, open(mf, "w", encoding="utf-8"))
        for op in glob.glob(os.path.join(jn, "*.oracle.gz")):
            lines = gzip.open(op, "rt", encoding="utf-8").read().splitlines(True)
            lines = [
                ln for ln in lines if "hash_manifest_fp" not in ln and "hash_input_epoch" not in ln
            ]
            lines.insert(1, "; hash_manifest_fp: %s\n" % cur)
            lines.insert(2, "; hash_input_epoch: %s\n" % _m.hash_input_epoch())
            with gzip.open(op, "wt", encoding="utf-8", newline="\n") as f:
                f.writelines(lines)

        def run(label, want_red, **kw):
            cur, errs = check(
                fixtures=kw.get("fixtures", fx),
                journals=kw.get("journals", jn),
                excusals=kw.get("excusals", exc),
                epoch=kw.get("epoch"),
                schema_fp=kw.get("schema_fp"),
            )
            red = bool(errs)
            ok = red == want_red
            print(
                "  %-52s %s%s"
                % (
                    label,
                    "PASS" if ok else "FAIL",
                    "" if ok else " (%s)" % ("; ".join(errs[:2]) or "no error"),
                )
            )
            return ok

        ok = run("clean copy is green", False)

        # (a) a fixture stamped with a foreign fingerprint
        mfs = glob.glob(os.path.join(fx, "*", "manifest.json"))
        mfs = [m for m in mfs if "step0" in json.load(open(m))]
        assert mfs, "no hash fixture to plant in"
        bak = open(mfs[0], encoding="utf-8").read()
        man = json.loads(bak)
        man["step0"]["hash_manifest_fp"] = "DEADBEEF"
        json.dump(man, open(mfs[0], "w", encoding="utf-8"))
        ok &= run("a fixture under another manifest goes RED", True)
        man = json.loads(bak)
        man["step0"]["hash_input_epoch"] = _m.hash_input_epoch() + 1
        json.dump(man, open(mfs[0], "w", encoding="utf-8"))
        ok &= run("a fixture of another hash-input epoch goes RED", True)
        man["step0"].pop("hash_input_epoch")
        json.dump(man, open(mfs[0], "w", encoding="utf-8"))
        ok &= run("a fixture with NO hash-input epoch goes RED", True)
        open(mfs[0], "w", encoding="utf-8").write(bak)
        ok &= run(
            "the build epoch moving alone reds the clean copy",
            True,
            epoch=_m.hash_input_epoch() + 1,
        )

        # (a2) world-blob schema: a fixture under a foreign/absent schema
        man = json.loads(bak)
        man["step0"]["world_schema_fp"] = "DEADBEEF"
        json.dump(man, open(mfs[0], "w", encoding="utf-8"))
        ok &= run("a fixture under another world-blob schema goes RED", True)
        man = json.loads(bak)
        man["step0"].pop("world_schema_fp")
        json.dump(man, open(mfs[0], "w", encoding="utf-8"))
        ok &= run("a fixture with NO world_schema_fp goes RED", True)
        open(mfs[0], "w", encoding="utf-8").write(bak)
        ok &= run("restored copy is green again (schema arm)", False)

        # (a3) THE 2026-09-22 SHAPE, reproduced: a region gains a measured extent (one extra
        # carried block) and the table's schema fingerprint moves ALONE -- every fixture recorded
        # under the old table, unmutated, must read STALE against it (this is what turned into
        # rc=-3/ERR_SCHEMA at libref instead of a lint line the first time). The block is
        # synthetic (this proves the LINT's detection, not the DLL -- the mirror itself was
        # mutation-proven against a real rebuild; see mp_analyze.world_schema_fingerprint's note).
        schema_doc = json.load(open(_m.WORLD_SNAPSHOT_SCHEMA, encoding="utf-8"))
        mutated = copy.deepcopy(schema_doc)
        mutated["blocks"].append(
            {
                "rid": max(b["rid"] for b in mutated["blocks"]) + 1000,
                "id": "SELFTEST_EXTRA_BLOCK",
                "name": "_G_LLM_SELFTEST_EXTRA_BLOCK",
                "len": 8,
            }
        )
        mutated_fp = _m.world_schema_fingerprint(schema=mutated)
        assert mutated_fp != cur_schema, "planted extra block did not move the schema fingerprint"
        ok &= run(
            "the 2026-09-22 shape (one extra carried block) reds the clean copy",
            True,
            schema_fp=mutated_fp,
        )

        # (b) an oracle without its stamp, (c) one with a foreign stamp
        ops = glob.glob(os.path.join(jn, "*.oracle.gz"))
        assert ops, "no oracle to plant in"
        lines = gzip.open(ops[0], "rt", encoding="utf-8").read().splitlines(True)
        with gzip.open(ops[0], "wt", encoding="utf-8", newline="\n") as f:
            f.writelines(ln for ln in lines if "hash_manifest_fp" not in ln)
        ok &= run("an UNSTAMPED oracle goes RED", True)
        with gzip.open(ops[0], "wt", encoding="utf-8", newline="\n") as f:
            f.writelines(
                re.sub(r"hash_manifest_fp:\s*\w+", "hash_manifest_fp: DEADBEEF", ln) for ln in lines
            )
        ok &= run("an oracle under another manifest goes RED", True)
        with gzip.open(ops[0], "wt", encoding="utf-8", newline="\n") as f:
            f.writelines(ln for ln in lines if "hash_input_epoch" not in ln)
        ok &= run("an oracle with NO hash-input epoch goes RED", True)
        with gzip.open(ops[0], "wt", encoding="utf-8", newline="\n") as f:
            f.writelines(lines)
        ok &= run("restored copy is green again", False)

        # (d) an excusal naming a region the manifest does not have
        ex_bak = open(exc, encoding="utf-8").read()
        d = json.loads(ex_bak)
        d["excusals"][0]["region"] = "no_such_region"
        json.dump(d, open(exc, "w", encoding="utf-8"))
        ok &= run("an excusal on a region that stopped existing goes RED", True)
        # (e) bytes outside the region
        d = json.loads(ex_bak)
        d["excusals"][0]["bytes"] = [{"offset": 10**9, "len": 4}]
        json.dump(d, open(exc, "w", encoding="utf-8"))
        ok &= run("an excusal whose bytes fall outside the region goes RED", True)
        # (f) the wrong leg
        d = json.loads(ex_bak)
        d["excusals"][0]["legs"] = ["A-vs-B", "B-vs-C"]
        json.dump(d, open(exc, "w", encoding="utf-8"))
        ok &= run("an excusal naming B-vs-C goes RED", True)
        # (g) an already-excluded region
        d = json.loads(ex_bak)
        d["excusals"][0]["region"] = "current_game_time"
        json.dump(d, open(exc, "w", encoding="utf-8"))
        ok &= run("an excusal on an excluded region goes RED", True)
        open(exc, "w", encoding="utf-8").write(ex_bak)
        ok &= run("restored excusals are green again", False)
        print("lint_fixture_currency --selftest: %s" % ("PASS" if ok else "FAIL"))
        return 0 if ok else 1
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args()
    if args.selftest:
        return selftest()
    cur, errs = check()
    return report(cur, errs)


if __name__ == "__main__":
    sys.exit(main())
