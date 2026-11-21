#!/usr/bin/env python3
"""
tools/check_cap_save.py -- patching:PT-CAP8 S3 (slice G): the RAISED-CAPS SAVE (spec tmp/capraise/s23/spec.md section 6, R4).

A raised world (records past the vanilla window: buildings [100,500), the five sub-rosters, projectiles [1500,4000)) must round-trip
through a save, and a save that carries such records must be REFUSED, with a message, by a build whose raise is OFF. The mechanism
is the composed storage-policy save carrier plus the existing overflow codec. The row is
`cap_save_seq` (tools/uiscripts/registry.yaml), four PROCESSES on one lane; each step has one mode:

  --step save      raise ON. The harness fixture (cap_* + cap_prj_synth + cap_save_at) fills the roster past every vanilla cap, stamps
                   600 projectile records into the pool slots 1500.., saves `capovf` through game_SaveGame and loads it back through
                   llm_game_load in the SAME step. Clauses:
                   1  mh_net.log: `[caps] relocation APPLIED` and `[caps] limits ON (`.
                   2  mh_harness.log: `; CAP SAVE ... rc=1` and `; CAP LOAD ... rc=1`; the two digests are EQUAL; the counts (ovf, bldg_ovf,
                      prj_ovf) are EQUAL; bldg_ovf > 0 and prj_ovf >= --prj (default 600: records past slot 1500 survive, which closes PT-CAP1).
                   3  Current policy+caps header, SPFT before MHOF, full-container preflight acceptance; the trailer and actual load
                      carry N == ovf records. Full native/member framing and CRC validation precede acceptance.
                   4  no `[caps] ... REFUSED`, no `*** DESYNC` banner.
  --step refuse    raise OFF, the committed fixture tools/uiscripts/saves/capovf.sav loaded from the main menu. Clauses: relocation APPLIED
                   and the limits stay OFF; `[caps] load REFUSED: the save is a raised-caps save and [caps] raise is OFF` and `showing the
                   raised-limits message`; NO `raised save accepted`, no `overflow ... applied`; the game never started stepping.
  --step vanilla   raise ON, a committed VANILLA save (2-almost) loaded from the main menu. Clauses: limits ON, no REFUSED, no `raised save
                   accepted` (it is not stamped), no overflow trailer applied, the game PLAYED (the per-step hash log reaches step >= 3000 past the load).

  --step old --records N [--plays S]
                   raise ON, an OLD raised save (PT-CAP9-U7f: the shipped v0.2.2-rc2 save 8-ayatus-rl-2.sav, MHOV v1, 8 records; the capovf.sav fixture,
                   MHOV v1, 1305 records) loaded from the main menu on the CURRENT build. Clauses: limits ON; `[caps] load: raised save accepted`;
                   `[caps] load: planet file overflow N records applied` with N == --records (the count the loaded PLANET's member file carries; N = 0
                   means that member has no trailer and NO such line may appear -- the rc2 save keeps its 8 records in the embedded planet-4 member, the
                   current planet is 8; ovftest (7a) applies those 8 from the committed save); no `[caps] ... REFUSED`, no `GUARD HIT` and no non-zero `guard=0x` in the orders census; no
                   DESYNC; and the game PLAYED: the per-step hash log reaches step >= --plays (default 300) past the load.
  --step off       raise OFF (the shipped default): the SAME native save/load-back with no overflow fixture. Current saves still
                   require storage-policy support: MSP1 header, SPFT policy tail and SPCF manifest, including all-default settings.
                   No MHOF/overflow application or raised acceptance is allowed. The real preflight validates every member and
                   manifest, then the native load returns 1 with equal zero-overflow counts and digest. This is NOT byte-identical
                   to a pre-policy save; an older reader must reject its required-support header before applying state.

  python tools/check_cap_save.py --step save|refuse|vanilla [--prj N] <run-dir> [<run-dir> ...]
  python tools/check_cap_save.py --step old --records N [--plays S] <run-dir>
  python tools/check_cap_save.py --step off <run-dir>
  python tools/check_cap_save.py --sites [--exe PATH]     the four splice sites' 5 expected bytes (and the version table), the LOAD splice behind the path
                                                          free-count block (PT-CAP9-U7b), and the refusal text ("raised limits") against the clean EN exe / the source
  python tools/check_cap_save.py --selftest               planted negatives + --sites (skipped, loudly, when no clean exe is found)
"""

import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import check_cancel_task  # noqa: E402  (net_log_lines)
import check_storage_purge  # noqa: E402  (harness_log_lines)

STAMP = re.compile(r"^\[[\d:.]+\]\s*")

SAVE_RE = re.compile(
    r"; CAP SAVE step=(?P<step>\d+) name=(?P<name>\S+) rc=(?P<rc>\d+) digest=(?P<digest>[0-9A-Fa-f]{16}) ovf=(?P<ovf>\d+) bldg_ovf=(?P<bldg>\d+) prj_ovf=(?P<prj>\d+)"
)
LOAD_RE = re.compile(
    r"; CAP LOAD step=(?P<step>\d+) name=(?P<name>\S+) rc=(?P<rc>\d+) digest=(?P<digest>[0-9A-Fa-f]{16}) ovf=(?P<ovf>\d+) bldg_ovf=(?P<bldg>\d+) prj_ovf=(?P<prj>\d+)"
)
TRAILER_RE = re.compile(
    r"; \[caps\] save: planet (?P<p>-?\d+) trailer (?P<n>\d+) records \((?P<b>\d+) bytes\)"
)
APPLIED_RE = re.compile(r"; \[caps\] load: planet file overflow (?P<n>\d+) records applied")
REFUSE_VERSION = "; [caps] load REFUSED: the save is a raised-caps save and [caps] raise is OFF"
REFUSE_SHOWN = "; [caps] load REFUSED: showing the raised-limits message"
FILE_RE = re.compile(
    r"; CAP SAVE FILE name=(?P<name>\S+) size=(?P<size>\d+) fnv=(?P<fnv>[0-9A-Fa-f]{8}) header_valid=(?P<valid>[01]) raised=(?P<raised>[01]) policy=(?P<policy>[01])$"
)
PLANET_RE = re.compile(
    r"; CAP SAVE PLANET file=(?P<file>\S+) size=(?P<size>\d+) footer=(?P<foot>[01]) footer_len=(?P<len>\d+) policy_tail=(?P<policy>[01]) policy_len=(?P<policy_len>\d+)$"
)
CONTAINER_RE = re.compile(
    r"; \[storage-save\] container accepted members=(?P<members>[0-9A-Fa-f]{8}) policy=(?P<policy>[0-9A-Fa-f]{8}) caps=(?P<caps>[0-9A-Fa-f]{8}) version=(?P<version>-?\d+)$"
)
ACCEPTED = "; [caps] load: raised save accepted"


