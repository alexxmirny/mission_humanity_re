"""gen_state_registry.py -- the STATE REGION REGISTRY (RI-STATE / ST1).

THE PROBLEM THIS EXISTS FOR. Five places in this tree independently answer "where does this state
region live and how big is it", and they agree only by coincidence:

  1. tools/data/dll_addr_manifest.json   -> addr/mh_addrs.gen.h, the module state views
  2. the shadow region sets  -> addr/mh_shadow.gen.h  (both REMOVED at fork F2D)
  3. tools/data/save_block_table.json    -> save/save_table.gen.h, the save format's blocks
  4. src/patcher/*.mh.patch.json         -> the relocation/cap-raise ref sites
  5. tools/data/hash_manifest.json       -> the determinism hash manifest (ST2M; was HAND-WRITTEN
                                           twice over, in harness.cpp's REGIONS[] and mp_analyze's
                                           REGION_NAMES, with a lint holding them level)

`SavePlanetToDisk`'s first block is 0x00bb4ed0/20400 -- the order QUEUE -- and (3) records its
`addr_sym` as null: the extractor never resolved it to the symbol (1) uses for the same bytes. So the
save driver reaches order state by a route that has no connection to `mh::orders`. That is tolerable
only while every region sits at its frozen .bss VA; the moment one MOVES, the manifests that were not
updated keep pointing at memory nothing writes, silently. Both live instances are already visible --
(5) annotates its projectile entry "stock .bss VA", and a promoted save on a cap-raised build walks a
table generated from UNPATCHED disassembly.

WHAT THIS EMITS. One merged table, `addr/mh_regions.gen.h`, with a `static_assert` tying every entry
whose name exists in `mh_addrs.gen.h` back to that constant -- so the two derivations become one
derivation and a check, and a future divergence is a COMPILE error rather than a wrong save file.
It also emits, into the same header, the ORDERED HASH MANIFEST (ST2M): the determinism harness's
region list expressed as (region, offset, length) SLICES over the registry -- 55 slices over 32
regions, because `p0_local` and friends are interior windows into one `player_data` region, not
regions of their own. `--check` additionally rewrites-and-diffs the two generated blocks in
tools/mp_analyze.py, so the analyzer's POSITIONAL column labels come from the same list.

LOCATION ONLY (L0). This changes where an address COMES FROM, not what it is, so the whole change is
provably behaviour-free under the existing gates. Giving an owner a serializer so its representation
can change at all is ST4.

TWO BASES SINCE ST2. `REGIONS[].base` is still `constexpr` -- it is the STOCK base, the address the
game's code and the save FORMAT name, and it is what `covering()` and the compile-time coverage
assert are built on. Alongside it the header now emits a `live_table` seeded from that same array,
and `live_base(rid)`: where the bytes ARE, equal to the stock base until a module calls `rebase()`.
Consumers that DESCRIBE a region keep the constexpr one; consumers that DEREFERENCE one (the
determinism hash, the shadow snapshots, `state_io::resolve`) read the live one. The seed is the
registry itself rather than a second emitted address list -- a parallel table would be precisely the
diverging derivation this file exists to abolish.

INPUTS ARE COMMITTED, so `--check` runs with no Ghidra and no 7 MB matrix -- same contract as
gen_dll_shadow.py. `--refresh` rebuilds tools/data/state_regions.json from the five sources above,
and uses tmp/state_matrix.json when present to enrich sizes and ownership (regenerate the matrix with
the Ghidra-side state-matrix dump).

Usage:
  python tools/gen_state_registry.py --refresh   # re-merge the five manifests into the data file
  python tools/gen_state_registry.py             # data file -> addr/mh_regions.gen.h
  python tools/gen_state_registry.py --check     # drift gate (tools/lint_repo.py)
  python tools/gen_state_registry.py --report    # who claims what, and every disagreement
"""

import argparse
import bisect
import json
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import _subsys  # noqa: E402  -- the shared subsystem taxonomy + the owner vocabulary

REPO = Path(__file__).resolve().parent.parent
ADDR_MANIFEST = REPO / "tools" / "data" / "dll_addr_manifest.json"
MEASURED_REGIONS = REPO / "tools" / "data" / "state_regions_measured.json"
SAVE_TABLE = REPO / "tools" / "data" / "save_block_table.json"
PATCH_DIR = REPO / "src" / "patcher"
HASH_MANIFEST = REPO / "tools" / "data" / "hash_manifest.json"
OWNERSHIP = REPO / "tools" / "data" / "region_ownership.json"
ACCESSORS = REPO / "tools" / "data" / "region_accessors.json"
ANALYZE = REPO / "tools" / "mp_analyze.py"
MATRIX = REPO / "tmp" / "state_matrix.json"
DATA = REPO / "tools" / "data" / "state_regions.json"
HEADER = REPO / "src" / "mh_dll" / "mh" / "addr" / "mh_regions.gen.h"

# The five claim tags, in the order they are reported. Also the bit order in `region::manifests`.
TAGS = ("view", "shadow", "save", "patch", "hash")

# THE EMITTED FLAG NAME, where it differs from the tag (fork F2E). `shadow` -> MF_MEASURED: the tag
# named the differential oracle that supplied the claims, and F2D deleted that oracle, so the flag
# was telling every reader of mh_regions.gen.h that a region was "shadowed" by a mechanism that no
# longer exists. What it always meant is "in the measured write census", and that is what it says now.
#
# ONLY THE EMITTED NAME MOVED, and the reason is a gate rather than timidity: the accessor census
# fingerprints tools/data/state_regions.json BY CONTENT (gen_region_accessors.currency), and that
# fingerprint can only be re-stamped by a `--refresh` that needs a Ghidra-generated state matrix.
# Renaming the tag in the data would move that fingerprint in the UNSAFE direction and take the
# ownership interlock red over a spelling change. So the DATA keeps the measurement's own vocabulary
# and the HEADER says what it means -- and this map is the single place the two are reconciled.
TAG_FLAG = {"shadow": "MEASURED"}

OWNERS = ("unknown", "island", "shared", "readonly")

# ---- the ORIGINAL block-table walkers (SB-HOSTFREE / dead-ends G139) -------------------------
#
# Every function in save_block_table.json reaches state through a list of (address, size) blocks
# baked into its own instruction stream -- `LZW_ReadCompressedFromFile(0x00bb4ed0, 0x4fb0, fh)` and
# 40 more like it in LoadPlanetFromDisk alone. While the ORIGINAL body runs, those addresses cannot
# follow a bind, so a relocated region it names is read or written on the abandoned .bss.
#
# The census cannot see this, and not because it is careless: by the census's own rule these four
# functions are OURS (a translated body exists for each), and `relocatable` means exactly that. What
# it does not mean is that the translated body is ARMED -- docs/state-boundary.md D7.7 wrote that
# limitation down before it bit, and G139 is it biting. (Two independent blind spots, in fact:
# Ghidra's reference manager does not record the reference at 0x00448222 either, so the address
# never reached the census as an external accessor -- dead-ends section A.)
#
# So the walker set is keyed to the ini switch that ARMS each one. A table with no switch gets
# WALK_OTHER, which nothing can arm -- the fail-safe direction, and it is why a newly extracted
# block table cannot silently make this gate inert.
SAVE_WALKERS = {
    "SavePlanetToDisk": ("WALK_PLANET_SAVE", "save"),
    "LoadPlanetFromDisk": ("WALK_PLANET_LOAD", "load"),
    "game__SaveGame": ("WALK_CONTAINER_SAVE", "container"),
    "llm_game_load": ("WALK_CONTAINER_LOAD", "container_load"),
}
WALK_OTHER = "WALK_OTHER"


# ---------------------------------------------------------------------------------------------
# Reading the five sources. Each returns claims: (addr, size_or_None, name_or_None).


def claims_view():
    """(1) The addr manifest's data entries -- what a module state view can bind.

    Sizes are normally NOT in this manifest (it is a symbol->VA map), so these claims carry a name
    and no size; the size comes from whichever other source has one.

    THE OPTIONAL `size` FIELD (LIB-BOOT, 2026-09-10) exists for the two regions no other source
    measures correctly, and it is deliberately a CLAIM rather than an override:

      * `Stones` (0x00d02a60, 800) is absent from the state matrix ENTIRELY -- no cell attributes
        the write, so a closure derivation is structurally blind to it and it lands here at size 0,
        which contains no address and is invisible to every consumer. Found only by diffing our
        derived schema against retail's own dead snapshot pair, which LZW-writes `&Stones, 800`.
      * `Tree` (0x00e14048) is measured 44 by the matrix and written as 0x1130 = 4400 by that same
        retail block. The matrix is under-measuring a BSS array; the Ghidra retype is a DB change
        and is tracked separately.

    A claim never silently wins: `merge()` keeps `size` from the matrix when the matrix has one, so
    Tree's disagreement is REPORTED by the SIZE DISAGREEMENT warning rather than resolved here --
    while `extent` (which is what a host actually binds, see host_bind.cpp note 2) takes the max, so
    the region reaches all 4400 bytes. For Stones, with no matrix claim, this is the only claim and
    it sizes the region outright.
    """
    d = json.loads(ADDR_MANIFEST.read_text(encoding="utf-8-sig"))
    return [(int(r["en"], 16), r.get("size"), r["name"]) for r in d["data"]]


def claims_shadow():
    """The MEASURED write-closure census: 317 (addr, size, name) claims, frozen at F2D.

    Read out of the differential oracle's per-site region sets (dll_shadow_regions.json) until
    F2D deleted the oracle. Nothing about the claim was oracle-specific: this reader took name,
    addr and size and discarded the owning-function grouping, exactly like its four siblings --
    it never asked "is this region shadowed". So the data was frozen into a flat, deduplicated
    committed input rather than dropped. state_regions_measured.json's note carries the full
    reasoning, including the 3 regions no other manifest claims (dropping them would renumber
    every later RID_* and invalidate two committed tables that index by that numbering).

    RENAMED AT F2E, one half of it: the EMITTED flag is MF_MEASURED now (see TAG_FLAG at the top of
    this file, which also records why the on-disk tag stays `shadow`). F2D left the old name on the
    reasoning that a rename buys no behaviour, which was true and was outweighed by what the name had
    become -- with the oracle deleted, a flag called `shadow` told every reader of mh_regions.gen.h
    that a region was covered by a mechanism the tree no longer contains. The 13 source comments and
    6 data files quoting the old flag were swept in the same change.
    """
    d = json.loads(MEASURED_REGIONS.read_text(encoding="utf-8-sig"))
    return [(int(r["addr"], 16), r["size"], r["name"]) for r in d["claims"]]


def claims_save():
    d = json.loads(SAVE_TABLE.read_text(encoding="utf-8-sig"))["tables"]
    out = []
    for _tbl, steps in d.items():
        for s in steps:
            if s.get("kind") == "raw" or not s.get("addr"):
                continue
            out.append((int(s["addr"], 16), s.get("size") or None, s.get("addr_sym")))
    return out


def emit_walkers(regions):
    """The per-region ORIGINAL-walker mask and the promotion-conditional movability predicate."""
    masks = walker_masks(regions)
    named = sum(1 for i in masks if masks[i])
    bits = [SAVE_WALKERS[t][0] for t in sorted(SAVE_WALKERS)] + [WALK_OTHER]
    L = [
        "// ---- the ORIGINAL block-table walkers, and why `is_movable` is not the last word --------",
        "//",
        "// MEASURED 2026-09-06, and it cost the one arm SB-HOSTFREE left open (dead-ends G139). A",
        "// relocated soak through a real LOADGAME diverged at exactly the load step, in `order_queue`",
        "// -- a region the census clears for relocation. The writer is the ORIGINAL",
        "// LoadPlanetFromDisk, whose destination for that block is the immediate 0x00bb4ed0 baked into",
        "// its own instruction stream (0x00448222), and an immediate cannot follow a bind.",
        "//",
        "// WHY `relocatable` SAYS YES ANYWAY, and it is not a bug in the census. We have a translated",
        "// body for all four of these walkers, so by the census's own rule they are OURS, and `ours`",
        "// never blocks. `relocatable` means A TRANSLATED BODY EXISTS -- never THAT BODY IS ARMED.",
        "// docs/state-boundary.md D7.7 wrote that gap down before it bit; this is it biting, and this",
        "// table is the gap closed for the one class where the address is data we already extract.",
        "//",
        "// ARMED IS A RUNTIME FACT, so it cannot live in the flag. The mask is static (which walkers",
        "// name these bytes); the host supplies which walkers its configuration armed, and",
        "// `movable_under` is where the two meet. Promoting the walker EARNS the region: measured, a",
        "// relocated LOADGAME soak with all four armed matches its golden over 3000 steps, and with",
        "// none armed the same run diverges at the load step.",
        "//",
        "// A TABLE WITH NO ini SWITCH GETS WALK_OTHER, which nothing can arm. That is the fail-safe",
        "// direction and it is the reason a newly extracted block table cannot silently make this",
        "// gate inert -- the failure mode every other gate in this file is shaped against.",
        "enum save_walker : uint8_t {",
        "    WALK_NONE           = 0u,",
    ]
    for n, name in enumerate(bits):
        who = next(
            (f"{t} (save walker {k})" for t, (b, k) in SAVE_WALKERS.items() if b == name), None
        )
        L.append(
            "    %-19s = 1u << %d,%s"
            % (name, n, ("  // " + who) if who else "  // a walker with no promotion switch")
        )
    L += [
        "    WALK_PROMOTABLE     = %du," % ((1 << (len(bits) - 1)) - 1),
        "};",
        "",
        "// Indexed by region_id. %d region(s) are named by at least one walker." % named,
        "inline constexpr uint8_t SAVE_WALKER_MASK[RID_COUNT] = {",
    ]
    row = []
    for i in range(len(regions)):
        row.append("%du," % sum(1 << bits.index(b) for b in masks.get(i, ())))
        if len(row) == 12:
            L.append("    " + " ".join(row))
            row = []
    if row:
        L.append("    " + " ".join(row))
    L += [
        "};",
        "",
        "constexpr uint8_t save_walkers(region_id r) { return SAVE_WALKER_MASK[r]; }",
        "",
        "// MAY THIS HOST MOVE IT? `is_movable` asks whether anything about the region forbids it;",
        "// this asks the remaining question, which only a running configuration can answer: is every",
        "// original walker that names these bytes standing down? Pass the armed set; 0 is the",
        "// shipping default (no save seam is promoted at ship -- mh::config::kSaveClosureOwned is",
        "// siblings are all 0), which is why the default answer is the conservative one.",
        "constexpr bool movable_under(region_id r, uint8_t armed) {",
        "    return is_movable(r) && (SAVE_WALKER_MASK[r] & static_cast<uint8_t>(~armed)) == 0;",
        "}",
        "",
        "constexpr int movable_count_under(uint8_t armed) {",
        "    int n = 0;",
        "    for (int i = 0; i < RID_COUNT; ++i)",
        "        if (movable_under(static_cast<region_id>(i), armed)) ++n;",
        "    return n;",
        "}",
        "",
        "// The regions this gate is ABOUT: movable on every other ground, and held back only by an",
        "// unarmed walker. If this is ever 0 the gate has gone inert and the arm below is vacuous --",
        "// which is why it is a named constant and not a comment.",
        "constexpr int WALKER_HELD_COUNT = movable_count() - movable_count_under(0u);",
        "static_assert(WALKER_HELD_COUNT > 0,",
        "              \"no movable region is named by an original save-block walker -- either the block\"",
        "              \" tables stopped being extracted or SAVE_WALKERS lost its table names; the\"",
        "              \" promotion-conditional refusal is now a no-op that still reports success\");",
        "",
    ]
    return L


