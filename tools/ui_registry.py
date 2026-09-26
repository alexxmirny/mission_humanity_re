#!/usr/bin/env python3
"""The UI-suite scenario registries, loaded from tools/uiscripts/registry.yaml and schema-checked.

Four registries live in the one data file: `tests` (the capture suite, test_ui.TESTS),
`sim_scenarios`, `uirec_scenarios` and `tact_scenarios`. Every row is checked against SCHEMA below:
an unknown key, a wrong-typed value, a missing required key, a duplicate key inside a row, a duplicate
row name, or an unresolved `!ref` each REFUSES the load with the registry and row name (TL-SUITE-REGDATA).

Two YAML tags carry the few values that are not literals; test_ui.py passes the names they resolve to:
    !ref NAME                    -> refs[NAME]  (a module constant or a callable, e.g. a `requires` hook)
    !frames [SECONDS, FLOOR]     -> refs["frames_for_seconds"](SECONDS, refs[FLOOR])
Without `refs` (a reader that only wants journals, e.g. coverage.py) tagged values stay unresolved
markers and are not type-checked.

    python tools/ui_registry.py --check      # load + validate the committed file
    python tools/ui_registry.py --selftest   # planted negatives, each must be refused by row name
"""

import argparse
import os
import sys
from collections import Counter

import yaml

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REGISTRY = os.path.join(REPO, "tools", "uiscripts", "registry.yaml")

# Type specs: "str" "int" "float" "bool" "callable", "list[T]", "tuple[T]" (a YAML list, loaded as a
# tuple -- consumers of these rows were written against tuples), "dict[K,V]", "record:NAME" (a nested
# mapping checked against RECORDS[NAME]), and "A|B" unions. int never accepts bool; float never
# accepts int (0 and 0.0 are different registry values).
RECORDS = {
    "shim_trigger": {"required": {"cmd": "str", "when": "list[list[str]]"}, "optional": {}},
}

SCHEMA = {
    "tests": {
        "required": {"name": "str", "kind": "str", "desc": "str", "budget_s": "int"},
        "optional": {
            "client_after_exit": "dict[int,int]",
            "client_dead_ip": "str",
            "client_expect_exit": "list[int]",
            "client_game_name": "str",
            "client_shares_lane": "dict[int,int]",
            "clients": "list[str]",
            "deploy_save": "str",
            "expect_red": "str",
            "extra_ini": "str|list[str]",
            "extra_ini_client": "str",
            "extra_ini_host": "str",
            "harness_extra": "str",
            "harness_extra_host": "str",
            "host": "str",
            "host_lanes": "list[int]",
            "lane_src": "str",
            "launch_args": "str",
            "long_why": "str",
            "budget_evidence": "str",  # log path / run id proving a budget_s raise (--check-budgets)
            "net_extra": "str",
            "net_extra_client": "str",
            "no_client_ip": "bool",
            "no_hash_why": "str",  # a multi row that opts out of the lockstep hash compare (HASHDEF)
            "omit_satellite": "list[str]",
            "optin": "bool",
            "post_check": "list[str]|list[list[str]]",
            "post_check_peers": "bool",
            "post_check_session": "bool",
            "relay": "bool",
            "relay_args": "list[str]",
            "relay_client_only": "bool",
            "relay_restart_on": "str",
            "requires": "callable",
            "script": "str",
            "share_lanes": "str|list[str]",
            "shim": "bool",
            "shim_delay": "int",
            "shim_timeline": "str",
            "shim_triggers": "list[record:shim_trigger]",
            "ship_pacing": "bool",
            "timeout": "int",
            "timeout_frames": "int",
            "tol": "float",
        },
        "enums": {"kind": ("solo", "multi")},
        # kind-dependent requirements: a solo walk names its script, a multi topology its peers.
        "requires_by": {"kind": {"solo": ("script",), "multi": ("host", "clients")}},
    },
    "sim_scenarios": {
        "required": {"name": "str", "desc": "str", "steps": "int", "speed": "int"},
        "optional": {"save": "str", "load_at": "int"},
    },
    "uirec_scenarios": {
        "required": {"name": "str", "desc": "str", "journal": "str"},
        "optional": {
            "coverage": "bool",
            "land_mode": "str",
            "landings": "int",
            "oracle": "str",
            "orders": "dict[str,int]",
            "orders_post": "dict[str,int]",
            "proven_rung": "int",
            "rungs": "tuple[int]",
        },
    },
    "tact_scenarios": {
        "required": {"name": "str", "desc": "str", "journal": "str", "arms": "tuple[str]"},
        "optional": {},
        "enums": {"arms": ("verify", "replay")},
    },
}