def strip(lines):
    # Session/process chunks can overlap and arrive in either order. Each bounded
    # run is shorter than half a day: the largest circular clock gap is outside it.
    # Order stamped events across midnight before dropping their timestamps.
    timed, plain = [], []
    for index, line in enumerate(lines):
        match = re.match(r"^\[(\d{2}):(\d{2}):(\d{2})\.(\d{3})\]", line.strip())
        if match:
            h, m, s, ms = map(int, match.groups())
            timed.append((((h * 60 + m) * 60 + s) * 1000 + ms, index, line))
        else:
            plain.append(line)
    if timed:
        day = 24 * 60 * 60 * 1000
        marks = sorted({t[0] for t in timed})
        gap = max(
            range(len(marks)),
            key=lambda i: (marks[(i + 1) % len(marks)] - marks[i]) % day,
        )
        start = marks[(gap + 1) % len(marks)]
        plain.extend(t[2] for t in sorted(timed, key=lambda t: ((t[0] - start) % day, t[1])))
    return [STAMP.sub("", ln.strip()) for ln in plain]


def _common_banners(net, want_limits):
    bad = []
    if not any("[caps] relocation APPLIED" in ln for ln in net):
        bad.append(
            "clause 1: no `[caps] relocation APPLIED` in mh_net.log (the run was not on the relocated layout)"
        )
    on = any("[caps] limits ON (" in ln for ln in net)
    if want_limits and not on:
        bad.append("clause 1: no `[caps] limits ON (` in mh_net.log (the raise never turned on)")
    if not want_limits and on:
        bad.append("clause 1: `[caps] limits ON (` in a raise=OFF step (the ini leaked the raise)")
    for ln in net:
        if "[caps]" in ln and "WARNING" in ln:
            bad.append("clause 1: " + ln)
    return bad


def _crash(net, harness):
    return ["clause 4: " + ln for ln in net + harness if "*** DESYNC" in ln]


def current_framing_problems(net, harness, *, current_raised):
    """Require current-writer metadata AND the actual reader's full preflight."""
    net, harness = strip(net), strip(harness)
    bad = []
    files = [m for m in (FILE_RE.match(ln) for ln in harness) if m]
    if not files:
        bad.append("clause 3: no current `; CAP SAVE FILE` line")
    else:
        f = files[-1]
        if (
            f.group("valid") != "1"
            or f.group("policy") != "1"
            or f.group("raised") != str(int(current_raised))
        ):
            bad.append("clause 3: saved container header requirements do not match this step")
    planets = [m for m in (PLANET_RE.match(ln) for ln in harness) if m]
    current_bit = 0
    if not planets:
        bad.append("clause 3: no current `; CAP SAVE PLANET` line")
    else:
        p = planets[-1]
        if p.group("policy") != "1" or int(p.group("policy_len")) <= 0:
            bad.append("clause 3: the saved planet file has no nonempty SPFT policy tail")
        if current_raised:
            if p.group("foot") != "1" or int(p.group("len")) <= 0:
                bad.append("clause 3: the saved planet file does not end in an overflow footer")
        elif p.group("foot") != "0" or int(p.group("len")) != 0:
            bad.append("clause 3: the raise-OFF planet file carries an overflow footer")
        planet = re.search(r"(?:^|[\\/])save(?P<planet>\d{1,2})\.dat$", p.group("file"))
        if planet is None or not 0 <= int(planet.group("planet")) < 32:
            bad.append("clause 3: the saved planet file has no valid planet index")
        else:
            current_bit = 1 << int(planet.group("planet"))
    accepted = [m for m in (CONTAINER_RE.match(ln) for ln in net) if m]
    if not accepted:
        bad.append("clause 3: no full-container preflight acceptance")
    else:
        a = accepted[-1]
        members, policy, caps = (int(a.group(k), 16) for k in ("members", "policy", "caps"))
        if (
            not members
            or not policy
            or policy & ~members
            or caps & ~members
            or not 1 <= int(a.group("version")) <= 5
            or (not current_raised and caps != 0)
            or (
                current_bit
                and (
                    not members & current_bit
                    or not policy & current_bit
                    or bool(caps & current_bit) != current_raised
                )
            )
        ):
            bad.append("clause 3: full-container preflight requirements are inconsistent")
    return bad


