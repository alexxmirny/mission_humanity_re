#!/usr/bin/env python3
"""lint_hash_epoch.py -- a change to what bytes reach the state hash needs an epoch decision (TL-GATE8).

HASH FINGERPRINT pins the sink algorithm and hash_manifest_fp pins region membership; neither sees a
change to the BYTES FED IN for an unchanged world (an emit view, a mask, a harness write into a hashed
region before the hash -- dead-ends G177). region_view.h's hand-bumped HASH_INPUT_EPOCH covers that
class, and this lint ties it to the code that defines it.

tools/data/hash_input_epoch.json lists the watched code -- whole files, or `// HASH-INPUT BEGIN <id>` /
`// HASH-INPUT END <id>` spans -- each with a reason and the sha256 of its content (CRLF-normalised,
marker lines excluded) recorded at the last epoch decision. RED when:
  * a watched file/span's content differs from its recorded sha256 (an undecided change);
  * the header's HASH_INPUT_EPOCH differs from the recorded epoch (a bump not rebaselined);
  * a span's markers are missing, duplicated or out of order, or a marker id is not in the data.

The decision is recorded with --rebaseline: after a header bump (E -> E+1, and every stamped artifact
re-captured -- lint_fixture_currency reds until they are) it is a `bump`; with no bump it needs
--neutral (the change does not alter hashed bytes -- a comment, a rename, a refactor) and a --reason.
Pure file reads: runs in CI from a checkout alone.
"""

from __future__ import annotations

import argparse
import datetime
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DATA_REL = os.path.join("tools", "data", "hash_input_epoch.json")
SRC_REL = os.path.join("src", "mh_dll")
EPOCH_RE = re.compile(r"inline constexpr uint32_t HASH_INPUT_EPOCH\s*=\s*(\d+)u?\s*;")
BEGIN_RE = re.compile(r"HASH-INPUT BEGIN (\S+)")
END_RE = re.compile(r"HASH-INPUT END (\S+)")


def _read(root, rel):
    with open(os.path.join(root, rel), "rb") as f:
        return f.read().decode("utf-8").replace("\r\n", "\n")


def content(root, entry):
    """The watched text of one entry, or raise ValueError naming what is wrong with its markers."""
    text = _read(root, entry["path"])
    sid = entry.get("span")
    if not sid:
        return text
    lines = text.split("\n")
    b = [i for i, ln in enumerate(lines) if (m := BEGIN_RE.search(ln)) and m.group(1) == sid]
    e = [i for i, ln in enumerate(lines) if (m := END_RE.search(ln)) and m.group(1) == sid]
    if len(b) != 1 or len(e) != 1 or e[0] <= b[0]:
        raise ValueError(
            "span %s in %s: %d BEGIN / %d END marker(s) -- need exactly one of each, BEGIN first"
            % (sid, entry["path"], len(b), len(e))
        )
    return "\n".join(lines[b[0] + 1 : e[0]])


def sha(text):
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def header_epoch(root, data):
    m = EPOCH_RE.search(_read(root, data["header"]))
    return int(m.group(1)) if m else None


def source_files(root):
    """Every .cpp/.h under src/mh_dll -- tracked files in a checkout, a walk elsewhere (selftest)."""
    try:
        out = subprocess.run(
            ["git", "-C", root, "ls-files", "--", SRC_REL.replace("\\", "/")],
            capture_output=True,
            text=True,
            check=True,
        ).stdout.split()
        if out and root == REPO:
            return [p for p in out if p.endswith((".cpp", ".h"))]
    except (OSError, subprocess.CalledProcessError):
        pass
    got = []
    for d, _dirs, files in os.walk(os.path.join(root, SRC_REL)):
        for fn in files:
            if fn.endswith((".cpp", ".h")):
                got.append(os.path.relpath(os.path.join(d, fn), root).replace("\\", "/"))
    return got