# Top-level keys that are not registries: `anchors` holds YAML anchors shared by several rows.
NON_REGISTRY_KEYS = ("anchors",)

# tooling:TL-SUITE-KEYS -- a `tests` optional key used by this many rows or fewer is a bespoke
# runner branch, not a shared mechanism, and must be either retired or declared here with a
# one-line reason + owning tracker id. `rare_key_problems` (called from `--check`/`--selftest`,
# and so from lint_repo's ui_registry row) reds an undeclared key at/under the threshold AND a
# declared key that no row uses any more (a stale entry, once its row is deleted or reworked).
RARE_KEY_THRESHOLD = 2
RARE_KEYS = {
    # R2b (done): browser_two_rows is two relay-hosted lobbies, one browser -- the second lobby
    # needs its own advertised name and its own bound port (a client that also hosts).
    "client_game_name": "browser_two_rows' second lobby needs a distinct name (mp:R2b, done)",
    "host_lanes": "browser_two_rows' second lobby's client hosts, needs its own port (mp:R2b, done)",
    # GS1(b) (done): ghost_exit_rejoin -- client2 relaunches into client1's own lane only after
    # client1's process has actually exited; no spare lane to give it a third of its own.
    "client_after_exit": "ghost_exit_rejoin's relaunch-after-exit shape (mp:GS1, done)",
    "client_shares_lane": "ghost_exit_rejoin's relaunch reuses client1's lane, no lane headroom (mp:GS1, done)",
    "client_expect_exit": "ghost_exit_rejoin: client1 ExitProcess()s, never reaches COMPLETE (mp:GS1, done)",
    # R7a (done): relay_punch / direct_dial_with_relay_set, two distinct relay-vs-direct proofs.
    "no_client_ip": "relay_punch omits the client IP to force the relay path (mp:R7a, done)",
    "relay_client_only": "direct_dial_with_relay_set: relay present but never contacted (mp:R7a, done)",
    # Single-row relay sub-options, each a distinct regression for its own fix (extensions of the
    # shared `relay: true` mechanism, not duplicates of it).
    "relay_restart_on": "relay_restart's redeploy-survives-the-match proof (mp:R4b, done)",
    "relay_args": "relay_stale_notice needs a lowered --advertise-level on the relay (mp:R4a, done)",
    "client_dead_ip": "ip_retry's typo'd-IP-is-retryable shape (mp:S8, done)",
    # Generic, cross-cutting mechanisms that happen to be narrow today because few rows need them
    # yet -- not bespoke branches, so not candidates for deletion.
    "budget_evidence": "generic --check-budgets evidence field; 1 row raised its budget_s recently (tooling:TL-SUITE-TIMEOUT-CLASS, done)",
    "expect_red": "generic XFAIL declaration; narrow today because most red-prone rows got fixed instead",
    "extra_ini_host": "generic per-side ini fragment (parallels extra_ini); codepage_adopt/codepage_refused use it (mp:F3c, done)",
    "extra_ini_client": "generic per-side ini fragment (parallels extra_ini); codepage_adopt/codepage_refused use it (mp:F3c, done)",
    "requires": "generic opt-in precondition hook; font_merged/chat_glyphs are its first users (mp:F2b, done)",
    "lane_src": "generic alternate-install lane source; font_merged/chat_glyphs use it (mp:F2b, done)",
    "harness_extra_host": "generic per-side harness knob (parallels harness_extra); mp_snapshot/p6_loser_gap use it (mp:X1b, done)",
    # tact_panel (reimpl:LIFT-TACT, done): tactical mission entry isn't reachable from a menu, so
    # this row drives it via the DLL's --tactical verb over a deployed save instead of a script walk.
    "launch_args": "tact_panel's non-menu tactical entry point (reimpl:LIFT-TACT, done)",
    "deploy_save": "tact_panel's save deployed before boot, paired with launch_args (reimpl:LIFT-TACT, done)",
    # mp:X1b (done) marked this for deletion once mp:X3 (mid-game resync/join-in-progress) lands;
    # X3 is `in-progress`, not yet -- delete `optin` + mp_snapshot's declaration together with X3.
    "optin": "DELETE when mp:X3 lands (X3 is in-progress as of 2026-09-26, not yet) (mp:X1b/X3)",
}