def save_problems(net, harness, prj_min=600):
    net, harness = strip(net), strip(harness)
    bad = _common_banners(net, True)
    bad += current_framing_problems(net, harness, current_raised=True)
    for ln in net:
        if "[caps]" in ln and "REFUSED" in ln:
            bad.append("clause 4: " + ln)
    sv = [m for m in (SAVE_RE.match(ln) for ln in harness) if m]
    lo = [m for m in (LOAD_RE.match(ln) for ln in harness) if m]
    if any(ln.startswith("; CAP PRJ SYNTH FAIL") for ln in harness):
        bad.append("clause 2: the projectile stamp failed")
    if not sv:
        bad.append(
            "clause 2: no `; CAP SAVE` line (cap_save_at never reached, or the process died first)"
        )
    if not lo:
        bad.append("clause 2: no `; CAP LOAD` line (the load-back never ran)")
    if not sv or not lo:
        return bad
    s, l = sv[-1], lo[-1]
    if int(s.group("rc")) != 1:
        bad.append("clause 2: game_SaveGame returned %s, not 1" % s.group("rc"))
    if int(l.group("rc")) != 1:
        bad.append(
            "clause 2: llm_game_load returned %s, not 1 (the raised save was refused or failed)"
            % l.group("rc")
        )
    if s.group("digest").upper() != l.group("digest").upper():
        bad.append(
            "clause 2: overflow digest %s at the save, %s after the load -- the round trip is NOT exact"
            % (s.group("digest"), l.group("digest"))
        )
    for k in ("ovf", "bldg", "prj"):
        if s.group(k) != l.group(k):
            bad.append(
                "clause 2: %s count %s at the save, %s after the load" % (k, s.group(k), l.group(k))
            )
    if int(s.group("bldg")) <= 0:
        bad.append(
            "clause 2: no building past the vanilla window (bldg_ovf=%s): the fixture proved nothing"
            % s.group("bldg")
        )
    if int(s.group("prj")) < prj_min:
        bad.append(
            "clause 2: prj_ovf=%s < %d: projectile records past slot 1500 are not covered"
            % (s.group("prj"), prj_min)
        )
    if int(s.group("ovf")) < int(s.group("bldg")) + int(s.group("prj")):
        bad.append("clause 2: ovf total %s is less than bldg_ovf + prj_ovf" % s.group("ovf"))
    tr = [m for m in (TRAILER_RE.search(ln) for ln in net) if m]
    if not tr:
        bad.append(
            "clause 3: no `[caps] save: planet P trailer N records` line (nothing was appended to the planet file)"
        )
    elif not any(int(m.group("n")) == int(s.group("ovf")) for m in tr):
        bad.append(
            "clause 3: no trailer carries all %s overflow records (trailers: %s)"
            % (s.group("ovf"), ",".join(m.group("n") for m in tr))
        )
    if not any(ln.startswith(ACCEPTED) for ln in net):
        bad.append("clause 3: no `[caps] load: raised save accepted`")
    ap = [m for m in (APPLIED_RE.search(ln) for ln in net) if m]
    if not ap:
        bad.append(
            "clause 3: no `[caps] load: planet file overflow N records applied` (the load tail never applied the trailer)"
        )
    elif not any(int(m.group("n")) == int(s.group("ovf")) for m in ap):
        bad.append(
            "clause 3: the load applied %s records, the save wrote %s"
            % (",".join(m.group("n") for m in ap), s.group("ovf"))
        )
    bad += _crash(net, harness)
    return bad


def refuse_problems(net, harness):
    net, harness = strip(net), strip(harness)
    bad = _common_banners(net, False)
    if not any(ln.startswith(REFUSE_VERSION) for ln in net):
        bad.append("clause 2: no `%s` (the stamped header was not refused)" % REFUSE_VERSION)
    if not any(ln.startswith(REFUSE_SHOWN) for ln in net):
        bad.append("clause 2: no `%s` (the message hook did not run)" % REFUSE_SHOWN)
    if any(ln.startswith(ACCEPTED) for ln in net):
        bad.append("clause 3: `raised save accepted` with the raise OFF")
    if any(APPLIED_RE.search(ln) for ln in net):
        bad.append("clause 3: an overflow trailer was APPLIED with the raise OFF")
    if any(ln.startswith("; CAP ") and ("SAVE" in ln or "LOAD" in ln) for ln in harness):
        bad.append("clause 3: the harness saved / loaded in a refuse step")
    if any(STEP_LINE.match(ln) for ln in harness):
        bad.append("clause 3: simulation started after a refused save")
    if any(CONTAINER_RE.match(ln) for ln in net):
        bad.append("clause 3: full-container preflight accepted a refused save")
    bad += _crash(net, harness)
    return bad


STEP_LINE = re.compile(r"^(\d+) [0-9A-Fa-f]{16} ")
PLAYS_STEPS = 3000  # a vanilla save must PLAY: the per-step hash log reaches at least this many steps past the load


def vanilla_problems(net, harness):
    net, harness = strip(net), strip(harness)
    bad = _common_banners(net, True)
    steps = [int(m.group(1)) for m in (STEP_LINE.match(ln) for ln in harness) if m]
    if not steps or max(steps) < PLAYS_STEPS:
        bad.append(
            "clause 3: the vanilla save did not PLAY: last per-step hash line is step %s, need >= %d"
            % (max(steps) if steps else "none", PLAYS_STEPS)
        )
    for ln in net:
        if "[caps]" in ln and "REFUSED" in ln:
            bad.append("clause 2: " + ln)
    if any(ln.startswith(ACCEPTED) for ln in net):
        bad.append("clause 2: `raised save accepted` for a VANILLA save (it must not be stamped)")
    if any(APPLIED_RE.search(ln) for ln in net):
        bad.append("clause 2: an overflow trailer was applied from a vanilla save")
    bad += _crash(net, harness)
    return bad


OLD_PLAYS_STEPS = 300  # "a few hundred sim steps"
GUARD_RE = re.compile(r"guard=0x(?P<g>[0-9A-Fa-f]+)")


def old_problems(net, harness, records, plays=OLD_PLAYS_STEPS):
    """An old raised save loads with the raise ON on the current build: accepted, every MHOV record applied, it plays, no guard hit."""
    net, harness = strip(net), strip(harness)
    bad = _common_banners(net, True)
    for ln in net:
        if "[caps]" in ln and "REFUSED" in ln:
            bad.append("clause 2: " + ln)
        if "GUARD HIT" in ln:
            bad.append("clause 5: " + ln)
        m = GUARD_RE.search(ln)
        if m and int(m.group("g"), 16) != 0:
            bad.append("clause 5: a cap guard is non-zero: " + ln)
    if not any(ln.startswith(ACCEPTED) for ln in net):
        bad.append("clause 2: no `%s` (the old raised save was not accepted)" % ACCEPTED)
    ap = [m for m in (APPLIED_RE.search(ln) for ln in net) if m]
    if records == 0:
        if ap:
            bad.append(
                "clause 3: %s MHOV records applied, the loaded planet's file carries none"
                % ap[-1].group("n")
            )
    elif not ap:
        bad.append(
            "clause 3: no `[caps] load: planet file overflow N records applied` (the trailer was not applied)"
        )
    elif int(ap[-1].group("n")) != records:
        bad.append(
            "clause 3: %s MHOV records applied, the file carries %d" % (ap[-1].group("n"), records)
        )
    steps = [int(m.group(1)) for m in (STEP_LINE.match(ln) for ln in harness) if m]
    if not steps or max(steps) < plays:
        bad.append(
            "clause 4: the loaded save did not PLAY: last per-step hash line is step %s, need >= %d"
            % (max(steps) if steps else "none", plays)
        )
    bad += _crash(net, harness)
    return bad


