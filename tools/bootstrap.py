#!/usr/bin/env python3
"""bootstrap.py --check -- report what THIS machine is missing to build + analyze this project.

One command, run at session start, that answers "is this environment set up?" and, for anything that
is not, names the remedy. It is the executable form of the by-hand setup notes scattered across
tools/ghidra/README.md and src/mh_dll/README.md -- and it is the specification the VM
provisioning scripts (bootstrap items V1/V2) implement (bootstrap item E3).

It reads the machine-specific values from tools/machine_config.py (E2) and the Python dependency set
from tools/requirements.txt (E1), so it stays correct as those move.

Every check is CHEAP and SIDE-EFFECT FREE (it is run at session start): a file stat, a version string
parse, a one-packet ping. Nothing is installed, launched, or written.

  python tools/bootstrap.py            # or --check; prints a grouped report, exit 1 if anything is red
  python tools/bootstrap.py --json     # machine-readable
  python tools/bootstrap.py --ci       # a build-only runner: the analysis + game + rig tiers are
                                       # EXPECTED-ABSENT (named, not failed); python + msvc still gate

Exit code is 0 only when every REQUIRED check is green. A check can be:
  OK    green  -- satisfied.
  FAIL  red    -- required and missing; the line names the remedy. Sets exit 1.
  SKIP  grey   -- could not be evaluated because a prerequisite failed (e.g. a Ghidra customization
                  when the Ghidra install itself is missing) -- attributed, not counted as its own
                  failure, so a single root cause reports as a single red line.
  ABSENT grey  -- --ci only: a group this environment is not expected to have. Named, never hidden.
"""

import argparse
import json
import os
import re
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lint_requirements  # noqa: E402  (reuse the pinned-distribution parser)
import machine_config as machine  # noqa: E402

REPO = Path(__file__).resolve().parent.parent
GHIDRA = Path(machine.GHIDRA_INSTALL)

OK, FAIL, SKIP, ABSENT = "OK", "FAIL", "SKIP", "ABSENT"

# fork F5D -- the --ci tiers. A CI runner builds and tests; it does not reverse-engineer and it has
# no game. So these GROUPS are expected-absent there and their reds become named ABSENT lines rather
# than failures. DECLARED as one table for the same reason lint_repo's skip list is: the set of
# things a green --ci does NOT prove has to be enumerable, or "bootstrap green" starts meaning
# whatever the runner happened to have installed.
#
# What deliberately stays a GATE: "Python + deps" and "MSVC Build Tools" -- the two the build and the
# selftests actually need. JDK 21 is in the absent tier because it exists in this repo ONLY to run
# Ghidra; nothing in the build/selftest/lint path touches a JVM.
CI_EXPECTED_ABSENT = {
    "JDK 21": "the JVM exists here only to run Ghidra; no build/test/lint step uses it",
    "Ghidra + customizations": "the analysis tier -- a CI runner does not reverse-engineer",
    "ReVa": "the analysis tier's MCP bridge (needs a running Ghidra)",
    "Game artifacts": "a retail game copy -- bring-your-own-game (fork Q10)",
    "Rig bootable": "a retail game install prepared for the UI/determinism rig",
    "Rig peers": "the LAN VM peers the multiplayer rig drives",
}


class Check:
    __slots__ = ("group", "name", "status", "detail", "remedy")

    def __init__(self, group, name, status, detail="", remedy=""):
        self.group = group
        self.name = name
        self.status = status
        self.detail = detail
        self.remedy = remedy


# ─────────────────────────────── individual checks ───────────────────────────────
def check_python():
    v = sys.version_info
    ok = v >= (3, 9)
    return [
        Check(
            "Python + deps",
            "Python interpreter",
            OK if ok else FAIL,
            f"{v.major}.{v.minor}.{v.micro}",
            "" if ok else "install Python >= 3.9",
        )
    ]


def check_deps():
    """Every distribution pinned in tools/requirements.txt is importable-by-metadata here."""
    from importlib import metadata

    out = []
    pinned = lint_requirements.parse_pinned()  # {normalized_dist: "dist==ver"}
    installed = {
        lint_requirements._norm(d.metadata["Name"]): d.version for d in metadata.distributions()
    }
    missing = []
    for norm, spec in sorted(pinned.items()):
        if norm not in installed:
            missing.append(spec)
    if missing:
        out.append(
            Check(
                "Python + deps",
                "pinned dependencies",
                FAIL,
                f"{len(missing)} missing: {', '.join(missing)}",
                "python -m pip install -r tools/requirements.txt",
            )
        )
    else:
        out.append(Check("Python + deps", "pinned dependencies", OK, f"{len(pinned)} present"))
    return out