def walker_masks(regions):
    """{rid_index: [WALK_* names]} -- which ORIGINAL block-table walkers reach each region.

    Every block with a resolved address counts, not only `kind: const`. A `raw`/`expr` block is one
    whose address the extractor could NOT pin to an immediate, which is a weaker claim to "this
    address follows a bind", not a stronger one -- so including it is the conservative direction and
    excluding it would be reading an absence of evidence as evidence.
    """
    d = json.loads(SAVE_TABLE.read_text(encoding="utf-8-sig"))["tables"]
    bounds = sorted(
        (r["base"], max(r.get("extent", 0), r["size"]), i) for i, r in enumerate(regions)
    )
    starts = [b for b, _s, _i in bounds]
    out = {}
    for tbl, steps in d.items():
        bit = SAVE_WALKERS.get(tbl, (WALK_OTHER, None))[0]
        for s in steps:
            if not s.get("addr") or not s.get("size"):
                continue
            a, n = int(s["addr"], 16), s["size"]
            j = max(0, bisect.bisect_right(starts, a) - 2)
            for base, size, i in bounds[j:]:
                if base >= a + n:
                    break
                if base + size > a and bit not in out.setdefault(i, []):
                    out[i].append(bit)
    return out


def claims_patch():
    """(4) Every `0xOLD->0xNEW` relocation note in the patch manifests.

    These are REF SITES, not regions -- `0xc3d2a2` is two bytes into `buildings`. They carry no size
    and are folded into whatever region contains them; a site that lands in no known region is what
    the report calls out, because it means an array is being relocated that nothing else describes.
    """
    out = []
    for p in sorted(PATCH_DIR.glob("*.mh.patch.json")):
        d = json.loads(p.read_text(encoding="utf-8-sig"))
        for entry in d.get("patches", []):
            m = re.search(r"0x([0-9a-f]{6,8})->0x([0-9a-f]{6,8})", entry.get("_note") or "")
            if m:
                out.append((int(m.group(1), 16), None, None))
    return out


def hash_manifest():
    """The ordered determinism-hash manifest. ORDER IS THE CONTRACT: the per-step `R` line emits one
    hash per entry in this order and mp_analyze labels the columns positionally, so anything but an
    APPEND silently re-labels every existing region."""
    return json.loads(HASH_MANIFEST.read_text(encoding="utf-8-sig"))["regions"]


def tact_manifest():
    """The TACTICAL hash manifest (TACT-PREP), a SECOND ordered list with the same append-only
    contract, emitted as its own `T` line on the llm_tact_frame cadence.

    It is separate rather than appended because the two oracles watch different code: mode 6 never
    calls llm_strat_sim_step, and folding ~290 KB of tactical arena into the strategic `combined`
    hash would let a strategic desync report name a region no strategic code touches. Absent (an
    older manifest) is not an error -- the tactical table is simply not emitted.
    """
    return json.loads(HASH_MANIFEST.read_text(encoding="utf-8-sig")).get("tact_regions", [])


def claims_hash():
    """(5) The hash manifest, as claims. Until ST2M this was PARSED OUT OF harness.cpp -- the table
    was the source, and the registry read it. The direction is now reversed.

    BOTH lists claim, because both are addresses an oracle hashes -- and a hashed address the
    registry cannot place is exactly the blind spot this file exists to close.
    """
    return [
        (int(e["addr"], 16), e["size"], "hash:" + e["name"])
        for e in hash_manifest() + tact_manifest()
    ]


READERS = {
    "view": claims_view,
    "shadow": claims_shadow,
    "save": claims_save,
    "patch": claims_patch,
    "hash": claims_hash,
}


# ---------------------------------------------------------------------------------------------
# The merge.


def load_matrix():
    """Optional enrichment: measured extents + island/shared ownership. Absent is not an error --
    every size the header needs is also available from (2)/(3)/(5); the matrix mainly supplies
    OWNERSHIP, which nothing else has."""
    if not MATRIX.exists():
        return None, {}
    m = json.loads(MATRIX.read_text(encoding="utf-8-sig"))
    islands = {i["region"] for i in m["islands"]}
    shared = {s["region"] if isinstance(s, dict) else s for s in m["shared"]}
    readonly = {s["region"] if isinstance(s, dict) else s for s in m["read_only"]}
    regs = {}
    for name, r in m["regions"].items():
        owner = (
            "island"
            if name in islands
            else "shared"
            if name in shared
            else "readonly"
            if name in readonly
            else "unknown"
        )
        regs[name] = {"addr": int(r["addr"], 16), "size": r["size"] or 1, "owner": owner}
    # `m["regions"]` is referenced-only, so a region no instruction ever names by address has no
    # size there and lands in the header as SIZE 0 -- present, but unmeasurable by every
    # attribution-based check. `m["sizes"]` carries the symbol's own size for the whole index and
    # is used ONLY to fill such a hole; it never creates a region (see fill_unmeasured_sizes).
    return regs, m.get("sizes") or {}


def ident(name, addr):
    """A C++ enumerator stem. `_G_LLM_STRAT_ORDER_QUEUE` -> `STRAT_ORDER_QUEUE`; an unnamed block
    keeps its address, which is stable and greppable."""
    if not name:
        return "BLK_%08X" % addr
    n = name
    for pre in ("_G_LLM_", "G_LLM_", "_G_", "hash:"):
        if n.startswith(pre):
            n = n[len(pre) :]
            break
    n = re.sub(r"[^A-Za-z0-9_]", "_", n).upper().strip("_")
    return n or ("BLK_%08X" % addr)


# Ghidra's auto-generated labels. A region carrying one of these is NAMELESS as far as this registry
# is concerned: `DAT_00fb4db0` says nothing a reader could not have derived from the address, and
# letting it win over a real symbol produced exactly the divergence this file exists to abolish --
# the addr manifest called 0x00fb4db0 `_G_LLM_STRAT_AI_ENGAGE_CANDIDATE_SCRATCH` while the registry
# called it `DAT_00fb4db0`, because the matrix is regenerated on its own schedule and was simply
# older than the rename (found 2026-08-01, AI0).
PLACEHOLDER_NAME = re.compile(r"^(DAT|FUN|LAB|UNK|SUB|OFF|PTR|ARRAY|s|u)_[0-9a-fA-F]{6,}$")


def is_real_symbol(name):
    """Does `name` carry information the address does not? Empty, a `hash:` label and a Ghidra
    auto-label are all equally uninformative, so they lose to any hand/LLM-assigned symbol."""
    if not name or name.startswith("hash:"):
        return False
    return not PLACEHOLDER_NAME.match(name)


def merge():
    """Fold every claim into a region set, keyed by CONTAINMENT rather than exact address.

    Order matters: the matrix's measured extents are laid down first so that a patch ref-site two
    bytes into `buildings` folds into `buildings` instead of inventing a region. Claims that land
    outside every known extent become regions of their own -- that is the correct outcome for the 23
    save blocks the matrix has no entry for, and the report is where an unexpected one shows up.
    """
    matrix, matrix_sizes = load_matrix()
    regions = {}  # base -> record
    warnings = []

    def add(base, size, name, tag, owner="unknown"):
        r = regions.get(base)
        if r is None:
            r = regions[base] = {
                "name": name,
                "base": base,
                "size": size or 0,
                "extent": 0,
                "owner": owner,
                "manifests": [],
                "size_claims": {},
            }
        if name and not is_real_symbol(r["name"]) and is_real_symbol(name):
            r["name"] = name  # prefer a real symbol over a hash label, a placeholder, or nothing
        if owner != "unknown" and r["owner"] == "unknown":
            r["owner"] = owner
        if size:
            r["size_claims"].setdefault(str(size), []).append(tag)
            # `size` is the CANONICAL extent -- the matrix's measurement when it has one, because it
            # is the only source that measures a symbol rather than a transfer. `extent` is the
            # furthest byte any claim reaches, which is what coverage has to be answered against: a
            # save block starting at _G_LLM_GAME_SESSION_MODE writes 1623 bytes across a dozen
            # adjacent globals, and that is a fact about the block, not a resizing of the symbol.
            if tag == "matrix" or not r["size"]:
                r["size"] = size
            r["extent"] = max(r.get("extent", 0), size)
        # `manifests` holds only the five real claimants -- the matrix pass seeds extents and
        # ownership, which is not a claim on the address by anything that would have to be updated.
        if tag in TAGS and tag not in r["manifests"]:
            r["manifests"].append(tag)

    if matrix:
        for name, r in matrix.items():
            add(r["addr"], r["size"], name, "matrix", r["owner"])

    def extents():
        keys = sorted(regions)
        return keys, [regions[k]["size"] or 1 for k in keys]

    for tag in TAGS:
        keys, sizes = extents()
        for base, size, name in READERS[tag]():
            i = bisect.bisect_right(keys, base) - 1
            if i >= 0 and base < keys[i] + sizes[i]:
                host = regions[keys[i]]
                if base != keys[i] and size and base + size > keys[i] + sizes[i]:
                    # A SHIFTED BLOCK, and the shape is worth naming rather than calling an overrun.
                    # The save's authoring idiom is `write(&array[0].some_field, sizeof(array))` --
                    # the address of a FIELD with the size of the WHOLE ARRAY -- so the transfer is
                    # displaced right by the field offset: it skips that many bytes at the head and
                    # runs the same number past the tail. Measured and settled 2026-07-30 against the
                    # indexing disp32s (docs/save-format.md "Blocks do not align to symbols"); both
                    # region bases involved are CORRECT, so this is a property of the block, not
                    # drift to fix. Reported every run because the day a region MOVES, the shifted
                    # window has to move with it.
                    head = base - keys[i]
                    tail = (base + size) - (keys[i] + sizes[i])
                    warnings.append(
                        "%s SHIFTED BLOCK on %s (0x%08x+%d): skips %+d at the head, runs %+d past "
                        "the tail" % (tag, host["name"], keys[i], sizes[i], head, tail)
                    )
                if base == keys[i]:
                    add(base, size, name, tag)
                else:
                    # Interior reference: it belongs to the host, and only the host is a region.
                    # Its reach still counts toward the host's extent -- the save block for
                    # _G_LLM_PROD_SHUTTLE_SLOTS starts 20 bytes into the array and runs 20 bytes
                    # past its measured end, and coverage has to know that.
                    if tag not in host["manifests"]:
                        host["manifests"].append(tag)
                    if size:
                        host["extent"] = max(host.get("extent", 0), base - host["base"] + size)
            else:
                add(base, size, name, tag)
                keys, sizes = extents()

    # A region nothing claims is matrix noise -- 2380 measured regions is not a useful header.
    out = [r for r in regions.values() if r["manifests"]]

    # UNMEASURABLE REGIONS: a claimed region that still has size 0 because no instruction names its
    # address. `_G_LLM_STRAT_AI_RESOURCE_VALUE_WEIGHTS` is the worked case -- a 1-based int[4] whose
    # every access reads `[idx*4 + <base-4>]`, so the disp32 in the code is the PREVIOUS symbol and
    # attribution can never land on it. A zero-length span contains no address, which makes the
    # region invisible to the closure derivation, the coverage lint and every reviewer. Fill it from
    # the symbol's own declared size. This only ever fills a hole in an ALREADY-claimed region, so it
    # cannot invent entries, and the size is tagged so a disagreement still shows up in the report.
    for r in out:
        if not r["size"] and r["name"] in matrix_sizes:
            size = matrix_sizes[r["name"]]
            if size:
                r["size"] = size
                r["extent"] = max(r.get("extent", 0), size)
                r["size_claims"].setdefault(str(size), []).append("symbol")
                warnings.append(
                    "UNMEASURABLE REGION sized from its symbol: %s (0x%08x) = %d bytes -- no "
                    "instruction names this address, so attribution alone reports it as SIZE 0"
                    % (r["name"], r["base"], size)
                )

    # Disagreements are REPORTED, never silently resolved: two manifests sizing one region
    # differently is exactly the class of drift this table exists to surface.
    for r in out:
        if len(r["size_claims"]) > 1:
            warnings.append(
                "SIZE DISAGREEMENT at 0x%08x (%s): %s"
                % (
                    r["base"],
                    r["name"] or "?",
                    "; ".join(
                        "%s bytes per %s" % (k, "+".join(v)) for k, v in r["size_claims"].items()
                    ),
                )
            )

    out.sort(key=lambda r: r["base"])
    # RESERVED: the enum's own sentinel. The game really does have globals named `count` and
    # `counter` (0x00708b1c / 0x0051de84, both in the region-graph save blocks), and `RID_COUNT`
    # would redefine the enumerator that terminates the table -- caught by the compiler, fixed here.
    seen = {"COUNT": None}
    for r in out:
        r["id"] = ident(r["name"], r["base"])
        if r["id"] in seen:
            r["id"] = "%s_%08X" % (r["id"], r["base"])
        seen[r["id"]] = r["base"]
        r["manifests"] = [t for t in TAGS if t in r["manifests"]]
    return out, warnings


# ---------------------------------------------------------------------------------------------
# Emission.


def addr_constants():
    """name -> VA for every `inline constexpr uintptr_t` in the generated address header. Used to
    emit the cross-check static_asserts, which are the whole point: one derivation, checked."""
    hdr = REPO / "src" / "mh_dll" / "mh" / "addr" / "mh_addrs.gen.h"
    txt = hdr.read_text(encoding="utf-8", errors="replace")
    return {
        m.group(1): int(m.group(2), 16)
        for m in re.finditer(
            r"inline constexpr uintptr_t\s+([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(0x[0-9a-fA-F]+)u?;", txt
        )
    }