def rare_key_problems(rows):
    """[] if every `tests` optional key used by <= RARE_KEY_THRESHOLD rows is declared in
    RARE_KEYS, and every RARE_KEYS entry is still used by at least one row -- else the problems,
    one string each (TL-SUITE-KEYS)."""
    core = set(SCHEMA["tests"]["required"])
    counts = Counter()
    for row in rows:
        for k in row:
            if k not in core:
                counts[k] += 1
    probs = [
        "key %r used by %d row(s) (<=%d) is not declared in ui_registry.RARE_KEYS"
        % (k, n, RARE_KEY_THRESHOLD)
        for k, n in counts.items()
        if n <= RARE_KEY_THRESHOLD and k not in RARE_KEYS
    ]
    probs += [
        "RARE_KEYS entry %r: 0 rows use this key any more -- drop the entry" % k
        for k in RARE_KEYS
        if counts.get(k, 0) == 0
    ]
    return probs


class RegistryError(ValueError):
    pass


class Ref:
    def __init__(self, name):
        self.name = name

    def __repr__(self):
        return "Ref(%r)" % self.name


class Frames:
    def __init__(self, seconds, floor):
        self.seconds, self.floor = seconds, floor

    def __repr__(self):
        return "Frames(%r, %r)" % (self.seconds, self.floor)


_BASE = getattr(yaml, "CSafeLoader", yaml.SafeLoader)


class _Loader(_BASE):
    pass


def _mapping(loader, node):
    # PyYAML silently keeps the LAST of two equal keys; a row that repeats a key would lose a value.
    loader.flatten_mapping(node)
    seen = set()
    for k, _v in node.value:
        key = loader.construct_object(k, deep=True)
        if key in seen:
            raise RegistryError("duplicate key %r at line %d" % (key, k.start_mark.line + 1))
        seen.add(key)
    return loader.construct_mapping(node, deep=True)


def _ref(loader, node):
    return Ref(loader.construct_scalar(node))


def _frames(loader, node):
    seq = loader.construct_sequence(node)
    if len(seq) != 2 or not isinstance(seq[0], int) or not isinstance(seq[1], str):
        raise RegistryError(
            "!frames wants [SECONDS, FLOOR_NAME] at line %d" % (node.start_mark.line + 1)
        )
    return Frames(seq[0], seq[1])


_Loader.add_constructor(yaml.resolver.BaseResolver.DEFAULT_MAPPING_TAG, _mapping)
_Loader.add_constructor("!ref", _ref)
_Loader.add_constructor("!frames", _frames)


def _split_top(spec, sep):
    """Split `spec` on `sep` outside brackets."""
    out, depth, cur = [], 0, ""
    for ch in spec:
        if ch == "[":
            depth += 1
        elif ch == "]":
            depth -= 1
        if ch == sep and depth == 0:
            out.append(cur)
            cur = ""
        else:
            cur += ch
    out.append(cur)
    return out


def _check(value, spec):
    """(ok, converted_value). Converts YAML lists to tuples where the spec says tuple."""
    alts = _split_top(spec, "|")
    if len(alts) > 1:
        for a in alts:
            ok, v = _check(value, a)
            if ok:
                return True, v
        return False, value
    if spec == "str":
        return isinstance(value, str), value
    if spec == "bool":
        return isinstance(value, bool), value
    if spec == "int":
        return isinstance(value, int) and not isinstance(value, bool), value
    if spec == "float":
        return isinstance(value, float), value
    if spec == "callable":
        return callable(value), value
    if spec.startswith("record:"):
        rec = RECORDS[spec[len("record:") :]]
        if not isinstance(value, dict) or _row_problems(value, rec):
            return False, value
        specs = dict(rec["required"], **rec["optional"])
        return all(_check(v, specs[k])[0] for k, v in value.items()), value
    for kind in ("list", "tuple"):
        if spec.startswith(kind + "["):
            inner = spec[len(kind) + 1 : -1]
            if not isinstance(value, list):
                return False, value
            out = []
            for x in value:
                ok, v = _check(x, inner)
                if not ok:
                    return False, value
                out.append(v)
            return True, (tuple(out) if kind == "tuple" else out)
    if spec.startswith("dict["):
        kspec, vspec = _split_top(spec[5:-1], ",")
        if not isinstance(value, dict):
            return False, value
        for k, v in value.items():
            if not _check(k, kspec)[0] or not _check(v, vspec)[0]:
                return False, value
        return True, value
    raise RegistryError("bad type spec %r in SCHEMA" % spec)