def _java_major(text):
    m = re.search(r'version "(\d+)(?:\.(\d+))?', text)
    if not m:
        return None
    major = int(m.group(1))
    # Legacy "1.8" style -> 8
    if major == 1 and m.group(2):
        return int(m.group(2))
    return major


def check_jdk():
    try:
        p = subprocess.run(["java", "-version"], capture_output=True, text=True, timeout=15)
    except (FileNotFoundError, subprocess.SubprocessError):
        return [
            Check(
                "JDK 21",
                "java on PATH",
                FAIL,
                "java not found",
                "install JDK 21 and add it to PATH",
            )
        ]
    text = (p.stderr or "") + (p.stdout or "")
    major = _java_major(text)
    line = text.strip().splitlines()[0] if text.strip() else "?"
    if major is not None and major >= 21:
        note = (
            line
            if major == 21
            else f"{line}  (JDK {major}; runs Ghidra fine -- 21 EXACTLY is needed only to BUILD ReVa from source, Gradle 8.14)"
        )
        return [Check("JDK 21", "java -version", OK, note)]
    return [
        Check(
            "JDK 21",
            "java -version",
            FAIL,
            f"found major {major}: {line}",
            "install JDK >= 21 (Ghidra 12.1.2 requires it; the from-source ReVa/Gradle build needs 21 exactly)",
        )
    ]


def _ghidra_installed_version():
    props = GHIDRA / "Ghidra" / "application.properties"
    if not props.is_file():
        return None
    for line in props.read_text(encoding="utf-8", errors="replace").splitlines():
        if line.startswith("application.version="):
            return line.split("=", 1)[1].strip()
    return None


def check_ghidra():
    """The install itself. Returns (checks, present:bool) so dependent checks can SKIP cleanly."""
    if not GHIDRA.is_dir():
        return (
            [
                Check(
                    "Ghidra + customizations",
                    "Ghidra install",
                    FAIL,
                    f"not found at {GHIDRA}",
                    f"install Ghidra {machine.GHIDRA_VERSION} to {GHIDRA} (or set MH_GHIDRA_INSTALL)",
                )
            ],
            False,
        )
    ver = _ghidra_installed_version()
    if ver != machine.GHIDRA_VERSION:
        return (
            [
                Check(
                    "Ghidra + customizations",
                    "Ghidra install",
                    FAIL,
                    f"version {ver} at {GHIDRA}, expected {machine.GHIDRA_VERSION}",
                    f"install Ghidra {machine.GHIDRA_VERSION} (or update machine_config.GHIDRA_VERSION)",
                )
            ],
            False,
        )
    return ([Check("Ghidra + customizations", "Ghidra install", OK, f"{ver} at {GHIDRA}")], True)


def check_cspec_fix(ghidra_ok):
    name = "watcall cspec fix"
    remedy = "re-apply tools/ghidra/x86watcom.cspec.fixed per tools/ghidra/README.md, then restart Ghidra"
    if not ghidra_ok:
        return [Check("Ghidra + customizations", name, SKIP, "Ghidra install missing")]
    installed = (
        GHIDRA
        / "Ghidra"
        / "Processors"
        / "ghidrawatcall"
        / "data"
        / "languages"
        / "x86watcom.cspec"
    )
    if not installed.is_file():
        return [
            Check(
                "Ghidra + customizations", name, FAIL, "ghidrawatcall cspec not installed", remedy
            )
        ]
    # Check the FUNCTIONAL fix, not byte-identity: the custom register models the fix introduces must
    # be present. A byte-exact compare to the repo .fixed copy false-fails on comment-only drift (the
    # repo copy carries doc comments the install legitimately does not) -- the register contract is
    # what actually affects decompilation.
    text = installed.read_text(encoding="utf-8", errors="replace")
    markers = ("__mh_watcall_ecx_volatile", "__mh_watcall_ebx_volatile", "__mh_stkprobe")
    missing = [m for m in markers if m not in text]
    if missing:
        return [
            Check(
                "Ghidra + customizations",
                name,
                FAIL,
                f"functional models missing from installed cspec: {', '.join(missing)}",
                remedy,
            )
        ]
    return [Check("Ghidra + customizations", name, OK, "custom __watcall register models present")]


