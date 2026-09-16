#!/usr/bin/env python3
r"""Guard the tree against machine-specific paths and operator identity leaking into what we publish.

THREE TIERS, and they answer two different questions.

  CLEAN     (E2b, unchanged) -- files that must be FULLY machine-path-free because they route through
            machine_config / __file__: everything under src/formats/*.py, plus
            the register-preservation classifier. Question: "did the E2b cleanup rot?"
  FALLBACK  (E2b, unchanged) -- the DLL build config, where a machine path is allowed ONLY on its
            documented default/fallback line (an overridable property, a vswhere fallback, an
            env-overridable default). Any OTHER machine path in those files is a regression.
  PUBLISHED (fork F5B, new)  -- EVERY path in the publish set (tools/data/publish_ledger.json via
            tools/check_publishable.py --publish-list), scanned for the four exposure classes the
            fork measurement found. Question: "would cutting the public repo today leak the
            operator?" That is a different question from the first two, which is why the classes and
            the allowances differ rather than being one widened regex.

---- WHY THE PUBLISHED TIER SCANS WHAT IT SCANS -------------------------------------------------

The F5 surface measurement found four classes in the tree, and each one is here in its MECHANICAL
form rather than as the literals it found -- deliberately, because a lint that hardcodes the strings
it is scrubbing publishes them itself, and because a literal only catches the exposure we already
know about:

  1. A PUBLIC IPv4 literal anywhere in a published file's BYTES. The measured exposure was one
     personal relay host in two scripts AND INSIDE A BINARY (tools/uiscripts/setup.seed.dat's MRU
     list) -- which is why this arm reads raw bytes, not decoded text. Generic beats literal here:
     a future relay is caught too. Private/loopback/link-local/CGNAT and the RFC 5737 + RFC 2544
     documentation ranges are not public and never fire. Measured over the 2,343-file publish set:
     five hits, four of them prose (declared below), zero false positives from PNG/save/journal
     bytes.
  2. A USER-PROFILE path (`<drive>:\Users\<name>`), also over bytes. The mechanical form of a
     username leak, and the one identity check that works on any machine.
  3. An ABSOLUTE PATH ON A NON-C: DRIVE outside machine_config's committed defaults, over text.
     C: is excluded on purpose: `C:\Program Files\...`, `C:\Windows` and the VM's own `C:\games\mh`
     are OS/remote locations, not this operator's disk layout. A drive letter the machine chose IS.
  4. IDENTITY TOKENS, DERIVED AT RUNTIME -- `git config user.name` / `user.email` plus %USERNAME%,
     each split into its parts, minus a generic denylist (user/admin/root/... would match half the
     tree). Anything those do not spell -- a handle, an old account -- goes in
     machine_config.IDENTITY_TOKENS, whose COMMITTED default is empty and must stay empty; the real
     tokens belong in the gitignored tools/machine.local.json. This arm is therefore best-effort by
     construction and SAYS SO IN ITS OUTPUT (it prints how many tokens it derived) -- a silent zero
     and a working scan must not look alike.

LAN IPs (192.168/10/172.16) are deliberately NOT a PUBLISHED-tier class. A private-range address is
meaningless outside the LAN it names: it is neither identity nor disk layout, machine_config already
declares the rig's five, and the only two residual sites in the publish set are a comment and an
argv-parsing fixture. They remain the CLEAN/FALLBACK tiers' business, where the question is
portability rather than publication.

---- THE FOUR ALLOWANCES, ALL DECLARED, ALL FALSIFIABLE -----------------------------------------

  A. machine_config's COMMITTED defaults. `F:/games/...`, `F:\apps\ghidra_12.1.2_PUBLIC`,
     `C:\games\mh` and the rest are the documented, overridable shape of a dev box -- the config's
     own values cannot be a violation, or the lint reds the thing it tells you to use. Prefix match,
     so `F:/games/mh_en/save/11.sav` rides `F:/games`. Read from committed_defaults(), NOT the
     resolved values: a machine.local.json entry must not be able to legitimise a committed
     hardcode. machine_config.py itself is exempt -- it is the home of the values.
  B. THE GHIDRA RUN-SCRIPT FALLBACK. ~38 pyghidra tools end a real derivation (the project locator,
     then ascend to the repo marker) with `return _projdir or r"<repo>"` / an `except:` assignment.
     The literal is reached only when the Ghidra API is not there at all, and it is the same shape
     the FALLBACK tier already blesses in the build config. Recognised STRUCTURALLY -- the file
     carries `getProjectLocator` AND the line is one of those two shapes -- so it cannot be claimed
     by an ordinary tool with a hardcoded path, and no literal is embedded here.
  C. SITE_EXCEPTIONS below: (path, needle, why), three of them, all prose or documentation
     addresses. A site exception that no longer matches anything FAILS -- the same
     stale-exception arm check_publishable.py's --closure has, and for the same reason: an excuse
     must not outlive the thing it excused.
  D. THE COPYRIGHT-HOLDER LINE OF THE REPO'S OWN `LICENSE` (fork F5F). The identity arm derives its
     tokens from the operator's git identity and the gitignored IDENTITY_TOKENS list, and the public
     identity the user ruled as the copyright holder is IN that list on the authoring box -- so the
     one line whose whole job is to name the holder reds. Scrubbing it would not remove an exposure,
     it would falsify the licence. Recognised STRUCTURALLY and as narrowly as the fact allows: the
     file must be the repo-root `LICENSE`, and the excused line must be the FIRST line matching
     `Copyright (c) <year>[-<year>] <holder>`. Exactly one line, in exactly one file; the same token
     anywhere else in LICENSE, and everywhere in every other published file, still reds. The other
     three classes (host, user-profile path, drive path) are NOT excused here -- a licence has no
     business carrying any of them.

Hermetic apart from `git ls-files` (via check_publishable) and `git config` (the identity tokens).

  python tools/lint_machine_paths.py             # check; exit 1 on a violation
  python tools/lint_machine_paths.py --selftest  # assert every arm still fires on a planted line
"""