def emit(regions):
    consts = addr_constants()
    L = [
        "//",
        "// addr/mh_regions.gen.h -- GENERATED by tools/gen_state_registry.py from",
        "// tools/data/state_regions.json. DO NOT EDIT: regenerate.",
        "//",
        "// THE STATE REGION REGISTRY (RI-STATE / ST1): one derivation of where each state region",
        "// lives, merged from the five manifests that used to answer that independently -- the module",
        "// state views, the shadow region sets, the save block table, the patcher's relocation sites",
        "// and harness.cpp's determinism hash. See the generator's header for why.",
        "//",
        "// TWO BASES, AND THE DIFFERENCE IS THE WHOLE OF ST2. `REGIONS[].base` is the STOCK base --",
        "// the .bss address the game's own code and the save FORMAT name, constexpr, never changing",
        "// while the binary does not. `live_base(rid)` is where those bytes ARE, which is the same",
        "// number until a module takes the region over and calls `rebase`. Consumers that DESCRIBE a",
        "// region (covering(), the save block table, a static_assert) use the stock base; consumers",
        "// that DEREFERENCE one (the hash, the shadow snapshots, state_io::resolve) use the live base.",
        "// Per-owner serializers -- a region that is not bytes at any address at all -- are ST4/ST6.",
        "//",
        "// `manifests` records WHO claims each region. A region claimed by one manifest is not wrong,",
        "// but a region claimed by several is one whose drift used to be silent.",
        "//",
        "#pragma once",
        "#include <cstdint>",
        "",
        '#include "addr/mh_addrs.gen.h"',
        "",
        "namespace mh::state {",
        "",
        "// Which manifest(s) claim a region -- a bitmask over the five sources.",
        "enum region_manifest : uint8_t {",
    ]
    for i, t in enumerate(TAGS):
        L.append("    MF_%-8s = 1u << %d," % (TAG_FLAG.get(t, t.upper()), i))
    L += [
        "};",
        "",
        "// Write-ownership as measured by the state matrix. ISLAND means one",
        "// subsystem writes it -- Law 2 says it may move once that subsystem is ours. SHARED means the",
        "// layout is frozen under Law 1 while any original accessor remains.",
        "enum region_owner : uint8_t {",
    ]
    for i, o in enumerate(OWNERS):
        L.append("    OWN_%-8s = %d," % (o.upper(), i))
    L += [
        "};",
        "",
        "struct region {",
        "    const char *name;",
        "    uint32_t    base;",
        "    uint32_t    size;  // the CANONICAL extent -- what the symbol measures. 0 = unestablished.",
        "    uint32_t    reach; // the furthest byte any manifest actually touches from `base`.",
        "                       // >= size when a consumer spans past the symbol: the save block that",
        "                       // starts at _G_LLM_GAME_SESSION_MODE writes 1623 bytes across a dozen",
        "                       // adjacent globals. That is a fact about the BLOCK, not a resizing of",
        "                       // the symbol, so both numbers are kept and coverage answers on reach.",
        "    uint8_t     owner;     // region_owner",
        "    uint8_t     manifests; // region_manifest bitmask",
        "",
        "    // ---- the ownership interlock (ST3), declared in tools/data/region_ownership.json ----",
        "    // `relocated` means the region has PHYSICALLY LEFT the binary: its bytes are somewhere",
        "    // the DLL allocated and the .bss address above is abandoned memory. It is emitted from",
        "    // that file's `relocated` field, NOT from `owned_by_dll`: the D4 split (SB-SOLE)",
        "    // redefined `owned_by_dll` to `libmh binds this region and no original function writes",
        "    // it`, which in the hosted build is true of hundreds of regions that have not moved an",
        "    // inch -- and `relocated && !l1` is a BUILD failure, so keying this on the weaker claim",
        "    // would have failed the build for stating a true thing.",
        "    // `l1` means a module answers for the content with a canonical stream instead of a",
        "    // consumer reading bytes. The two are INDEPENDENT -- mh::orders has served QUEUE since",
        "    // ST4 without moving it, because Law 1 freezes a layout the original still reads.",
        "    // `relocated && !l1` is the state nothing can reach at all, and the interlock refuses it.",
        "    bool        relocated;",
        "    bool        l1;",
        "",
        "    // ---- may a HOST put this region somewhere else? (SB-HOSTFREE, from the census) ----",
        "    // `false` means at least one instruction WE DO NOT OWN still names this address -- a",
        "    // baked disp32 in an original body, which cannot follow a bind. Relocating such a region",
        "    // puts the original half of the game on the abandoned .bss while our half reads the",
        "    // arena, so Law 1 forbids the move and this flag is how a host learns that mechanically",
        "    // instead of by judgement. DERIVED from tools/data/region_accessors.json:",
        "    // live_external_writers() (a `dead` original is not a writer) plus external_readers --",
        "    // a READER disqualifies just as a writer does, because it would read a stale snapshot.",
        "    // Note this is INDEPENDENT of `relocated`: `relocated` says a region HAS left the",
        "    // binary, `relocatable` says one COULD.",
        "    bool        relocatable;",
        "};",
        "",
        "enum region_id : uint16_t {",
    ]
    for i, r in enumerate(regions):
        L.append("    RID_%s = %d," % (r["id"], i))
    L += [
        "    RID_COUNT = %d," % len(regions),
        "};",
        "",
        "// ---- THE STOCK BASE, AND WHY IT IS A MACRO (LIB-REF-SPLIT) --------------------------",
        "//",
        "// `base` is an ORIGINAL-IMAGE ADDRESS. In the hosted build that is exactly what it must be:",
        "// the game's own code names those addresses, the save FORMAT resolves against them, and 765",
        "// static_asserts below pin each one to its mh::addr:: constant. In the STANDALONE build there",
        "// is no image, nothing is mapped at any of them, and a host answers for every region through",
        "// libmh_bind_regions -- so the column is not merely unused there, it is a 845-entry table of",
        "// addresses into a binary that is not loaded. Measured 2026-09-11: it was the single largest",
        "// source of original VAs in libmh.lib, 354 distinct / 5352 immediates in the code window and",
        "// 465 more above it, instantiated in fifteen state TUs.",
        "//",
        "// A MACRO RATHER THAN A SECOND TABLE, for crt_select.h's reason one table over: two emitted",
        "// arms of an 845-row table would be 845 rows nobody diffs, and the arm nobody compiles would",
        "// rot. This way the row is written once and the ADDRESS is what the configuration selects.",
        "//",
        "// WHAT STANDALONE CODE MAY DO WITH A ZERO BASE: nothing. Every consumer that DESCRIBES a",
        "// region by its stock address -- base_of(), covering(), region_at(), translate() -- is",
        "// #ifndef'd out of the standalone build rather than left to return a plausible answer about",
        "// address zero, so reaching for one is a COMPILE error naming the site. What survives is the",
        "// rid-keyed half: live_base()/bind()/ptr()/reach_of(), which never needed the stock column.",
        "#ifdef MH_LIBMH_BUILD",
        "#define MH_STOCK_BASE(va) 0u",
        "#else",
        "#define MH_STOCK_BASE(va) va",
        "#endif",
        "",
        "inline constexpr region REGIONS[RID_COUNT] = {",
    ]
    own = load_ownership()
    # SB-HOSTFREE: the census decides `relocatable`, and its ABSENCE must not read as "nothing may
    # move". A table of 827 falses would make the whole relocation arm a silent no-op that still
    # reports success -- the exact vacuous-green shape this registry exists to refuse -- so a missing
    # census stops the header being written instead.
    acc = load_accessors()
    if acc is None:
        raise SystemExit(
            "no accessor census (tools/data/region_accessors.json) -- `relocatable` cannot be\n"
            "derived, and defaulting it to false would make SB-HOSTFREE's relocation arm pass\n"
            "vacuously. Run `python tools/gen_region_accessors.py --refresh` first."
        )
    reloc, reloc_bytes = 0, 0
    for r in regions:
        mf = " | ".join("MF_" + TAG_FLAG.get(t, t.upper()) for t in r["manifests"]) or "0"
        rid = r.get("id") or ""
        decl = own.get(rid, {})
        can_move = not blocking_accessors(acc.get(rid))
        if can_move:
            reloc += 1
            reloc_bytes += max(r.get("extent", 0), r["size"])
        L.append(
            '    {"%s", MH_STOCK_BASE(0x%08xu), %uu, %uu, OWN_%s, static_cast<uint8_t>(%s), %s, %s, %s},'
            % (
                r["name"] or r["id"],
                r["base"],
                r["size"],
                max(r.get("extent", 0), r["size"]),
                r["owner"].upper(),
                mf,
                "true" if decl.get("relocated") else "false",
                "true" if decl.get("l1_serializer") else "false",
                "true" if can_move else "false",
            )
        )
    L += [
        "};",
        "",
        "// ---- what a relocating host may actually take (SB-HOSTFREE) -----------------------------",
        "//",
        "// These are COUNTED HERE, at generation, rather than by a host walking REGIONS[] -- so the",
        "// numbers a relocation run reports come from the same census that decided the flag, and a",
        "// run cannot claim a scope the data does not support. `RELOCATABLE_BYTES` sums `reach`,",
        "// because reach is what bind() forwards as the size and therefore what a host must",
        "// allocate.",
        "//",
        "// THESE ARE THE CENSUS'S NUMBERS, NOT THE HOST'S. `RELOCATABLE_COUNT` answers only `does a",
        "// live original accessor still name this address` -- the LAYOUT refuses some of those too",
        "// (D7.3), so the count a host actually moves is `movable_count()` below, and the bytes it",
        "// consumes are fewer than RELOCATABLE_BYTES. Sizing an arena off RELOCATABLE_BYTES is",
        "// therefore safe (it over-reserves) and is what the harness does; asserting a moved count",
        "// against RELOCATABLE_COUNT is NOT, and doing it was a real bug for one build.",
        "//",
        "// A ZERO-SIZE region is relocatable in principle and has nothing to move; it is counted",
        "// separately so no report has to explain a discrepancy it did not expect.",
        "constexpr int      RELOCATABLE_COUNT = %d;" % reloc,
        "constexpr uint32_t RELOCATABLE_BYTES = %uu;" % reloc_bytes,
        "constexpr int      BLOCKED_COUNT     = %d; // a live original accessor still names them"
        % (len(regions) - reloc),
        "constexpr int      ZERO_SIZE_COUNT   = %d;"
        % sum(1 for r in regions if not max(r.get("extent", 0), r["size"])),
        "",
        "// Is region `r` free of live original accessors? The census's verdict, and only that.",
        "constexpr bool is_relocatable(region_id r) { return REGIONS[r].relocatable; }",
        "",
        "// ---- and the TWO refusals that are properties of the LAYOUT, not of the census ----------",
        "//",
        "// Ten regions have `reach > size`: a manifest claims bytes PAST the symbol (the ten",
        "// overrunning save blocks, docs/state-boundary.md D6.6). That forbids two different moves,",
        "// and only the second one bites today:",
        "//",
        "//   1. THE OVERRUNNING REGION ITSELF. Moving it copies a window over its NEIGHBOURS into the",
        "//      arena and then poisons those neighbours' live storage -- regions the bind never",
        "//      claimed. All ten are also blocked by an original accessor, so this is unreachable",
        "//      now; it is written down because `relocatable` GROWS as promotion retires accessors.",
        "//",
        "//   2. ANY REGION LYING UNDER SUCH A WINDOW -- 21 of them under _G_LLM_GAME_SESSION_MODE's",
        "//      1623-byte block alone. Move one and the block keeps reading the stock address after",
        "//      the bytes left: it saves 0xCD where live values belong, and THAT FAILURE DOES NOT",
        "//      DIVERGE, because the damage is in bytes only the save format reads.",
        "//",
        "//      LIFTED 2026-09-06. Those four blocks are now DECOMPOSED -- save_table.gen.h's",
        "//      SLICED_BLOCKS carries each one's per-region runs and save_driver gathers/scatters",
        "//      them run by run, so a spanned region is read at ITS live base rather than at the",
        "//      block's host. tools/check_save_block_slices.py refuses a new block that spans regions",
        "//      and is not decomposed, which is what makes lifting the refusal safe rather than",
        "//      optimistic. The clause below therefore no longer consults this predicate; it is kept",
        "//      because refusal (1) still needs it and because it is what the lint's C++ mirror is.",
        "//",
        "// DERIVED FROM REGIONS[], not declared, so a new overrunning block starts protecting its",
        "// neighbours the moment this header is regenerated -- there is no list to forget. And",
        "// constexpr, so the counts below and bind_relocated() cannot come to disagree about which",
        "// regions moved, which is the drift that would make the non-vacuity assertion meaningless.",
        "constexpr bool under_overrunning_window(region_id r) {",
        "    for (int j = 0; j < RID_COUNT; ++j) {",
        "        if (j == static_cast<int>(r) || REGIONS[j].reach <= REGIONS[j].size) continue;",
        "        const uint32_t wlo = REGIONS[j].base + REGIONS[j].size; // the OVERRUN, not the window",
        "        const uint32_t whi = REGIONS[j].base + REGIONS[j].reach;",
        "        if (REGIONS[r].base < whi && wlo < REGIONS[r].base + REGIONS[r].reach) return true;",
        "    }",
        "    return false;",
        "}",
        "",
        "// MAY A HOST ACTUALLY MOVE THIS REGION? The one predicate bind_relocated() and every count",
        "// below agree on. `is_relocatable` alone is not it: that answers the census question only.",
        "// NO LONGER REFUSES A REGION MERELY FOR LYING UNDER AN OVERRUNNING WINDOW (2026-09-06): the",
        "// four blocks that span regions are decomposed now, so a spanned region is read at its own",
        "// live base. The region that OVERRUNS still may not move -- relocating it would carry a",
        "// window over its neighbours and poison their live storage, which decomposition does not",
        "// address.",
        "constexpr bool is_movable(region_id r) {",
        "    return REGIONS[r].relocatable && REGIONS[r].reach != 0 &&",
        "           REGIONS[r].reach == REGIONS[r].size;",
        "}",
        "",
        "constexpr int movable_count() {",
        "    int n = 0;",
        "    for (int i = 0; i < RID_COUNT; ++i)",
        "        if (is_movable(static_cast<region_id>(i))) ++n;",
        "    return n;",
        "}",
        "",
    ]
    L += emit_walkers(regions)
    L += [
        "// ---- the cross-check that makes this ONE derivation instead of a sixth -------------------",
        "// Every registry entry whose name is also an address constant is pinned to it. A Ghidra-side",
        "// VA change that reaches mh_addrs.gen.h but not this table (or the reverse) is now a COMPILE",
        "// error -- which is precisely the failure that produced a save block pointing at bytes",
        "// nothing writes.",
    ]
    pinned = 0
    # HOSTED ARM ONLY. Each of these compares the table's base against the address constant it was
    # derived from, which is the check that makes this one derivation instead of a sixth -- and it is
    # exactly as load-bearing as ever in the build where the addresses mean something. Standalone
    # MH_STOCK_BASE is 0 and every one of them would read `0 == mh::addr::X`, i.e. 765 assertions
    # that would all fail for a reason that is a configuration fact rather than a drift. Guarding the
    # BLOCK (one emitted #ifndef, not 765) keeps the check where it works and silent where it cannot.
    L.append("#ifndef MH_LIBMH_BUILD")
    for r in regions:
        n = r["name"]
        if n in consts and consts[n] == r["base"]:
            L.append("static_assert(REGIONS[RID_%s].base == mh::addr::%s);" % (r["id"], n))
            pinned += 1
    L.append("#endif // !MH_LIBMH_BUILD")
    L += [
        "",
        "// The STOCK base/size: what the manifests say, constexpr. Use these to DESCRIBE a region",
        "// (which region does this save-format address belong to?), never to dereference it.",
        "//",
        "// base_of is HOSTED-ONLY (LIB-REF-SPLIT): standalone it would answer 0 for every region,",
        "// which is not a smaller truth but a wrong one -- 845 regions all claiming to start at the",
        "// same address. size_of stays, because a size is a size in either configuration.",
        "#ifndef MH_LIBMH_BUILD",
        "constexpr uint32_t base_of(region_id r) { return REGIONS[r].base; }",
        "#endif",
        "constexpr uint32_t size_of(region_id r) { return REGIONS[r].size; }",
        "// What a HOST must allocate for this region, and what `bind()` forwards as its size: the",
        "// furthest byte any manifest touches, not what the symbol measures. They differ for 11",
        "// regions and the difference is load-bearing -- `covering()` below already answers on reach,",
        "// so a save block that overruns its symbol's tail resolves to a region, and then translate()",
        "// would bounds-check it against the SHORTER `size` and hand back nullptr. Binding reach is",
        "// what keeps that from firing (SB-BIND, docs/state-boundary.md D6.5-D6.6).",
        "constexpr uint32_t reach_of(region_id r) { return REGIONS[r].reach; }",
        "",
        "// ---- the live location (ST2) --------------------------------------------------------",
        "//",
        "// SEEDED FROM REGIONS[], not from a second generated address list. That is deliberate: a",
        "// parallel table of 243 bases would be exactly the kind of silently-diverging derivation",
        "// this header exists to abolish, and it would also have an INIT-ORDER window -- a consumer",
        "// running before the fill would read zero and hash nothing. A magic static has neither",
        "// problem: the table cannot be observed unfilled, at the price of one guard check per call.",
        "struct live_table {",
        "    uint32_t base[RID_COUNT];",
        "    uint32_t size[RID_COUNT];",
        "    uint32_t count[RID_COUNT]; // host-declared element capacity; 0 = not declared (SB-BIND)",
        "    bool     moved[RID_COUNT]; // the bytes are NOT at the stock base",
        "    bool     bound[RID_COUNT]; // the HOST has answered for this region (SB-BIND)",
        "    live_table() {",
        "        for (int i = 0; i < RID_COUNT; ++i) {",
        "            base[i]  = REGIONS[i].base;",
        "            size[i]  = REGIONS[i].size;",
        "            count[i] = 0;",
        "            moved[i] = false;",
        "            bound[i] = false;",
        "        }",
        "    }",
        "};",
        "// ONE TABLE PER PROCESS, NOT PER IMAGE (fork F4D). This used to be an unconditional magic",
        "// static, which is exactly right while the spine and the injection layer are ONE dll: the",
        "// linker folds the COMDAT and every reader shares the object. The split makes that silently",
        "// false -- libmh.dll and mh.dll would each get their own copy, mh::ai::island_move()'s 48",
        "// rebases would land in libmh's, and mh.dll's seams (harness, desync_watch, net_lockstep,",
        "// launch, ui_drive, gfx_overlay, net_diag, net_seams) would keep reading the stock bases. No",
        "// existing gate can see that: the arm log, the UI suite and even determinism stay green in a",
        "// run where nothing rebases. So the image that CONTAINS the spine owns the table, and any",
        "// other image declares it and reaches it across the boundary like every other row.",
        "//",
        "// MH_SPINE_IN_IMAGE is set by every project that compiles the 627-TU roster (libmh.vcxproj,",
        "// libmh_dll.vcxproj, libmh_std.vcxproj, mh_nettest.vcxproj) and by none that does not. It",
        "// needs no gate of its own: a roster-compiling project missing it fails to LINK (nothing",
        "// defines live()), and mh.vcxproj acquiring it fails to link too (its forwarding definition in",
        "// seams/libmh_bind.cpp would be a duplicate).",
        "//",
        "// libref_host.vcxproj USED TO SET IT and stopped at fork F4G, which is the rule working rather",
        "// than an exception to it: the standalone host imports its spine from Release\\standalone\\",
        "// libmh.dll now instead of linking the archive into its own image, so it no longer contains the",
        "// spine and must not own the table. Dropping the define turned these three into undefined",
        "// externals, i.e. into export rows of libmh_std.def -- which is exactly the boundary the",
        "// F4D finding says they have to cross.",
        "#ifdef MH_SPINE_IN_IMAGE",
        "inline live_table &live() {",
        "    static live_table t;",
        "    return t;",
        "}",
        "#else",
        "live_table &live(); // seams/libmh_bind.cpp -- libmh's table when bound, mh.dll's own when not",
        "#endif",
        "",
        "// Where region `r`'s bytes actually are. Identical to base_of(r) until someone rebases it.",
        "inline uint32_t live_base(region_id r) { return live().base[r]; }",
        "inline uint32_t live_size(region_id r) { return live().size[r]; }",
        "inline bool     is_rebased(region_id r) { return live().moved[r]; }",
        "",
        "// A module that has taken a region over declares where it really put it. Everything that",
        "// DEREFERENCES the region follows from here on; everything that DESCRIBES it does not.",
        "//",
        "// `size` may SHRINK or GROW: a relocated array is still the same region, and the size is what",
        "// the hash and the shadow snapshot will walk, so a caller that moves a region into a smaller",
        "// allocation than the .bss reserved for it must say so or the walk reads past its own buffer.",
        "inline void rebase(region_id r, uint32_t base, uint32_t size) {",
        "    live().base[r]  = base;",
        "    live().size[r]  = size;",
        "    live().moved[r] = true;",
        "}",
        "",
        "// Put a region back where the binary has it. For test fixtures and for a module that",
        "// relinquishes a region -- NOT something the live build should ever need.",
        "inline void unrebase(region_id r) {",
        "    live().base[r]  = REGIONS[r].base;",
        "    live().size[r]  = REGIONS[r].size;",
        "    live().count[r] = 0;",
        "    live().moved[r] = false;",
        "    live().bound[r] = false;",
        "}",
        "",
        "// ---- the host bind (SB-BIND / D4) ---------------------------------------------------",
        "//",
        "// THE HOST answers where each region's bytes are. This is `rebase` with one difference that",
        "// is the whole point of the hosted arm: `moved` is DERIVED from the answer rather than set",
        "// unconditionally, so binding a region to the address it already has is a genuine no-op --",
        "// not merely one that measures as zero. That matters three ways, all of them things a",
        "// set-it-always version breaks: translate() stays on its identity path (it is gated on",
        "// `moved`), ai::island_move() can still take its 48 regions later (it refuses to rebase a",
        "// region twice), and statetest's opening `nothing has moved` premise still holds.",
        "//",
        "// `bound` is tracked separately BECAUSE of that: once `moved` no longer means `the host",
        "// answered`, something else has to, or `did every region get an answer?` becomes",
        "// unanswerable in exactly the configuration where the answer is `yes, all of them, stock`.",
        "inline void bind(region_id r, uint32_t base, uint32_t size, uint32_t count = 0) {",
        "    live().base[r]  = base;",
        "    live().size[r]  = size;",
        "    live().count[r] = count;",
        "    live().moved[r] = (base != REGIONS[r].base);",
        "    live().bound[r] = true;",
        "}",
        "// The host's declared element capacity, or 0 for `it did not say`. STORED BUT NOT YET READ",
        "// BY ANY CAP: SB-BIND T2 is what turns the eight per-player roster constants into reads of",
        "// this, with 0 meaning `fall back to the compile-time constant`. It is accepted and kept now",
        "// rather than dropped, so that a host filling the ABI struct's `count` field is not silently",
        "// ignored in the window between T1 and T2.",
        "inline uint32_t live_count(region_id r) { return live().count[r]; }",
        "inline bool is_bound(region_id r) { return live().bound[r]; }",
        "inline int  bound_count() {",
        "    int n = 0;",
        "    for (int i = 0; i < RID_COUNT; ++i)",
        "        if (live().bound[i]) ++n;",
        "    return n;",
        "}",
        "",
        "// A typed pointer at a region's LIVE base. This is the call the module state views make, so",
        "// that binding a view and describing a save block cannot disagree about an address again --",
        "// and so that a view keeps pointing at the bytes after its region moves.",
        "template <class T> inline T *ptr(region_id r) {",
        "    return reinterpret_cast<T *>(static_cast<uintptr_t>(live_base(r)));",
        "}",
        "",
        "// The region wholly containing [addr, addr+size), or nullptr. The save driver resolves every",
        "// block through this, so a block naming an address no region covers is a hard failure rather",
        "// than a silent read of whatever happens to be there.",
        "//",
        "// constexpr ON PURPOSE: it lets save_driver.cpp assert AT COMPILE TIME that every block in the",
        "// generated save table is covered. The generator checks the same property (against the",
        "// COMMITTED registry, which is the arm that can actually go red -- see check_coverage), so",
        "// this is the belt to that braces: it catches a hand-edit of either generated header, which",
        "// the tool-side check by construction cannot see.",
        "// ---- the ownership interlock, compile-time arm (ST3) --------------------------------------",
        "// The arm that can actually go red on a DATA change is gen_state_registry.py's check_ownership,",
        "// which names the region AND the manifest that still reaches it -- read ITS message, not this",
        "// assert, when this fires. This is the belt to that braces: it catches a hand-edit of the",
        "// generated header, which the tool-side check by construction cannot see, and it makes 'a region",
        "// that left the binary is still reachable by an address' a BUILD failure, not a runtime surprise.",
        "constexpr bool interlock_ok() {",
        "    for (const region &r : REGIONS)",
        "        if (r.relocated && !r.l1) return false;",
        "    return true;",
        "}",
        "static_assert(interlock_ok(),",
        "              \"RI-STATE/ST3: a region is marked `relocated` (it has LEFT the binary) but no \"",
        "              \"module serves it -- every consumer would be reading the address it abandoned. \"",
        "              \"Run tools/gen_state_registry.py --check for the region and the manifest.\");",
        "",
        "// HOSTED-ONLY (LIB-REF-SPLIT). This is the stock-address resolver: it answers `which region",
        "// owns this original .bss address`, a question that has no meaning without the image. With a",
        "// zeroed base column every region would start at 0 and the first row with a non-zero reach",
        "// would swallow every query -- a plausible answer, always wrong, and silent. So it is not",
        "// declared standalone, and a standalone consumer of a stock address is a COMPILE error at the",
        "// site rather than a resolver that lies. The rid-keyed route is live_base(rid) + offset.",
        "#ifndef MH_LIBMH_BUILD",
        "constexpr const region *covering(uint32_t addr, uint32_t size) {",
        "    for (const region &r : REGIONS)",
        "        if (r.reach && addr >= r.base && uint64_t(addr) + size <= uint64_t(r.base) + r.reach)",
        "            return &r;",
        "    return nullptr;",
        "}",
        "#endif // !MH_LIBMH_BUILD",
        "",
    ]
    L += emit_hash_view(regions)
    L += [
        "} // namespace mh::state",
        "",
    ]
    return "\n".join(L), pinned