def check_sleigh_patch(ghidra_ok):
    name = "REP MOVS->memcpy SLEIGH patch"
    remedy = (
        "apply tools/ghidra/sleigh_memcpy/rep-movs-memcpy.patch to ia.sinc + recompile x86 (README)"
    )
    if not ghidra_ok:
        return [Check("Ghidra + customizations", name, SKIP, "Ghidra install missing")]
    sinc = GHIDRA / "Ghidra" / "Processors" / "x86" / "data" / "languages" / "ia.sinc"
    if not sinc.is_file():
        return [Check("Ghidra + customizations", name, FAIL, "x86 ia.sinc not found", remedy)]
    patched = "define pcodeop memcpy" in sinc.read_text(encoding="utf-8", errors="replace")
    return [
        Check(
            "Ghidra + customizations",
            name,
            OK if patched else FAIL,
            "memcpy pcodeop present in ia.sinc" if patched else "patch not present in ia.sinc",
            "" if patched else remedy,
        )
    ]


def check_fidb():
    name = "Watcom FID database"
    fidb = REPO / "tools" / "ghidra" / "watcom_fidb" / "watcom106_v2.fidb"
    if fidb.is_file():
        return [
            Check(
                "Ghidra + customizations",
                name,
                OK,
                f"{fidb.name} ({fidb.stat().st_size} B), attach by path",
            )
        ]
    return [
        Check(
            "Ghidra + customizations",
            name,
            FAIL,
            "watcom106_v2.fidb missing",
            "rebuild via tools/ghidra/watcom_fidb/build_watcom_fidb.java",
        )
    ]


def check_reva(ghidra_ok):
    name = "ReVa extension"
    remedy = "install/build ReVa into the Ghidra user settings dir (tools/ghidra/README.md 'Building ReVa')"
    appdata = os.environ.get("APPDATA")
    candidates = []
    if appdata:
        candidates.append(
            Path(appdata)
            / "ghidra"
            / f"ghidra_{machine.GHIDRA_VERSION}_PUBLIC"
            / "Extensions"
            / "reverse-engineering-assistant"
        )
    for c in candidates:
        if c.is_dir():
            return [Check("ReVa", name, OK, f"installed at {c}")]
    return [
        Check(
            "ReVa",
            name,
            FAIL,
            "reverse-engineering-assistant extension not found in the Ghidra user settings dir",
            remedy,
        )
    ]


def check_ghidra_shims():
    """Every Ghidra-side tool has a runnable `mh_<tool>.py` in a registered script directory.

    Not cosmetic: without a shim, ReVA answers `Script not found` and the tool is simply
    unavailable on that machine -- which is how the codemap generator went unrun here for months
    (2026-08-22). The set is derived, so this check cannot go stale against a new tool.
    """
    name = "Ghidra script shims"
    # THE INSTALLER IS DISCOVERED, NOT HARD-CODED (fork F5M S4b). It belongs to the Ghidra pipeline,
    # which is archive-class tooling the public cut withholds -- so a hard-coded path here would be
    # a pointer into a file this tree may legitimately not contain, and the check would report a
    # FAIL that means "you are not the research repo". Discovering it turns that into a named
    # ABSENCE: no installer, no shim question, and the analysis tier is expected-absent anyway
    # (CI_EXPECTED_ABSENT, "ReVa"), so --ci stays green while a real research checkout still gets
    # the full check.
    remedy = "run the Ghidra script-shim installer that ships with the research tooling"
    script = next(iter(sorted((REPO / "tools").glob("install_ghidra_shim*.py"))), None)
    if script is None:
        return [
            Check(
                "ReVa",
                name,
                FAIL,
                "no Ghidra script-shim installer in this tree (the Ghidra pipeline is not carried "
                "here) -- without a shim, ReVA answers `Script not found` for every Ghidra-side tool",
                remedy,
            )
        ]
    try:
        r = subprocess.run(
            [sys.executable, str(script), "--check"], capture_output=True, text=True, timeout=60
        )
    except (OSError, subprocess.SubprocessError) as e:
        return [Check("ReVa", name, FAIL, "could not run the shim check: %s" % e, remedy)]
    head = (r.stdout or "").strip().splitlines()
    detail = head[0] if head else "no output"
    return [Check("ReVa", name, OK if r.returncode == 0 else FAIL, detail, remedy)]


