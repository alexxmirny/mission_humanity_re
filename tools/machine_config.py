#!/usr/bin/env python3
"""machine_config.py -- the ONE place every machine-specific constant lives (bootstrap item E2).

LAN peer IPs, the game-install directories, the Hyper-V VM name / ssh key, and the Ghidra + Visual
Studio install paths used to be inlined across ~18 tools/*.py scripts. Every one of them is a
bootstrap blocker on a new machine: someone had to grep the tree and edit a dozen argparse defaults.
Now they are all HERE, and the tools reference `machine_config.<NAME>` instead.

The committed values are THIS box's values -- so nothing changes for the current machine (E2 is a
behaviour-neutral refactor). A different machine overrides them WITHOUT editing this tracked file, two
ways, checked in this order (later wins):

  1. tools/machine.local.json  -- a gitignored JSON object of {NAME: value} overrides.
  2. environment variables      -- MH_<NAME>, e.g. MH_POLYGON=D:/mh, MH_RIG_PEER_A=10.0.0.5.

So `RIG_PEER_A` is `os.environ["MH_RIG_PEER_A"]` if set, else machine.local.json's "RIG_PEER_A" if
present, else the committed default below.

Import it as:  import machine_config as machine   (then machine.POLYGON, machine.RIG_PEERS, ...)
Inspect the resolved values:  python tools/machine_config.py            (prints NAME = value, source)
                              python tools/machine_config.py --json     (machine-readable)

NOTE for pyghidra tools that strip tools/ from sys.path (the ReVA service helper): import this module BEFORE
that removal -- it is pure-stdlib and safe to import while tools/ is still on the path.
"""

import json
import os
from pathlib import Path

_HERE = Path(__file__).resolve().parent
_LOCAL_JSON = _HERE / "machine.local.json"

# ─────────────────────────── committed defaults (THIS machine) ───────────────────────────
# Slash style: game/rig paths use forward slashes (Windows accepts them everywhere these are used --
# os.path / pathlib / ssh -i); the toolchain install paths keep native backslashes to match what
# Ghidra / vswhere hand back. Both are just strings; override them per the docstring above.
_DEFAULTS = {
    # --- LAN rig peers -------------------------------------------------------------------
    # The rig is a Hyper-V host (this dev box) plus VM peers on the LAN. .37/.38 are the two VM
    # peers; .61 is the host's own LAN IP (used when the dev box takes a peer slot); .35 is mp_run's
    # legacy deploy-target VM; .200 is a deliberately-unreachable address for the "dead IP" UI test.
    "RIG_PEER_A": "192.168.0.37",  # primary VM peer (rig host slot / --vm-ip in sweeps / peers[0])
    "RIG_PEER_B": "192.168.0.38",  # second VM peer (peers[1])
    "HOST_IP": "192.168.0.61",  # this box's LAN IP (mp_run/sweeps/ui_test --host-ip default)
    "MP_VM_IP": "192.168.0.35",  # mp_run's deploy-target VM (legacy default)
    "DEAD_PEER_IP": "192.168.0.200",  # unreachable on purpose -- the "types a dead IP" UI scenario
    # --- game installs -------------------------------------------------------------------
    "GAMES_ROOT": "F:/games",  # parent of every game install below
    "POLYGON": "F:/games/mh_en",  # EN test install (the polygon); DEFAULT target for the rig
    "POLYGON_CLEAN": "F:/games/mh_en_clean",  # pristine, never-played EN install
    "RU_CLEAN": "F:/games/mh_clean",  # pristine RU install (frozen reference)
    "RU_POLYGON": "F:/games/MH",  # RU install root (historical)
    "SAVE_STORAGE": "F:/games/mh_en/saves_storage",  # committed .sav fixtures for the UI/soak tests
    "LANE_ROOT": "F:/games/mh_lanes",  # per-test parallel-lane installs (make_lane.py)
    "DEFAULT_SAV": "F:/games/mh_en/save/11.sav",  # gen_lzw_fixtures sample save
    # --- Hyper-V VM / SSH ----------------------------------------------------------------
    "VM_NAME": "Win11Gen1",  # the Hyper-V VM peer
    "VM_USER": "vmadmin",  # ssh user on the VM peers
    "VM_DIR": r"C:\games\mh",  # the game install path inside the VM (remote Windows path)
    "SSH_KEY": r"F:\HyperV\Win11Gen1\ssh\vm_key",  # private key for the VM peers
    # --- toolchain installs --------------------------------------------------------------
    "GHIDRA_INSTALL": r"F:\apps\ghidra_12.1.2_PUBLIC",  # Ghidra 12.1.2 (reva_service / pyghidra)
    "GHIDRA_VERSION": "12.1.2",  # expected Ghidra version (E3 verifies the install matches)
    "VS_INSTALL_ROOT": r"C:\Program Files\Microsoft Visual Studio\2022\Community",  # MSVC/MSBuild root
    # --- publish scrub (fork F5B) --------------------------------------------------------
    # Extra identity literals tools/lint_machine_paths.py must refuse to find anywhere in the
    # published set: a handle, an old account name, anything the git identity below does not spell.
    # Comma-separated. THE COMMITTED DEFAULT IS EMPTY AND MUST STAY EMPTY -- a lint that carries the
    # names it scrubs publishes them itself. Put yours in the GITIGNORED tools/machine.local.json
    # (or MH_IDENTITY_TOKENS). The lint also derives tokens from `git config user.name/user.email`
    # and %USERNAME%, so this is only for what those two do not spell.
    "IDENTITY_TOKENS": "",
    # --- host-global lock dir ------------------------------------------------------------
    # Where tools/hostlock.py keeps the leases that serialize the shared SINGLETONS -- the one
    # Ghidra DB's WRITES (`ghidra-write`) and the rig's VMs/ports/desktop (`rig`). It MUST be an
    # absolute path OUTSIDE any repo tree: the whole point is that a loop running in a `git worktree`
    # (../mh_loop) and your interactive session in the main tree resolve the SAME lock. A REPO-relative
    # path would give each tree its own lock and defeat the coordination. Default: a per-user dir under
    # LOCALAPPDATA; override via machine.local.json / MH_SHARED_LOCK_DIR if two trees run as different
    # users (then point both at one shared, writable dir).
    "SHARED_LOCK_DIR": os.path.join(
        os.environ.get("LOCALAPPDATA") or os.environ.get("PROGRAMDATA") or os.path.expanduser("~"),
        "mh_loop",
        "locks",
    ),
}