# ---------------------------------------------------------------------------------------------
# The ordered hash manifest (ST2M): the determinism harness's region list, as SLICES.


def resolve_slices(regions, entries=None):
    """Attach every hash-manifest entry to the registry region that contains it.

    A miss is fatal rather than a warning: an entry the registry cannot place is an address the
    determinism oracle would hash without anything else in the tree knowing those bytes exist --
    which is precisely the blind spot the registry was built to close.
    """
    ext = sorted(
        (r["base"], r["base"] + max(r.get("extent", 0), r["size"]), i)
        for i, r in enumerate(regions)
        if max(r.get("extent", 0), r["size"])
    )
    starts = [e[0] for e in ext]
    out, misses = [], []
    for e in hash_manifest() if entries is None else entries:
        base, size = int(e["addr"], 16), e["size"]
        i = bisect.bisect_right(starts, base) - 1
        host = None
        while i >= 0 and starts[i] + 0x200000 >= base:
            if base >= ext[i][0] and base + size <= ext[i][1]:
                host = regions[ext[i][2]]
                break
            i -= 1
        if host is None:
            misses.append(e)
            continue
        out.append(dict(e, rid=host["id"], offset=base - host["base"], host=host["name"]))
    return out, misses


def emit_hash_view(regions):
    slices, misses = resolve_slices(regions)
    if misses:
        raise SystemExit(
            "hash manifest entries that no registry region contains:\n"
            + "\n".join("   %-20s %s +%d" % (m["name"], m["addr"], m["size"]) for m in misses)
            + "\n(re-run with --refresh, or the entry is hashing bytes nothing else describes)"
        )
    L = [
        "// ---- the ordered determinism hash manifest (ST2M) ----------------------------------------",
        "// seams/harness.cpp hashes these, in THIS ORDER, once per sim step; the per-step `R` line",
        "// carries one hash per entry and tools/mp_analyze.py labels the columns POSITIONALLY. So the",
        "// order is a wire contract: anything but an APPEND silently re-labels every existing region,",
        "// and every desync report after it names the wrong one while looking well-formed.",
        "//",
        "// These are SLICES, not regions: %d entries over %d registry regions, because `p0_local` and"
        % (len(slices), len({s["rid"] for s in slices})),
        "// its siblings are interior windows into the single `player_data` region. Carrying (region,",
        "// offset, length) rather than a bare address is what lets a moved region take its slices with",
        "// it.",
        "//",
        "// A slice may be a WINDOW into its region; it may NOT run past that region's measured size.",
        "// The static_asserts below check containment against `reach`, which is no check at all for an",
        "// entry like this -- a hash claim RAISES the reach it is then measured against. So the real",
        "// rule lives in the generator (check_slice_overruns, SB-HOSTFREE H0) and it is what stops",
        "// another `time_globals`: one 48-byte entry that quietly spanned six separate, independently",
        "// bindable clock regions, and would have hashed 40 bytes of nothing the moment a host moved",
        "// them.",
        "//",
        "// `excluded` = dropped from the STATE-ONLY verdict (still hashed, still printed per-region).",
        "// The adjudication for each one is the comment where it is declared.",
        "struct hash_region {",
        "    const char *name;",
        "    region_id   rid;    // the registry region this slice lives in",
        "    uint32_t    offset; // bytes from that region's base",
        "    uint32_t    base;   // == REGIONS[rid].base + offset -- the STOCK address, constexpr, so",
        "                        // the static_asserts below can pin the slice inside its region at",
        "                        // COMPILE time. To READ the slice use hash_base(i), which follows a",
        "                        // rebase; this field deliberately does not.",
        "    uint32_t    len;",
        "    bool        excluded;",
        "};",
        "",
        "inline constexpr hash_region HASH_REGIONS[] = {",
    ]
    for s in slices:
        for p in s["pre"]:
            L.append(("    // " + p).rstrip() if p else "    //")
        row = '    {"%s", RID_%s, %uu, MH_STOCK_BASE(0x%08xu), %uu, %s},' % (
            s["name"],
            s["rid"],
            s["offset"],
            int(s["addr"], 16),
            s["size"],
            "true" if s["excluded"] else "false",
        )
        L.append(row + (" // " + s["inline"] if s["inline"] else ""))
        for c in s["cont"]:
            L.append(("    // " + c).rstrip() if c else "    //")
    L += [
        "};",
        "constexpr int HASH_REGION_COUNT = %d;" % len(slices),
        "",
        "// Named indices. Generated, so they cannot drift out of step with the table the way the",
        "// hand-written IDX_* constants could -- that drift is what the `APPENDED, not inserted`",
        "// warnings above were guarding by hand.",
        "enum hash_region_index : int {",
    ]
    for i, s in enumerate(slices):
        L.append("    HIDX_%-20s = %d," % (s["name"].upper(), i))
    L += [
        "};",
        "",
        "// Each slice really is inside the region it claims, and really is at the address the manifest",
        "// says. Both are compile errors rather than a silent out-of-bounds hash.",
    ]
    # The ADDRESS half of this pair is hosted-only for MH_STOCK_BASE's reason; the CONTAINMENT half
    # is not, so it is emitted in both arms. Splitting them keeps the check that still means something
    # standalone (a slice must fit inside its region) instead of losing both to one guard.
    L.append("#ifndef MH_LIBMH_BUILD")
    for i, s in enumerate(slices):
        L.append(
            "static_assert(HASH_REGIONS[%d].base == REGIONS[RID_%s].base + %uu);"
            % (i, s["rid"], s["offset"])
        )
    L.append("#endif // !MH_LIBMH_BUILD")
    for i, s in enumerate(slices):
        L.append(
            "static_assert(%uu + HASH_REGIONS[%d].len <= REGIONS[RID_%s].reach);"
            % (s["offset"], i, s["rid"])
        )
    L += [
        "",
        "// Where slice `i` is RIGHT NOW: its region's live base plus the slice's offset. Not",
        "// constexpr, and not HASH_REGIONS[i].base -- that one is frozen at the stock address on",
        "// purpose (see the field comment). A consumer that hashes, dumps or pokes a slice reads",
        "// THIS, which is what stops the determinism oracle hashing bytes nothing writes after a",
        '// region moves (gates can pass vacuously).',
        "inline uint32_t hash_base(int i) { return live_base(HASH_REGIONS[i].rid) + HASH_REGIONS[i].offset; }",
        "#ifndef MH_LIBMH_BUILD // LIB-REF-SPLIT: the stock address, absent standalone",
        "inline uint32_t hash_stock_base(int i) { return HASH_REGIONS[i].base; }",
        "#endif",
        "",
    ]
    L += emit_tact_hash_view(regions)
    L += [
        "// ---- is a relocation run WATCHING anything? (SB-HOSTFREE) --------------------------------",
        "//",
        "// A host that relocates every region it is allowed to relocate has proved nothing unless the",
        "// determinism hash actually reads some of the moved bytes. Most hash slices sit on regions",
        "// an original accessor pins in place, so that is a real risk rather than a rhetorical one:",
        "// the run would be green, the report would say `N regions relocated`, and every hashed byte",
        "// would still be at its stock address. These two counts are what a relocation report",
        "// asserts against, and they are constexpr so the assertion cannot drift from the tables.",
        "//",
        "// They will RISE as promotion retires original accessors, which is the point -- the",
        "// relocatable frontier is the same frontier LIB-REF and SB-SOLE advance.",
        "// ON `is_movable`, NOT `relocatable`: the census clears 463 regions, but the layout refuses",
        "// some of those (a window that overruns its symbol, D7.3), and what the hash can see is what",
        "// actually MOVES. Counting the wider set here would have made the non-vacuity assertion",
        "// disagree with the bind by exactly the regions the bind was right to refuse.",
        "constexpr int hash_slices_on_relocatable() {",
        "    int n = 0;",
        "    for (int i = 0; i < HASH_REGION_COUNT; ++i)",
        "        if (is_movable(HASH_REGIONS[i].rid)) ++n;",
        "    return n;",
        "}",
    ]
    if tact_manifest():
        L += [
            "constexpr int tact_hash_slices_on_relocatable() {",
            "    int n = 0;",
            "    for (int i = 0; i < TACT_HASH_REGION_COUNT; ++i)",
            "        if (is_movable(TACT_HASH_REGIONS[i].rid)) ++n;",
            "    return n;",
            "}",
        ]
    L += [""]
    return L