def _find_vcvars():
    """vcvars32.bat via vswhere, then machine.VS_INSTALL_ROOT (mirrors check_const_view)."""
    roots = []
    vswhere = (
        Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"))
        / "Microsoft Visual Studio"
        / "Installer"
        / "vswhere.exe"
    )
    if vswhere.is_file():
        try:
            out = subprocess.run(
                [str(vswhere), "-latest", "-products", "*", "-property", "installationPath"],
                capture_output=True,
                text=True,
                timeout=30,
            ).stdout.strip()
            roots += [ln.strip() for ln in out.splitlines() if ln.strip()]
        except (OSError, subprocess.SubprocessError):
            pass
    roots.append(machine.VS_INSTALL_ROOT)
    for root in roots:
        bat = Path(root) / "VC" / "Auxiliary" / "Build" / "vcvars32.bat"
        if bat.is_file():
            return bat
    return None


def check_msvc():
    name = "MSVC Build Tools (v143 / x86)"
    remedy = "install Build Tools for VS 2022 (v143 + Windows SDK 10); the full IDE is not required"
    bat = _find_vcvars()
    if bat:
        return [Check("MSVC Build Tools", name, OK, f"vcvars32.bat at {bat}")]
    return [
        Check(
            "MSVC Build Tools",
            name,
            FAIL,
            "vcvars32.bat not found (no VS/BuildTools x86 toolset)",
            remedy,
        )
    ]


def check_game_artifacts():
    out = []
    polygon = Path(machine.POLYGON)
    out.append(
        Check(
            "Game artifacts",
            "EN polygon install",
            OK if polygon.is_dir() else FAIL,
            str(polygon) if polygon.is_dir() else f"{polygon} not found",
            ""
            if polygon.is_dir()
            else "stage the EN game install (bootstrap A1) or set MH_POLYGON",
        )
    )
    clean = Path(machine.POLYGON_CLEAN) / "mh.exe"
    out.append(
        Check(
            "Game artifacts",
            "pristine EN mh.exe",
            OK if clean.is_file() else FAIL,
            str(clean) if clean.is_file() else f"{clean} not found",
            ""
            if clean.is_file()
            else "stage the pristine EN install (bootstrap A1) or set MH_POLYGON_CLEAN",
        )
    )
    return out