import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "tools"))

import check_publishable  # noqa: E402
import machine_config  # noqa: E402

# ─────────────────────────────── tiers 1 + 2 (E2b, unchanged) ───────────────────────────────

# A machine-specific path/host literal: F:\games / F:/apps / C:\games, a LAN IP, HyperV, or a
# hardcoded Visual Studio install root. Deliberately NOT matching a bare OS drive path (C:\Windows,
# Program Files (x86)) -- those are OS defaults, not machine-specific.
PATTERN = re.compile(
    r"[FCD]:[\\/](?:games|apps|HyperV)"
    r"|(?:192\.168|10\.0\.0|172\.16)\.\d"
    r"|Program Files[\\/]+Microsoft Visual Studio",
    re.IGNORECASE,
)

# Files that must be COMPLETELY free of machine paths (they derive/route instead).
#
# THE SECOND ENTRY IS DISCOVERED, NOT NAMED (fork F5M S4b). It is the register-preservation
# classifier, which belongs to the Ghidra pipeline -- archive-class tooling a public tree does not
# carry -- so a literal path here would be a rule about a file that may not exist. Globbing keeps
# the rule true in both trees: where the classifier is present it must stay clean (it is the file
# the E2b cleanup made clean, and this is what stops it rotting back), and where it is absent the
# list is simply shorter. A glob that matches nothing is not a silent pass either: the CLEAN tier
# is one of three, and the tier-2/3 scans below still cover the whole publish set.
CLEAN_FILES = sorted(
    p for p in (REPO / "src" / "formats").rglob("*.py") if "__pycache__" not in p.parts
) + sorted((REPO / "tools").glob("classify_reg_preserv*.py"))