def emit_tact_hash_view(regions):
    """The TACTICAL hash manifest, as a sibling table. Empty list if the manifest has none."""
    NL = chr(10)
    entries = tact_manifest()
    if not entries:
        return []
    slices, misses = resolve_slices(regions, entries)
    if misses:
        raise SystemExit(
            "TACTICAL hash manifest entries that no registry region contains:"
            + NL
            + NL.join("   %-20s %s +%d" % (m["name"], m["addr"], m["size"]) for m in misses)
            + NL
            + "(re-run with --refresh so the registry learns the tactical arena)"
        )
    L = [
        "// ---- the ordered TACTICAL hash manifest (TACT-PREP) --------------------------------------",
        "// The same shape as HASH_REGIONS above and the same append-only order contract, but a",
        "// SEPARATE list on a SEPARATE cadence: seams/harness.cpp hashes these once per llm_tact_frame",
        "// and emits them as a `T` line. Two tables rather than one appended table because mode 6 never",
        "// calls llm_strat_sim_step -- the strategic hook cannot see a tactical frame at all -- and",
        "// because folding the arena into the strategic `combined` hash would let a strategic desync",
        "// report name a region no strategic code touches.",
        "//",
        "// `tile_objects` and `passable` appear in BOTH lists on purpose: they are shared planes that",
        "// tactical uses as scratch and the exit restores from disk (the tactical-probe work Sect. 5).",
        "inline constexpr hash_region TACT_HASH_REGIONS[] = {",
    ]
    for sl in slices:
        for pre in sl["pre"]:
            L.append(("    // " + pre).rstrip() if pre else "    //")
        row = '    {"%s", RID_%s, %uu, MH_STOCK_BASE(0x%08xu), %uu, %s},' % (
            sl["name"],
            sl["rid"],
            sl["offset"],
            int(sl["addr"], 16),
            sl["size"],
            "true" if sl["excluded"] else "false",
        )
        L.append(row + (" // " + sl["inline"] if sl["inline"] else ""))
        for c in sl["cont"]:
            L.append(("    // " + c).rstrip() if c else "    //")
    L += [
        "};",
        "constexpr int TACT_HASH_REGION_COUNT = %d;" % len(slices),
        "",
        "// Named indices, same reasoning as HIDX_* above: an ALIAS slice (one that re-emits another",
        "// slice's bytes under a different traversal) has to be told apart from its source by INDEX, and a",
        "// hand-written constant would drift the moment the table grows.",
        "enum tact_hash_region_index : int {",
    ]
    for i, sl in enumerate(slices):
        L.append("    TIDX_%-24s = %d," % (sl["name"].upper(), i))
    L += [
        "};",
        "",
    ]
    L.append("#ifndef MH_LIBMH_BUILD")
    for i, sl in enumerate(slices):
        L.append(
            "static_assert(TACT_HASH_REGIONS[%d].base == REGIONS[RID_%s].base + %uu);"
            % (i, sl["rid"], sl["offset"])
        )
    L.append("#endif // !MH_LIBMH_BUILD")
    for i, sl in enumerate(slices):
        L.append(
            "static_assert(%uu + TACT_HASH_REGIONS[%d].len <= REGIONS[RID_%s].reach);"
            % (sl["offset"], i, sl["rid"])
        )
    L += [
        "",
        "// Where tactical slice `i` is right now -- the rebase-following read, same contract as",
        "// hash_base() above.",
        "inline uint32_t tact_hash_base(int i) {",
        "    return live_base(TACT_HASH_REGIONS[i].rid) + TACT_HASH_REGIONS[i].offset;",
        "}",
        "",
    ]
    return L


# ---------------------------------------------------------------------------------------------


BEGIN = "# --- BEGIN generated region manifest (tools/gen_state_registry.py) ---"
END = "# --- END generated region manifest ---"


def analyze_block():
    """The two positional lists mp_analyze.py needs, as a generated marker block.

    Only the LITERALS are generated. mp_analyze's own commentary -- which is not a copy of the
    harness's, it is the analyzer-side adjudication history -- stays hand-maintained above the
    BEGIN marker, so nothing that was written down is lost to a regeneration.
    """
    entries = hash_manifest()
    L = [
        BEGIN,
        "# Source: tools/data/hash_manifest.json. DO NOT EDIT between the markers -- edit the JSON",
        "# and run `python tools/gen_state_registry.py`. REGION_NAMES is POSITIONAL: it maps the",
        "# `R <step> h0 h1 ...` columns to names, so it must stay in the same order as the DLL's",
        "# HASH_REGIONS[] -- which is now the same list, which is the point.",
        "REGION_NAMES = [",
    ]
    for e in entries:
        for p in e["pre"]:
            L.append(("    # " + p).rstrip() if p else "    #")
        L.append('    "%s",%s' % (e["name"], "  # " + e["inline"] if e["inline"] else ""))
        for c in e["cont"]:
            L.append(("    # " + c).rstrip() if c else "    #")
    L += [
        "]",
        "# Mirrors the DLL's hash_region::excluded flag. NOTE the frame-rate-dependent regions (the",
        "# six clock doubles, frame_ring, fps_estimate) ARE in this set: the harness has always",
        "# excluded them, but it resolved them by NAME at init rather than through an IDX_* constant,",
        "# so the old mirror lint -- which scanned for IDX_ tokens -- could not see them and reported",
        "# the two sides as agreeing while they did not.",
        "STATE_EXCLUDED = {",
    ]
    for e in entries:
        if e["excluded"]:
            L.append('    "%s",' % e["name"])
    L += ["}", END]
    return "\n".join(L)


def analyze_text():
    """mp_analyze.py with its marker block refreshed."""
    txt = ANALYZE.read_text(encoding="utf-8")
    i, j = txt.index(BEGIN), txt.index(END) + len(END)
    return txt[:i] + analyze_block() + txt[j:]


# ---------------------------------------------------------------------------------------------
# THE OWNERSHIP INTERLOCK (RI-STATE / ST3).
#
# ST2 made a region's address LIVE, so a consumer that dereferences one follows it when a module
# rebases it. That is enough for a region that MOVED and is still an array. It is not enough for a
# region that LEFT: once a subsystem owns its state for real, the representation stops being bytes
# at any address -- there is no base to follow, and `base + len` is not a wrong answer, it is not an
# answer. Anything still reaching such a region by address is therefore a BUILD ERROR, not a runtime
# surprise. Same trick as C1, which made a promoted seam that silently un-fixes a patch a compile
# error rather than something to remember.
#
# IT ALSO CLOSES A VACUOUS ORACLE, which is the sharper reason to have it. SV1-P-LOAD's A/B compares
# "the state regions the block table names" at FIXED addresses; after an island move it would compare
# stale addresses on both arms and pass green. The interlock, not the oracle, is what catches that.


def load_ownership():
    """The hand-declared ownership file. Absent = nothing is owned, which is a valid state."""
    if not OWNERSHIP.exists():
        return {}
    return json.loads(OWNERSHIP.read_text(encoding="utf-8-sig"))["regions"]


def load_accessors():
    """The ORIGINAL-accessor census (SIM-BOUNDARY), keyed by the same region id as the ownership file.

    Committed data, so `--check` stays Ghidra-free -- same contract as every other input here.
    Produced by tools/gen_region_accessors.py --refresh from tmp/state_matrix.json; absent = rules
    4-6 below cannot run, which is itself reported rather than passed over.
    """
    if not ACCESSORS.exists():
        return None
    return json.loads(ACCESSORS.read_text(encoding="utf-8-sig"))["regions"]


def blocking_accessors(entry):
    """The original accessors that FORBID relocating a region (SB-HOSTFREE). Names, not a count.

    An original accessor is a baked `disp32` in an instruction we do not own. It cannot follow a
    bind, so moving a region one still touches puts the original half of the game on the abandoned
    .bss while our half reads the arena -- the determinism hash would then compare a frozen copy
    against a live one, or worse, a frozen copy against itself.

    FOUR JUDGEMENTS, every one of them the conservative direction:
      * a `dead` original writer is NOT a blocker. `external_writers_dead` is an evidenced claim
        that nothing reaches the function (three or four concurring tools), so its writes never
        happen. This is the same distinction live_external_writers() draws for the ownership
        interlock, deliberately spelled the same way.
      * a READER blocks exactly as a writer does. It would read a stale snapshot of bytes that have
        moved, which is quieter than a lost write and no less wrong.
      * an ADDRESS-TAKER blocks too, and this one was learned the hard way (dead-ends G136). The
        census used to DROP a cell with only `addr_of`/`imm` counts -- correct for "who reads these
        bytes", wrong for "may a host move them", because `push offset X; call CreateMutexA` reaches
        the bytes through a pointer no bind can follow. `_G_LLM_SINGLE_INSTANCE_MUTEX_NAME` read as
        accessor-free on that rule; relocating it made every lane's mutex name garbage and every
        game exited on the single-instance guard at step 0, with no crash to look at.
      * UNMEASURED blocks. `measured: false` means the instruction scan attributed NOTHING to these
        bytes -- which is the absence of evidence, not evidence of absence, and 60 regions are in
        that state. A region nothing measured is the last one to hand to a relocating host.

    Returns the blocking function names, or the sentinel `["(unmeasured)"]` when there is nothing
    measured to name -- callers only ask whether the list is empty.
    """
    if not entry:
        # Not "measured, nothing found" -- the census has no row for this region at all.
        return ["(unmeasured)"]
    if not entry.get("measured"):
        return ["(unmeasured)"]
    dead = set(entry.get("external_writers_dead") or ())
    return (
        [f for f in (entry.get("external_writers") or ()) if f not in dead]
        + list(entry.get("external_readers") or ())
        + list(entry.get("external_addr_takers") or ())
    )


def claim_windows(regions):
    """Every manifest claim, resolved to the registry region that HOSTS it.

    Yields (region, tag, offset, size, name). This is deliberately re-derived from the five readers
    rather than read off `region["manifests"]`: the merged field records WHICH manifests claim a
    region and the interlock needs WHERE each one reaches, which is the whole of rule 2.
    """
    by_base = {r["base"]: r for r in regions}
    keys = sorted(by_base)
    reach = [max(by_base[k].get("extent", 0), by_base[k]["size"] or 0) or 1 for k in keys]
    for tag in TAGS:
        for base, size, name in READERS[tag]():
            i = bisect.bisect_right(keys, base) - 1
            if i < 0 or base >= keys[i] + reach[i]:
                continue
            yield by_base[keys[i]], tag, base - keys[i], (size or 0), name


def check_ownership(regions, ownership, accessors=None):
    """Violations of the interlock, as printable strings. Empty = the tree is consistent.

    RULES 1-3 KEY ON `relocated`, NOT ON `owned_by_dll` -- the D4 split (SB-SOLE, 2026-09-10).
    `owned_by_dll` used to mean "the bytes have PHYSICALLY LEFT the binary" and every rule hung off
    it. D4 (docs/state-boundary.md) redefined it to "libmh binds this region", and in the HOSTED
    configuration the host answers the bind table with the stock .bss base -- nothing moves, so a
    manifest that reaches the region by address reaches the live bytes and is not a violation at
    all. The physical-move claim did not disappear, it got its own field: `relocated`. That is what
    arms rules 1-3 and rule 6, it is what LIB-REF's standalone build will set, and it is what the AI
    island's 48 regions carry today because ai::island_move() really does move them. Rules 4, 5 and
    9 stay on `owned_by_dll`, because "no original function writes it" is exactly what the redefined
    mark asserts.

    SEVEN rules in three families. Rules 1-3 (here) ask whether one of OUR manifests still reaches a
    RELOCATED region by address; rules 4-6 (check_original_accessors, below) ask whether the ORIGINAL
    BINARY still does; rules 9-10 (check_writer_boundary) police the pair of claims themselves. Both
    of the first two halves have to hold: a region can be perfectly served to every manifest of ours
    and still be indexed by 116 original functions.

    Three rules, and the second and third exist because the first one alone would pass things that
    are still broken:

      1. relocated with NO l1_serializer, reached by any manifest -> the bytes are not where that
         address is, and nothing can serve them. Names the region AND the manifest, because "which
         manifest still reaches it" is the entire actionable content.
      2. relocated WITH a serializer, but a manifest window that is not the WHOLE region ->
         mh::state::owner_serves requires offset==0 and len==size, so a sub-window or a SHIFTED block
         falls back to the RAW address, which after a move is abandoned memory. Reported with the
         head skip and the tail length rather than as a generic miss, because those two numbers are
         what a serializer would have to reproduce.
      3. relocated reached by the PATCH manifest, serializer or not -> a patch ref site is a
         constant baked into the original binary's instruction stream. It cannot follow anything, so
         a serializer does not rescue it; that region cannot leave while any patch names it.
    """
    # AGGREGATED, one line per (region, manifest, rule), not one per claim. The patch manifest alone
    # names thousands of ref sites inside `buildings`; twelve thousand lines saying the same thing is
    # a report nobody reads. The count and one exemplar address carry the actionable content, EXCEPT
    # for rule 2, where the head skip and tail length differ per window and are the whole message --
    # so those stay per distinct window (deduped, since the save and load tables carry each one).
    out, seen = [], {}
    for r, tag, off, size, name in claim_windows(regions):
        decl = ownership.get(r.get("id") or "")
        if not decl or not decl.get("relocated"):
            continue
        who = r["name"] or r.get("id") or "0x%08x" % r["base"]
        served = decl.get("l1_serializer")
        canon = r["size"] or 0
        if tag == "patch":
            key = ("patch", who)
        elif not served:
            key = ("unserved", who, tag)
        elif off != 0 or (size and canon and size != canon):
            key = ("shifted", who, tag, off, size)
        else:
            continue
        if key in seen:
            seen[key][0] += 1
            continue
        seen[key] = [1, (r, tag, off, size, name, who, served, canon)]
        out.append(key)

    lines = []
    for key in out:
        n, (r, tag, off, size, name, who, served, canon) = seen[key]
        more = "" if n == 1 else " (and %d more site(s))" % (n - 1)
        if key[0] == "patch":
            lines.append(
                "RELOCATED REGION REACHED BY A BAKED ADDRESS: %s is relocated, and the PATCH manifest "
                "names 0x%08x (+%d) inside it%s. A patch ref site is a constant in the original "
                "instruction stream -- it cannot follow a move, and %s does not rescue it. That "
                "region cannot leave the binary while any patch names it."
                % (
                    who,
                    r["base"] + off,
                    off,
                    more,
                    ("serializer %s" % served) if served else "no serializer",
                )
            )
        elif key[0] == "unserved":
            lines.append(
                "RELOCATED REGION REACHED BY AN ADDRESS: %s is relocated with NO l1_serializer, and "
                "the %s manifest still reaches it at 0x%08x (+%d, %d bytes, '%s')%s. Its bytes are "
                "no longer at that address and no module can answer for them -- give it a "
                "serializer (state/region_owner.h) or take the claim out of that manifest."
                % (who, tag.upper(), r["base"] + off, off, size, name or "?", more)
            )
        else:
            tail = off + size - canon if (size and canon) else 0
            lines.append(
                "SHIFTED WINDOW ON A RELOCATED REGION: %s is relocated and served by %s, but the %s "
                "manifest reaches it with a window that is not the whole region -- 0x%08x skips +%d "
                "at the head and runs %+d past the tail of a %d-byte region%s. owner_serves() only "
                "serves offset==0/len==size, so this window would fall back to the raw address, "
                "which is the memory the region left. The serializer must reproduce that window "
                "explicitly, or the block must be re-expressed as slices."
                % (who, served, tag.upper(), r["base"] + off, off, tail, canon, more)
            )

    lines += check_original_accessors(ownership, accessors)
    lines += check_writer_boundary(ownership, accessors)
    return lines