def check(root=REPO, data_path=None):
    """-> (data, errs, changed_ids). `changed_ids` are entries whose content moved."""
    data_path = data_path or os.path.join(root, DATA_REL)
    data = json.load(open(data_path, encoding="utf-8"))
    errs, changed = [], []
    he = header_epoch(root, data)
    if he is None:
        errs.append("%s has no HASH_INPUT_EPOCH constant" % data["header"])
    elif he != data["epoch"]:
        errs.append(
            "HASH_INPUT_EPOCH is %d in %s but %s records %d -- a bump is not rebaselined: re-capture "
            "the stamped artifacts, then `python tools/lint_hash_epoch.py --rebaseline --reason ...`"
            % (he, data["header"], DATA_REL.replace("\\", "/"), data["epoch"])
        )
    ids = set()
    for e in data["watched"]:
        ids.add(e.get("span") or "")
        if not e.get("reason"):
            errs.append("watched %s: no `reason`" % e["id"])
        try:
            got = sha(content(root, e))
        except (OSError, ValueError) as ex:
            errs.append("watched %s: %s" % (e["id"], ex))
            continue
        if got != e.get("sha256"):
            changed.append(e["id"])
    if changed:
        errs.append(
            "hashed-input code changed with no epoch decision: %s. If the bytes fed to the hash "
            "for an unchanged world changed, bump HASH_INPUT_EPOCH in %s, re-capture every stamped "
            "artifact, then `python tools/lint_hash_epoch.py --rebaseline --reason '...'`; if they "
            "did not, `--rebaseline --neutral --reason '...'`."
            % (", ".join(changed), data["header"])
        )
    for rel in source_files(root):
        try:
            text = _read(root, rel)
        except (OSError, UnicodeDecodeError):
            continue
        for m in BEGIN_RE.finditer(text):
            if m.group(1) not in ids:
                errs.append(
                    "%s: span %s is marked in the source but not watched in %s"
                    % (rel, m.group(1), DATA_REL.replace("\\", "/"))
                )
    return data, errs, changed