def _row_problems(row, schema):
    probs = []
    allowed = dict(schema["required"], **schema["optional"])
    for k in schema["required"]:
        if k not in row:
            probs.append("missing required key %r" % k)
    for k in row:
        if k not in allowed:
            probs.append("unknown key %r" % k)
    return probs


def _resolve(value, refs, where):
    if isinstance(value, Ref):
        if value.name not in refs:
            raise RegistryError("%s: unresolved !ref %s" % (where, value.name))
        return refs[value.name]
    if isinstance(value, Frames):
        for need in ("frames_for_seconds", value.floor):
            if need not in refs:
                raise RegistryError("%s: unresolved !frames name %s" % (where, need))
        return refs["frames_for_seconds"](value.seconds, refs[value.floor])
    return value


def validate(data, refs=None, lint=True):
    """Validate the parsed document in place; returns {registry: [rows]}. `lint=False` skips the
    row-semantics lints (the hash rule) for reading an older committed registry."""
    if not isinstance(data, dict):
        raise RegistryError("registry file: top level must be a mapping")
    out = {}
    for top in data:
        if top not in SCHEMA and top not in NON_REGISTRY_KEYS:
            raise RegistryError("registry file: unknown top-level key %r" % top)
    for reg, schema in SCHEMA.items():
        rows = data.get(reg)
        if not isinstance(rows, list):
            raise RegistryError("%s: missing or not a list" % reg)
        names = set()
        for i, row in enumerate(rows):
            name = row.get("name") if isinstance(row, dict) else None
            where = "%s row %s" % (reg, repr(name) if name is not None else "#%d" % i)
            if not isinstance(row, dict):
                raise RegistryError("%s: not a mapping" % where)
            probs = _row_problems(row, schema)
            if probs:
                raise RegistryError("%s: %s" % (where, "; ".join(probs)))
            if name in names:
                raise RegistryError("%s: duplicate row name" % where)
            names.add(name)
            allowed = dict(schema["required"], **schema["optional"])
            for k in list(row):
                v = row[k]
                if isinstance(v, (Ref, Frames)):
                    if refs is None:
                        continue
                    v = _resolve(v, refs, "%s key %r" % (where, k))
                ok, v = _check(v, allowed[k])
                if not ok:
                    raise RegistryError(
                        "%s: key %r wants %s, got %s %r"
                        % (where, k, allowed[k], type(v).__name__, v)
                    )
                enum = schema.get("enums", {}).get(k)
                if enum is not None:
                    vals = v if isinstance(v, (list, tuple)) else (v,)
                    bad = [x for x in vals if x not in enum]
                    if bad:
                        raise RegistryError("%s: key %r value %r not in %s" % (where, k, bad, enum))
                row[k] = v
            for key, by in schema.get("requires_by", {}).items():
                for need in by.get(row.get(key), ()):
                    if need not in row:
                        raise RegistryError(
                            "%s: %s=%s needs key %r" % (where, key, row.get(key), need)
                        )
            if reg == "tests" and lint:
                probs = hash_problems(row)
                if probs:
                    raise RegistryError("%s: %s" % (where, "; ".join(probs)))
        out[reg] = rows
    return out


def load_text(text, refs=None, lint=True):
    try:
        data = yaml.load(text, Loader=_Loader)
    except yaml.YAMLError as e:
        raise RegistryError("registry file: YAML error: %s" % e) from e
    return validate(data, refs, lint)


def load(refs=None, path=REGISTRY):
    with open(path, encoding="utf-8") as f:
        return load_text(f.read(), refs)