def check_writer_boundary(ownership, accessors=None):
    """RULES 9-10 (SB-SOLE): the two claims about original WRITERS have to stay honest.

     9. `relocated` without `owned_by_dll`. Since the D4 split the two are a ladder, not two
        independent flags: `owned_by_dll` says libmh binds the region and no original code writes
        it; `relocated` says its bytes are somewhere else entirely. A region whose bytes have left
        while original code still writes them is the failure rules 1-6 exist for, so the pairing is
        refused here rather than left to be noticed.

    10. THE KEPT-WRITER RATCHET, and it is the half of SB-SOLE that does not fit in `owned_by_dll`.
        Driving the external writer count to zero has two legal outcomes per region: it reaches
        zero (mark it owned_by_dll, rule 4 keeps it true), or it keeps a writer for a NAMED reason.
        The second outcome is what rots -- a reason recorded once, in prose, over a set that then
        changes. So a region libmh writes that keeps a live external writer must carry
        `kept_writers` (the exact list) + `kept_writers_why`, and this rule refuses four things:

          a. a libmh-written region with live external writers and no `kept_writers` -- the state
             SB-SOLE closed against, and exactly how a NEW original writer would arrive;
          b. a `kept_writers` list that disagrees with the census -- a writer that appeared or
             disappeared under a standing reason;
          c. `kept_writers` on a region that has none left, or alongside `owned_by_dll` -- a reason
             that outlived its writer reads as an accepted one forever;
          d. an empty `kept_writers_why`.

        WHAT IT DELIBERATELY DOES NOT DO is re-adjudicate: the per-writer reason lives in
        tools/data/writer_dispositions.json and is gated by the writer-attribution census. This rule
        only proves the region-level record still names the writers the census actually reports.
    """
    if accessors is None:
        accessors = load_accessors()
    lines = []
    for rid, decl in sorted(ownership.items()):
        if decl.get("relocated") and not decl.get("owned_by_dll"):
            lines.append(
                "RELOCATED BUT NOT OWNED: %s declares `relocated` without `owned_by_dll`. Since the "
                "D4 split those are a ladder: owning is `libmh binds it and no original function "
                "writes it`, relocating is `and its bytes are somewhere else`. A region whose bytes "
                "have moved while original code still writes the stock address is precisely what "
                "rules 1-6 exist to refuse." % rid
            )
    if accessors is None:
        return lines
    for rid, acc in sorted(accessors.items()):
        decl = ownership.get(rid)
        if decl is None:
            continue
        who = acc.get("name") or rid
        kept = decl.get("kept_writers")
        if not acc.get("ours_writers"):
            # Not part of SB-SOLE's population; a stray record is still refused under (c).
            live = []
        else:
            _dead = set(acc.get("external_writers_dead") or [])
            live = sorted(w for w in (acc.get("external_writers") or []) if w not in _dead)
        if live and kept is None:
            more = "" if len(live) <= 6 else " (and %d more)" % (len(live) - 6)
            lines.append(
                "LIBMH-WRITTEN REGION WITH AN UNRECORDED WRITER: %s is written by libmh and %d "
                "original function(s) still write it -- %s%s -- with no `kept_writers` record. "
                "SB-SOLE's boundary has two legal outcomes per region and this is neither: drive the "
                "count to zero and mark it owned_by_dll, or record the writers here with a named "
                "reason (per-writer adjudication in tools/data/writer_dispositions.json)."
                % (who, len(live), ", ".join(live[:6]), more)
            )
            continue
        if kept is None:
            continue
        if decl.get("owned_by_dll"):
            lines.append(
                "OWNED AND KEPT AT ONCE: %s carries both `owned_by_dll` and `kept_writers`. A region "
                "whose external writer count reached zero has no writer to keep; delete one." % who
            )
        if not live:
            lines.append(
                "STALE `kept_writers`: %s records %d kept writer(s) but the census reports no live "
                "external writer for it. A reason that outlives its writer reads as an accepted one "
                "forever -- delete the record and mark the region owned_by_dll." % (who, len(kept))
            )
        elif sorted(kept) != live:
            gained = sorted(set(live) - set(kept))
            lost = sorted(set(kept) - set(live))
            lines.append(
                "`kept_writers` DISAGREES WITH THE CENSUS on %s: %d not recorded (%s), %d recorded "
                "but no longer a live writer (%s). A standing reason may not silently absorb a new "
                "writer, which is the whole point of listing them."
                % (
                    who,
                    len(gained),
                    ", ".join(gained[:6]) or "-",
                    len(lost),
                    ", ".join(lost[:6]) or "-",
                )
            )
        if not (decl.get("kept_writers_why") or "").strip():
            lines.append(
                "`kept_writers` WITH NO REASON on %s: a list of writers we are keeping, with no "
                "`kept_writers_why`, is a record of the fact rather than an adjudication of it."
                % who
            )
    return lines


def check_original_accessors(ownership, accessors=None):
    """Rules 4-6: LAW 1's release condition, made non-vacuous (SIM-BOUNDARY).

    Rules 1-3 above ask whether one of OUR manifests still reaches an owned region by address.
    These ask the other half, which nothing checked: whether the ORIGINAL BINARY still does.
    Law 1 (the reimplementation plan) freezes a region's layout for as long as even one original accessor
    remains, so a region cannot be declared `owned_by_dll` -- physically moved out of the binary --
    while original code still indexes it at its .bss address.

      4. An original WRITER remains. Unconditional and unrescuable: two implementations of one piece
         of state, and after a move the original's write lands in the memory the region left. Names
         the writers, because "which functions" is the entire actionable content and it is also the
         migration worklist. THIS IS THE WHOLE OF THE HOSTED RELEASE CONDITION (D3, surviving D4) and
         it is what `owned_by_dll` asserts under its redefined meaning.
      5. The region is UNMEASURED -- the instruction scan never saw it. "Nothing measured it" and
         "nothing accesses it" are different sentences, and only one of them licenses a claim. This
         is the clause that stops the check being narrower than the claim it appears to certify
         (check_dispatch_closure reported OK for days over a candidate set that
         excluded the functions that were missing).
      6. A RELOCATED region keeps an original READER and no `reader_seam` is declared. Readers are
         survivable, but only through something that redirects them -- a relocation manifest that
         rewrites their ref sites. Declaring `reader_seam: "<name>"` asserts that seam exists and
         covers them; without it, a reader is a read of abandoned memory.
         READERS ARE NOT A GATE ON `owned_by_dll`, DELIBERATELY, and this is the one place to read
         why so nobody re-derives the rejected option: in the hosted configuration the host binds the
         stock .bss base, so an original reader reads the LIVE bytes libmh just wrote. The reader
         clause was never protecting correctness in place -- it was protecting against a region that
         had moved out from under its readers (docs/state-boundary.md D4, and option (a) "own the
         READERS too" was measured and rejected at 305 functions / 117 KB). Remove the move and the
         clause has nothing to protect, so it keys on `relocated`, where it is the standalone build's
         gate rather than dead code.

    A declaration whose id is in no census entry is SKIPPED, not failed -- the census covers the
    registry, and an id outside it is not a registry region.
    """
    from_disk = accessors is None
    if accessors is None:
        accessors = load_accessors()
    if accessors is None:
        if any(d.get("owned_by_dll") for d in ownership.values()):
            return [
                "NO ACCESSOR CENSUS: tools/data/region_accessors.json is missing, so Law 1's "
                "release condition cannot be checked at all. Run `python "
                "tools/gen_region_accessors.py --refresh`. Until it exists, an owned_by_dll mark "
                "is unverified, which is not the same as verified."
            ]
        return []

    claimed = [rid for rid, d in sorted(ownership.items()) if d.get("owned_by_dll")]
    if claimed and from_disk:
        # RULE 7 (SB2's currency guard). Rules 4-6 read the census; a census whose committed inputs
        # have moved in the UNSAFE direction can UNDER-report -- an accessor nobody counted -- and
        # that is exactly how a claim gets granted that should have been refused. So the staleness
        # is checked HERE, where a permission is issued, not only in the census's own drift gate.
        # Conservative staleness (more functions are ours than the census knows) is deliberately not
        # fatal: it can only make rules 4-6 refuse a move they could have allowed.
        try:
            import gen_region_accessors

            unsafe, _conservative = gen_region_accessors.currency(
                json.loads(ACCESSORS.read_text(encoding="utf-8-sig"))
            )
        except Exception as exc:  # a broken census must not silently grant the claim
            unsafe = ["(census unreadable: %s)" % exc]
        if unsafe:
            return [
                "STALE CENSUS UNDER AN OWNERSHIP CLAIM: %d region(s) are declared owned_by_dll (%s), "
                "but the accessor census is out of date in the UNSAFE direction -- %s changed since "
                "it was built, so its accessor lists may be missing an original accessor entirely. "
                "A claim granted over an under-reporting census is exactly the failure rules 4-6 "
                "exist to prevent. Run `python tools/gen_region_accessors.py --refresh` and re-check."
                % (len(claimed), ", ".join(claimed[:4]), ", ".join(unsafe))
            ]

    lines = []
    for rid, decl in sorted(ownership.items()):
        if not decl.get("owned_by_dll"):
            continue
        acc = accessors.get(rid)
        if acc is None:
            continue
        who = acc.get("name") or rid
        if not acc.get("measured"):
            lines.append(
                "UNMEASURED REGION DECLARED OWNED: %s is owned_by_dll, but the instruction scan "
                "never saw it -- it entered the registry from a manifest, not from a measured "
                "access. Its original-accessor set is UNKNOWN, not empty. Regenerate the state "
                "matrix and the census before declaring it owned." % who
            )
            continue
        # LIVE writers only (schema 4). A `dead` row is an evidenced claim that NOTHING REACHES
        # the function, so its writes never happen and it cannot hold a region against an
        # owned_by_dll mark. Refusing the mark over an unreachable body would be an over-refusal --
        # and it was one: three AI scratch regions were held by nothing but dead writers.
        _dead = set(acc.get("external_writers_dead") or [])
        writers = [w for w in (acc.get("external_writers") or []) if w not in _dead]
        readers = acc.get("external_readers") or []
        if writers:
            more = "" if len(writers) <= 6 else " (and %d more)" % (len(writers) - 6)
            lines.append(
                "ORIGINAL WRITER ON AN OWNED REGION: %s is owned_by_dll, but %d original "
                "function(s) still write it -- %s%s. That is two implementations of one piece of "
                "state in the hosted build, and abandoned memory in a relocated one. Own those "
                "functions, or take the owned_by_dll mark off and record them under `kept_writers`."
                % (who, len(writers), ", ".join(writers[:6]), more)
            )
        if readers and decl.get("relocated") and not decl.get("reader_seam"):
            more = "" if len(readers) <= 6 else " (and %d more)" % (len(readers) - 6)
            lines.append(
                "ORIGINAL READER ON A RELOCATED REGION, NO SEAM: %s is relocated with %d original "
                "reader(s) -- %s%s -- and no `reader_seam` declared. Those reads resolve to the "
                "address the region left. Declare the seam that redirects them (it must cover all "
                "%d), or own the readers."
                % (who, len(readers), ", ".join(readers[:6]), more, len(readers))
            )
    return lines


def check_region_classification(regions, ownership, accessors=None):
    """RULE 8 (SB2): every registry region carries ONE declared owner_subsystem and a `why`.

    Rules 1-7 police regions someone has declared MOVED. This one polices the far larger claim the
    migration rests on -- "own all the writers of sim state" -- which is not a finite sentence until
    each region has an owner. Measured against the 152 regions the migrated sim writes, the 169
    external writers included 32 presentation/render and 7 input/selection functions writing
    UI-owned flags; without a declared owner per region, "own all writers" reads as a mandate to
    migrate the renderer.

    FOUR ways to fail, and the fourth is the one that keeps the rule honest:

      8a. No entry, or no `owner_subsystem`. The message carries the mechanical suggestion
          (_subsys.classify_data on the symbol name) so classifying a newly merged region is a
          one-line edit rather than an investigation -- but a SUGGESTION is not a declaration, and
          nothing writes it for you.
      8b. An `owner_subsystem` outside _subsys.OWNER_SUBSYSTEMS. "Other named" is deliberately not
          in that vocabulary: it is the bucket for functions whose symbol is off-convention, i.e. a
          naming gap, and a naming gap cannot own state.
      8c. No `why`, or one too short to be one. The `why` is the whole durable content -- an owner
          with no reason cannot be re-litigated on evidence, only overruled by taste.
      8d. `unattributed` on a region the state matrix DID see touched. The sentinel exists for the
          47 regions nothing measured reaches (they entered the registry from a manifest); using it
          on a region with a measured accessor would launder "I did not decide" as "nothing to
          decide", which is the exact move rule 5 already refuses for owned_by_dll claims.

    A declaration whose id is in no registry region is also reported: the registry is regenerated
    from five manifests, so a stale entry means the region moved or vanished and its declaration is
    now describing nothing.
    """
    if accessors is None:
        accessors = load_accessors() or {}
    known, missing, bad_vocab, bad_why, bad_sentinel = set(), [], [], [], []
    for r in regions:
        rid = r.get("id") or ""
        if not rid:
            continue
        known.add(rid)
        decl = ownership.get(rid)
        owner = (decl or {}).get("owner_subsystem")
        if not owner:
            missing.append((rid, _subsys.classify_data(r["name"] or rid)))
            continue
        if owner not in _subsys.OWNER_SUBSYSTEMS:
            bad_vocab.append((rid, owner))
            continue
        why = (decl.get("why") or "").strip()
        if len(why) < 16:
            bad_why.append(rid)
        if owner == _subsys.UNATTRIBUTED:
            acc = accessors.get(rid) or {}
            touched = sum(
                len(acc.get(k) or [])
                for k in ("external_writers", "external_readers", "ours_writers", "ours_readers")
            )
            if touched:
                bad_sentinel.append((rid, touched))
    stale = sorted(set(ownership) - known)

    def few(items, fmt):
        head = ", ".join(fmt(i) for i in items[:8])
        return head + ("" if len(items) <= 8 else " (and %d more)" % (len(items) - 8))

    lines = []
    if missing:
        lines.append(
            "UNCLASSIFIED REGION(S): %d of %d registry regions carry no `owner_subsystem` in "
            "tools/data/region_ownership.json -- %s. Every region needs ONE declared owner plus a "
            "`why`, or the migration's 'own all the writers of sim state' has no finite meaning. "
            "The name taxonomy's SUGGESTION is shown after each id; it is a starting point, not a "
            "declaration -- check it against the region's accessors "
            "(`gen_region_accessors.py --region <id>`) before writing it down."
            % (len(missing), len(regions), few(missing, lambda i: "%s -> %s?" % i))
        )
    if bad_vocab:
        lines.append(
            "OWNER OUTSIDE THE VOCABULARY: %s. Valid owners are %s."
            % (
                few(bad_vocab, lambda i: "%s = %r" % i),
                " | ".join(_subsys.OWNER_SUBSYSTEMS),
            )
        )
    if bad_why:
        lines.append(
            "OWNER WITH NO `why`: %s. The reason is the durable half of the declaration -- an owner "
            "with no evidence behind it cannot be reopened on evidence." % few(bad_why, lambda i: i)
        )
    if bad_sentinel:
        lines.append(
            "`unattributed` ON A MEASURED REGION: %s. That sentinel is only for regions the state "
            "matrix never saw touched; these have measured accessors to attribute them from."
            % few(bad_sentinel, lambda i: "%s (%d accessor(s))" % i)
        )
    if stale:
        lines.append(
            "OWNERSHIP DECLARED FOR A REGION THE REGISTRY NO LONGER HAS: %s. The registry is "
            "re-merged from the five manifests; drop the entry or restore the region."
            % few(stale, lambda i: i)
        )
    return lines


def report_ownership(violations):
    print("OWNERSHIP INTERLOCK: %d violation(s) (RI-STATE / ST3)." % len(violations))
    for v in violations:
        print("   " + v)
    print(
        "In tools/data/region_ownership.json, `owned_by_dll` means LIBMH BINDS THIS REGION and no "
        "original function writes it (D4); `relocated` is the separate, stronger claim that its "
        "bytes have physically left the binary, and nothing may then reach it by address."
    )


def check_coverage(regions):
    """Every save block must fall inside a COMMITTED registry region. Returns the misses.

    THIS IS NOT VACUOUS, and the distinction is the whole design. `--refresh` merges the save table
    IN, so a check run against a freshly merged set could never fail. This runs against the file on
    disk instead: change save_block_table.json (a new disassembly, a new block) and the emit path
    fails until someone explicitly re-refreshes and reviews the result. The registry is a REVIEWED
    artifact that the save format may not silently outrun -- which is the property whose absence let
    a block address exist that no other consumer of those bytes had ever heard of.

    It is also what ST3 will hang the ownership interlock on: once a region is marked as having left
    the binary, "covered" stops meaning "safe to block-copy" and starts meaning "has a serializer".
    """
    ext = []
    for r in regions:
        reach = max(r.get("extent", 0), r["size"])
        if reach:
            ext.append((r["base"], r["base"] + reach, r["name"]))
    ext.sort()
    starts = [e[0] for e in ext]
    misses = []
    for base, size, name in claims_save():
        i = bisect.bisect_right(starts, base) - 1
        ok = False
        while i >= 0 and starts[i] + 0x200000 >= base:
            if base >= ext[i][0] and base + (size or 1) <= ext[i][1]:
                ok = True
                break
            i -= 1
        if not ok:
            misses.append((base, size, name))
    return misses


