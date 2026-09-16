#!/usr/bin/env python3
"""Domain profiles for the reimplementation-migration toolchain.

WHY THIS EXISTS. The AI migration (RI-AI) grew a seven-tool loop -- manifest generator, readiness
predicate, session bookends, unattended driver, export-request builder, translation lint, gate-site
dumper -- and every one of them hardcoded the same handful of AI facts: the root function, the
`llm_strat_ai_` selector, `tools/data/ai_migration.json`, the batch letters, `aitest`. That was
correct while there was one cluster. SIM1 is the second, and the honest options were to fork seven
files or to lift the facts into data. This is the lift.

It is deliberately the SAME SHAPE as the tracker CLI's `--domain`: a `--domain` flag, per-domain
configuration in data, and the tool bodies carrying only mechanism. The tracker CLI reads its per-domain
paths from the YAML's `meta` block; the migration tools read theirs from `tools/data/migration/
<domain>.json`.

THE SELECTOR IS THE PART THAT GENUINELY DIFFERS, and pretending otherwise would have been the bug.
The AI cluster is selected by NAME PREFIX from a root -- `llm_strat_ai_` -- which works because
someone had already named the cluster consistently. The sim has no such prefix: its closure spans
`llm_strat_unit_`, `llm_strat_bldg_`, `llm_prod_`, `game_*`, `map_*` and more, and the raw closure
from `llm_strat_sim_step` is 778 functions because it runs straight out through the UI, sound and
net layers into most of the binary. So `sim` selects by CLOSURE MINUS CUTS instead: walk forward
from the root, refusing to descend into declared cut families, and subtract the functions another
domain's manifest already owns. Both modes emit the same ledger shape.

  mode `name_prefix` -- reachable(root) AND name starts with `prefix`      (ai)
  mode `closure`     -- reachable(root, not descending into `cut_prefixes`)
                        minus every address in `excludes[]`'s manifests    (sim)

WHAT A PROFILE IS NOT. It holds no progress and no judgement: no batch assignment for a specific
function (that is the generator's mechanical rule plus the overrides file), no evidence tier, no
item status. Those live in the manifest and the tracker, which are the things a session writes.
A profile is the description of a DOMAIN, and it changes about once per domain.

Usage:
    from migration_domain import load_domain, add_domain_arg
    dom = load_domain("sim")
    dom.path("manifest")          # absolute, resolved against the repo root
    dom.batch_item("A")           # -> "SIM1A"
"""

from __future__ import annotations

import argparse
import json
import os

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PROFILE_DIR = os.path.join(REPO, "tools", "data", "migration")

# The default stays `ai` for one reason only: it is the domain that already has 209 ledger rows and
# a live loop, so an existing invocation that predates this module keeps meaning what it meant.
# New callers should pass --domain explicitly; the session tooling does.
DEFAULT_DOMAIN = "ai"

# Every key a profile must carry. Checked on load rather than at first use, because a profile is
# read by seven tools and a missing key should name itself once, at the top, instead of surfacing
# as a KeyError three layers into whichever tool happened to touch it first.
REQUIRED = ("domain", "title", "select", "paths", "batches")
REQUIRED_PATHS = ("manifest", "ready_report")