def check_rig_bootable():
    """The four things a hand-copied game tree lacks that a full installer would provide, each of
    which fails SILENTLY (the game hangs before its first frame, or the map list is empty). Remedy
    for all: `python tools/provision_rig.py` (the disc-check RE, tools/data/mh_registry.reg). Cheap:
    one `reg query` + three file stats."""
    out = []
    polygon = Path(machine.POLYGON)
    remedy = "python tools/provision_rig.py  (elevated shell for the registry write)"

    key = r"HKLM\SOFTWARE\WOW6432Node\Techland\Mission Humanity"
    try:
        present = (
            subprocess.run(["reg", "query", key], capture_output=True, text=True).returncode == 0
        )
    except (OSError, subprocess.SubprocessError):
        present = False
    out.append(
        Check(
            "Rig bootable",
            "Techland registry key",
            OK if present else FAIL,
            "present" if present else "MISSING -- game hangs on a blank modal at boot",
            "" if present else remedy,
        )
    )

    focus = polygon / "mh.focus.exe"
    out.append(
        Check(
            "Rig bootable",
            "mh.focus.exe (run-without-focus + mh.dll import)",
            OK if focus.is_file() else FAIL,
            str(focus) if focus.is_file() else "MISSING -- the rig has no exe to launch",
            "" if focus.is_file() else remedy,
        )
    )

    maps = polygon / "Maps"
    mpm = [f for f in maps.glob("*.mpm")] if maps.is_dir() else []
    out.append(
        Check(
            "Rig bootable",
            "Maps\\*.mpm",
            OK if mpm else FAIL,
            f"{len(mpm)} map(s)"
            if mpm
            else "MISSING/empty -- the map list is empty, no match starts",
            ""
            if mpm
            else "upload the game's Maps\\*.mpm into the EN polygon (uploaded artifact, not built)",
        )
    )

    dll = polygon / "mh.dll"
    out.append(
        Check(
            "Rig bootable",
            "mh.dll deployed to polygon",
            OK if dll.is_file() else FAIL,
            "deployed" if dll.is_file() else "not deployed",
            "" if dll.is_file() else remedy,
        )
    )
    # fork F4B: the transport is a SEPARATE FILE now (mh_net.dll, loaded by mh.dll from beside
    # itself). Its absence is a SUPPORTED configuration at runtime -- the game boots and the MP
    # browser says why -- and that is exactly what makes it worth checking HERE rather than leaving
    # to a failing run: a polygon carrying only mh.dll plays single-player perfectly and fails every
    # multiplayer scenario, with nothing anywhere saying which of the two it is except the first
    # line of mh_net.log. FAIL, like the mh.dll row above, because the rig exists for MP.
    net = polygon / "mh_net.dll"
    out.append(
        Check(
            "Rig bootable",
            "mh_net.dll (the MP transport satellite) deployed to polygon",
            OK if net.is_file() else FAIL,
            "deployed" if net.is_file() else "absent -- the polygon boots with NO multiplayer",
            ""
            if net.is_file()
            else "copy src\\mh_dll\\Release\\mh_net.dll next to mh.dll (docs/dll-split.md)",
        )
    )
    # fork F4D: and the SPINE is a separate file too (libmh.dll -- 627 of mh.dll's old 666 TUs).
    # Its absence is CONFIGURATION (1), a shipped configuration rather than a degradation: the game
    # runs the original binary's own bodies, and a player cannot tell. That is precisely why it
    # belongs here and not in a failing run -- a polygon carrying only mh.dll boots, plays, renders
    # every menu, passes most of the UI suite, and silently measures the ORIGINAL engine. The one
    # place that says which configuration it is in is the `[modules] libmh:` line in mh_net.log and
    # the `[libmh] crossings=` count at the end of the arm. FAIL, because the rig exists to measure
    # what we wrote.
    spine = polygon / "libmh.dll"
    out.append(
        Check(
            "Rig bootable",
            "libmh.dll (the spine satellite) deployed to polygon",
            OK if spine.is_file() else FAIL,
            "deployed"
            if spine.is_file()
            else "absent -- the polygon runs CONFIGURATION (1), i.e. the original engine",
            ""
            if spine.is_file()
            else "copy src\\mh_dll\\Release\\libmh.dll next to mh.dll (docs/dll-split.md)",
        )
    )
    # fork F4E: and the INSTRUMENT is a separate file too (mh_harness.dll, the determinism/replay
    # harness). Its absence is the quietest of the three: the game is bit-for-bit the game it always
    # was, because the harness only ever observes and it ships disarmed. So a polygon without it
    # boots, plays, renders, and passes every pixel scenario in the suite -- and then `--determinism`,
    # `--sp-determinism` and both `--ui-abc` arms have no mh_harness.log to read. FAIL, because those
    # are the measurements this rig exists for.
    inst = polygon / "mh_harness.dll"
    out.append(
        Check(
            "Rig bootable",
            "mh_harness.dll (the determinism harness satellite) deployed to polygon",
            OK if inst.is_file() else FAIL,
            "deployed"
            if inst.is_file()
            else "absent -- the polygon boots UNINSTRUMENTED: no per-step hashes, no journal",
            ""
            if inst.is_file()
            else "copy src\\mh_dll\\Release\\mh_harness.dll next to mh.dll (docs/dll-split.md)",
        )
    )
    return out


def _ping(host):
    try:
        p = subprocess.run(
            ["ping", "-n", "1", "-w", "800", host], capture_output=True, text=True, timeout=5
        )
        return p.returncode == 0
    except (OSError, subprocess.SubprocessError):
        return False


def check_rig():
    out = []
    peers = [("host (this box)", machine.HOST_IP)] + [
        (f"rig peer {c}", ip) for c, ip in zip("AB", machine.RIG_PEERS)
    ]
    for label, ip in peers:
        up = _ping(ip)
        out.append(
            Check(
                "Rig peers",
                f"{label} {ip}",
                OK if up else FAIL,
                "reachable" if up else "no ping reply",
                "" if up else "power on the LAN peer, or update machine_config RIG_PEER_*/HOST_IP",
            )
        )
    return out