# Build-config files: a machine path is allowed ONLY on a line containing its documented marker.
FALLBACK_FILES = {
    REPO / "src" / "mh_dll" / "mh" / "mh.vcxproj": ("MhDeployDir Condition",),
    REPO / "src" / "mh_dll" / "mh_nettest" / "build_selftest.bat": ('if "%VSDIR%"=="" set VSDIR=',),
    REPO / "src" / "mh_dll" / "mh_nettest" / "det_ab_test.ps1": ("$env:MH_DET_DIR",),
}

# ─────────────────────────────── tier 3 (fork F5B) ───────────────────────────────

QUAD_RE = re.compile(rb"(?<![\w.])(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})(?![\w.])")
USERDIR_RE = re.compile(rb"[A-Za-z]:[\\/]Users[\\/](?![<%$])[A-Za-z0-9_.-]+")

# An absolute path on a drive OTHER than C:. Each segment must be >= 2 chars and not start with a
# dot, which is what keeps `"...\n"`-style escapes and elision placeholders (`F:\...`) out: measured,
# those were the only false positives a naive drive-letter regex produced over the publish set.
DRIVEPATH_RE = re.compile(
    r"(?<![\w:])(?![Cc]:)[A-Za-z]:[\\/]"
    r"[A-Za-z0-9_+()-][A-Za-z0-9_.+()-]+"
    r"(?:[\\/][A-Za-z0-9_+()-][A-Za-z0-9_.+()-]+)*"
)

# Identity tokens that are far too generic to scan for -- every one of these is a real %USERNAME% or
# git name somewhere, and every one of them appears hundreds of times in ordinary English prose.
GENERIC_IDENTITY = {
    "user",
    "users",
    "admin",
    "administrator",
    "root",
    "guest",
    "default",
    "owner",
    "build",
    "builder",
    "runner",
    "test",
    "tester",
    "dev",
    "vmadmin",
    "github",
    "gmail",
    "outlook",
    "example",
    "local",
    "com",
    "net",
    "org",
}

# (path, needle, why) -- a declared, dead-checked exception for one site. Keep the list tiny: an
# exception is a statement that the class is right and this ONE occurrence is not an exposure.
SITE_EXCEPTIONS = [
    (
        "tools/crash_report.py",
        "11.0.0.0",
        "A verbatim Windows Error Reporting sample in a docstring; `version: 11.0.0.0` is notepad's "
        "file version, not an address -- the parser is written against this exact text.",
    ),
    (
        "tools/ui_test.py",
        "1.2.3.4",
        "The `--host <ip>:<script>` grammar's documentation example, in the docstring that explains "
        "why a dotted quad is what disambiguates a peer spec from a drive path.",
    ),
    (
        "tools/ui_test.py",
        "8.8.8.8",
        "A public DNS address used as an UNCONNECTED UDP route probe to learn this box's own "
        "outbound LAN address. Nothing is sent; it is a routing-table question, not a host.",
    ),
]


def _norm(s):
    return s.replace("\\", "/").lower().rstrip("/")


def default_path_prefixes():
    """machine_config's committed defaults that are absolute paths, normalised for prefix matching."""
    return sorted(
        {
            _norm(v)
            for v in machine_config.committed_defaults().values()
            if isinstance(v, str) and re.match(r"^[A-Za-z]:[\\/]", v)
        }
    )


def identity_tokens():
    """Derived, never committed. See the module docstring, class 4."""
    raw = []
    for key in ("user.name", "user.email"):
        try:
            out = subprocess.run(
                ["git", "-C", str(REPO), "config", "--get", key],
                capture_output=True,
                text=True,
            ).stdout.strip()
        except OSError:
            out = ""
        if out:
            raw.append(out)
            raw.extend(re.split(r"[@.\s_+-]+", out))
    raw.append(os.environ.get("USERNAME") or os.environ.get("USER") or "")
    extra = machine_config.committed_defaults().get("IDENTITY_TOKENS", "")
    extra = os.environ.get("MH_IDENTITY_TOKENS", extra)
    try:
        import json

        local = REPO / "tools" / "machine.local.json"
        if local.is_file():
            extra = json.loads(local.read_text(encoding="utf-8")).get("IDENTITY_TOKENS", extra)
    except (ValueError, OSError):
        pass
    raw.extend(t for t in re.split(r"[,\s]+", extra or "") if t)
    toks = set()
    for t in raw:
        t = t.strip().lower()
        if len(t) >= 5 and t not in GENERIC_IDENTITY:
            toks.add(t)
    return sorted(toks)