# ---- the lockstep hash compare, default-on for every multi row (tooling:TL-SUITE-HASHDEF) -------
#
# Arming the harness with only these two keys changes no sim input: seed_mode=2/seed_step=0 (the
# ui_test base block) never reseed, fixed_step=0 leaves the clock alone, synth_move=0 issues no
# orders; the sim_step/sim_tick detours chain to the same bodies and replace the shipped D21 hook
# one-for-one (both call MH_Lockstep_StepPin). region_hash_step=50 only adds `R` localisation rows.
HASH_HARNESS = "region_hash_step=50;synth_move=0"
# Host-only [harness] knobs that make one peer diverge on purpose (D3 / D11 / U30b negative tests).
HASH_PERTURB_KEYS = (
    "rng_perturb_slot",
    "rng_perturb_step",
    "region_poke_at",
    "region_poke_min",
    "region_poke_only",
    "garble_at",
)
# --plant-desync's knob (test_ui.py): the strategic PRNG slot, flipped once on the host.
PLANT_STEP = 200
PLANT_HOST = "rng_perturb_slot=0;rng_perturb_step=%d" % PLANT_STEP


def harness_kv(s):
    """';'-separated k=v -> ordered dict (the --harness-extra format)."""
    out = {}
    for kv in (s or "").split(";"):
        if "=" in kv:
            k, v = kv.split("=", 1)
            out[k.strip()] = v.strip()
    return out


def harness_merge(base, over):
    """Merge two k=v strings by key; `over` wins."""
    m = harness_kv(base)
    m.update(harness_kv(over))
    return ";".join("%s=%s" % kv for kv in m.items())


def hash_problems(row):
    """Why this row's hash config is invalid ([] = fine). A multi row either hashes cleanly or
    declares `no_hash_why`; a config that would void the compare without saying so is refused."""
    why = row.get("no_hash_why")
    if row.get("kind") != "multi":
        return ["no_hash_why on a solo row (only multi rows hash)"] if why is not None else []
    if why is not None:
        return [] if why.strip() else ["no_hash_why is empty -- give the reason"]
    probs = []
    if row.get("expect_red"):
        probs.append("expect_red row hashes: its XFAIL would absorb a hash red (add no_hash_why)")
    for key in ("harness_extra", "harness_extra_host"):
        kv = harness_kv(row.get(key))
        if kv.get("enable") == "0":
            probs.append("%s disarms the harness, so nothing is hashed (add no_hash_why)" % key)
        bad = [k for k in HASH_PERTURB_KEYS if k in kv] if key == "harness_extra_host" else []
        if bad:
            probs.append(
                "%s perturbs one peer (%s): the compare reds by design (add no_hash_why)"
                % (key, ", ".join(bad))
            )
    if row.get("client_shares_lane"):
        probs.append(
            "a client reuses another's lane: the peer runs cannot be paired (add no_hash_why)"
        )
    if len(row.get("clients") or ()) < 1:
        probs.append("no client peer to compare against (add no_hash_why)")
    return probs


def hash_plan(row):
    """The row's merged harness_extra when it runs the hash compare, else None."""
    if row.get("kind") != "multi" or row.get("no_hash_why") is not None:
        return None
    return harness_merge(HASH_HARNESS, row.get("harness_extra"))


def hash_verdict(j, rc, plant_step=0):
    """(ok, line) from mp_analyze's JSON + exit code. 0 compared steps is a FAIL, never a pass.
    With plant_step, ok means the compare CAUGHT the planted desync at or after that step."""
    if not isinstance(j, dict):
        return False, "hash: FAIL -- mp_analyze wrote no JSON (rc %s)" % rc
    if j.get("refused"):
        return False, "hash: FAIL -- mp_analyze refused the pair (%s)" % j["refused"]
    pairs = j.get("desync_pairs") or []
    if not pairs:
        return False, "hash: FAIL -- no peer pair compared (a peer wrote no harness log)"
    n = min(int(p.get("overlap") or 0) for p in pairs)
    if n == 0:
        return False, "hash: FAIL -- 0 compared steps (a 0-step compare is not a pass)"
    verdict = j.get("verdict") or "no verdict"
    if plant_step:
        steps = [(p.get("first_mismatch") or {}).get("step") for p in pairs]
        hit = [s for s in steps if s is not None and s >= plant_step]
        if hit:
            return (
                True,
                "hash: PLANT CAUGHT -- DESYNC from step %d (planted at %d, %d compared)"
                % (
                    min(hit),
                    plant_step,
                    n,
                ),
            )
        return False, "hash: PLANT MISSED -- %s over %d steps (planted at %d)" % (
            verdict,
            n,
            plant_step,
        )
    if rc == 0 and not j.get("run_invalid") and verdict.startswith("ALL PAIRS IDENTICAL"):
        return True, "hash: IDENTICAL over %d steps" % n
    return False, "hash: FAIL -- %s (%d compared steps)" % (verdict, n)