class Domain:
    """One migration domain, loaded from tools/data/migration/<name>.json."""

    def __init__(self, raw, source):
        self.raw = raw
        self.source = source
        self.name = raw["domain"]
        self.title = raw["title"]
        # MIG-SET: a seeded domain has no single root -- its membership is a query, not a walk from
        # one entry -- so `root` is optional exactly there and required everywhere else.
        root = raw.get("root") or {}
        self.root_addr = root["addr"].lower().replace("0x", "") if root.get("addr") else None
        self.root_name = root.get("name", "")
        self.select = raw["select"]
        self.batches = raw["batches"]

    @property
    def seed_queries(self):
        """The `select.seeds` query list for a seeded domain; empty for root-based domains."""
        return list(self.select.get("seeds", ()))

    @property
    def walk(self):
        """`forward` (default) closes over the seeds' callees; `none` makes the seeds the membership.

        RD-READY is the `none` case and it is not a corner: nothing there is translated, so pulling
        in a callee frontier would enlarge a set whose whole purpose is to be named and prototyped.
        """
        return self.select.get("walk", "forward")

    # -- paths -------------------------------------------------------------------------------
    def path(self, key, required=True):
        """Absolute path for a declared repo-relative path key, or None if optional and absent."""
        rel = self.raw["paths"].get(key)
        if rel is None:
            if required:
                # SystemExit, not KeyError: an absent path is usually a DELIBERATE statement that the
                # domain does not have that thing yet (see sim's missing `loop_prompt`), and the
                # reader needs the profile named, not a traceback through three call frames.
                raise SystemExit(
                    "migration domain %r declares no path %r.\n"
                    "  profile: %s\n"
                    "  If that is deliberate, the tool you ran does not apply to this domain yet."
                    % (self.name, key, os.path.relpath(self.source, REPO).replace("\\", "/"))
                )
            return None
        return os.path.join(REPO, *rel.split("/"))

    def has_path(self, key):
        return self.raw["paths"].get(key) is not None

    # -- batches -----------------------------------------------------------------------------
    @property
    def batch_order(self):
        return list(self.batches.get("order", []))

    def batch_item(self, letter):
        """Tracker item id for a batch letter, e.g. 'A' -> 'AI1A'. None if the batch has no item."""
        return self.batches.get("item", {}).get(letter)

    @property
    def batch_rules(self):
        """[(letter, (substring, ...)), ...] in priority order -- first match wins."""
        return [(r["batch"], tuple(r["match"])) for r in self.batches.get("rules", [])]

    # -- misc declared facts -----------------------------------------------------------------
    def get(self, key, default=None):
        return self.raw.get(key, default)

    @property
    def roster_regions(self):
        return frozenset(self.raw.get("roster_regions", ()))

    @property
    def dead_tag(self):
        return self.raw.get("dead_tag", "todo:dead")

    @property
    def records_state(self):
        """Does a row in this domain carry a `state` a human writes down? (default yes)

        DECISION B, 2026-08-22 (MIG-SET's detail): a READINESS domain -- one whose deliverable is
        "this function is named and prototyped", not "this function was reimplemented and proven
        equivalent" -- records NO per-function state. Its terminal condition is the readiness
        predicate over its scope, re-derived from committed data every run, so a `state` column
        would be a second hand-written source of truth for a fact a tool already measures.

        The manifest still exists, because migration_ready scopes off it. What this flag removes is
        the WRITING: no progress fields are emitted, migration_ledger skips the file rather than
        validating a vocabulary it does not use, and migration_session prints the predicate instead
        of a state rollup. The alternative -- a new terminal state in ALLOWED_STATES -- was rejected:
        it adds a word someone must write into rows, and this removes the writing.
        """
        return bool(self.raw.get("records_state", True))

    @property
    def readiness_domain(self):
        """Is this domain's deliverable READINESS rather than a reimplementation? (the flip of
        `records_state`, named for the question the callers actually ask)

        The two are one fact, deliberately: a domain records no per-function `state` precisely
        because nothing in it is translated, and every consequence follows from that -- there is no
        oracle to compare against, no shadow site to arm, no verification debt to pace, and the only
        session shape it has is the Ghidra-writing one `migration_loop` calls PREP. Splitting it
        into a second profile key would let a profile declare one and not the other, which is not a
        state anything could act on.
        """
        return not self.records_state

    @property
    def selftest(self):
        """The offline oracle's suite name, e.g. 'aitest'. None if none exists yet."""
        return self.raw.get("oracle", {}).get("selftest")

    @property
    def selftest_cmd(self):
        """The suite as a RUNNABLE command line -- `<exe> <suite>` -- or None if the domain has no
        oracle yet.

        WHY THIS IS NOT `"net_selftest.exe " + self.selftest` (fork F5I S4). There are two offline
        test executables since the libmh_test split, and every migration domain's oracle
        (aitest / simtest / tacttest / issuetest / libtranstest / ...) is on the side that MOVED to
        `libmh_selftest.exe`. The routing lives in tools/data/selftest_roster.json's `exe` column,
        which is the one list; this reads it rather than repeating it.

        It matters because the consumer is an AGENT BRIEF. A stale `net_selftest.exe aitest` in a
        prompt does not fail loudly for the agent that runs it -- the exe exits 2 with its mode
        list, which has no `N checks, M failures` line and no FAIL line, and an agent skimming for
        the word FAIL can read that as a clean run. A suite the roster does not carry is a named
        error here instead.
        """
        suite = self.selftest
        if not suite:
            return None
        roster = json.load(
            open(os.path.join(REPO, "tools", "data", "selftest_roster.json"), encoding="utf-8")
        )
        exe = {r["suite"]: r["exe"] for r in roster["suites"]}.get(suite)
        if exe is None:
            raise SystemExit(
                "migration_domain: domain %r declares oracle.selftest %r, which is not in "
                "tools/data/selftest_roster.json" % (self.name, suite)
            )
        return "%s %s" % (roster["exes"][exe]["staged"], suite)

    @property
    def rig_modes(self):
        """The RUN VEHICLES that can reach this domain's shadow sites, in preference order.

        Until TACT-RIG (2026-08-25) this was argparse prose inside migration_sweep.py -- a hardcoded
        `choices=("soak", "sp")`. `ai` and `sim` got away with that because they share one vehicle;
        `tact` is the SECOND one, and it is where the hardcoding becomes a real defect rather than
        an untidiness: soak and sp are BOTH strategic, neither ever sets _G_LLM_GAME_MODE to 6, so a
        tactical site armed under either reads ZERO CALLS. The anti-vacuity rule catches that and
        reports NOT COVERED -- correctly, but only after a rig run has been spent on it. Reading the
        vehicle off the profile turns a wasted run into a configuration error.

        Empty tuple when the profile names none, which migration_sweep refuses to guess past.
        """
        return tuple(self.raw.get("oracle", {}).get("rig", {}).get("modes", ()))

    @property
    def rig_default(self):
        """The vehicle to use when --mode is not given. None if the profile names no rig."""
        rig = self.raw.get("oracle", {}).get("rig") or {}
        d = rig.get("default")
        if d and d not in self.rig_modes:
            raise SystemExit(
                "%s: oracle.rig.default %r is not in oracle.rig.modes %r"
                % (self.name, d, list(self.rig_modes))
            )
        return d or (self.rig_modes[0] if self.rig_modes else None)

    def require_rig(self, mode=None):
        """The vehicle for a sweep of this domain, or a fatal error naming what is missing.

        Fatal rather than defaulting, and that is the point of the item: silently falling back to a
        strategic soak for a domain whose code only runs in mode 6 produces a green-looking run with
        a zero call count, which is the exact failure the profile field exists to prevent.
        """
        if not self.rig_modes:
            raise SystemExit(
                "domain %r names no oracle.rig in %s.json, so there is no run vehicle to sweep it "
                "with.\nAdd one, e.g.:\n"
                '  "oracle": { "rig": { "modes": ["soak"], "default": "soak" } }\n'
                "Defaulting to a strategic soak here would arm the sites and report zero calls."
                % (self.name, self.name)
            )
        if mode is None:
            return self.rig_default
        if mode not in self.rig_modes:
            raise SystemExit(
                "domain %r cannot be swept with --mode %s. Its profile names %s.\n"
                "This is refused rather than attempted because a vehicle that never reaches the "
                "domain's code arms every site and reports zero calls -- which reads like a run "
                "that happened." % (self.name, mode, " / ".join(self.rig_modes))
            )
        return mode

    @property
    def ab_profile(self):
        """The `oracle.ab` block -- the PROMOTED-GOLDEN A/B's configuration. None if undeclared.

        The third verification instrument, and the one a domain can most easily be missing without
        noticing. The offline oracle (`oracle.selftest`) proves self-consistency with our reading of
        the disassembly; the rig (`oracle.rig`) arms per-call shadow sites and compares the return
        value plus the DECLARED regions. Neither can see a write through a caller-supplied
        out-pointer, because that lands on the caller's stack. Promotion can: our body replaces the
        entry, the caller's table fills as it always did, and the comparison moves out to the
        per-frame state-hash trajectory of a whole run.

        None -- not an empty dict -- when the profile declares none, so `require_ab` can refuse by
        name instead of running the wrong vehicle. Same distinction `effect_prefixes` draws.
        """
        return (self.raw.get("oracle") or {}).get("ab")

    def require_ab(self, mode=None):
        """The A/B VEHICLE configuration for this domain, or a fatal error naming what is missing.

        Deliberately the same shape as `require_rig` one method up, because it answers the same
        question for a different instrument -- which run vehicle reaches this domain's code -- and
        the two must not drift into different postures. It carries the vehicle and its scenario and
        NOT the module list: what to A/B is derived from tools/data/promote_modules.json crossed
        with the domain's ledger, so a row cannot be promoted here and forgotten there.

        FATAL RATHER THAN DEFAULTING. Until TACT-AB the promoted-vs-original A/B driver drove `test_ui.py --soak` and
        nothing else, which was right while every promote module was strategic. A TACTICAL module
        A/B'd under a soak is installed, never entered, and reports a MATCHED golden over a body
        that never ran -- the tool's own liveness check catches it, but only after the runs are
        spent. Guessing the vehicle is how that costs an afternoon; refusing is how it costs a line.
        """
        ab = self.ab_profile
        if not ab:
            raise SystemExit(
                "domain %r declares no oracle.ab in %s.json, so there is no A/B vehicle to run "
                "it with.\nAdd one, e.g.:\n"
                '  "oracle": { "ab": { "driver": "soak_golden", "steps": 3000 } }\n'
                "Defaulting to a strategic soak here would promote the bodies, never enter them, "
                "and report a golden that MATCHED over code that did not run."
                % (self.name, self.name)
            )
        driver = ab.get("driver")
        if not driver:
            raise SystemExit(
                "%s: oracle.ab declares no `driver` -- name the vehicle ('tact' / 'soak_golden')"
                % self.name
            )
        if mode is not None and mode != driver:
            raise SystemExit(
                "domain %r cannot be A/B'd with driver %r. Its profile names %r.\n"
                "Refused rather than attempted: a vehicle that never reaches the domain's code "
                "installs every promotion and enters none, which reads like a clean run."
                % (self.name, mode, driver)
            )
        return ab

    @property
    def non_autonomous(self):
        """Tracker items the loop driver must refuse to open unattended."""
        return frozenset(self.raw.get("non_autonomous", ()))

    @property
    def effect_prefixes(self):
        """Families whose functions may have an EXTERNAL side effect a state restore cannot undo.

        Defaults to `cut_prefixes`, because for a closure-mode domain the walls and the effect
        surface are the same set by construction: what the sim is not allowed to own is sound, UI,
        input and net, and those are exactly the things that do something a snapshot cannot take
        back. Declare `select.effect_prefixes` explicitly to separate them.

        None -- not an empty tuple -- when the domain declares neither, so a consumer can tell "no
        effect families declared" from "no effects found". They are not the same claim.
        """
        p = self.select.get("effect_prefixes") or self.select.get("cut_prefixes")
        return tuple(p) if p else None

    @property
    def determinism_gate(self):
        """The DETERMINISM gate command for this domain, as a shell string.

        Defaults to the strategic oracle, which is right for every domain whose code runs under
        llm_strat_sim_step. It is NOT right for `tact`: mode 6 dispatches to llm_tact_frame and
        never calls llm_strat_sim_step, so the strategic cadence hook never fires and the run emits
        zero hash lines for the whole excursion -- a gate that passes because it measured nothing.
        Declare `oracle.determinism_gate` to override.
        """
        o = self.raw.get("oracle") or {}
        return o.get("determinism_gate") or "python tools/test_ui.py --determinism"

    @property
    def determinism_gate_why(self):
        """One sentence on what this domain's determinism run covers -- and what it does not.

        Separate from the command because the two diverge: the strategic run launches through the
        real menu -> lobby -> Start and therefore covers the liveness the UI suite would, while the
        tactical one force-enters a mission and covers no menu path at all. Quoting the strategic
        sentence at a tactical session is how a real gap gets read as already handled.
        """
        o = self.raw.get("oracle") or {}
        return o.get("determinism_gate_why") or (
            "The determinism run launches through the real menu -> lobby -> Start, so it covers "
            "the liveness the UI suite would."
        )

    @property
    def effect_names(self):
        """Outward-effect targets named one at a time, for callees OUTSIDE the domain's closure.

        `effect_prefixes` and the named walls both only ever match code the domain owns or walls off
        -- i.e. code inside the closure. Everything a member calls into SHARED, non-exclusive
        territory (the UI blit primitives, the input queue, the resource loader, the strategic
        roster) is invisible to both, and a shadow arm double-fires it just the same. This is the
        list that closes that gap. Empty tuple, not None: a domain that declares no such names has
        made a claim about its frontier, and it is a checkable one.
        """
        return tuple(self.select.get("effect_names", ()))

    @property
    def region_hints(self):
        """Uppercase substrings that put a state region in this domain, for the R6 standing check.

        A HINT, not a membership test. R6 asks which zero-size regions are unmeasurable *for this
        cluster*; a region carrying none of these is still reported in the registry-wide list.
        """
        return tuple(h.upper() for h in self.raw.get("region_hints", ()))

    def __repr__(self):
        return "<Domain %s: %s>" % (self.name, self.title)