def _load_overrides():
    """{NAME: value} from machine.local.json then MH_<NAME> env vars (env wins). Unknown keys in the
    JSON are ignored with no error so a shared local file can carry keys for other tools too."""
    resolved = dict(_DEFAULTS)
    if _LOCAL_JSON.is_file():
        try:
            data = json.loads(_LOCAL_JSON.read_text(encoding="utf-8"))
            for k, v in data.items():
                if k in resolved:
                    resolved[k] = v
        except (ValueError, OSError) as exc:  # malformed local file must be loud, not silent
            raise RuntimeError(f"machine.local.json is unreadable: {exc}") from exc
    for k in resolved:
        env = os.environ.get("MH_" + k)
        if env is not None:
            resolved[k] = env
    return resolved


def _source_of(name):
    """Where the resolved value for NAME came from (for the --check / print output)."""
    if os.environ.get("MH_" + name) is not None:
        return "env MH_" + name
    if _LOCAL_JSON.is_file():
        try:
            if name in json.loads(_LOCAL_JSON.read_text(encoding="utf-8")):
                return "machine.local.json"
        except (ValueError, OSError):
            pass
    return "default"


# Resolve once at import and bind every key as a module-level attribute.
_RESOLVED = _load_overrides()
globals().update(_RESOLVED)

# Derived convenience values (built from the resolved scalars above).
RIG_PEERS = (_RESOLVED["RIG_PEER_A"], _RESOLVED["RIG_PEER_B"])  # [0]=host slot, [1]=client, per rig
CLANG_FORMAT = str(
    Path(_RESOLVED["VS_INSTALL_ROOT"]) / "VC" / "Tools" / "Llvm" / "bin" / "clang-format.exe"
)
# REPO_ROOT is DERIVED from this file's own location, so it is correct wherever the repo lives -- it
# is not machine-specific once derived (the hardcoded copies scattered across tools/ + src/ were).
REPO_ROOT = str(_HERE.parent)
# Unpacked game-resource roots used by the src/formats modding tools (cfg/map/sprite pipelines).
RES_UNPACK = (
    _RESOLVED["RU_CLEAN"] + "/res_unpack/data/uncompressed"
)  # pristine unpacked RU resources
BANKI = _RESOLVED["RU_POLYGON"] + "/bnk_unpack/BANKI"  # unpacked sprite banks


def as_dict():
    """All resolved scalar constants -- for bootstrap.py --check and diagnostics."""
    return dict(_RESOLVED)


def committed_defaults():
    """The COMMITTED defaults, before machine.local.json / MH_* overrides.

    lint_machine_paths.py needs precisely these and not the resolved ones: it asks "is this literal
    in a COMMITTED file one of the values this repo documents as its default?", and a local override
    must not be able to change that answer -- otherwise a machine.local.json entry could quietly
    legitimise a hardcode in a file everyone else publishes."""
    return dict(_DEFAULTS)


def _main():
    import argparse

    ap = argparse.ArgumentParser(description="print the resolved machine configuration")
    ap.add_argument("--json", action="store_true", help="emit JSON instead of a table")
    args = ap.parse_args()
    if args.json:
        print(json.dumps(_RESOLVED, indent=2))
        return
    print(f"machine config  (local file: {_LOCAL_JSON if _LOCAL_JSON.is_file() else 'none'})\n")
    width = max(len(k) for k in _RESOLVED)
    for k in _DEFAULTS:  # stable, grouped order
        print(f"  {k:<{width}} = {_RESOLVED[k]:<34}  [{_source_of(k)}]")
    print(f"\n  {'RIG_PEERS':<{width}} = {RIG_PEERS}  (derived)")
    print(f"  {'CLANG_FORMAT':<{width}} = {CLANG_FORMAT}  (derived)")
    print(f"  {'REPO_ROOT':<{width}} = {REPO_ROOT}  (derived from __file__)")
    print(f"  {'RES_UNPACK':<{width}} = {RES_UNPACK}  (derived)")
    print(f"  {'BANKI':<{width}} = {BANKI}  (derived)")


if __name__ == "__main__":
    _main()