def off_problems(net, harness):
    """Default-off caps still persist required policy; no overflow may be applied."""
    net, harness = strip(net), strip(harness)
    bad = []
    if not any("[caps] relocation APPLIED" in ln for ln in net):
        bad.append("clause 1: no `[caps] relocation APPLIED` in mh_net.log")
    if any("[caps] limits ON (" in ln for ln in net):
        bad.append("clause 1: `[caps] limits ON (` in a raise=OFF step (the ini leaked the raise)")
    bad += current_framing_problems(net, harness, current_raised=False)
    sv = [m for m in (SAVE_RE.match(ln) for ln in harness) if m]
    lo = [m for m in (LOAD_RE.match(ln) for ln in harness) if m]
    fl = [m for m in (FILE_RE.match(ln) for ln in harness) if m]
    if not sv or not lo or not fl:
        bad.append(
            "clause 2: no `; CAP SAVE` / `; CAP LOAD` / `; CAP SAVE FILE` line (cap_save_at never reached)"
        )
        return bad
    for tag, m in (("save", sv[-1]), ("load", lo[-1])):
        if int(m.group("rc")) != 1:
            bad.append("clause 2: the %s returned %s, not 1" % (tag, m.group("rc")))
        if any(int(m.group(k)) != 0 for k in ("ovf", "bldg", "prj")):
            bad.append("clause 2: %s counted overflow records with the raise OFF" % tag)
    if sv[-1].group("digest").upper() != lo[-1].group("digest").upper():
        bad.append("clause 2: the digest moved across the save/load")
    for ln in net:
        if (
            ln.startswith("; [caps] save: ")
            or ln.startswith(ACCEPTED)
            or APPLIED_RE.search(ln)
            or ("[caps]" in ln and "REFUSED" in ln)
        ):
            bad.append("clause 3: " + ln)
    bad += _crash(net, harness)
    return bad


# ---- the four splice sites against the clean exe -------------------------------------------------------------------------
SITES = {  # name: (VA, 5 bytes, the call target)
    "ADDR_SAVE_CLOSE": (0x004480E0, "E8EC760800", 0x004CF7D1),
    "ADDR_CONTAINER_CLOSE": (0x004475C5, "E807820800", 0x004CF7D1),
    "ADDR_HDR_READ": (0x004476B4, "E89F860800", 0x004CFD58),
    "ADDR_LOADFAIL_MSG": (0x004BB76D, "E8380F0000", 0x004BC6AA),
}
TABLE_VA, INDEX_VA, SLOT = 0x005D08F4, 0x005D09E4, 40
# PT-CAP9-U7b: the planet-LOAD splice sits behind the LAST block of the wide set, the path free-count block: `mov edx,0x20 ; mov eax,0xb63bc0 ; call
# LZW_ReadCompressedFromFile` at 0x44850c..0x448520, then the six stolen bytes `add [ebp-0x20],eax ; mov ebx,[ebp-0x28]`. A splice any earlier scatters the
# stock mirror before the path buffers / flags / count were read (every planet load lost the pool).
LOAD_SPLICE_VA = 0x0044851B
LOAD_SPLICE_BYTES = "0145E08B5DD8"
LOAD_PRE_VA, LOAD_PRE_BYTES = (
    0x0044850C,
    "BA20000000B8C03BB600",
)  # the free-count block's EDX / EAX set-up
LOAD_CALL_VA, LOAD_CALL_BYTES, LOAD_CALL_TARGET = 0x00448516, "E8CB010000", 0x004486E6


def source_sites():
    """The 5-byte expectations and addresses as caps_install.cpp spells them."""
    src = open(
        os.path.join(REPO, "src", "mh_dll", "mh", "patch", "caps_install.cpp"), encoding="utf-8"
    ).read()
    out = {}
    for name in SITES:
        a = re.search(r"constexpr uintptr_t %s\s*=\s*(0x[0-9a-fA-F]+);" % name, src)
        e = re.search(
            r"const uint8_t\s+%s_EXPECT\[5\]\s*=\s*\{([^}]*)\}"
            % {
                "ADDR_SAVE_CLOSE": "SAVE_CLOSE",
                "ADDR_CONTAINER_CLOSE": "CONTAINER_CLOSE",
                "ADDR_HDR_READ": "HDR_READ",
                "ADDR_LOADFAIL_MSG": "LOADFAIL_MSG",
            }[name],
            src,
        )
        out[name] = (
            int(a.group(1), 16) if a else None,
            "".join("%02X" % int(x, 16) for x in e.group(1).split(",") if x.strip()) if e else None,
        )
    return out


def load_splice_problems(read):
    bad = []
    src = open(
        os.path.join(REPO, "src", "mh_dll", "mh", "patch", "caps_install.cpp"), encoding="utf-8"
    ).read()
    a = re.search(r"constexpr uintptr_t ADDR_LOAD_ARRAYS_END\s*=\s*(0x[0-9a-fA-F]+);", src)
    if not a or int(a.group(1), 16) != LOAD_SPLICE_VA:
        bad.append(
            "ADDR_LOAD_ARRAYS_END in caps_install.cpp is %s, the load splice belongs behind the path free-count block at %08X"
            % (a.group(1) if a else None, LOAD_SPLICE_VA)
        )
    e = re.search(r"LOAD_SPLICE_EXPECT\[6\]\s*=\s*\{([^}]*)\}", src)
    got_src = (
        "".join("%02X" % int(x, 16) for x in e.group(1).split(",") if x.strip()) if e else None
    )
    if got_src != LOAD_SPLICE_BYTES:
        bad.append(
            "LOAD_SPLICE_EXPECT in caps_install.cpp is %s, expected %s"
            % (got_src, LOAD_SPLICE_BYTES)
        )
    if bytes(read(LOAD_SPLICE_VA, 6)).hex().upper() != LOAD_SPLICE_BYTES:
        bad.append(
            "load splice %08X: the clean exe holds %s, expected %s"
            % (LOAD_SPLICE_VA, bytes(read(LOAD_SPLICE_VA, 6)).hex().upper(), LOAD_SPLICE_BYTES)
        )
    if bytes(read(LOAD_PRE_VA, 10)).hex().upper() != LOAD_PRE_BYTES:
        bad.append(
            "load splice: the instructions before it at %08X are not the free-count block's `mov edx,0x20 ; mov eax,0xb63bc0` (the splice is not behind the path count block)"
            % LOAD_PRE_VA
        )
    call = bytes(read(LOAD_CALL_VA, 5))
    rel = int.from_bytes(call[1:5], "little", signed=True)
    if (
        call.hex().upper() != LOAD_CALL_BYTES
        or (LOAD_CALL_VA + 5 + rel) & 0xFFFFFFFF != LOAD_CALL_TARGET
    ):
        bad.append(
            "load splice: the block read at %08X is not the CALL to LZW_ReadCompressedFromFile"
            % LOAD_CALL_VA
        )
    return bad