def rebaseline(root, reason, neutral, data_path=None):
    data_path = data_path or os.path.join(root, DATA_REL)
    data, errs, changed = check(root, data_path)
    he = header_epoch(root, data)
    if not reason:
        return "REFUSED: --rebaseline needs --reason"
    if he == data["epoch"] + 1:
        kind = "bump"
    elif he == data["epoch"]:
        if not changed:
            return "nothing to rebaseline: epoch %d and every watched sha256 already match" % he
        if not neutral:
            return (
                "REFUSED: %s changed but HASH_INPUT_EPOCH is still %d. Bump it if the hashed bytes "
                "changed; pass --neutral if they did not." % (", ".join(changed), he)
            )
        kind = "neutral"
    else:
        return "REFUSED: header epoch %s is neither %d nor %d" % (
            he,
            data["epoch"],
            data["epoch"] + 1,
        )
    for e in data["watched"]:
        e["sha256"] = sha(content(root, e))
    data["epoch"] = he
    data["history"].append(
        {
            "epoch": he,
            "kind": kind,
            "date": datetime.date.today().isoformat(),
            "changed": changed,
            "reason": reason,
        }
    )
    with open(data_path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(data, f, indent=1, ensure_ascii=False)
        f.write("\n")
    return "rebaselined: epoch %d (%s), %d watched entr(ies)" % (he, kind, len(data["watched"]))


def selftest():
    tmp = tempfile.mkdtemp(prefix="lint_hash_epoch_")
    try:
        data = json.load(open(os.path.join(REPO, DATA_REL), encoding="utf-8"))
        paths = {e["path"] for e in data["watched"]} | {data["header"]}
        for rel in paths:
            os.makedirs(os.path.dirname(os.path.join(tmp, rel)), exist_ok=True)
            shutil.copy(os.path.join(REPO, rel), os.path.join(tmp, rel))
        dp = os.path.join(tmp, DATA_REL)
        os.makedirs(os.path.dirname(dp), exist_ok=True)
        # Normalise the copy's recorded shas: the selftest proves the mechanism, not the tree's state.
        for e in data["watched"]:
            e["sha256"] = sha(content(tmp, e))
        data["epoch"] = header_epoch(tmp, data)
        json.dump(data, open(dp, "w", encoding="utf-8"))
        pristine = {rel: open(os.path.join(tmp, rel), "rb").read() for rel in paths}
        bad = 0

        def restore():
            for rel, b in pristine.items():
                open(os.path.join(tmp, rel), "wb").write(b)
            json.dump(data, open(dp, "w", encoding="utf-8"))

        def case(label, want_red):
            nonlocal bad
            _d, errs, _c = check(tmp, dp)
            ok = bool(errs) == want_red
            bad += not ok
            print("  %-4s %s%s" % ("ok" if ok else "FAIL", label, "" if ok else " (%s)" % errs[:1]))

        def edit(rel, fn):
            p = os.path.join(tmp, rel)
            t = open(p, encoding="utf-8", newline="").read()
            open(p, "w", encoding="utf-8", newline="").write(fn(t))

        whole = next(e for e in data["watched"] if not e.get("span"))
        span = next(e for e in data["watched"] if e.get("span"))
        case("clean copy is green", False)
        edit(whole["path"], lambda t: t + "\n// planted\n")
        case("an edit to a whole-file entry goes RED", True)
        restore()
        edit(
            span["path"],
            lambda t: t.replace(
                "HASH-INPUT END %s" % span["span"],
                "planted();\n// HASH-INPUT END %s" % span["span"],
                1,
            ),
        )
        case("an edit INSIDE a span goes RED", True)
        restore()
        edit(span["path"], lambda t: "// planted outside every span\n" + t)
        case(
            "an edit OUTSIDE the span is green",
            any(not e.get("span") and e["path"] == span["path"] for e in data["watched"]),
        )
        restore()
        edit(
            data["header"],
            lambda t: EPOCH_RE.sub(
                lambda m: m.group(0).replace(m.group(1), str(int(m.group(1)) + 1)), t
            ),
        )
        case("a header bump that is not rebaselined goes RED", True)
        msg = rebaseline(tmp, "selftest bump", False, dp)
        case("... and green once rebaselined as a bump (%s)" % msg.split(":")[0], False)
        restore()
        edit(
            span["path"],
            lambda t: t.replace("HASH-INPUT END %s" % span["span"], "HASH-INPUT-GONE", 1),
        )
        case("a span with no END marker goes RED", True)
        restore()
        edit(
            span["path"],
            lambda t: "// HASH-INPUT BEGIN unwatched_span\n// HASH-INPUT END unwatched_span\n" + t,
        )
        case("a marked span missing from the data goes RED", True)
        restore()
        edit(whole["path"], lambda t: t + "\n// comment only\n")
        refused = rebaseline(tmp, "comment", False, dp).startswith("REFUSED")
        case("an unbumped change is REFUSED a plain rebaseline (still red)", True)
        bad += not refused
        rebaseline(tmp, "comment only", True, dp)
        case("... and green after --neutral", False)
        restore()
        print("lint_hash_epoch --selftest: %s" % ("PASS" if not bad else "%d FAILED" % bad))
        return 1 if bad else 0
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--rebaseline", action="store_true", help="record the current epoch decision")
    ap.add_argument("--neutral", action="store_true", help="the change does not alter hashed bytes")
    ap.add_argument("--reason", default="")
    args = ap.parse_args()
    if args.selftest:
        return selftest()
    if args.rebaseline:
        msg = rebaseline(REPO, args.reason, args.neutral)
        print(msg)
        return 1 if msg.startswith("REFUSED") else 0
    data, errs, _c = check()
    print("hash-input epoch %s: %d watched entr(ies)" % (data["epoch"], len(data["watched"])))
    for e in errs:
        print("  RED: %s" % e)
    print("  %s" % ("PASS" if not errs else "%d problem(s)" % len(errs)))
    return 1 if errs else 0


if __name__ == "__main__":
    sys.exit(main())
