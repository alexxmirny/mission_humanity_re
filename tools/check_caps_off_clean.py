#!/usr/bin/env python3
"""check_caps_off_clean.py -- PT-CAP9-U8b: after a raised match, the toggle layers' code sites are the CLEAN exe's bytes.

THE BUG THIS GATES. inmem_patch.cpp toggle(OFF) used to write back only the mask-0xff bytes of each site; a site with a
wildcard byte (48 of the 92 units-limit sites) kept the ON image there, while toggle_current() still called it OFF. A
raise=0 client that adopted the host's raise=1 for one match therefore ran its NEXT game on corrupted code. A crash at
the game over (the revert flipped the sites mid-sim-step) was the visible half; this is the silent half.

HOW. The row sets `[caps] post_match_dump=1`. After the session-close revert the DLL writes each toggle layer's live
site bytes (the parity dump's `extent site <i> <va> <hex>` lines) to `<logs>\\caps_post_match_<pid>.<layer>`, plus the two
RELOCATION layers' (`.reloc`, `.units`). This check reads those files for each peer (attributed to the run by time, like
check_no_crash_marker) and compares every toggle-layer site with the EXPECTED image: the bytes at the same VA in the
CLEAN EN exe (MH_EN_CLEAN_EXE, else machine_config POLYGON_CLEAN/mh.exe), overlaid with the relocation layers' live
bytes. The overlay is the definition of "original" for the wildcard bytes, not a loophole: a limit site's masked-off
operand is an address the boot-time relocation layer rewrote (48 of the 92 units-limit sites), and it must come back as
THAT -- not as the clean exe's pre-relocation address, and not as the ON image (the bug).

Only the peer that REVERTS writes a dump: the host keeps its own raise at the session close (the checker skips it).

Usage:  check_caps_off_clean.py <host run dir> <client run dir>   (test_ui: post_check_peers)
        check_caps_off_clean.py --selftest
Exit 0 = PASS, 1 = a site differs from the clean exe, 2 = REFUSED (the evidence is not there: no dump, no clean exe).
"""

import glob
import os
import re
import shutil
import struct
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

LAYERS = ("limits", "units.limits", "ai", "orders")
RELOC_LAYERS = ("reloc", "units")
DUMP_RE = re.compile(r"caps_post_match_(\d+)\.(limits|units\.limits|ai|orders|reloc|units)$")


class Refusal(Exception):
    pass


def clean_exe_path():
    p = os.environ.get("MH_EN_CLEAN_EXE")
    if p:
        return p
    import machine_config as machine

    return machine.POLYGON_CLEAN + "/mh.exe"


class Pe:
    """VA -> bytes for a PE file, from its section table (no third-party parser)."""

    def __init__(self, data):
        self.data = data
        e = struct.unpack_from("<I", data, 0x3C)[0]
        if data[e : e + 4] != b"PE\0\0":
            raise Refusal("not a PE file")
        nsec = struct.unpack_from("<H", data, e + 6)[0]
        optsz = struct.unpack_from("<H", data, e + 20)[0]
        self.base = struct.unpack_from("<I", data, e + 24 + 28)[0]
        so = e + 24 + optsz
        self.rows = []
        for i in range(nsec):
            vsz, va, rawsz, raw = struct.unpack_from("<IIII", data, so + 40 * i + 8)
            self.rows.append((self.base + va, vsz, raw, rawsz))

    def read(self, va, n):
        for b, vsz, raw, rawsz in self.rows:
            if b <= va < b + max(vsz, rawsz):
                off = raw + (va - b)
                if off + n <= raw + rawsz:
                    return self.data[off : off + n]
        return None


def parse_sites(path):
    """-> [(va, bytes)] from a parity dump's `extent site` lines."""
    out = []
    with open(path, encoding="ascii", errors="replace") as fh:
        for line in fh:
            t = line.split()
            if len(t) >= 4 and t[0] == "extent" and t[1] == "site":
                out.append((int(t[3], 16), bytes.fromhex(t[4]) if len(t) > 4 else b""))
    return out


def dumps_for(run_dir):
    """The post-match dumps under the lane's logs dir written at or after this run dir's creation."""
    run_dir = os.path.abspath(run_dir).rstrip("\\/")
    if not os.path.isfile(os.path.join(run_dir, "mh_net.log")):
        raise Refusal("%s has no mh_net.log" % run_dir)
    logs = os.path.dirname(run_dir)
    if os.path.basename(logs) != "logs":
        raise Refusal("%s is not a run directory under a lane's logs/" % run_dir)
    t0 = os.path.getctime(run_dir) - 1.0
    found = {}
    for fp in glob.glob(os.path.join(logs, "caps_post_match_*.*")):
        m = DUMP_RE.search(os.path.basename(fp))
        if m and os.path.getmtime(fp) >= t0:
            found.setdefault(m.group(2), []).append(fp)
    return found