def is_public_ipv4(a, b, c, d):
    """Public = not private, loopback, link-local, CGNAT, multicast/reserved, or a documentation
    range (RFC 5737's three TEST-NETs, RFC 2544's benchmark block)."""
    if max(a, b, c, d) > 255:
        return False
    if a in (0, 10, 127) or a >= 224:
        return False
    if a == 192 and b == 168:
        return False
    if a == 172 and 16 <= b <= 31:
        return False
    if a == 169 and b == 254:
        return False
    if a == 100 and 64 <= b <= 127:
        return False
    if a == 192 and b == 0 and c in (0, 2):
        return False
    if a == 198 and b == 51 and c == 100:
        return False
    if a == 203 and b == 0 and c == 113:
        return False
    if a == 198 and b in (18, 19):
        return False
    return True


LICENSE_PATH = "LICENSE"
COPYRIGHT_RE = re.compile(r"^Copyright \(c\) \d{4}(?:-\d{4})? +\S")


def _license_holder_line(relpath, blob):
    """Allowance D: the 1-based line number of LICENSE's copyright-holder line, or 0.

    Structural: the path must be the repo-root LICENSE and the line must be the FIRST one matching
    the copyright shape. Returns 0 for every other file, so no other file can claim the allowance."""
    if relpath != LICENSE_PATH:
        return 0
    try:
        text = blob.decode("utf-8")
    except UnicodeDecodeError:
        return 0
    for i, line in enumerate(text.splitlines(), 1):
        if COPYRIGHT_RE.match(line):
            return i
    return 0


def _ghidra_fallback_lines(text):
    """Line numbers (1-based) carrying the documented last-resort repo-root fallback: allowance B.

    Structural, not value-based -- the file must carry the Ghidra-API derivation AND the line must be
    one of the two shapes that derivation ends with."""
    if "getProjectLocator" not in text:
        return set()
    lines = text.splitlines()
    out = set()
    for i, line in enumerate(lines):
        if " or r" in line:
            out.add(i + 1)
            continue
        j = i - 1
        while j >= 0 and not lines[j].strip():
            j -= 1
        if j >= 0 and lines[j].strip().startswith("except"):
            out.add(i + 1)
    return out


# ─────────────────────────────── scanners ───────────────────────────────


def scan_clean(relpath, text):
    """A CLEAN file: every machine-path hit is a violation."""
    out = []
    for i, line in enumerate(text.splitlines(), 1):
        if PATTERN.search(line):
            out.append((relpath, i, line.strip()[:100]))
    return out


def scan_fallback(relpath, text, allowed_markers):
    """A FALLBACK file: a machine-path hit is a violation unless its line carries an allowed marker."""
    out = []
    for i, line in enumerate(text.splitlines(), 1):
        if PATTERN.search(line) and not any(m in line for m in allowed_markers):
            out.append((relpath, i, line.strip()[:100]))
    return out