def refusal_text_problems(def_text=None):
    """The raised-limits refusal text (player_strings.def, caps.load_refused) names 'raised limits' -- the save is refused for ANY raised
    content (buildings, units, paths ...), not for building limits alone (PT-CAP9-U7a)."""
    if def_text is None:
        def_text = open(
            os.path.join(REPO, "src", "mh_dll", "mh", "ui", "player_strings.def"), encoding="utf-8"
        ).read()
    m = re.search(r'MH_STR\(CAPS_LOAD_REFUSED,\s*"caps\.load_refused",\s*L"([^"]*)"\)', def_text)
    if not m:
        return ["no CAPS_LOAD_REFUSED row in player_strings.def"]
    t = m.group(1)
    bad = []
    if "raised limits" not in t.lower():
        bad.append("the refusal text does not say 'raised limits': " + t)
    if "building" in t.lower():
        bad.append(
            "the refusal text still says 'building' (a raised save is refused for any raised content): "
            + t
        )
    return bad


def sites_problems(read):
    """`read(va, n) -> bytes` over the clean EN image. The source constants must equal the exe's own bytes, and each `E8 rel32`
    must really call the target the stub tail jumps to."""
    bad = []
    src = source_sites()
    for name, (va, want, target) in SITES.items():
        got = bytes(read(va, 5)).hex().upper()
        if got != want:
            bad.append(
                "site %s %08X: the clean exe holds %s, this checker expects %s"
                % (name, va, got, want)
            )
        rel = int.from_bytes(bytes(read(va, 5))[1:5], "little", signed=True)
        if (va + 5 + rel) & 0xFFFFFFFF != target:
            bad.append(
                "site %s %08X: the CALL lands on %08X, not %08X"
                % (name, va, (va + 5 + rel) & 0xFFFFFFFF, target)
            )
        sv, se = src[name]
        if sv != va or se != want:
            bad.append(
                "site %s: caps_install.cpp has VA %s bytes %s, the exe has %08X %s"
                % (name, hex(sv) if sv else None, se, va, want)
            )
    bad += load_splice_problems(read)
    table = bytes(read(TABLE_VA, SLOT * 6))
    idx = int.from_bytes(bytes(read(INDEX_VA, 4)), "little")
    if idx != 5:
        bad.append(
            "version index at %08X is %d, expected 5 (the EN build's slot)" % (INDEX_VA, idx)
        )
    slots = [table[i * SLOT : (i + 1) * SLOT] for i in range(6)]
    stamp = bytearray(slots[idx if idx < 6 else 5])
    stamp[29:33] = b"MHC1"
    if any(bytes(stamp) == s for s in slots):
        bad.append("the stamp equals one of the six version strings of the clean exe")
    if len(set(slots)) != 6:
        bad.append("the six version strings of the clean exe are not distinct")
    return bad


def exe_reader(path=None):
    sys.path.insert(0, HERE)
    import scan_cap_reloc_sites as scan

    path = path or scan.default_exe()
    if not path or not os.path.isfile(path):
        return None
    img = scan.Image(open(path, "rb").read())
    return lambda va, n: img.read(va, n)


def run(run_dir, step, prj, records=0, plays=OLD_PLAYS_STEPS):
    net = check_cancel_task.net_log_lines(run_dir)
    harness = check_storage_purge.harness_log_lines(run_dir)
    if step == "off":
        bad = off_problems(net, harness)
        for ln in strip(harness):
            if ln.startswith("; CAP SAVE") or ln.startswith("; CAP LOAD"):
                print("  " + ln)
    elif step == "save":
        bad = save_problems(net, harness, prj)
        for ln in strip(harness):
            if (
                ln.startswith("; CAP SAVE")
                or ln.startswith("; CAP LOAD")
                or ln.startswith("; CAP PRJ")
            ):
                print("  " + ln)
        for ln in strip(net):
            if "[caps] save" in ln or "[caps] load" in ln:
                print("  " + ln[:200])
    elif step == "refuse":
        bad = refuse_problems(net, harness)
        for ln in strip(net):
            if "[caps] load" in ln or "[caps] save splices" in ln:
                print("  " + ln[:200])
    elif step == "old":
        bad = old_problems(net, harness, records, plays)
        for ln in strip(net):
            if "[caps] load" in ln or "[caps] limits ON" in ln or "[caps] orders step" in ln:
                print("  " + ln[:220])
    else:
        bad = vanilla_problems(net, harness)
        for ln in strip(net):
            if "[caps] load" in ln or "[caps] limits ON" in ln:
                print("  " + ln[:200])
    for b in bad:
        print("  check_cap_save: " + b)
    print("check_cap_save --step %s: %s" % (step, "FAIL" if bad else "PASS"))
    return 1 if bad else 0