def check(run_dirs, pe):
    if len(run_dirs) != 2:
        raise Refusal(
            "want exactly two peer run directories (host, client), got %d" % len(run_dirs)
        )
    fails = []
    for role, rd in zip(("host", "client"), run_dirs):
        found = dumps_for(rd)
        if "limits" not in found and role == "host":
            # The host is the peer that RAISES: its own settings keep the raise at the session close, so it
            # reverts nothing and may write no dump. The client (raise=0 locally) is the one that must.
            print("  host   no post-match dump (it keeps its own raise) -- skipped")
            continue
        if "limits" not in found:
            raise Refusal(
                "%s: no caps_post_match_*.limits dump since the run began -- did the row set "
                "[caps] post_match_dump=1, did the match end through the session close, and is MH_LOG_ROOT set?"
                % role
            )
        nsites = 0
        overlay = {}  # VA -> byte the boot-time relocation layers own
        for layer in RELOC_LAYERS:
            for fp in found.get(layer, []):
                for va, live in parse_sites(fp):
                    for k, b in enumerate(live):
                        overlay[va + k] = b
        for layer in LAYERS:
            for fp in found.get(layer, []):
                sites = parse_sites(fp)
                if not sites:
                    fails.append("%s %s: %s holds no sites" % (role, layer, os.path.basename(fp)))
                    continue
                bad = 0
                for va, live in sites:
                    clean = pe.read(va, len(live))
                    if clean is None:
                        raise Refusal("%08X is not inside the clean exe's image" % va)
                    clean = bytes(overlay.get(va + k, b) for k, b in enumerate(clean))
                    nsites += 1
                    if clean != live:
                        bad += 1
                        if bad <= 3:
                            fails.append(
                                "%s %s: site %08X live %s != expected %s"
                                % (role, layer, va, live.hex(), clean.hex())
                            )
                print(
                    "  %-6s %-13s %3d sites, %d differ from the expected image"
                    % (role, layer, len(sites), bad)
                )
        print("  %-6s %d site(s) compared" % (role, nsites))
    for f in fails:
        print("  [FAIL] %s" % f)
    if fails:
        print("check_caps_off_clean: FAIL (%d)" % len(fails))
        return 1
    print(
        "check_caps_off_clean: PASS -- every toggle-layer site is back at its original bytes after the match"
    )
    return 0


# ---- selftest ---------------------------------------------------------------------------------


def _fake_pe():
    """A minimal PE: one .text section at VA 0x1000 (image base 0x400000) holding bytes 0..255 repeating."""
    e = 0x80
    data = bytearray(0x400 + 0x1000)
    data[0:2] = b"MZ"
    struct.pack_into("<I", data, 0x3C, e)
    data[e : e + 4] = b"PE\0\0"
    struct.pack_into("<H", data, e + 6, 1)
    struct.pack_into("<H", data, e + 20, 0xE0)
    struct.pack_into("<I", data, e + 24 + 28, 0x400000)
    so = e + 24 + 0xE0
    struct.pack_into("<IIII", data, so + 8, 0x1000, 0x1000, 0x1000, 0x400)
    for i in range(0x1000):
        data[0x400 + i] = (i * 7 + 3) & 0xFF
    return Pe(bytes(data))


def plant(root, role, corrupt=False, dump=True, layers=("limits", "ai")):
    logs = os.path.join(root, role, "logs")
    run = os.path.join(logs, "20261022T060000Z_menu_" + role)
    os.makedirs(run)
    with open(os.path.join(run, "mh_net.log"), "w") as fh:
        fh.write("[00:00:00.000] ; [build] mh 0.0.0-dev\n")
    if dump:
        pe = _fake_pe()
        # the boot-time relocation layer rewrote the 4-byte operand at +3 of the second site (the toggle layer's wildcard bytes)
        with open(os.path.join(logs, "caps_post_match_4242.units"), "w") as fh:
            fh.write("# mh in-memory static-patch parity dump v1\nmanifest reloc\n")
            fh.write("extent site 0 00401237 %s\nend\n" % bytes.fromhex("aa00ac03").hex())
        for layer in layers:
            with open(os.path.join(logs, "caps_post_match_4242.%s" % layer), "w") as fh:
                fh.write("# mh in-memory static-patch parity dump v1\nmanifest x\n")
                for i, va in enumerate((0x401010, 0x401234, 0x4015F0)):
                    b = bytearray(pe.read(va, 7))
                    if i == 1:
                        b[3:7] = bytes.fromhex(
                            "aa00ac03"
                        )  # the relocated operand: NOT the clean exe's bytes
                        if corrupt:
                            b[3] ^= 0x5A  # a wildcard byte that kept the ON image
                    fh.write("extent site %d %08X %s\n" % (i, va, bytes(b).hex()))
                fh.write("end\n")
    return run


def selftest():
    pe = _fake_pe()
    ok_pe = pe.read(0x401010, 4) == bytes(((0x10 * 7 + 3 + k * 7) & 0xFF) for k in range(4))
    cases = [
        ("green: both peers' sites are the clean bytes", dict(), dict(), 0),
        ("RED: a corrupt WILDCARD byte on the client", dict(), dict(corrupt=True), 1),
        ("RED: a corrupt site on the host", dict(corrupt=True), dict(), 1),
        ("REFUSED: the client wrote no dump", dict(), dict(dump=False), 2),
        ("green: the host wrote no dump (it keeps its raise)", dict(dump=False), dict(), 0),
    ]
    bad = 0 if ok_pe else 1
    print("  [%s] the fake PE reader maps VA -> bytes" % ("ok" if ok_pe else "BAD"))
    for title, hk, ck, want in cases:
        root = tempfile.mkdtemp(prefix="caps_off_clean_selftest_")
        try:
            h = plant(root, "host", **hk)
            c = plant(root, "client", **ck)
            try:
                got = check([h, c], pe)
            except Refusal as e:
                print("  [REFUSED] %s" % e)
                got = 2
        finally:
            shutil.rmtree(root, ignore_errors=True)
        good = got == want
        bad += 0 if good else 1
        print("  [%s] %-58s want=%s got=%s" % ("ok" if good else "BAD", title, want, got))
    print("selftest: %s" % ("PASS" if bad == 0 else "FAIL (%d)" % bad))
    return 0 if bad == 0 else 1


def main(argv):
    if "--selftest" in argv:
        return selftest()
    dirs = [a for a in argv if not a.startswith("--")]
    try:
        with open(clean_exe_path(), "rb") as fh:
            pe = Pe(fh.read())
        return check(dirs, pe)
    except (Refusal, OSError) as e:
        print("[REFUSED] %s" % e)
        return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