def check_slice_overruns(regions, tables=None):
    """No hash slice may run past the MEASURED size of the region it is attached to (SB-HOSTFREE H0).

    WHY THIS IS NOT ALREADY COVERED by the static_asserts the header emits. Those pin each slice
    inside its host's `reach`, and `reach` is `max(measured size, the furthest byte any manifest
    claims)` -- so a hash entry that claims 48 bytes at an 8-byte symbol RAISES the reach to 48 and
    then passes its own containment test. The assertion is circular for exactly the entries it would
    need to catch, which is how `time_globals` sat for five weeks as a 48-byte window over six
    separate registry regions with every gate green.

    WHY IT MATTERS, and it is the whole reason SB-HOSTFREE needs it before anything else: the five
    trailing regions are independently BINDABLE. Relocate them (which is precisely the arrangement
    this item exists to prove) and the slice reads 8 live bytes plus 40 bytes of whatever now lies
    past the relocated host. The determinism hash keeps printing a value for the clock family and
    stops observing it -- a gate that cannot go red, over dead memory, in the instrument the
    relocation proof itself rests on.

    So the rule is `offset + len <= size`, on the MEASURED size only, for both slice tables.
    Returns a list of (table, name, host, size, offset, len, spanned) tuples; empty is the
    committed state. `tables` overrides the two manifests, which is how selftest() drives the
    check from the direction the real defect came from -- a manifest edit, not a registry edit.
    """
    by_base = sorted(regions, key=lambda r: r["base"])
    starts = [r["base"] for r in by_base]
    by_name = {r["name"]: r for r in regions}
    out = []
    if tables is None:
        tables = [("HASH_REGIONS", hash_manifest()), ("TACT_HASH_REGIONS", tact_manifest())]
    for table, entries in tables:
        slices, _ = resolve_slices(regions, entries)
        for s in slices:
            host = by_name[s["host"]]
            end = s["offset"] + s["size"]
            if end <= (host["size"] or 0):
                continue
            # WHICH regions the overrun eats, because "it overruns" is not actionable and
            # "it swallows LAST_GAME_TIME and four more, each independently bindable" is.
            lo, hi = host["base"] + host["size"], host["base"] + end
            spanned, i = [], max(bisect.bisect_right(starts, lo) - 1, 0)
            while i < len(by_base) and by_base[i]["base"] < hi:
                r = by_base[i]
                if r["base"] + max(r["size"], 1) > lo and r["name"] != host["name"]:
                    spanned.append(r["name"])
                i += 1
            out.append(
                (table, s["name"], host["name"], host["size"], s["offset"], s["size"], spanned)
            )
    return out


def report_slice_overruns(overruns):
    print("HASH SLICES OVERRUNNING THEIR REGION: %d (SB-HOSTFREE H0)." % len(overruns))
    for table, name, host, size, off, ln, spanned in overruns:
        print(
            "   %-17s %-20s host %s (size %d) +%d..%d -- runs %d byte(s) past it, over %d other "
            "region(s): %s"
            % (
                table,
                name,
                host,
                size,
                off,
                off + ln,
                off + ln - size,
                len(spanned),
                ", ".join(spanned) or "(none named)",
            )
        )
    print(
        "Each region named above is INDEPENDENTLY BINDABLE, so under a relocated bind this slice "
        "would hash the host's live bytes followed by whatever lies past them -- a hash that keeps "
        "printing and stops observing. Split the entry in tools/data/hash_manifest.json into one "
        "slice per region (APPEND the new ones; the order is a wire contract) and re-refresh."
    )


def report_misses(misses):
    print(
        "UNCOVERED SAVE BLOCKS: %d block address(es) fall inside no registry region." % len(misses)
    )
    for base, size, name in misses:
        print("   0x%08x +%-9s %s" % (base, size or "?", name or "(unnamed)"))
    print(
        "The save format names bytes the registry does not describe. Re-run with --refresh, review"
    )
    print("the diff in tools/data/state_regions.json, and commit both.")