GOOD_NET = [
    "; [caps] relocation APPLIED (3945 sites, 2 sections, base .mhcapd=0x040C0000) in 10.1 ms; limits OFF",
    "; [caps] limits ON (gameplay settings): buildings 500, storage 127, turrets 127, mines 127, labs 127, productions 127, projectiles 4000",
    "; [caps] save: planet 31 trailer 1060 records (203124 bytes)",
    "; [storage-save] container accepted members=80000000 policy=80000000 caps=80000000 version=5",
    "; [caps] load: raised save accepted",
    "; [caps] load: planet file overflow 1060 records applied",
]
GOOD_HARNESS = [
    "; CAP PRJ SYNTH step=2800 stamped=600 slots 1500..2099 (row 4000)",
    "; CAP SAVE step=2800 name=capovf rc=1 digest=0123456789ABCDEF ovf=1060 bldg_ovf=460 prj_ovf=600",
    "; CAP LOAD step=2800 name=capovf rc=1 digest=0123456789ABCDEF ovf=1060 bldg_ovf=460 prj_ovf=600",
    "; CAP SAVE PLANET file=save\\temp\\save31.dat size=306616 footer=1 footer_len=203124 policy_tail=1 policy_len=128",
    "; CAP SAVE FILE name=capovf size=330678 fnv=DEADBEEF header_valid=1 raised=1 policy=1",
]
REFUSE_NET = [
    "; [caps] relocation APPLIED (3945 sites, 2 sections, base .mhcapd=0x040C0000) in 10.1 ms; limits OFF",
    REFUSE_VERSION,
    REFUSE_SHOWN,
]
VANILLA_NET = GOOD_NET[:2]
PLAYS_LOG = [
    "1 40938BDCDC770859 1F46C09AC9E17F4B 4485456729C22EAC",
    "6000 4093A2FB94C8F3C7 E7919E21190E2E76 93093FB5EC6CEC3C",
]