def hash_compare(run_dirs, json_path, plant_step=0):
    """Run mp_analyze over the peers' run dirs; (ok, line, output)."""
    import json
    import subprocess

    argv = [sys.executable, os.path.join(REPO, "tools", "mp_analyze.py"), "--json", json_path]
    try:
        os.remove(json_path)  # never read a previous run's verdict
    except OSError:
        pass
    r = subprocess.run(argv + list(run_dirs), capture_output=True, text=True, check=False)
    try:
        with open(json_path, encoding="utf-8") as fh:
            j = json.load(fh)
    except (OSError, ValueError):
        j = None
    ok, line = hash_verdict(j, r.returncode, plant_step)
    return ok, line, (r.stdout or "") + (r.stderr or "")


# ---- selftest ----------------------------------------------------------------------------------

_GOOD = """\
anchors:
  chk: &chk [check-cmd, --a]
tests:
  - name: ok_solo
    kind: solo
    desc: d
    budget_s: 30
    script: s.txt
    tol: 0.0
    requires: !ref hook
    timeout_frames: !frames [2, FLOOR]
  - name: ok_multi
    kind: multi
    desc: d
    budget_s: 30
    host: h.txt
    clients: [c.txt]
    post_check: *chk
sim_scenarios: []
uirec_scenarios:
  - {name: u, desc: d, journal: j, rungs: [200, 2000], orders: {'0A': 1, '33': 2}}
tact_scenarios:
  - {name: t, desc: d, journal: j, arms: [replay]}
"""

_REFS = {"hook": lambda args: (True, ""), "FLOOR": 10, "frames_for_seconds": lambda s, f: s * f}


def _plant(old, new):
    assert old in _GOOD, old
    return _GOOD.replace(old, new, 1)


# (label, planted text, substrings the refusal must contain -- always the row name)
_NEGATIVES = [
    (
        "unknown key",
        _plant("    tol: 0.0\n", "    tol: 0.0\n    tolerance: 1\n"),
        ["'ok_solo'", "unknown key"],
    ),
    ("wrong type (int for float)", _plant("tol: 0.0", "tol: 0"), ["'ok_solo'", "'tol'"]),
    (
        "wrong type (str for int)",
        _plant("budget_s: 30\n    host", "budget_s: '30'\n    host"),
        ["'ok_multi'", "'budget_s'"],
    ),
    (
        "wrong type (bool for int)",
        _plant("budget_s: 30\n    script", "budget_s: true\n    script"),
        ["'ok_solo'", "'budget_s'"],
    ),
    (
        "missing required key",
        _plant("    desc: d\n    budget_s: 30\n    host", "    budget_s: 30\n    host"),
        ["'ok_multi'", "missing required key 'desc'"],
    ),
    ("kind-dependent key", _plant("    script: s.txt\n", ""), ["'ok_solo'", "'script'"]),
    ("bad enum", _plant("kind: multi", "kind: trio"), ["'ok_multi'", "'kind'"]),
    ("tuple element type", _plant("rungs: [200, 2000]", "rungs: [200, '2k']"), ["'u'", "'rungs'"]),
    (
        "duplicate row name",
        _plant("name: ok_multi", "name: ok_solo"),
        ["'ok_solo'", "duplicate row name"],
    ),
    (
        "duplicate key in row",
        _plant("    tol: 0.0\n", "    tol: 0.0\n    tol: 1.0\n"),
        ["duplicate key 'tol'", "line"],
    ),
    ("unresolved ref", _plant("!ref hook", "!ref nohook"), ["'ok_solo'", "nohook"]),
    ("unknown top-level", _GOOD + "extra: []\n", ["unknown top-level key 'extra'"]),
    (
        "record sub-key",
        _plant("    post_check: *chk\n", "    shim_triggers: [{cmd: x, whn: []}]\n"),
        ["'ok_multi'", "'shim_triggers'"],
    ),
    # TL-SUITE-HASHDEF: a multi row that cannot hash must say why.
    (
        "hash: expect_red, no reason",
        _plant("    post_check: *chk\n", "    expect_red: 'mp:X'\n"),
        ["'ok_multi'", "expect_red", "no_hash_why"],
    ),
    (
        "hash: one-peer perturb",
        _plant("    post_check: *chk\n", "    harness_extra_host: rng_perturb_slot=0\n"),
        ["'ok_multi'", "rng_perturb_slot", "no_hash_why"],
    ),
    (
        "hash: harness disarmed",
        _plant("    post_check: *chk\n", "    harness_extra: enable=0\n"),
        ["'ok_multi'", "disarms", "no_hash_why"],
    ),
    (
        "hash: shared client lane",
        _plant("    post_check: *chk\n", "    client_shares_lane: {2: 1}\n"),
        ["'ok_multi'", "lane", "no_hash_why"],
    ),
    (
        "hash: empty reason",
        _plant("    post_check: *chk\n", "    no_hash_why: ' '\n"),
        ["'ok_multi'", "no_hash_why is empty"],
    ),
    (
        "hash: reason on a solo row",
        _plant("    tol: 0.0\n", "    tol: 0.0\n    no_hash_why: x\n"),
        ["'ok_solo'", "solo row"],
    ),
]


