"""lint_encoding.py -- catch UTF-8 text that has been re-decoded through a legacy codepage.

THE BUG THIS EXISTS FOR. The reimpl tracker YAML accumulated 181 runs of mojibake: every em dash had
become a seven-character Cyrillic run (U+0420 U+0406 U+0420 U+201A U+0432 U+0402 U+045C), middle dots
and section signs and the box-drawing banners likewise, and its generated view inherited all of it
because the tracker CLI copies the strings faithfully. Found 2026-07-31 by the user reading the generated
view.

(Codepoints, not the characters themselves: pasting a real sample into this file would make the
check flag its own documentation -- which it did, on the first run.)

HOW IT HAPPENS, and it is silent. This box's locale codepage is cp1251. A script that reads a UTF-8
file with the LOCALE default and writes it back as UTF-8 --

    open(path).read()                      # cp1251 here: 'E2 80 94' becomes three characters
    open(path, 'w', encoding='utf-8')      # ...which are then encoded as three characters

-- adds one layer of damage per run, invisibly, with no exception. Reading and writing with the same
wrong codec round-trips clean, which is why this survives casual testing; it is the MIXTURE that
corrupts. The tracker had text at BOTH one and two layers deep, i.e. it happened more than once.
The tracker CLI itself is innocent -- it passes `encoding="utf-8"` on both sides.

THE RULE: pass `encoding="utf-8"` explicitly on every text read and write in this repo. Never rely on
the locale default.

WHY THE CHECK IS SAFE. Re-encoding text through cp1251 and decoding it as UTF-8 succeeds only if the
bytes really were a UTF-8 sequence that had been mis-decoded: a CLEAN em dash is one cp1251 byte
(0x97), which is not valid UTF-8, so it raises and the run is left alone. On top of that a run is only
reported when the peeled result lands inside the punctuation whitelist -- so genuine non-Latin text
(a Cyrillic or Polish game string quoted in a doc) is never flagged.

  python tools/lint_encoding.py           # gate (tools/lint_repo.py)
  python tools/lint_encoding.py --fix     # repair in place, preserving each file's newlines
"""

import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SKIP_DIRS = {".git", "idata", "tmp", "Release", "Debug", ".vs", "node_modules", "__pycache__"}
EXTS = (".md", ".yaml", ".yml", ".py", ".json", ".h", ".cpp", ".c", ".txt", ".ini")
RUN = re.compile(r"[^\x00-\x7F]+")

# What a REPAIRED run may contain -- the punctuation this repo's prose actually uses. A peel whose
# result falls outside this is reported as suspicious rather than rewritten.
ALLOWED = set("—–‘’“”…→←·§×° ─│┌┐└┘├┤┬┴┼Σ≤≥≠")


def peel(s, rounds=4):
    """Undo 'UTF-8 bytes were decoded as cp1251', repeatedly. Returns (result, layers_removed)."""
    cur, n = s, 0
    for _ in range(rounds):
        try:
            nxt = cur.encode("cp1251").decode("utf-8")
        except Exception:
            break
        if nxt == cur:
            break
        cur, n = nxt, n + 1
    return cur, n


def scan(text):
    """Yield (run, repaired, layers) for every mis-decoded run."""
    for m in RUN.finditer(text):
        s = m.group(0)
        out, n = peel(s)
        if n and out != s and all(ord(c) < 128 or c in ALLOWED for c in out):
            yield s, out, n


def files():
    for root, dirs, names in os.walk(REPO):
        dirs[:] = [d for d in dirs if d not in SKIP_DIRS]
        for n in names:
            if n.endswith(EXTS):
                yield os.path.join(root, n)


def main():
    fix = "--fix" in sys.argv
    bad = 0
    for p in sorted(files()):
        try:
            raw = open(p, "rb").read()
            text = raw.decode("utf-8")
        except Exception:
            continue  # a non-UTF-8 file is a different problem; this check is about mis-decoding
        hits = list(scan(text))
        if not hits:
            continue
        rel = os.path.relpath(p, REPO)
        bad += len(hits)
        if fix:
            out = RUN.sub(lambda m: next((r for s, r, _ in scan(m.group(0))), m.group(0)), text)
            nl = b"\r\n" if b"\r\n" in raw else b"\n"
            data = out.encode("utf-8")
            if nl == b"\r\n":
                data = data.replace(b"\r\n", b"\n").replace(b"\n", b"\r\n")
            open(p, "wb").write(data)
            print("fixed %-46s %d run(s)" % (rel, len(hits)))
        else:
            print(
                "MOJIBAKE %-42s %d run(s), e.g. %r -> %r (%d layer(s))"
                % (rel, len(hits), hits[0][0][:16], hits[0][1][:16], hits[0][2])
            )

    if bad and not fix:
        print()
        print(
            "%d mis-decoded run(s). A UTF-8 file was read with the locale codepage and rewritten."
            % bad
        )
        print(
            "Repair with: python tools/lint_encoding.py --fix   (then regenerate any derived view)"
        )
        print("Prevent: pass encoding='utf-8' explicitly on EVERY text open() in this repo.")
        return 1
    if not bad:
        print("ok: no mis-decoded text")
    return 0


if __name__ == "__main__":
    sys.exit(main())
