#!/usr/bin/env python3
# tools/provision_rig.py -- bring a fresh/relocated machine's game installs to a RIG-BOOTABLE state.
#
# The UI/soak/determinism rig launches the real game (mh.focus.exe) with mh.dll injected. A hand-copied
# game tree is NOT bootable as-is; four things a full installer would have done are missing, and every
# one of them fails SILENTLY (the game hangs before its first frame, or the map list comes up empty):
#
#   1. THE REGISTRY KEY.  HKLM\SOFTWARE\WOW6432Node\Techland\Mission Humanity must EXIST or the game's
#      whole boot body no-ops and it hangs on a blank "Ok" modal (the disc-check RE; tools/data/mh_registry.reg).
#   2. mh.focus.exe.  The run-without-focus + no-CD + mh.dll-import patched exe. Built here per install
#      from mh.exe via  net_load(_EN) -> no_cd -> run_without_focus  (mp_run.build_focus). Without the
#      net_load stage the exe has no mh.dll import and the DLL never loads (game boots but does nothing).
#   3. mh.dll DEPLOYED into each install (the freshly built one wins; make_lane also copies it per lane).
#   4. GAME CONTENT: Maps\*.mpm (else the "Available maps" list is empty and no match can start) and
#      save\*.sav (for the sp/loadgame + determinism vehicles). These are uploaded artifacts, not built
#      here -- this script only CHECKS them and tells you what is missing.
#   5. THE INBOUND FIREWALL RULE for the rig's whole port band. Windows' default inbound action is
#      BLOCK, so a peer that binds a lane port is reachable by nobody -- and the block is a silent DROP,
#      so the dialling peer sits in connect() until WSAETIMEDOUT (10060) and the runner reports what
#      looks like a discovery failure. Found 2026-08-29: both rig VMs had exactly ONE hand-made rule,
#      `MH host 6501`, so the DEFAULT port worked and every lane port (6600.., 6620, 6631, 6700..) was
#      dropped. That is why 2-peer capture tests on 6501 passed for months while the first run to use a
#      lane port (the 3-peer determinism attempt) failed, and why it failed identically from the dev box
#      and from the other VM -- it was never a host-specific reachability problem. Measured A/B against
#      one live listener: rule off -> both callers time out at 21 s; rule on -> both connect in <0.5 s.
#
# Run (ELEVATED shell -- the registry write needs admin):
#     python tools/provision_rig.py            # provision EN (+ RU clean if present) + apply reg + check content
#     python tools/provision_rig.py --check    # report only, change nothing
#     python tools/provision_rig.py --validate # provision, then run a plain all-AI soak and assert it steps
#     python tools/provision_rig.py --peers ''  # skip the ssh firewall pass (local machine only)
#
# Validated 2026-08-19 (the machine relocated 2026-08-17 had NONE of the four; a plain soak went from
# "did not present a frame" to  ui_test: PASS  converted=1 spawn-verified=1/1  hashed steps 290/300).

import argparse
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import machine_config as machine  # noqa: E402
import mp_run  # reuse build_focus (no_cd + run_without_focus chain)  # noqa: E402

PATCHER = os.path.join(REPO, "src", "patcher")
MHPATCH = os.path.join(PATCHER, "mhpatch.py")
DLL = os.path.join(REPO, "src", "mh_dll", "Release", "mh.dll")
SHIM = os.path.join(REPO, "src", "mh_dll", "Release", "msvfw32.dll")
REG_FILE = os.path.join(HERE, "data", "mh_registry.reg")
REG_KEY = r"HKLM\SOFTWARE\WOW6432Node\Techland\Mission Humanity"
PROBE = os.path.join(HERE, "udp_rtt_probe.py")  # mp:T3b's independent RTT probe (TL-PROBEDEPLOY)

# The rig's inbound port band. Every port the harness can hand a peer must be inside it:
#   6501            ui_test/mp_run default ([net] port)
#   6600 + n        test_ui LOCAL_PORT_BASE, one per concurrent test
#   6620            DET_LOCAL_PORT
#   6600 + lane     the single-lane runners (soak / tactical / ui_play / sp-det) -- lane numbers come
#                   from tools/lane_alloc.py, so this band moves when a block there does
#   6700 + n        test_ui LOCAL_SHIM_PORT_BASE (the msvfw32 shim tests)
# Widen HI rather than adding a second rule if a new band appears in test_ui.py.
FW_LO, FW_HI = 6500, 6799
FW_RULE = "MH rig lanes %d-%d" % (FW_LO, FW_HI)