# ─────────────────────────────── driver ───────────────────────────────
def run_all():
    checks = []
    checks += check_python()
    checks += check_deps()
    checks += check_jdk()
    gchecks, ghidra_ok = check_ghidra()
    checks += gchecks
    checks += check_cspec_fix(ghidra_ok)
    checks += check_sleigh_patch(ghidra_ok)
    checks += check_fidb()
    checks += check_reva(ghidra_ok)
    checks += check_ghidra_shims()
    checks += check_msvc()
    checks += check_game_artifacts()
    checks += check_rig_bootable()
    checks += check_rig()
    return checks


def apply_ci_tiers(checks):
    """--ci: rewrite every red in an expected-absent GROUP to ABSENT, and return the groups that
    were actually rewritten (so the report can name them rather than quietly omit them)."""
    hit = {}
    for c in checks:
        if c.group in CI_EXPECTED_ABSENT and c.status == FAIL:
            c.status = ABSENT
            c.remedy = ""
            hit.setdefault(c.group, 0)
            hit[c.group] += 1
    return hit


def check_ci_tier_list(checks):
    """The dead-rule arm, same discipline as lint_repo's: a CI_EXPECTED_ABSENT key naming a group
    that no longer exists is an exemption for nothing, and it would hide the day that group is
    renamed into the gated tier. Returns a list of problems (empty == ok)."""
    groups = {c.group for c in checks}
    return [
        "CI_EXPECTED_ABSENT names no such group: %r" % g
        for g in CI_EXPECTED_ABSENT
        if g not in groups
    ]


_COLORS = {OK: "\033[32m", FAIL: "\033[31m", SKIP: "\033[90m", ABSENT: "\033[90m"}
_RESET = "\033[0m"


def _fmt(status):
    tag = {OK: "OK  ", FAIL: "FAIL", SKIP: "SKIP", ABSENT: "ABSN"}[status]
    if sys.stdout.isatty():
        return f"{_COLORS[status]}{tag}{_RESET}"
    return tag


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--check", action="store_true", help="run the checks (default action)")
    ap.add_argument("--json", action="store_true", help="emit JSON instead of a table")
    ap.add_argument(
        "--ci",
        action="store_true",
        help="build-only runner: the analysis/game/rig groups report ABSENT (named) instead of FAIL",
    )
    args = ap.parse_args()

    checks = run_all()
    stale = check_ci_tier_list(checks)
    absent_groups = apply_ci_tiers(checks) if args.ci else {}
    failed = [c for c in checks if c.status == FAIL]

    if args.json:
        print(
            json.dumps(
                {
                    "ok": not failed and not stale,
                    "ci": bool(args.ci),
                    "expected_absent": {g: CI_EXPECTED_ABSENT[g] for g in absent_groups},
                    "ci_tier_problems": stale,
                    "checks": [
                        {
                            "group": c.group,
                            "name": c.name,
                            "status": c.status,
                            "detail": c.detail,
                            "remedy": c.remedy,
                        }
                        for c in checks
                    ],
                },
                indent=2,
            )
        )
        return 0 if not failed and not stale else 1

    group = None
    for c in checks:
        if c.group != group:
            group = c.group
            suffix = ""
            if group in absent_groups:
                suffix = f"   [EXPECTED-ABSENT (ci) -- {CI_EXPECTED_ABSENT[group]}]"
            print(f"\n{group}{suffix}")
        line = f"  [{_fmt(c.status)}] {c.name}"
        if c.detail:
            line += f" -- {c.detail}"
        print(line)
        if c.status == FAIL and c.remedy:
            print(f"         remedy: {c.remedy}")

    print()
    for problem in stale:
        print(f"bootstrap: {problem}")
    if args.ci:
        n_absent = sum(absent_groups.values())
        print(
            f"bootstrap --ci: {len(checks) - n_absent} check(s) gated, {n_absent} in "
            f"{len(absent_groups)} expected-absent group(s) -- each named above"
        )
    if failed or stale:
        print(f"bootstrap: {len(failed)} check(s) FAILED -- see remedies above")
        return 1
    print(f"bootstrap: all {len(checks)} checks green")
    return 0


if __name__ == "__main__":
    sys.exit(main())