def available():
    """Sorted list of domain names with a profile on disk."""
    if not os.path.isdir(PROFILE_DIR):
        return []
    return sorted(
        f[:-5] for f in os.listdir(PROFILE_DIR) if f.endswith(".json") and not f.startswith("_")
    )


def all_rig_modes():
    """Every run vehicle named by any profile on disk, sorted (TACT-RIG).

    Used to DERIVE migration_sweep's `--mode` choices instead of hand-keeping them beside the data
    -- adding a fourth domain with a fifth vehicle should not need an argparse edit. Reads the JSON
    directly rather than through `load_domain`, because a profile can be present and incomplete
    (ai_reclassified.json is one) and a hard failure while merely listing vehicles would take the
    whole tool down over a file it does not need.
    """
    modes = set()
    for n in available():
        # The schema access is INSIDE the try, not after it. It was outside, which made the
        # docstring's claim false in the only way that matters: a JSON-parse failure was survived,
        # but `"oracle": "<string>"` or `"rig": [...]` would raise AttributeError at IMPORT time and
        # take migration_sweep down for EVERY domain over a profile it does not need.
        try:
            raw = json.loads(
                open(os.path.join(PROFILE_DIR, "%s.json" % n), encoding="utf-8-sig").read()
            )
            modes.update((raw.get("oracle") or {}).get("rig", {}).get("modes", ()))
        except Exception:
            continue
    return tuple(sorted(modes))