def selftest():
    """Prove the coverage check can go RED. A check nobody has watched fail is a check nobody has.

    Mutates the committed region set IN MEMORY -- shrinking whichever region hosts the order QUEUE
    block, i.e. the exact region the whole ST1 story is about -- and asserts the miss is reported.
    """
    regions = json.loads(DATA.read_text(encoding="utf-8-sig"))["regions"]
    if check_coverage(regions):
        print("SELFTEST FAIL: the committed registry does not cover the save table to begin with")
        return 1
    target = 0xBB4ED0  # _G_LLM_STRAT_ORDER_QUEUE -- SavePlanetToDisk's first block
    hit = None
    for r in regions:
        if r["base"] <= target < r["base"] + max(r.get("extent", 0), r["size"]):
            hit = r
            break
    if hit is None:
        print("SELFTEST FAIL: no region hosts 0x%08x -- the fixture is stale" % target)
        return 1
    hit["size"] = hit["extent"] = 4  # the region no longer reaches the block it must cover
    misses = check_coverage(regions)
    if not any(m[0] == target for m in misses):
        print("SELFTEST FAIL: shrinking %s did NOT produce an uncovered block" % hit["name"])
        return 1
    print(
        "ok: coverage check fires (shrinking %s -> %d uncovered block(s))"
        % (hit["name"], len(misses))
    )

    # ST2M: the same property for the HASH manifest. A slice that outruns its host region is the
    # exact failure a relocated build produces -- the old hand-written table would have kept hashing
    # the stale address and every gate would have stayed green over dead memory.
    regions = json.loads(DATA.read_text(encoding="utf-8-sig"))["regions"]
    slices, misses = resolve_slices(regions)
    if misses:
        print(
            "SELFTEST FAIL: the committed registry does not place all %d hash slices" % len(slices)
        )
        return 1
    if len(slices) != len(hash_manifest()):
        print("SELFTEST FAIL: slice count %d != manifest length" % len(slices))
        return 1
    for r in regions:  # shrink the region hosting the order QUEUE hash slice
        if r["base"] == 0xBB4ED0:
            r["size"] = r["extent"] = 4
    _, misses = resolve_slices(regions)
    if not any(m["name"] == "order_queue" for m in misses):
        print("SELFTEST FAIL: shrinking the order queue did NOT unplace its hash slice")
        return 1
    print(
        "ok: hash-slice placement fires (%d slices placed; shrinking order_queue -> %d unplaced)"
        % (len(slices), len(misses))
    )

    # SB-HOSTFREE H0: the OVERRUN arm. Placement (above) asks whether a slice fits inside its host's
    # `reach`; this asks whether it fits inside the host's MEASURED size, which is the question that
    # decides whether the slice is still watching anything once the regions past it are bound
    # elsewhere. The two differ precisely because a hash claim raises the reach it is then checked
    # against -- so this arm mutates the MANIFEST rather than the registry, which is the direction
    # the real defect came from.
    regions = json.loads(DATA.read_text(encoding="utf-8-sig"))["regions"]
    if check_slice_overruns(regions):
        print("SELFTEST FAIL: the committed manifest already has an overrunning slice")
        return 1
    entries = [dict(e) for e in hash_manifest()]
    tg = next(e for e in entries if e["name"] == "current_game_time")
    tg["size"] = 48  # the pre-H0 shape: one window over six independently bindable regions
    bad = check_slice_overruns(regions, [("HASH_REGIONS", entries)])
    if not any(o[1] == "current_game_time" for o in bad):
        print("SELFTEST FAIL: restoring the 48-byte time window did NOT read as an overrun")
        return 1
    # ASSERT THE FIVE BY NAME, not a count. Restoring the 48-byte window re-attaches it to a
    # DIFFERENT host than it had before H0: CURRENT_GAME_TIME's reach is 8 now that nothing claims
    # 48 bytes there, so resolve_slices walks outward and lands on _G_LLM_GAME_SESSION_MODE, whose
    # 1623-byte save block does reach that far. The swallowed set is correspondingly wider. What
    # must not change is that the five regions the pre-H0 window made invisible are named in it --
    # that is the finding, and a count would have been a fact about the host lookup instead.
    spanned = set(bad[0][6])
    want = {
        "LAST_GAME_TIME",
        "TOTAL_GAME_TIME",
        "_G_LLM_STRAT_SIM_STEP_INTERVAL",
        "GAME_TIME_DELTA",
        "game_speed",
    }
    if not want <= spanned:
        print("SELFTEST FAIL: the overrun report does not name %s" % sorted(want - spanned))
        return 1
    print(
        "ok: slice-overrun check fires (the pre-H0 48-byte `current_game_time` window -> 1 overrun, "
        "naming all five swallowed clock regions among its %d)" % len(spanned)
    )

    # ST3: the OWNERSHIP INTERLOCK. The committed tree is GREEN over 342 owned_by_dll / 48
    # relocated / 124 kept_writers regions, so a green `--check` is evidence the rules are satisfied
    # and NOT evidence they can fire -- these arms are the only evidence of that. Each one mutates a
    # DECLARATION in memory (never the file) and asserts both that the right thing fires and, where
    # it matters more, that the wrong thing does NOT.
    #
    # FIXTURES ARE DELTAS ON THE REAL DECLARATION, not standalone dicts, since the D4 split: a
    # one-key fixture would be missing the region's own `kept_writers` record and would trip rule 10
    # in every arm, drowning the arm's own signal and making the must-not-over-refuse arms unusable.
    regions = json.loads(DATA.read_text(encoding="utf-8-sig"))["regions"]
    _own_real = load_ownership()
    if check_ownership(regions, _own_real):
        print("SELFTEST FAIL: the committed tree already violates the ownership interlock")
        return 1

    def fx(rid, drop=(), **kw):
        """{rid: the committed declaration, minus `drop`, overridden by kw}."""
        d = dict(_own_real.get(rid) or {})
        for k in drop:
            d.pop(k, None)
        d.update(kw)
        return {rid: d}

    def arm(name, decl, want, forbid=None):
        v = check_ownership(regions, decl)
        blob = " | ".join(v)
        if want is None:
            if v:
                print("SELFTEST FAIL (%s): expected NO violation, got: %s" % (name, blob))
                return False
            print("ok: %s -- clean, as it must be" % name)
            return True
        if want not in blob:
            print("SELFTEST FAIL (%s): expected %r in the violations, got: %s" % (name, want, blob))
            return False
        if forbid and forbid in blob:
            print("SELFTEST FAIL (%s): did not expect %r, got: %s" % (name, forbid, blob))
            return False
        print("ok: %s -- fires (%d violation(s))" % (name, len(v)))
        return True

    # 1. A SAVED region marked owned_by_dll with no serializer. The message must name the region AND
    #    the manifest -- "something is wrong" is not actionable, "the SAVE manifest still reaches
    #    _G_LLM_UPGRADES" is.
    if not arm(
        "a SAVED region RELOCATED with no serializer",
        fx(
            "UPGRADES",
            drop=("kept_writers", "kept_writers_why"),
            relocated=True,
            owned_by_dll=True,
            l1_serializer=None,
        ),
        "SAVE manifest still reaches",
    ):
        return 1

    # 2. THE MUST-NOT-OVER-REFUSE ARM, and the one that decides whether this rule is usable: a
    #    region that has left the binary AND has a serializer AND every manifest reaches it as a
    #    whole region is CLEAN. Order QUEUE is the real future case -- view + shadow + save + hash,
    #    all at offset 0 for the full 20400 bytes. A rule that refused this would ban the very move
    #    it exists to make safe.
    if not arm(
        "a relocated+served region whose every window is the WHOLE region",
        fx("STRAT_ORDER_QUEUE", relocated=True, owned_by_dll=True, l1_serializer="mh::orders"),
        None,
    ):
        return 1

    # 2b. THE D4 SPLIT ITSELF, and it is the arm the redefinition stands on: the SAME region, marked
    #     owned_by_dll but NOT relocated, with NO serializer and reached by four manifests, is CLEAN.
    #     That is the whole content of "libmh binds this region" -- the host answers the bind table
    #     with the stock base, so a manifest reaching it by address reaches the live bytes. Keyed on
    #     owned_by_dll this configuration produced 570 rule-1 violations across the 310 marks.
    if not arm(
        "owned_by_dll WITHOUT relocated: manifests may reach it, and no serializer is needed",
        fx("STRAT_ORDER_QUEUE", owned_by_dll=True, relocated=False, l1_serializer=None),
        None,
    ):
        return 1

    # 3. The SHIFTED-BLOCK clause. player_data's save block starts +16 into the array and runs +16
    #    past its end (the `write(&array[0].field, sizeof(array))` idiom -- docs/save-format.md). A
    #    serializer does NOT rescue that: owner_serves() only serves offset==0/len==size, so the
    #    block would fall back to the raw address. Reported with the head skip and the tail length.
    if not arm(
        "a SHIFTED block on a relocated+served region",
        fx(
            "PLAYER_DATA",
            relocated=True,
            owned_by_dll=True,
            l1_serializer="mh::sim",
            reader_seam="selftest-fixture",
        ),
        "skips +16 at the head",
    ):
        return 1
    if not arm(
        "the second shifted block (prod_shuttle_slots +20)",
        fx(
            "PROD_SHUTTLE_SLOTS",
            relocated=True,
            owned_by_dll=True,
            l1_serializer="mh::sim",
            reader_seam="selftest-fixture",
        ),
        "skips +20 at the head",
    ):
        return 1

    # 4. A PATCH ref site is a constant baked into the original instruction stream. It cannot follow
    #    a move, so unlike every other manifest a serializer does not rescue it -- the rule has to be
    #    unconditional, and this arm is what proves it is.
    if not arm(
        "a PATCH ref site into a relocated region, WITH a serializer",
        fx(
            "BUILDINGS",
            relocated=True,
            owned_by_dll=True,
            l1_serializer="mh::sim",
            reader_seam="selftest-fixture",
        ),
        "RELOCATED REGION REACHED BY A BAKED ADDRESS",
    ):
        return 1

    # 5. TARGETED, NOT A BLANKET BAN. An island region that no manifest reaches by address -- which
    #    is every one of the AI's 28, measured 2026-07-30 -- can be declared moved and the tree stays
    #    clean. That is the case AI1-P needs, and a rule that fired here would block it.
    if not arm(
        "an island region no manifest reaches (the AI1-P case)",
        {
            "AI_ISLAND_FIXTURE_NOT_IN_ANY_MANIFEST": {
                "relocated": True,
                "owned_by_dll": True,
                "l1_serializer": None,
            }
        },
        None,
    ):
        return 1

    # 6. And the mark itself has to be what arms it: the same declaration WITHOUT relocated is
    #    clean, so a green tree is not green merely because the check is inert.
    if not arm(
        "the same region declared but NOT relocated",
        fx("UPGRADES", relocated=False, owned_by_dll=False, l1_serializer=None),
        None,
    ):
        return 1

    # 6b. RULE 9, the ladder. `relocated` is a claim ON TOP of `owned_by_dll`, never instead of it:
    #     a region whose bytes have moved while original code still writes the stock address is the
    #     exact failure rules 1-6 exist for, so the pairing is refused rather than left to be noticed.
    if not arm(
        "relocated WITHOUT owned_by_dll is refused (rule 9)",
        fx("STRAT_ORDER_QUEUE", relocated=True, owned_by_dll=False),
        "RELOCATED BUT NOT OWNED",
    ):
        return 1

    # SIM-BOUNDARY, rules 4-6: the ORIGINAL binary's accessors, which rules 1-3 say nothing about.
    # These are the arms that matter most, because the clause they enforce is the one SIM1-P was
    # carrying VACUOUSLY -- "no original accessor remains for any region declared moved" passes
    # trivially while no region is declared moved. Each arm below is a region declared owned.
    if load_accessors() is None:
        print("SELFTEST FAIL: no accessor census -- run gen_region_accessors.py --refresh")
        return 1

    # 7. The headline case, and it is SB-SOLE's own release condition: `units` still has an original
    #    writer, so it may not be marked. Must fire, and must NAME it -- the list is the worklist.
    if not arm(
        "an owned region with an original WRITER (units)",
        fx(
            "UNITS",
            drop=("kept_writers", "kept_writers_why"),
            owned_by_dll=True,
            l1_serializer="mh::sim",
        ),
        "ORIGINAL WRITER ON AN OWNED REGION",
    ):
        return 1

    # 8. Readers alone, no writers. Survivable in principle -- but only through a seam that
    #    redirects them, so an undeclared one is a violation ON A RELOCATED REGION. Note the arm
    #    carries `relocated`: readers are deliberately NOT a gate on owned_by_dll (D4 -- in the
    #    hosted configuration they read the live bytes), and arm 8b is what proves that half.
    if not arm(
        "a relocated region with original READERS and no reader_seam",
        fx(
            "STRAT_AI_TILE_SPIRAL_OFFSETS",
            relocated=True,
            owned_by_dll=True,
            l1_serializer="mh::ai",
        ),
        "ORIGINAL READER ON A RELOCATED REGION, NO SEAM",
    ):
        return 1

    # 8b. THE READERS HALF OF THE D4 SPLIT: the same region with its original readers, marked
    #     owned_by_dll and NOT relocated, is CLEAN with no seam declared. This arm records the
    #     rejected option mechanically rather than in prose -- "own the READERS too" was measured at
    #     305 functions / 117 KB and rejected (docs/state-boundary.md option (a)) -- and 89 of the
    #     310 marks would be refused if this rule stayed on owned_by_dll.
    if not arm(
        "owned_by_dll WITHOUT relocated: original READERS need no seam",
        fx("STRAT_AI_TILE_SPIRAL_OFFSETS", owned_by_dll=True, relocated=False),
        None,
    ):
        return 1

    # 9. THE MUST-NOT-OVER-REFUSE ARM for rule 6, and the reason rule 6 is usable rather than a ban
    #    on ever moving anything read from outside: the same region, with the seam declared, is
    #    clean. A rule with no negative arm passes its own test by refusing everything.
    if not arm(
        "the same region WITH a reader_seam declared",
        fx(
            "STRAT_AI_TILE_SPIRAL_OFFSETS",
            relocated=True,
            owned_by_dll=True,
            l1_serializer="mh::ai",
            reader_seam="selftest-fixture",
        ),
        None,
    ):
        return 1

    # 10. UNMEASURED is not clean. This region entered the registry from a manifest and the
    #     instruction scan never saw it, so its accessor set is unknown -- the candidate-set trap
    #     arm'd rather than argued.
    if not arm(
        "an UNMEASURED region declared owned",
        fx("STRAT_UPGRADE_MSG_TRAILER", owned_by_dll=True, l1_serializer=None),
        "UNMEASURED REGION DECLARED OWNED",
    ):
        return 1

    # 10a-e. RULE 10, SB-SOLE's KEPT-WRITER RATCHET. Driving the external writer count to zero has
    # two legal outcomes per region, and the second is the one that rots: a reason recorded once, in
    # prose, over a set that then changes. UPGRADES is the fixture because it really does keep one
    # (cfg_final_upgrade_Construct, excepted `load-path`).
    if not arm(
        "a libmh-written region keeping a writer with NO record (rule 10a)",
        fx("UPGRADES", drop=("kept_writers", "kept_writers_why")),
        "LIBMH-WRITTEN REGION WITH AN UNRECORDED WRITER",
    ):
        return 1
    if not arm(
        "a `kept_writers` list that disagrees with the census (rule 10b)",
        fx("UPGRADES", kept_writers=["llm_not_a_writer_fixture"]),
        "DISAGREES WITH THE CENSUS",
    ):
        return 1
    if not arm(
        "a STALE `kept_writers` on a region with none left (rule 10c)",
        fx(
            "STRAT_ORDER_QUEUE",
            kept_writers=["llm_gone_fixture"],
            kept_writers_why="fixture",
            owned_by_dll=False,
        ),
        "STALE `kept_writers`",
    ):
        return 1
    if not arm(
        "owned_by_dll AND kept_writers at once (rule 10c, the other half)",
        fx("UPGRADES", owned_by_dll=True),
        "OWNED AND KEPT AT ONCE",
    ):
        return 1
    if not arm(
        "a `kept_writers` record with an empty reason (rule 10d)",
        fx("UPGRADES", kept_writers_why="   "),
        "WITH NO REASON",
    ):
        return 1

    # 11-13. RULE 7, SB2's currency guard. Rules 4-6 are only as good as the census they read, and a
    # census whose inputs have moved in the UNSAFE direction can under-report -- an accessor nobody
    # counted -- which is precisely how a claim gets granted that should have been refused. The three
    # arms below stub `currency()` rather than corrupting the committed file, and the second and
    # third are the ones that decide whether the rule is usable at all.
    import gen_region_accessors

    real_currency = gen_region_accessors.currency
    try:
        gen_region_accessors.currency = lambda _doc: (["en_functions"], [])
        v = check_ownership(
            regions, {"STRAT_ORDER_QUEUE": {"owned_by_dll": True, "l1_serializer": "mh::orders"}}
        )
        if not any("STALE CENSUS UNDER AN OWNERSHIP CLAIM" in x for x in v):
            print(
                "SELFTEST FAIL (stale census under a claim): expected a refusal, got: %s"
                % (v or "clean")
            )
            return 1
        print("ok: a claim over an UNSAFELY stale census -- refused")

        # The must-not-over-refuse arm, twice over. CONSERVATIVE staleness (more functions are ours
        # than the census knows) can only make rules 4-6 refuse a move they could have allowed, so it
        # must NOT block a claim; and a stale census with NOTHING claimed must not fail either, or
        # every session would be gated on a refresh it has no reason to run.
        gen_region_accessors.currency = lambda _doc: ([], ["reimpl_done"])
        v = check_ownership(
            regions, {"STRAT_ORDER_QUEUE": {"owned_by_dll": True, "l1_serializer": "mh::orders"}}
        )
        if v:
            print(
                "SELFTEST FAIL (conservative staleness): expected NO violation, got: %s"
                % " | ".join(v)
            )
            return 1
        print("ok: a claim over a CONSERVATIVELY stale census -- clean, as it must be")

        gen_region_accessors.currency = lambda _doc: (["en_functions"], [])
        v = check_ownership(
            regions, {"STRAT_ORDER_QUEUE": {"owned_by_dll": False, "l1_serializer": "mh::orders"}}
        )
        if v:
            print(
                "SELFTEST FAIL (stale census, nothing claimed): expected NO violation, got: %s"
                % " | ".join(v)
            )
            return 1
        print("ok: an unsafely stale census with NOTHING claimed -- clean, as it must be")
    finally:
        gen_region_accessors.currency = real_currency

    # 14-18. SB2 RULE 8: one declared owner per region. Unlike rules 1-7 this one is armed by the
    # COMMITTED file being complete, so its first arm is the over-refusal arm -- if 554 declarations
    # produced even one violation the tree would be red already. The other four each delete or
    # corrupt exactly one declaration in memory and demand the region be NAMED: "something is
    # unclassified" is not actionable, "STRAT_UI_PANEL_MODE is" is.
    ownership = load_ownership()
    if check_region_classification(regions, ownership):
        print(
            "SELFTEST FAIL: the committed tree already fails rule 8 -- %s"
            % " | ".join(check_region_classification(regions, ownership))
        )
        return 1
    print("ok: every registry region carries a declared owner -- clean, as it must be")

    victim = "STRAT_UI_PANEL_MODE"
    if victim not in ownership:
        print("SELFTEST FAIL: fixture %s is not declared -- the arm is stale" % victim)
        return 1

    def arm8(name, mutate, want):
        decl = json.loads(json.dumps(ownership))
        mutate(decl)
        v = check_region_classification(regions, decl)
        blob = " | ".join(v)
        if want not in blob or victim not in blob:
            print(
                "SELFTEST FAIL (%s): expected %r naming %s, got: %s"
                % (name, want, victim, blob or "clean")
            )
            return False
        print("ok: %s -- fires, naming the region" % name)
        return True

    def _drop(d):
        del d[victim]

    def _blank_owner(d):
        d[victim]["owner_subsystem"] = ""

    def _bad_vocab(d):
        d[victim]["owner_subsystem"] = "Other named"

    def _blank_why(d):
        d[victim]["why"] = "n/a"

    def _sentinel(d):
        d[victim]["owner_subsystem"] = _subsys.UNATTRIBUTED

    for name, mut, want in (
        ("a region with NO declaration", _drop, "UNCLASSIFIED REGION"),
        ("a declaration with an empty owner", _blank_owner, "UNCLASSIFIED REGION"),
        ("an owner outside the vocabulary", _bad_vocab, "OUTSIDE THE VOCABULARY"),
        ("an owner with no `why`", _blank_why, "OWNER WITH NO `why`"),
        ("`unattributed` on a MEASURED region", _sentinel, "ON A MEASURED REGION"),
    ):
        if not arm8(name, mut, want):
            return 1

    # And the stale-entry arm: a declaration for a region the registry no longer has.
    decl = json.loads(json.dumps(ownership))
    decl["NO_SUCH_REGION_FIXTURE"] = {"owner_subsystem": "Netcode", "why": "selftest fixture only"}
    v = check_region_classification(regions, decl)
    if not any(
        "REGION THE REGISTRY NO LONGER HAS" in x and "NO_SUCH_REGION_FIXTURE" in x for x in v
    ):
        print("SELFTEST FAIL (stale ownership entry): expected a report, got: %s" % (v or "clean"))
        return 1
    print("ok: a declaration for a vanished region -- reported by name")
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument(
        "--refresh", action="store_true", help="re-merge the five manifests into the data file"
    )
    ap.add_argument("--check", action="store_true", help="drift gate: fail if the header is stale")
    ap.add_argument(
        "--report", action="store_true", help="print the claim matrix and every disagreement"
    )
    ap.add_argument("--selftest", action="store_true", help="prove the coverage check can go red")
    args = ap.parse_args()

    if args.selftest:
        return selftest()

    if args.refresh or args.report:
        regions, warnings = merge()
        if args.report:
            for r in regions:
                ext = "" if r.get("extent", 0) <= r["size"] else " (reach %d)" % r["extent"]
                print(
                    "  0x%08x %9d  %-9s %-46s %s%s"
                    % (
                        r["base"],
                        r["size"],
                        r["owner"],
                        (r["name"] or "?")[:46],
                        "+".join(r["manifests"]),
                        ext,
                    )
                )
            own = load_ownership()
            known = {r.get("id") for r in regions}
            if own:
                print("\nOWNERSHIP (tools/data/region_ownership.json):")
                for rid, d in sorted(own.items()):
                    print(
                        "  %-34s %-14s %s%s"
                        % (
                            rid,
                            "owned_by_dll" if d.get("owned_by_dll") else "in-binary",
                            d.get("l1_serializer") or "(no serializer)",
                            "" if rid in known else "   [no manifest reaches it]",
                        )
                    )
            multi = [r for r in regions if len(r["manifests"]) >= 2]
            print("\n%d region(s); %d claimed by 2 or more manifests" % (len(regions), len(multi)))
            for w in warnings:
                print("  !! " + w)
            return 0
        payload = {
            "_comment": [
                "GENERATED by tools/gen_state_registry.py --refresh. The merged state-region registry",
                "(RI-STATE / ST1): one derivation of where each region lives, folded from the five",
                "manifests that used to answer that independently. Read-only input to the header",
                "generator -- edit a SOURCE manifest and re-refresh, never this file.",
            ],
            "regions": regions,
        }
        DATA.write_text(json.dumps(payload, indent=1) + "\n", encoding="utf-8", newline="\n")
        print("wrote %s (%d regions)" % (DATA, len(regions)))
        for w in warnings:
            print("  !! " + w)
        return 0

    regions = json.loads(DATA.read_text(encoding="utf-8-sig"))["regions"]

    if args.check:
        # THE UPSTREAM ARM (SB-BIND T4, 2026-09-06). Everything below re-derives the HEADER from the
        # data file, so it can only ever catch a stale header -- and a stale DATA FILE was invisible
        # to it. That is not hypothetical: the four LIB-ABI stage-E globals overlay_hoist.cpp binds
        # (_G_LLM_GAME_MODE_SAVED, _G_LLM_UI_WGT_LIST_LOCKSTEP_SYNC, _G_LLM_NET_LOCKSTEP_
        # {WAIT_PLAYER_IDX,OVERLAY_RESULT}) were added to the addr manifest on 2026-09-03 and were
        # still unregistered three days later, with this check printing "matches its inputs"
        # throughout -- because the inputs it meant were the data file, not the five manifests the
        # data file is a merge OF. A region nothing registers is a region no bind can move, so the
        # hole ate exactly the thing SB-BIND exists to guarantee.
        #
        # merge() is ~0.2 s, so this runs on every --check rather than behind a flag.
        fresh, _w = merge()
        if fresh != regions:
            have = {r["base"]: r for r in regions}
            want = {r["base"]: r for r in fresh}
            added = sorted(set(want) - set(have))
            gone = sorted(set(have) - set(want))
            changed = sorted(b for b in set(have) & set(want) if have[b] != want[b])
            print(
                "DRIFT: %s is stale against the five manifests -- run "
                "gen_state_registry.py --refresh and commit." % DATA
            )
            for label, bases in (("MISSING", added), ("EXTRA", gone), ("CHANGED", changed)):
                for b in bases[:12]:
                    r = want.get(b) or have[b]
                    print("   %-8s 0x%08x %s" % (label, b, r.get("name") or "?"))
                if len(bases) > 12:
                    print("   %-8s ... and %d more" % (label, len(bases) - 12))
            return 1

    misses = check_coverage(regions)
    if misses:
        report_misses(misses)
        return 1

    # SB-HOSTFREE H0. On EVERY invocation, like the ownership interlock below and for the same
    # reason: a slice that outruns its region must stop the header being written, not be written
    # and then complained about by a later --check.
    overruns = check_slice_overruns(regions)
    if overruns:
        report_slice_overruns(overruns)
        return 1

    # ST3. Runs on EVERY invocation, not only --check, so a violation stops the header being written
    # rather than being written and then complained about.
    own = load_ownership()
    violations = check_ownership(regions, own)
    if violations:
        report_ownership(violations)
        return 1

    # SB2 rule 8, same discipline: an unclassified region stops the header being written.
    unclassified = check_region_classification(regions, own)
    if unclassified:
        print("REGION OWNERSHIP: %d problem(s) (RI-STATE / SB2)." % len(unclassified))
        for v in unclassified:
            print("   " + v)
        return 1

    text, pinned = emit(regions)
    analyze = analyze_text()

    if args.check:
        on_disk = HEADER.read_text(encoding="utf-8") if HEADER.exists() else ""
        if on_disk != text:
            print(
                "DRIFT: %s differs from a fresh regen -- run gen_state_registry.py and commit."
                % HEADER
            )
            return 1
        if ANALYZE.read_text(encoding="utf-8") != analyze:
            print(
                "DRIFT: %s's generated region manifest differs from a fresh regen -- run "
                "gen_state_registry.py and commit." % ANALYZE
            )
            return 1
        # ST1 done_when: the singles are REPORTED, not silently accepted. A region only one manifest
        # knows about is not wrong -- it is the blind spot the merge exists to make countable.
        singles = sum(1 for r in regions if len(r["manifests"]) == 1)
        owned = sum(1 for d in own.values() if d.get("owned_by_dll"))
        by_owner = {}
        for d in own.values():
            o = d.get("owner_subsystem")
            if o:
                by_owner[o] = by_owner.get(o, 0) + 1
        top = ", ".join(
            "%s %d" % (k, v) for k, v in sorted(by_owner.items(), key=lambda kv: -kv[1])[:4]
        )
        print(
            "ok: mh_regions.gen.h matches its inputs (%d regions, %d pinned; %d claimed by 2+ "
            "manifests, %d by only one -- see --report); ownership interlock clean "
            "(%d declared, %d owned_by_dll); every region classified "
            "(%d owners declared -- %s, ... %d unattributed)"
            % (
                len(regions),
                pinned,
                len(regions) - singles,
                singles,
                len(own),
                owned,
                len(by_owner),
                top,
                by_owner.get(_subsys.UNATTRIBUTED, 0),
            )
        )
        return 0

    HEADER.parent.mkdir(parents=True, exist_ok=True)
    HEADER.write_text(text, encoding="utf-8", newline="\n")
    ANALYZE.write_text(analyze, encoding="utf-8", newline="\n")
    print("wrote %s (%d regions, %d pinned to mh_addrs.gen.h)" % (HEADER, len(regions), pinned))
    print("wrote %s (%d hash-manifest slices)" % (ANALYZE, len(hash_manifest())))
    return 0


if __name__ == "__main__":
    sys.exit(main())