def scan_published(relpath, blob, prefixes, tokens, exceptions):
    """A PUBLISHED file: the four exposure classes.

    `blob` is RAW BYTES. Classes 1/2/4 read them directly (the measured exposure lived inside a
    binary); class 3 needs line context, so it runs only when the bytes decode as UTF-8.
    Returns (violations, exception_hits) -- exception_hits keyed by (path, needle)."""
    out = []
    hits = {}
    exc_here = [(n, w) for p, n, w in exceptions if p == relpath]

    def excused(s):
        """Does a declared site exception cover this hit? Records the hit either way, because the
        dead-exception arm needs to know an exception still reproduces, not just that it fired."""
        for needle, _why in exc_here:
            if needle in s or s in needle:
                hits[(relpath, needle)] = hits.get((relpath, needle), 0) + 1
                return True
        return False

    def lineno(off):
        return blob.count(b"\n", 0, off) + 1

    for m in QUAD_RE.finditer(blob):
        quad = tuple(int(x) for x in m.groups())
        if not is_public_ipv4(*quad):
            continue
        s = m.group(0).decode("latin1")
        if excused(s):
            continue
        out.append((relpath, lineno(m.start()), "public-ipv4", s))

    for m in USERDIR_RE.finditer(blob):
        s = m.group(0).decode("latin1")
        if excused(s):
            continue
        out.append((relpath, lineno(m.start()), "user-profile-path", s))

    if tokens:
        low = blob.lower()
        holder_line = _license_holder_line(relpath, blob)  # allowance D; 0 for every other file
        for t in tokens:
            start = 0
            tb = t.encode("latin1", "replace")
            while True:
                k = low.find(tb, start)
                if k < 0:
                    break
                start = k + 1
                i = lineno(k)
                if holder_line and i == holder_line:
                    continue
                if excused(t):
                    continue
                out.append((relpath, i, "identity", t))

    try:
        text = blob.decode("utf-8")
    except UnicodeDecodeError:
        return out, hits

    fallback = _ghidra_fallback_lines(text)
    for i, line in enumerate(text.splitlines(), 1):
        if i in fallback:
            continue
        for m in DRIVEPATH_RE.finditer(line):
            tok = m.group(0)
            n = _norm(tok)
            if any(n.startswith(p) for p in prefixes):
                continue
            if excused(tok):
                continue
            out.append((relpath, i, "machine-drive-path", tok[:70]))
    return out, hits


SELF_EXEMPT = ("tools/machine_config.py", "tools/lint_machine_paths.py")


def published_paths():
    r"""The publish set, minus what the CLEAN/FALLBACK tiers already own, minus SELF_EXEMPT.

    TWO FILES ARE EXEMPT FROM THE PUBLISHED TIER, BOTH FOR THE SAME REASON: they are the home of the
    values, not users of them. machine_config.py holds the committed defaults every other file is
    told to route through. THIS file holds the planted-violation corpus --selftest fires the arms on
    (a relay-shaped address, a C:\Users path, a bare hardcode) plus the documented examples in the
    module docstring; a scanner that reds on its own test data cannot have test data. Neither is a
    blind spot in practice: machine_config's contents are printed by `machine_config.py --json`, and
    every literal here is either a planted input to a function under test or prose about the classes
    -- nothing a public cut would carry as configuration."""
    led = check_publishable.load_ledger()
    sel = check_publishable.publish_list(check_publishable.tracked_paths(), led)
    owned = {
        str(p.relative_to(REPO)).replace("\\", "/")
        for p in list(CLEAN_FILES) + list(FALLBACK_FILES)
    }
    owned.update(SELF_EXEMPT)
    return [p for p in sel if p not in owned]


# ─────────────────────────────── check ───────────────────────────────