def load_domain(name=None):
    """Load and validate a domain profile."""
    name = name or DEFAULT_DOMAIN
    src = os.path.join(PROFILE_DIR, "%s.json" % name)
    if not os.path.isfile(src):
        raise SystemExit(
            "unknown migration domain %r -- profiles on disk: %s\n(add tools/data/migration/%s.json"
            " to create it)" % (name, ", ".join(available()) or "none", name)
        )
    with open(src, encoding="utf-8") as fh:
        raw = json.load(fh)

    missing = [k for k in REQUIRED if k not in raw]
    if missing:
        raise SystemExit("%s: profile is missing required key(s): %s" % (src, ", ".join(missing)))
    if raw["domain"] != name:
        raise SystemExit(
            "%s: profile declares domain %r but is filed as %r -- the filename is the id"
            % (src, raw["domain"], name)
        )
    mode = raw["select"].get("mode")
    if mode not in ("name_prefix", "closure", "seeded"):
        raise SystemExit(
            "%s: select.mode must be 'name_prefix', 'closure' or 'seeded', got %r" % (src, mode)
        )
    # MIG-SET: `root` stayed in REQUIRED for the two root-based modes and is meaningless for a
    # seeded one, so the requirement moved here where the mode is known.
    if mode in ("name_prefix", "closure") and not (raw.get("root") or {}).get("addr"):
        raise SystemExit("%s: select.mode %r needs a root.addr" % (src, mode))
    if mode == "seeded":
        if not raw["select"].get("seeds"):
            raise SystemExit(
                "%s: select.mode 'seeded' needs select.seeds -- a membership query, since this "
                "domain has no root to walk from" % src
            )
        walk = raw["select"].get("walk", "forward")
        if walk not in ("forward", "none"):
            raise SystemExit("%s: select.walk must be 'forward' or 'none', got %r" % (src, walk))
        if walk == "forward" and not raw["select"].get("cut_prefixes"):
            raise SystemExit(
                "%s: select.walk 'forward' needs select.cut_prefixes -- an uncut walk runs straight "
                "out through the UI/sound layers, the same way an uncut sim closure does" % src
            )
    if mode == "name_prefix" and not raw["select"].get("prefix"):
        raise SystemExit("%s: select.mode 'name_prefix' needs select.prefix" % src)
    if mode == "closure" and not raw["select"].get("cut_prefixes"):
        raise SystemExit(
            "%s: select.mode 'closure' needs select.cut_prefixes -- an uncut closure from a sim root"
            " reaches most of the binary, so an empty cut list is never what was meant" % src
        )
    for key in REQUIRED_PATHS:
        if key not in raw.get("paths", {}):
            raise SystemExit("%s: profile is missing required path %r" % (src, key))
    return Domain(raw, src)


def add_domain_arg(ap, help_extra=""):
    """Register the standard --domain flag on an argparse parser."""
    ap.add_argument(
        "--domain",
        default=DEFAULT_DOMAIN,
        help="migration domain: %s (default %s)%s"
        % (", ".join(available()) or "none", DEFAULT_DOMAIN, help_extra),
    )
    return ap


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    add_domain_arg(ap)
    ap.add_argument("--list", action="store_true", help="list the profiles on disk and exit")
    args = ap.parse_args()
    if args.list:
        for n in available():
            d = load_domain(n)
            print(
                "%-6s %-28s root=%s select=%s"
                % (d.name, d.title, d.root_name or d.root_addr, d.select["mode"])
            )
        raise SystemExit(0)
    dom = load_domain(args.domain)
    print(json.dumps(dom.raw, indent=2))