def _hash_peer(d, steps, flip_from=None):
    """A synthetic peer run dir holding the one file mp_analyze's hash compare reads."""
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, "mh_harness.log"), "w", encoding="utf-8") as fh:
        fh.write(
            "; ==== mh replay harness armed: seed_step=0 seed_mode=2 stop_step=0 fixed_step=0 "
            "pin_fpu=1 region_hash_step=50 order_mode=0 replay_ai_off=0 suppress_enqueue=0 ====\n"
        )
        for s in steps:
            st = 0xAAAA0000 + s
            if flip_from is not None and s >= flip_from:
                st ^= 0xF0F0
            fh.write("%d %016X %016X %016X\n" % (s, 0x3F80000000000000 + s, 0xBBBB0000 + s, st))


def hash_selftest():
    """hash_verdict / hash_compare over synthetic peer logs, through the real mp_analyze."""
    import shutil
    import tempfile

    root = tempfile.mkdtemp(prefix="ui_registry_hash_")
    fails = 0
    try:
        arms = [
            # (label, host steps, client steps, client flip_from, plant, want ok, in line, in
            # mp_analyze's own output). The pair's COMBINED hashes are equal: mp_analyze's NO DATA
            # floor used to count combined differences and read this clean pair as NO DATA.
            (
                "identical pair",
                range(1, 301),
                range(1, 301),
                None,
                0,
                True,
                "IDENTICAL over 300",
                "compared=300  OK",
            ),
            (
                "0 compared steps",
                range(1, 101),
                range(201, 301),
                None,
                0,
                False,
                "0 compared",
                "NO DATA",
            ),
            ("desync", range(1, 301), range(1, 301), 150, 0, False, "DESYNC", "DESYNC"),
            ("one peer unhashed", range(1, 301), None, None, 0, False, "no peer pair", ""),
            ("plant caught", range(1, 301), range(1, 301), 200, 200, True, "PLANT CAUGHT", ""),
            ("plant missed", range(1, 301), range(1, 301), None, 200, False, "PLANT MISSED", ""),
        ]
        for i, (label, hs, cs, flip, plant, want_ok, want, want_out) in enumerate(arms):
            base = os.path.join(root, "arm%d" % i)
            _hash_peer(os.path.join(base, "host"), hs)
            cdir = os.path.join(base, "client1")
            if cs is None:
                os.makedirs(cdir)
            else:
                _hash_peer(cdir, cs, flip)
            jp = os.path.join(base, "mp_analyze.json")
            ok, line, out = hash_compare([os.path.join(base, "host"), cdir], jp, plant)
            good = ok == want_ok and want in line and want_out in out
            print("  %s  hash compare: %-18s %s" % ("ok  " if good else "FAIL", label, line))
            fails += not good
        good = hash_verdict(None, 1)[0] is False
        print("  %s  hash compare: no JSON is a FAIL" % ("ok  " if good else "FAIL"))
        fails += not good
    finally:
        shutil.rmtree(root, ignore_errors=True)
    return fails