def selftest():
    fails = []

    def expect(what, ok):
        if not ok:
            fails.append(what)
            print("  SELFTEST FAIL: " + what)

    def has(bad, frag):
        return any(frag in b for b in bad)

    expect("save: the good logs are green", save_problems(GOOD_NET, GOOD_HARNESS) == [])
    expect(
        "save: timestamps are stripped",
        save_problems(
            ["[12:00:01.5] " + x for x in GOOD_NET], ["[12:00:01.5] " + x for x in GOOD_HARNESS]
        )
        == [],
    )

    def mut(lines, i, a, b):
        out = list(lines)
        out[i] = out[i].replace(a, b)
        return out

    expect(
        "save: a digest that moved across the round trip is RED",
        has(
            save_problems(GOOD_NET, mut(GOOD_HARNESS, 2, "0123456789ABCDEF", "0123456789ABCDEE")),
            "NOT exact",
        ),
    )
    expect(
        "save: a count that moved is RED",
        has(
            save_problems(GOOD_NET, mut(GOOD_HARNESS, 2, "prj_ovf=600", "prj_ovf=599")), "prj count"
        ),
    )
    expect(
        "save: a refused load (rc=0) is RED",
        has(
            save_problems(GOOD_NET, mut(GOOD_HARNESS, 2, "rc=1", "rc=0")), "llm_game_load returned"
        ),
    )
    expect(
        "save: no building past the window is RED",
        has(
            save_problems(
                GOOD_NET,
                mut(
                    mut(GOOD_HARNESS, 1, "bldg_ovf=460", "bldg_ovf=0"),
                    2,
                    "bldg_ovf=460",
                    "bldg_ovf=0",
                ),
            ),
            "proved nothing",
        ),
    )
    expect(
        "save: too few projectile records is RED",
        has(
            save_problems(
                GOOD_NET,
                [GOOD_HARNESS[0]]
                + [x.replace("prj_ovf=600", "prj_ovf=10") for x in GOOD_HARNESS[1:]],
            ),
            "slot 1500",
        ),
    )
    expect(
        "save: a missing LOAD line is RED",
        has(save_problems(GOOD_NET, GOOD_HARNESS[:2]), "no `; CAP LOAD`"),
    )
    expect("save: a missing SAVE line is RED", has(save_problems(GOOD_NET, []), "no `; CAP SAVE`"))
    expect(
        "save: a planet file without the footer is RED",
        has(
            save_problems(GOOD_NET, mut(GOOD_HARNESS, 3, "footer=1", "footer=0")),
            "does not end in an overflow footer",
        ),
    )
    expect(
        "save: no PLANET line is RED",
        has(save_problems(GOOD_NET, GOOD_HARNESS[:3]), "CAP SAVE PLANET"),
    )
    expect(
        "save: a trailer that lost records is RED",
        has(
            save_problems(mut(GOOD_NET, 2, "1060 records", "1000 records"), GOOD_HARNESS),
            "no trailer carries all",
        ),
    )
    expect(
        "save: a load that applied fewer records is RED",
        has(
            save_problems(mut(GOOD_NET, 5, "1060 records", "1000 records"), GOOD_HARNESS),
            "load applied",
        ),
    )
    expect(
        "save: no `accepted` is RED",
        has(
            save_problems([x for x in GOOD_NET if "accepted" not in x], GOOD_HARNESS),
            "raised save accepted",
        ),
    )
    expect(
        "save: a REFUSED line is RED",
        has(save_problems(GOOD_NET + ["; [caps] load REFUSED: x"], GOOD_HARNESS), "REFUSED"),
    )
    expect(
        "save: a DESYNC banner is RED",
        has(save_problems(GOOD_NET + ["; [desync] *** DESYNC step=9"], GOOD_HARNESS), "DESYNC"),
    )
    expect(
        "save: limits OFF is RED",
        has(
            save_problems([x for x in GOOD_NET if "limits ON" not in x], GOOD_HARNESS), "limits ON"
        ),
    )
    expect("refuse: the good logs are green", refuse_problems(REFUSE_NET, []) == [])
    expect(
        "refuse: no version refusal is RED",
        has(refuse_problems([x for x in REFUSE_NET if x != REFUSE_VERSION], []), "stamped header"),
    )
    expect(
        "refuse: no message hook line is RED",
        has(refuse_problems([x for x in REFUSE_NET if x != REFUSE_SHOWN], []), "message hook"),
    )
    expect(
        "refuse: an accepted raised save is RED",
        has(refuse_problems(REFUSE_NET + [ACCEPTED], PLAYS_LOG), "raise OFF"),
    )
    expect(
        "refuse: an applied trailer is RED",
        has(refuse_problems(REFUSE_NET + [GOOD_NET[5]], PLAYS_LOG), "APPLIED"),
    )
    expect(
        "refuse: limits ON is RED", has(refuse_problems(REFUSE_NET + [GOOD_NET[1]], []), "leaked")
    )
    expect(
        "refuse: a harness save is RED",
        has(refuse_problems(REFUSE_NET, GOOD_HARNESS), "harness saved"),
    )
    expect("vanilla: the good logs are green", vanilla_problems(VANILLA_NET, PLAYS_LOG) == [])
    expect(
        "vanilla: a save that stops stepping early is RED",
        has(vanilla_problems(VANILLA_NET, PLAYS_LOG[:1]), "did not PLAY"),
    )
    expect(
        "vanilla: a REFUSED line is RED",
        has(vanilla_problems(VANILLA_NET + [REFUSE_VERSION], PLAYS_LOG), "REFUSED"),
    )
    expect(
        "vanilla: an accepted stamp is RED",
        has(vanilla_problems(VANILLA_NET + [ACCEPTED], PLAYS_LOG), "VANILLA"),
    )
    expect(
        "vanilla: an applied trailer is RED",
        has(vanilla_problems(VANILLA_NET + [GOOD_NET[5]], PLAYS_LOG), "applied"),
    )
    OLD_NET = GOOD_NET[:2] + [ACCEPTED, "; [caps] load: planet file overflow 8 records applied"]
    expect("old: the good logs are green", old_problems(OLD_NET, PLAYS_LOG, 8) == [])
    expect(
        "old: records 0 and nothing applied is green", old_problems(OLD_NET[:3], PLAYS_LOG, 0) == []
    )
    expect(
        "old: records 0 but a trailer applied is RED",
        has(old_problems(OLD_NET, PLAYS_LOG, 0), "carries none"),
    )
    expect(
        "old: a wrong record count is RED",
        has(old_problems(OLD_NET, PLAYS_LOG, 9), "the file carries 9"),
    )
    expect(
        "old: no trailer applied is RED",
        has(old_problems(OLD_NET[:3], PLAYS_LOG, 8), "not applied"),
    )
    expect(
        "old: not accepted is RED",
        has(old_problems(OLD_NET[:2] + OLD_NET[3:], PLAYS_LOG, 8), "not accepted"),
    )
    expect(
        "old: a REFUSED line is RED",
        has(old_problems(OLD_NET + [REFUSE_VERSION], PLAYS_LOG, 8), "REFUSED"),
    )
    expect(
        "old: a GUARD HIT is RED",
        has(
            old_problems(OLD_NET + ["; [caps] ORDERS GUARD HIT array=queue step=3"], PLAYS_LOG, 8),
            "clause 5",
        ),
    )
    expect(
        "old: a non-zero census guard is RED",
        has(
            old_problems(OLD_NET + ["; [caps] orders step=5 hw q=1 guard=0x2"], PLAYS_LOG, 8),
            "non-zero",
        ),
    )
    expect(
        "old: a zero census guard is green",
        old_problems(OLD_NET + ["; [caps] orders step=5 hw q=1 guard=0x0"], PLAYS_LOG, 8) == [],
    )
    expect(
        "old: a save that stops stepping early is RED",
        has(old_problems(OLD_NET, PLAYS_LOG[:1], 8), "did not PLAY"),
    )
    expect(
        "vanilla: limits OFF is RED",
        has(
            vanilla_problems([x for x in VANILLA_NET if "limits ON" not in x], PLAYS_LOG),
            "limits ON",
        ),
    )

    # the raise-OFF arm
    off_net = [
        GOOD_NET[0],
        "; [storage-save] container accepted members=80000000 policy=80000000 caps=00000000 version=5",
    ]
    off_h = [
        "; CAP SAVE step=1500 name=capovf rc=1 digest=CBF29CE484222325 ovf=0 bldg_ovf=0 prj_ovf=0",
        "; CAP SAVE FILE name=capovf size=330678 fnv=DEADBEEF header_valid=1 raised=0 policy=1",
        "; CAP SAVE PLANET file=save\\temp\\save31.dat size=306616 footer=0 footer_len=0 policy_tail=1 policy_len=128",
        "; CAP LOAD step=1500 name=capovf rc=1 digest=CBF29CE484222325 ovf=0 bldg_ovf=0 prj_ovf=0",
    ]
    expect("off: the good logs are green", off_problems(off_net, off_h) == [])
    expect(
        "off: a footer on the raise-OFF planet file is RED",
        has(
            off_problems(
                off_net, mut(off_h, 2, "footer=0 footer_len=0", "footer=1 footer_len=900")
            ),
            "footer",
        ),
    )
    expect(
        "off: a missing PLANET line is RED",
        has(off_problems(off_net, off_h[:2] + off_h[3:]), "PLANET"),
    )
    expect(
        "off: a trailer line is RED",
        has(
            off_problems(
                off_net + ["; [caps] save: planet 3 trailer 5 records (500 bytes)"], off_h
            ),
            "clause 3",
        ),
    )
    expect(
        "off: overflow counted with the raise OFF is RED",
        has(off_problems(off_net, mut(off_h, 0, "ovf=0", "ovf=2")), "overflow records"),
    )
    expect(
        "off: a failed load-back is RED",
        has(off_problems(off_net, mut(off_h, 3, "rc=1", "rc=0")), "load returned"),
    )
    expect(
        "off: the raise leaking is RED", has(off_problems(off_net + [GOOD_NET[1]], off_h), "leaked")
    )
    expect("off: no CAP lines is RED", has(off_problems(off_net, []), "cap_save_at never"))
    expect(
        "refuse: a process that starts simulation is RED",
        bool(refuse_problems(REFUSE_NET, PLAYS_LOG)),
    )
    expect(
        "current: footer presence without full validation is RED",
        bool(
            current_framing_problems(
                [x for x in GOOD_NET if not CONTAINER_RE.match(x)],
                GOOD_HARNESS,
                current_raised=True,
            )
        ),
    )
    expect(
        "current: requirements for a different planet are RED",
        bool(
            current_framing_problems(
                [x.replace("caps=80000000", "caps=40000000") for x in GOOD_NET],
                GOOD_HARNESS,
                current_raised=True,
            )
        ),
    )
    expect(
        "current: another legacy member need not carry policy",
        current_framing_problems(
            [x.replace("members=80000000", "members=C0000000") for x in GOOD_NET],
            GOOD_HARNESS,
            current_raised=True,
        )
        == [],
    )
    expect(
        "off: required policy must include the current planet",
        bool(
            current_framing_problems(
                [x.replace("policy=80000000", "policy=40000000") for x in off_net],
                off_h,
                current_raised=False,
            )
        ),
    )

    current_admission = next(x for x in GOOD_NET if CONTAINER_RE.match(x))
    legacy_admission = current_admission.replace("policy=80000000", "policy=00000000").replace(
        "caps=80000000", "caps=00000000"
    )
    base_net = [x for x in GOOD_NET if not CONTAINER_RE.match(x)]
    for label, source_time, reload_time in (
        ("same day", "12:00:00.000", "12:00:01.000"),
        ("midnight", "23:59:59.000", "00:00:01.000"),
    ):
        # The real reader concatenates the later session before the earlier process.
        events = [
            f"[{reload_time}] {current_admission}",
            f"[{source_time}] {legacy_admission}",
        ]
        expect(
            label + ": later raised reload wins over legacy source",
            not save_problems(base_net + events, GOOD_HARNESS),
        )
        invalid_reload = [
            f"[{reload_time}] {legacy_admission}",
            f"[{source_time}] {current_admission}",
        ]
        expect(
            label + ": later invalid reload cannot borrow earlier acceptance",
            bool(save_problems(base_net + invalid_reload, GOOD_HARNESS)),
        )

    # the four sites against a fake image: planted drift must go red
    real = {}
    for name, (va, hx, tgt) in SITES.items():
        real[va] = bytes.fromhex(hx)
    real[LOAD_SPLICE_VA] = bytes.fromhex(LOAD_SPLICE_BYTES)
    real[LOAD_PRE_VA] = bytes.fromhex(LOAD_PRE_BYTES)
    real[LOAD_CALL_VA] = bytes.fromhex(LOAD_CALL_BYTES)
    table = b"".join((b"slot%d" % i).ljust(40, b"\0") for i in range(6))

    def reader(drift=None, idx=5):
        def read(va, n):
            if drift and va == drift:
                return bytes.fromhex("9090909090")[:n]
            if va in real:
                return real[va][:n]
            if va == TABLE_VA:
                return table[:n]
            if va == INDEX_VA:
                return idx.to_bytes(4, "little")
            raise KeyError(hex(va))

        return read

    expect("sites: the fake clean image is green", sites_problems(reader()) == [])
    expect(
        "sites: a drifted site is RED",
        has(sites_problems(reader(drift=0x004475C5)), "ADDR_CONTAINER_CLOSE"),
    )
    expect(
        "sites: a wrong version index is RED", has(sites_problems(reader(idx=4)), "version index")
    )
    expect(
        "sites: a load splice that moved off the free-count block is RED",
        has(sites_problems(reader(drift=LOAD_SPLICE_VA)), "load splice"),
    )
    expect(
        "sites: a load splice with the wrong block before it is RED",
        has(sites_problems(reader(drift=LOAD_PRE_VA)), "free-count block"),
    )
    good_def = 'MH_STR(CAPS_LOAD_REFUSED, "caps.load_refused", L"This save uses raised limits. Turn on Raised limits in the launcher to load it.")'
    expect("text: the current refusal row is green", refusal_text_problems() == [])
    expect("text: the planted good row is green", refusal_text_problems(good_def) == [])
    expect(
        "text: 'raised building limits' is RED",
        has(
            refusal_text_problems(good_def.replace("raised limits", "raised building limits")),
            "building",
        ),
    )
    expect(
        "text: a row without 'raised limits' is RED",
        has(
            refusal_text_problems(
                good_def.replace("raised limits", "more things").replace("Raised limits", "More")
            ),
            "raised limits",
        ),
    )
    p = None
    try:
        rd = exe_reader()
    except Exception as e:  # noqa: BLE001
        rd = None
        print("check_cap_save --selftest: exe lookup failed: %s" % e)
    if rd is None:
        print(
            "check_cap_save --selftest: NOTE no clean EN exe found (MH_EN_CLEAN_EXE / machine_config POLYGON_CLEAN): the real-image site check is SKIPPED"
        )
    else:
        p = sites_problems(rd)
        expect(
            "sites: the REAL clean exe agrees with caps_install.cpp (%s)" % "; ".join(p), p == []
        )
    print(
        "check_cap_save --selftest: %s%s"
        % ("FAIL" if fails else "PASS", "" if rd else " (real-image site check skipped)")
    )
    return 1 if fails else 0