# The rig launches lanes provisioned (make_lane) from the EN POLYGON, so that is the only install that
# MUST be rig-bootable. The clean/RU trees are pristine references (Ghidra imports, diffing) and are
# deliberately left unpatched. Pass --with-clean to also build a focus exe into them (e.g. for an RU
# rig run); RU shares the data layout so its net_load manifest differs only by VA.
INSTALLS = [
    ("EN polygon", machine.POLYGON, "net_load_EN.mh.patch.json", True),
    ("EN clean", machine.POLYGON_CLEAN, "net_load_EN.mh.patch.json", False),
    ("RU clean", getattr(machine, "RU_CLEAN", None), "net_load.mh.patch.json", False),
]

OK, WARN, FAIL = "OK", "WARN", "FAIL"


def _p(status, msg):
    tag = {OK: "  ok ", WARN: " warn", FAIL: "FAIL "}[status]
    print(f"[{tag}] {msg}")


def reg_key_present():
    r = subprocess.run(["reg", "query", REG_KEY], capture_output=True, text=True)
    return r.returncode == 0


def apply_registry(check_only):
    if reg_key_present():
        _p(OK, f"registry key present: {REG_KEY}")
        return True
    if check_only:
        _p(FAIL, f"registry key MISSING: {REG_KEY}  (reg import {os.path.relpath(REG_FILE, REPO)})")
        return False
    r = subprocess.run(["reg", "import", REG_FILE], capture_output=True, text=True)
    if r.returncode == 0 and reg_key_present():
        _p(OK, f"registry key created from {os.path.relpath(REG_FILE, REPO)}")
        return True
    _p(FAIL, f"reg import failed (run an ELEVATED shell): {r.stderr.strip() or r.stdout.strip()}")
    return False


def _fw_ps(check_only):
    """PowerShell that reports (or creates) the inbound allow rules, one line per protocol.

    Scoped to -RemoteAddress LocalSubnet: the rig is a LAN of peers, and nothing outside it has any
    business reaching a game port. -Profile Any because a VM's adapter may come up Public or Private
    depending on how the network was classified at first boot, and a rule that only covers the profile
    that happened to be active is the same silent failure one layer down.
    """
    out = []
    for proto in ("TCP", "UDP"):
        name = "%s %s" % (FW_RULE, proto)
        if check_only:
            out.append(
                "if (Get-NetFirewallRule -DisplayName '{n}' -EA SilentlyContinue | "
                "Where-Object {{ $_.Enabled -eq 'True' }}) {{ 'FW {p} PRESENT' }} "
                "else {{ 'FW {p} MISSING' }}".format(n=name, p=proto)
            )
        else:
            out.append(
                "if (Get-NetFirewallRule -DisplayName '{n}' -EA SilentlyContinue) {{ "
                "Enable-NetFirewallRule -DisplayName '{n}'; 'FW {p} PRESENT' }} else {{ "
                "New-NetFirewallRule -DisplayName '{n}' -Direction Inbound -Action Allow "
                "-Protocol {p} -LocalPort {lo}-{hi} -Profile Any -RemoteAddress LocalSubnet "
                "| Out-Null; 'FW {p} CREATED' }}".format(n=name, p=proto, lo=FW_LO, hi=FW_HI)
            )
    return "; ".join(out)


def _fw_report(where, text, check_only):
    """Turn the PS output into one status line. Returns True if both protocols are covered."""
    got = {}
    for line in text.splitlines():
        f = line.split()
        if len(f) == 3 and f[0] == "FW":
            got[f[1]] = f[2]
    missing = [p for p in ("TCP", "UDP") if got.get(p) not in ("PRESENT", "CREATED")]
    if missing:
        _p(
            FAIL,
            "%s: inbound rule %s %s -- %s"
            % (
                where,
                FW_RULE,
                "/".join(missing) + " MISSING",
                "run without --check to create it"
                if check_only
                else "creation failed (needs an ELEVATED shell)",
            ),
        )
        return False
    made = [p for p in ("TCP", "UDP") if got[p] == "CREATED"]
    _p(
        OK,
        "%s: inbound %d-%d allowed (%s)"
        % (where, FW_LO, FW_HI, ("created " + "+".join(made)) if made else "already present"),
    )
    return True


def apply_firewall(check_only, peers):
    """Ensure the rig port band is allowed inbound here and on each peer.

    A peer we cannot reach is a WARN, not a FAIL -- same convention as a VM-down UI test being
    SKIPped: this script must stay runnable with the VMs powered off.
    """
    ok = True
    ps = _fw_ps(check_only)
    r = subprocess.run(["powershell", "-NoProfile", "-Command", ps], capture_output=True, text=True)
    ok &= _fw_report("local", r.stdout or "", check_only)

    for ip in peers:
        r = mp_run.ssh(
            machine.SSH_KEY, machine.VM_USER, ip, 'powershell -NoProfile -Command "%s"' % ps
        )
        if r.returncode != 0:
            _p(WARN, "peer %s: unreachable, firewall NOT checked (%s)" % (ip, mp_run._ssh_err(r)))
            continue
        ok &= _fw_report("peer %s" % ip, r.stdout or "", check_only)
    return ok