def rare_key_selftest():
    """rare_key_problems over synthetic row sets, then the committed registry (TL-SUITE-KEYS)."""
    fails = 0
    rows = [{"name": "a", "undeclared_narrow_key": 1}, {"name": "b", "undeclared_narrow_key": 1}]
    probs = rare_key_problems(rows)
    ok = any("undeclared_narrow_key" in p and "not declared" in p for p in probs)
    print("  %s  rare key: undeclared key at the threshold reds" % ("ok  " if ok else "FAIL"))
    fails += not ok
    saved = dict(RARE_KEYS)
    try:
        RARE_KEYS["ghost_key_never_used"] = "test-only"
        probs = rare_key_problems([{"name": "a"}])
        ok = any("ghost_key_never_used" in p and "drop the entry" in p for p in probs)
        print("  %s  rare key: declared key with 0 uses reds" % ("ok  " if ok else "FAIL"))
        fails += not ok
    finally:
        RARE_KEYS.clear()
        RARE_KEYS.update(saved)
    try:
        regs = load()
        probs = rare_key_problems(regs["tests"])
        ok = not probs
        print(
            "  %s  rare key: committed registry roster is complete%s"
            % ("ok  " if ok else "FAIL", "" if ok else (" -- " + "; ".join(probs)))
        )
        fails += not ok
    except RegistryError as e:
        print("  FAIL  rare key: committed registry: %s" % e)
        fails += 1
    return fails


def selftest():
    fails = 0
    got = load_text(_GOOD, _REFS)
    pos = [
        ("tuple restored", got["uirec_scenarios"][0]["rungs"] == (200, 2000)),
        ("hex-ish keys stay str", list(got["uirec_scenarios"][0]["orders"]) == ["0A", "33"]),
        ("!frames resolved", got["tests"][0]["timeout_frames"] == 20),
        ("!ref resolved", callable(got["tests"][0]["requires"])),
        ("anchor shared", got["tests"][1]["post_check"] == ["check-cmd", "--a"]),
        ("float kept", isinstance(got["tests"][0]["tol"], float)),
        ("multi row hashes by default", hash_plan(got["tests"][1]) == HASH_HARNESS),
        ("solo row never hashes", hash_plan(got["tests"][0]) is None),
        (
            "row harness keys win the merge",
            hash_plan(dict(got["tests"][1], harness_extra="region_hash_step=0;order_log=1"))
            == "region_hash_step=0;synth_move=0;order_log=1",
        ),
        (
            "no_hash_why opts out",
            hash_plan(dict(got["tests"][1], no_hash_why="lobby only")) is None
            and not hash_problems(dict(got["tests"][1], no_hash_why="x", expect_red="y")),
        ),
    ]
    for label, ok in pos:
        print("  %s  positive: %s" % ("ok  " if ok else "FAIL", label))
        fails += not ok
    for label, text, want in _NEGATIVES:
        try:
            load_text(text, _REFS)
            msg = None
        except RegistryError as e:
            msg = str(e)
        ok = msg is not None and all(w in msg for w in want)
        print("  %s  refused: %-28s %s" % ("ok  " if ok else "FAIL", label, msg or "(ACCEPTED)"))
        fails += not ok
    try:
        n = sum(len(v) for v in load().values())
        print("  ok    committed registry loads (%d rows, refs unresolved)" % n)
    except RegistryError as e:
        print("  FAIL  committed registry: %s" % e)
        fails += 1
    fails += hash_selftest()
    fails += rare_key_selftest()
    print("ui_registry selftest: %s" % ("PASS" if not fails else "FAIL (%d)" % fails))
    return 1 if fails else 0


def main(argv=None):
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--check", action="store_true", help="load + validate the committed registry")
    a = ap.parse_args(argv)
    if a.selftest:
        return selftest()
    try:
        regs = load()
    except RegistryError as e:
        print("FAIL: %s" % e)
        return 1
    probs = rare_key_problems(regs["tests"])
    if probs:
        print(
            "FAIL: rare-key roster is incomplete (tooling:TL-SUITE-KEYS):\n  " + "\n  ".join(probs)
        )
        return 1
    print("OK: %s" % ", ".join("%s=%d" % (k, len(v)) for k, v in regs.items()))
    return 0


if __name__ == "__main__":
    sys.exit(main())