def main(argv):
    if "--selftest" in argv:
        return selftest()
    if "--sites" in argv:
        exe = None
        if "--exe" in argv:
            exe = argv[argv.index("--exe") + 1]
        rd = exe_reader(exe)
        if rd is None:
            print("check_cap_save --sites: no clean EN exe (pass --exe)")
            return 2
        bad = sites_problems(rd) + refusal_text_problems()
        for b in bad:
            print("  check_cap_save: " + b)
        print("check_cap_save --sites: %s" % ("FAIL" if bad else "PASS"))
        return 1 if bad else 0
    if "--step" not in argv:
        print(__doc__)
        return 2
    step = argv[argv.index("--step") + 1]
    if step not in ("save", "refuse", "vanilla", "off", "old"):
        print(__doc__)
        return 2
    prj = 600
    if "--prj" in argv:
        prj = int(argv[argv.index("--prj") + 1])
    records, plays = 0, OLD_PLAYS_STEPS
    if "--records" in argv:
        records = int(argv[argv.index("--records") + 1])
    if "--plays" in argv:
        plays = int(argv[argv.index("--plays") + 1])
    if step == "old" and "--records" not in argv:
        print(__doc__)
        return 2
    skip = set()
    for flag in ("--step", "--prj", "--records", "--plays"):
        if flag in argv:
            skip.add(argv.index(flag) + 1)
    dirs = [a for i, a in enumerate(argv) if not a.startswith("--") and i not in skip]
    if not dirs:
        print(__doc__)
        return 2
    return max(run(d, step, prj, records, plays) for d in dirs)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