def deploy_probe(check_only, peers):
    """Place tools/udp_rtt_probe.py (mp:T3b's independent RTT instrument) on each rig peer.

    Unlike mh.dll/mh.focus.exe/the UI harness script, the probe is not redeployed per run -- it is
    HAND-RUN when a link-condition claim needs re-verifying (`echo` on the host-slot peer, `probe`
    on the client-slot peer; see the probe's own docstring), so it only needs to be PRESENT in the
    peer's game dir (machine.VM_DIR), not launched. Before this it was scp'd by hand each time
    (TL-PROBEDEPLOY). A peer we cannot reach is a WARN, same convention as the firewall pass above --
    this script stays runnable with the VMs powered off.
    """
    ok = True
    remote_dir = machine.VM_DIR
    remote_fwd = remote_dir.replace("\\", "/")
    for ip in peers:
        r = mp_run.ssh(
            machine.SSH_KEY,
            machine.VM_USER,
            ip,
            'if exist "%s\\udp_rtt_probe.py" (echo PRESENT) else (echo ABSENT)' % remote_dir,
        )
        if r.returncode != 0:
            _p(
                WARN,
                "peer %s: unreachable, udp_rtt_probe.py NOT checked (%s)"
                % (ip, mp_run._ssh_err(r)),
            )
            continue
        present = "PRESENT" in (r.stdout or "")
        if check_only:
            _p(
                OK if present else FAIL,
                "peer %s: udp_rtt_probe.py %s"
                % (ip, "present" if present else "MISSING (python tools/provision_rig.py)"),
            )
            ok &= present
            continue
        if present:
            _p(OK, "peer %s: udp_rtt_probe.py already present" % ip)
            continue
        dst = "%s@%s:%s/udp_rtt_probe.py" % (machine.VM_USER, ip, remote_fwd)
        r2 = mp_run.scp(machine.SSH_KEY, PROBE, dst)
        if r2.returncode != 0:
            _p(FAIL, "peer %s: scp of udp_rtt_probe.py failed" % ip)
            ok = False
            continue
        _p(OK, "peer %s: deployed udp_rtt_probe.py" % ip)
    return ok


def build_focus_for(label, install, net_manifest, check_only):
    if not install or not os.path.isdir(install):
        _p(WARN, f"{label}: install dir absent ({install}) -- skipping")
        return None
    exe = os.path.join(install, "mh.exe")
    focus = os.path.join(install, "mh.focus.exe")
    if not os.path.isfile(exe):
        _p(WARN, f"{label}: no mh.exe in {install} -- skipping")
        return None
    if check_only:
        _p(
            OK if os.path.isfile(focus) else FAIL,
            f"{label}: mh.focus.exe {'present' if os.path.isfile(focus) else 'MISSING'}",
        )
        return os.path.isfile(focus)
    # mh.exe -> net_load (adds the mh.dll import) -> mh.mp.exe -> build_focus (no_cd + run_without_focus)
    mp_exe = os.path.join(install, "mh.mp.exe")
    man = os.path.join(PATCHER, net_manifest)
    r = subprocess.run(
        [sys.executable, MHPATCH, "apply", exe, man, mp_exe], capture_output=True, text=True
    )
    out = (r.stdout or "") + (r.stderr or "")
    if r.returncode != 0 or any(
        t in out for t in ("GUARD-FAIL", "UNRESOLVED", "checksum mismatch")
    ):
        _p(
            FAIL,
            f"{label}: net_load ({net_manifest}) failed -- {out.strip().splitlines()[-1] if out.strip() else '?'}",
        )
        return False
    try:
        mp_run.build_focus(mp_exe, focus)
    except Exception as e:  # noqa: BLE001
        _p(FAIL, f"{label}: build_focus failed: {e}")
        return False
    _p(OK, f"{label}: built mh.focus.exe ({os.path.getsize(focus)} B)")
    return True