def check(say=print):
    violations = []
    for p in CLEAN_FILES:
        if p.is_file():
            violations += [
                (str(p.relative_to(REPO)), i, "clean", line)
                for _r, i, line in scan_clean(
                    str(p.relative_to(REPO)), p.read_text(encoding="utf-8", errors="replace")
                )
            ]
    for p, markers in FALLBACK_FILES.items():
        if p.is_file():
            violations += [
                (str(p.relative_to(REPO)), i, "fallback", line)
                for _r, i, line in scan_fallback(
                    str(p.relative_to(REPO)),
                    p.read_text(encoding="utf-8", errors="replace"),
                    markers,
                )
            ]
    prefixes = default_path_prefixes()
    tokens = identity_tokens()
    pub = published_paths()
    exc_hits = {}
    scanned = 0
    for rel in pub:
        full = REPO / rel.replace("/", os.sep)
        if not full.is_file():
            continue
        scanned += 1
        v, h = scan_published(rel, full.read_bytes(), prefixes, tokens, SITE_EXCEPTIONS)
        violations += v
        for k, n in h.items():
            exc_hits[k] = exc_hits.get(k, 0) + n

    ok = not violations

    if scanned == 0:
        say("lint_machine_paths: FAIL -- the published tier scanned NO file.")
        say("     A scan that finds nothing and a scan that is broken look identical.")
        return 1

    dead = [(p, n) for p, n, _w in SITE_EXCEPTIONS if (p, n) not in exc_hits]
    if dead:
        ok = False

    if violations:
        say(
            "lint_machine_paths: FAIL -- machine-specific path(s) / identity where they should not be:",
            file=sys.stderr,
        )
        for rel, i, kind, what in violations:
            say(f"  {rel}:{i}: [{kind}] {what}", file=sys.stderr)
        say(
            "  Route the value through tools/machine_config.py (or derive from __file__); the DLL build\n"
            "  config allows a machine path only on its documented default/fallback line; a published\n"
            "  file may carry neither a public host, a user-profile path, a non-C: absolute path outside\n"
            "  machine_config's committed defaults, nor an identity literal.",
            file=sys.stderr,
        )
    if dead:
        say(
            "lint_machine_paths: FAIL -- %d declared site exception(s) match nothing any more:"
            % len(dead),
            file=sys.stderr,
        )
        for p, n in dead:
            say(f"  {p}: {n!r} -- DELETE the row; the site it excused is gone.", file=sys.stderr)

    if ok:
        say(
            "lint_machine_paths: OK (%d clean + %d build-config + %d published file(s); "
            "%d path default(s), %d identity token(s), %d site exception(s))"
            % (
                len(CLEAN_FILES),
                len(FALLBACK_FILES),
                scanned,
                len(prefixes),
                len(tokens),
                len(SITE_EXCEPTIONS),
            )
        )
        if not tokens:
            say(
                "  note: 0 identity tokens derived (no git user.name/user.email, no "
                "IDENTITY_TOKENS) -- the identity arm proved nothing this run."
            )
    return 0 if ok else 1


# ─────────────────────────────── selftest ───────────────────────────────


def selftest():
    """Every arm re-fired against a planted line, and every allowance re-fired against a clean one."""
    ok = True
    results = []

    def expect(name, cond):
        nonlocal ok
        results.append((name, bool(cond)))
        if not cond:
            ok = False

    # ---- tiers 1 + 2, unchanged ------------------------------------------------------------
    expect(
        "CLEAN: flags a hardcoded F:/games path",
        scan_clean("x.py", 'DEFAULT = "F:/games/mh_clean/res_unpack"\n'),
    )
    expect(
        "CLEAN: does not flag a machine_config reference",
        not scan_clean("x.py", "DEFAULT = machine.RES_UNPACK\n"),
    )
    expect(
        "FALLBACK: the marked line is allowed",
        not scan_fallback(
            "b.bat",
            'if "%VSDIR%"=="" set VSDIR=C:\\Program Files\\Microsoft Visual Studio\\2022\\Community\n',
            ('if "%VSDIR%"=="" set VSDIR=',),
        ),
    )
    expect(
        "FALLBACK: an unmarked machine path is not",
        scan_fallback(
            "b.bat",
            "set VSDIR=C:\\Program Files\\Microsoft Visual Studio\\2022\\Community\n",
            ('if "%VSDIR%"=="" set VSDIR=',),
        ),
    )
    expect("CLEAN: a bare LAN IP is caught", scan_clean("x.py", 'HOST = "192.168.0.37"\n'))

    # ---- tier 3: the four classes ----------------------------------------------------------
    pfx = ["f:/games", "f:/apps/ghidra_12.1.2_public", "c:/games/mh"]
    toks = ["somehandle"]

    def kinds(blob, prefixes=pfx, tokens=toks, exc=(), rel="pub/x.py"):
        v, h = scan_published(rel, blob, prefixes, tokens, list(exc))
        return {k for _r, _i, k, _w in v}, v, h

    # THE PLANTED ADDRESS IS SYNTHETIC, NOT THE ONE F5B SCRUBBED. A lint whose test data is the
    # literal it exists to remove republishes that literal on every clone -- which is the same
    # mistake as writing the class as a list of known strings. 198.100.200.155 is a public address
    # nothing here has ever talked to, and it is 15 characters so the binary plant below keeps a
    # truthful length prefix.
    k, _v, _h = kinds(b'RELAY = "198.100.200.155"\n')
    expect("PUBLISHED: a public IPv4 literal is a violation", "public-ipv4" in k)
    k, _v, _h = kinds(b'PEER = "192.168.0.37"\nLOOP = "127.0.0.1"\nDOC = "192.0.2.9"\n')
    expect("PUBLISHED: private / loopback / RFC5737 addresses are not", "public-ipv4" not in k)

    # the binary arm: the measured exposure was inside a BINARY (setup.seed.dat's MRU record, whose
    # shape this blob copies -- count, byte length, NUL-terminated string), so a NUL-bearing blob
    # must be scanned rather than skipped as "not text".
    k, _v, _h = kinds(b"\x00\x01\x00\x00\x00\x10\x00\x00\x00198.100.200.155\x00host\x00")
    expect("PUBLISHED: a public IPv4 inside BINARY bytes is caught", "public-ipv4" in k)
    k, _v, _h = kinds(b"\x00\x01LZW\x00\x0c\x00\x00\x00192.168.0.37\x00host\x00")
    expect("PUBLISHED: the regenerated placeholder seed shape is clean", not k)

    k, _v, _h = kinds(rb'SPEC = r"C:\Users\someone\AppData\Local\Temp\x.json"' + b"\n")
    expect("PUBLISHED: a user-profile path is a violation", "user-profile-path" in k)
    k, _v, _h = kinds(rb'SPEC = r"%USERPROFILE%\ghidra_scripts"' + b"\n")
    expect("PUBLISHED: %USERPROFILE% is not", "user-profile-path" not in k)

    k, _v, _h = kinds(rb'OUT = r"F:\repo\mh_decompile.rep\tools\data\x.json"' + b"\n")
    expect(
        "PUBLISHED: a non-C: absolute path outside the defaults is a violation",
        "machine-drive-path" in k,
    )
    k, _v, _h = kinds(rb'POLY = "F:/games/mh_en/save/11.sav"' + b"\n")
    expect(
        "PUBLISHED: a committed machine_config default is allowed", "machine-drive-path" not in k
    )
    k, _v, _h = kinds(rb'VS = r"C:\Program Files\Microsoft Visual Studio\2022\BuildTools"' + b"\n")
    expect("PUBLISHED: a C: OS location is out of class", "machine-drive-path" not in k)
    k, _v, _h = kinds(b'MSG = "error: bad opcode:\\n"\nELIDED = "F:\\\\...\\\\x.obj"\n')
    expect("PUBLISHED: escapes and elision placeholders are not paths", not k)

    k, _v, _h = kinds(b"# contact someHandle for the key\n")
    expect("PUBLISHED: a derived identity token is a violation", "identity" in k)
    k, _v, _h = kinds(b"# contact the operator for the key\n", tokens=[])
    expect("PUBLISHED: no tokens derived -> the identity arm is silent", "identity" not in k)

    # ---- allowance B: the Ghidra run-script fallback ---------------------------------------
    ghidra_ok = (
        b"def repo_root():\n"
        b"    try:\n"
        b"        p = currentProgram.getDomainFile().getProjectLocator().getProjectDir()\n"
        b"    except Exception:\n"
        b"        p = None\n"
        b'    return p or r"F:\\repo\\mh_decompile.rep"\n'
    )
    k, _v, _h = kinds(ghidra_ok)
    expect(
        "ALLOWANCE B: the documented `or r\"...\"` fallback is allowed",
        "machine-drive-path" not in k,
    )
    ghidra_exc = (
        b"try:  # Ghidra run-script\n"
        b"    REPO = currentProgram.getDomainFile().getProjectLocator().getProjectDir()\n"
        b"except Exception:\n"
        b'    REPO = r"F:\\repo\\mh_decompile.rep"\n'
    )
    k, _v, _h = kinds(ghidra_exc)
    expect(
        "ALLOWANCE B: the documented `except:` fallback is allowed", "machine-drive-path" not in k
    )
    k, _v, _h = kinds(ghidra_ok + b'OTHER = r"F:\\scratch\\notes.txt"\n')
    expect("ALLOWANCE B: it excuses ONLY the fallback line", "machine-drive-path" in k)
    k, _v, _h = kinds(b'REPO = r"F:\\repo\\mh_decompile.rep"  # no derivation anywhere\n')
    expect(
        "ALLOWANCE B: a plain hardcode without the derivation is NOT excused",
        "machine-drive-path" in k,
    )

    # ---- allowance C: site exceptions, and the dead-exception arm --------------------------
    exc = [("pub/x.py", "8.8.8.8", "why")]
    k, _v, h = kinds(b'PROBE = ("8.8.8.8", 53)\n', exc=exc)
    expect("ALLOWANCE C: a declared site exception excuses its own site", "public-ipv4" not in k)
    expect("ALLOWANCE C: ...and records the hit, so a dead row is detectable", h)
    k, _v, h = kinds(b"nothing here\n", exc=exc)
    expect("ALLOWANCE C: a non-reproducing exception records NO hit", not h)
    k, _v, _h = kinds(b'PROBE = ("8.8.8.8", 53)\n', exc=[("other/y.py", "8.8.8.8", "why")])
    expect("ALLOWANCE C: an exception does not leak to another file", "public-ipv4" in k)

    # ---- allowance D: the LICENSE copyright-holder line (fork F5F) -------------------------
    # The token here is the selftest's synthetic handle, never a real one -- same rule as the
    # planted address above: a lint must not republish what it exists to scrub.
    lic = b"MIT License\n\nCopyright (c) 2026 somehandle\n\nPermission is hereby granted...\n"
    k, _v, _h = kinds(lic, rel=LICENSE_PATH)
    expect("ALLOWANCE D: LICENSE's copyright-holder line may name the holder", "identity" not in k)
    k, _v, _h = kinds(lic, rel="docs/notlicense.md")  # CITATION-OK
    expect("ALLOWANCE D: the same text in another file is NOT excused", "identity" in k)
    k, _v, _h = kinds(lic + b"contact somehandle for terms\n", rel=LICENSE_PATH)
    expect("ALLOWANCE D: it excuses ONLY that one line of LICENSE", "identity" in k)
    k, _v, _h = kinds(b"MIT License\n\n(c) 2026 somehandle\n", rel=LICENSE_PATH)
    expect("ALLOWANCE D: a LICENSE with no copyright-shaped line excuses nothing", "identity" in k)
    k, _v, _h = kinds(
        b'Copyright (c) 2026 somehandle\nRELAY = "198.100.200.155"\n', rel=LICENSE_PATH
    )
    expect("ALLOWANCE D: the other three classes are not excused in LICENSE", "public-ipv4" in k)

    # ---- the real tree ---------------------------------------------------------------------
    pub = published_paths()
    expect("the published tier resolves a non-trivial publish set", len(pub) > 1000)
    expect("the publish set excludes machine_config.py", "tools/machine_config.py" not in pub)
    expect("machine_config declares absolute-path defaults", len(default_path_prefixes()) >= 5)

    for name, passed in results:
        print("  %-70s %s" % (name, "ok" if passed else "FAIL"))
    print("lint_machine_paths --selftest: %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--selftest", action="store_true", help="assert every arm still fires")
    args = ap.parse_args()

    def say(*a, **k):
        k.pop("file", None)
        print(*a, **k)

    sys.exit(selftest() if args.selftest else check(say=say))


if __name__ == "__main__":
    main()