def deploy_shim(label, install, check_only):
    """Deploy the msvfw32 proxy shim next to the exe.

    Since 2026-08-27 the rig runs a byte-for-byte RETAIL mh.exe by default and mh.dll is force-loaded
    by this shim. Without it the game boots as a clean retail copy with NO mh.dll
    -- which is the silent-inert failure this whole script exists to catch, so a miss is FAIL, not a
    warning. Built by the msvfw32_shim project in mh.sln, so any machine that built mh.dll has it.
    """
    if not install or not os.path.isdir(install):
        return None
    dst = os.path.join(install, "msvfw32.dll")
    if not os.path.isfile(SHIM):
        _p(FAIL, f"{label}: no built msvfw32 shim at {SHIM} -- build the solution first")
        return False
    if check_only:
        present = os.path.isfile(dst)
        _p(
            OK if present else FAIL,
            f"{label}: msvfw32 shim {'deployed' if present else 'NOT deployed'}",
        )
        return present
    import shutil

    shutil.copy2(SHIM, dst)
    _p(OK, f"{label}: deployed the msvfw32 shim")
    return True


def deploy_dll(label, install, check_only):
    if not install or not os.path.isdir(install):
        return None
    dst = os.path.join(install, "mh.dll")
    if not os.path.isfile(DLL):
        _p(FAIL, f"{label}: no built mh.dll at {DLL} -- build the solution first")
        return False
    if check_only:
        present = os.path.isfile(dst)
        _p(OK if present else FAIL, f"{label}: mh.dll {'deployed' if present else 'NOT deployed'}")
        return present
    import shutil

    shutil.copy2(DLL, dst)
    _p(OK, f"{label}: deployed mh.dll")
    return True


def check_content():
    polygon = machine.POLYGON
    maps = os.path.join(polygon, "Maps")
    mpm = [f for f in os.listdir(maps)] if os.path.isdir(maps) else []
    mpm = [f for f in mpm if f.lower().endswith(".mpm")]
    _p(
        OK if mpm else FAIL,
        f"Maps: {len(mpm)} .mpm map(s) in {maps}"
        if mpm
        else f"Maps MISSING/empty ({maps}) -- upload the game's Maps\\*.mpm (else the map list is empty)",
    )
    save = os.path.join(polygon, "save")
    savs = (
        [f for f in os.listdir(save) if f.lower().endswith(".sav")] if os.path.isdir(save) else []
    )
    _p(
        OK if savs else WARN,
        f"saves: {len(savs)} .sav in {save}"
        if savs
        else f"no .sav in {save} -- the sp/loadgame + determinism vehicles need them (the save index)",
    )
    return bool(mpm)


def main():
    ap = argparse.ArgumentParser(description="Provision game installs to a rig-bootable state.")
    ap.add_argument("--check", action="store_true", help="report only, change nothing")
    ap.add_argument(
        "--validate", action="store_true", help="after provisioning, run a plain all-AI soak"
    )
    ap.add_argument(
        "--with-clean",
        action="store_true",
        help="also build a focus exe into the clean/RU installs (default: EN polygon only)",
    )
    ap.add_argument(
        "--peers",
        default=",".join(machine.RIG_PEERS),
        help="comma-separated rig peers to firewall-provision over ssh "
        "(default: %(default)s; pass '' for local only)",
    )
    args = ap.parse_args()
    check_only = args.check

    print("=== rig provisioning (%s) ===" % ("CHECK" if check_only else "APPLY"))
    peer_list = [x for x in args.peers.split(",") if x.strip()]
    ok = True
    ok &= bool(apply_registry(check_only))
    ok &= bool(apply_firewall(check_only, peer_list))
    ok &= bool(deploy_probe(check_only, peer_list))
    for label, install, man, required in INSTALLS:
        if not required and not args.with_clean:
            continue
        r1 = build_focus_for(label, install, man, check_only)
        r2 = deploy_dll(label, install, check_only)
        r3 = deploy_shim(label, install, check_only)
        if required and (r1 is False or r2 is False or r3 is False):
            ok = False
    ok &= bool(check_content())

    if args.validate and not check_only:
        print(
            "\n=== validate: plain all-AI soak (expect ui_test: PASS, converted=1, hashed steps > 0) ==="
        )
        cmd = [
            sys.executable,
            "-u",
            os.path.join(HERE, "soak_test.py"),
            "--soak",
            "--steps",
            "300",
            "--soak-speed",
            "1000",
            "--harness-extra",
            "stop_step=290;ai_probe_step=40",
        ]
        r = subprocess.run(cmd, cwd=REPO, capture_output=True, text=True)
        tail = (r.stdout or "")[-1500:]
        print(tail)
        good = (
            "converted=1" in tail and "spawn-verified=1/1" in tail and "hashed steps: 0" not in tail
        )
        _p(
            OK if good else FAIL,
            "soak stepped the sim with a verified all-AI conversion"
            if good
            else "soak did NOT step / convert -- see the tail above",
        )
        ok &= good

    print()
    _p(
        OK if ok else FAIL,
        "rig provisioning %s" % ("complete" if ok else "INCOMPLETE -- see FAIL lines"),
    )
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
