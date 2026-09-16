# In-memory static patching — the F1E spike's settled findings

**Measured 2026-09-12** (fork item F1E; conductor-verified: parity run, 48-check `patchtest`
suite, full selftest gate). The fork's supplementary ruling moves the `src/patcher` manifests
in-DLL: compiled into mh.dll, applied to the LOADED image at DllMain, VirtualAlloc replacing the
appended section cave, so the distribution becomes a stock retail exe. This doc records what the
one-manifest spike (`no_cd_EN`) proved and what F4's full mechanism must build. The spike code:
`src/mh_dll/mh/patch/` (`mh::patch`, `[patch] inmem` default OFF), the generator
`tools/gen_inmem_manifest.py` (JSON → compiled header, `--check` drift-gated in lint), the
oracle `tools/check_inmem_patch_parity.py` (`--run` provisions a lane, launches, compares).

## Verdict: FEASIBLE — parity OK — with one hard design consequence

`no_cd_EN` (1 site + a 63-byte RWX cave holding both relocation-sensitive constructs: a rel32
back into the game and the site's own rel32 into the cave) applied in memory from the DLL and
compared against `mhpatch apply`'s on-disk output: **all compared bytes identical outside the
declared rel32s, and those resolve to the same absolute targets** — verified by the tool, which
also diffs the reference exe whole-file against the clean exe and requires every differing byte
to fall in a dumped extent or a justified class (PE header/section table, file-alignment padding
past EOF — loader-immaterial by construction). The armed process boots to full depth; disarmed,
zero trace.

## The hard finding: fixed-VA cave placement is DEAD

`mhpatch` bakes the cave at `ImageBase + SizeOfImage` (`0x010f0000` for EN). In a live process
that VA is **already taken before any user code runs — deterministically, every boot**.
`VirtualAlloc` at the baked VA fails always. *(CORRECTION, F4C-GATE 2026-09-13: F1E wrote "an OS
MEM_MAPPED read-only region", from one lane. WHAT is holding it varies — measured MEM_MAPPED on the
local parity lane, MEM_PRIVATE on a standard local lane with the transport armed, and MEM_IMAGE —
`mh.focus.exe`'s own image — on the rig VM. The invariant is that it is not free, not who has it,
which is why the applier's diagnostic NAMES the occupant and why the arm-order baseline records
the three measured spellings rather than one.)*
So relocation is not a fallback, it is the ONLY path: the "which dwords are relative" metadata
is **mandatory** for every manifest, and in-memory parity is content-parity-plus-reference-
consistency, never raw byte equality at reference sites.

## What F4 must build

1. **The `abs32_to_section` ref kind — the main build.** *(LANDED. Generator side at F4C and
   applier side at F4C-COMP, both 2026-09-13 — see the spec + outcome section below, and "The
   composition rule" section for the applier half.)* The generator decodes cave/site blobs
   with capstone and proves ref completeness; rel32s alone cover only `no_cd_EN` and
   `storage_cap_variable`. The caphike/relocate family fails completeness because its "patches"
   are **bare 4-byte absolute operands carved out of instructions that start before the patch
   VA** (no opcode in the blob — recognisable by value, not by decoding; disassembling one
   yields a plausible-looking wrong branch). One new ref kind (whole-blob and operand-embedded
   absolute-into-declared-section) makes the whole family representable. Nothing else about
   those manifests is hard.
2. **Guard timing.** The manifest `sha256` pins a file; a loaded image is already
   loader-mutated (IAT), so the per-site expected-bytes guard is the only usable pin (and
   strictly stronger — it localises drift). The spike carries sha256 as informational only;
   F4 drops it or replaces it with a hash over static code extents. The guard runs BEFORE any
   allocation or write (proven by `patchtest`: a moved guarded byte refuses with the cave never
   allocated).
3. **Entry-claim interaction (G68 ordering preserved).** The applier reproduces
   `patch_bytes_guarded`'s C1 interlock and asks it FIRST — which forces the arm point after
   `set_owner_table` + `reimpl_probe_install()` and before the seam byte-patches/detours, so
   whichever mechanism arrives second is refused. **F3 consequence — SETTLED 2026-09-13 (F3B
   moved the call, F3G proved it):** the placement no longer sits behind `[net] enable`. The arm
   point is `MH_Core_Arm` (`mh/seams/net_seams.cpp`), after `set_owner_table` +
   `reimpl_probe_install()` and before the byte patches and detours — G68's order, unchanged —
   and `[net] enable` is the net arm's key alone. Proven on a live lane: `[patch] inmem=1` with
   `[net] enable=0` applies the manifest and `check_inmem_patch_parity` is green, which is the
   configuration that tool's `--run` lane now uses so a regression back behind the gate reads as
   `applied 0` rather than as nothing at all.
3b. **Which manifests compile in — SETTLED 2026-09-13 (fork ruling Q7, built at F4C-GATE).** The
   compile list is derived from a manifest FIELD (`class`) instead of a Python constant, and the
   parity oracle runs over the whole shipped set. See "The shipped set" section below.
4. **The composition rule — SETTLED 2026-09-13 (fork ruling Q6, built at F4C-COMP).** `no_cd_EN`
   patches a byte INSIDE `llm_cd_locate_and_open_audio` while `seams/standalone.cpp` claims that
   function's ENTRY for the behavioural twin. Different addresses — neither registry fired — and
   both armed and composed benignly *by luck*. The rule is now the applier registering each site's
   containing function with the C1 interlock, and the answer for that particular pair turns out to
   be **compose, on purpose**: the twin installs a TRAMPOLINE, which steals 8 entry bytes and lets
   the body run on, and the site is at +0x2f. See "The composition rule" section below for the
   disposition per class and what it refuses. Related ruling: **cave manifests whose behaviour is
   already reimplemented (`no_cd`, `run_without_focus`) should likely not compile in at all** —
   F4's real target is the caphike/relocate family, which cannot be reimplemented. The spike
   compiled `no_cd_EN` in because F1E asked for a manifest, not because it should ship. (The census
   in that section now shows this as a measurement rather than an opinion: those two manifests are
   the ONLY two whose sites a registered DLL byte patch overlaps byte-for-byte.)
5. **Scale.** `grand_all_caphike_storagecap_EN` is 7770 sites. The applier's two-pass design is
   O(1) state (a dry protect pass proves writability before the first write — no per-site
   rollback snapshots), but `MAX_SECTIONS=4` / `MAX_SITE_LEN=64` are spike caps and per-site
   `VirtualProtect` doubles at scale; measure before optimising (one protect over the union of
   sites within `BEGTEXT` is the obvious move).
6. **`asm`-sourced manifests are refused by the generator** (`sight_redirect`, `recovered`):
   their `[import:dll!func]` resolves against the target file's `addimport`-built IAT, which
   the fork retires. F4 re-points these at runtime `GetProcAddress` before they can compile in.
   *(F4C status: still refused, now with the reason spelled out. Re-pointing needs the applier to
   carry an import table and a ref kind resolving into it — an `src/mh_dll` contract the generator
   has nothing to emit into yet, so it is applier-side work, not generator-side. `recovered` is the
   same case reached by a different key: its `fixups` rows name the same IAT slots.)*

## The `abs32_to_section` ref kind — the F4C design (spec, 2026-09-13)

This section is the spec `tools/gen_inmem_manifest.py` implements. It was written before the
code, so the argument for *why the classification is total* is a design claim the code then has
to satisfy, rather than a description of whatever the code happened to do.

### The ref kind

```
abs32_to_section:  the four bytes at `off` are an ABSOLUTE VA naming declared section
                   `target_section` at byte offset `target`. At placement the applier writes
                   `actual_base[target_section] + target` there, little-endian.
```

One kind covers both shapes the corpus contains, because they differ only in how the dword was
*found*, never in how it is rewritten:

- **whole-blob** — the entire 4-byte patch IS the pointer (`bytes: "79 00 0f 01"` at
  `0x0043fd8e` is `0x010f0079`, the relocated pool base + 0x79). The instruction that owns those
  four operand bytes *starts before the patch VA* and is not in the blob at all.
- **operand-embedded** — the dword sits inside a longer blob that does contain its opcode
  (`mov dword ptr [0x00e1538c], <abs32>`), or inside a cave section's assembled code.

It joins the two kinds F1E already emitted (`rel32_to_section`, `rel32_to_image`), and it is the
kind the relocate/caphike family is made of: 4,076 of `grand_all_caphike_storagecap_EN`'s 7,770
sites are exactly one whole-blob `abs32_to_section` each.

### What makes a site classifiable — two proofs, and which blob gets which

**Placement-sensitivity is relative to what moves.** A *site* blob overwrites a fixed image VA and
never moves; only its references INTO a declared section are sensitive. A *section* blob moves as a
unit; every reference OUT of it is sensitive, plus any absolute naming a section.

Those two blob classes admit different proofs, and the split is a property of the blobs, not a
preference:

- **A section blob is generator-assembled code with a known entry at offset 0.** Its proof is a
  FULL DECODE: every byte is consumed by a decoded instruction and every operand is enumerated, so
  the sensitive set is exact. A byte that will not decode ⇒ `refs_complete=false` (F1E's rule,
  unchanged).
- **A site blob is an arbitrary byte fragment that may begin mid-instruction.** It has no entry
  point, so decoding it is *unsound* — the F1E header already records that disassembling a bare
  relocation dword "yields a plausible-looking wrong branch". Its proof is therefore a decode-free
  **VALUE SCAN**: at every byte offset `o` in `[0, len-4]`, read the dword both ways —
  as an absolute (`LE32`) and as a rel32 (`va + o + 4 + i32`) — and call it sensitive iff either
  reading lands inside a declared section. The scan is **total by construction** (every offset is
  examined, so nothing can be missed) and **conservative** (a coincidental dword over-reports; it
  can never under-report). `o + 4` is the exact end-of-instruction for every x86 rel32 encoding,
  since `E8`/`E9`/`0F 8x` all carry the immediate last.

Measured over the whole corpus the value scan over-reports **zero** times on 46,294 site blobs —
the declared section VAs (`0x010f0000`+) are a region no real constant in these fragments occupies.
It is *not* usable on section blobs, where dense code yields misaligned windows (15 false hits in
`grand_all_caphike_storagecap`'s `.mhcap` alone) — which is why the decode proof lives there.

**Corroboration, not a second source of truth.** Where a site blob longer than 4 bytes *does*
decode cleanly end-to-end, the decode runs as a corroborating pass. It may only (a) DISAGREE with
the value scan, which is a refusal, or (b) contribute a refusal of its own. It may never invent a
ref, because that is the unsound direction. Corroboration agrees with the value scan on every one
of the corpus's 1,063 multi-byte site blobs.

### What refuses

A refusal is by name, with the manifest, the blob and the offset. The generator never emits
`refs_complete=true` alongside a shape it did not represent, and never ignores manifest content:

1. **An unread key.** Every manifest / patch / section key is either consumed or documentation
   (`_`-prefixed). Anything else refuses as *unknown key*. This is the bug class the item exists to
   kill: `recovered.mh.patch.json` carried per-patch `fixups` (an offset + an import name, read
   only by `src/patcher/mhpatch.py:429`) and a top-level `imports`, and was emitted
   `refs_complete=true` with both silently dropped — a manifest claiming to be relocatable while
   the thing that makes it work was never looked at. `imports` also retires the four zero-site
   `*_load` manifests and `example`: their entire content is an import-table edit the in-memory
   path cannot reproduce, so accepting them would mean claiming parity with an `mhpatch` output
   that differs.
2. **An `asm` patch** (`sight_redirect`) — unchanged from F1E; its `[import:dll!func]` has no
   meaning until it is re-pointed at a runtime `GetProcAddress`, which needs an applier-side
   import table and a ref kind resolving into it (see "Applier prerequisites" below).
3. **An absolute naming CAVE SPACE that no declared section covers** — `recovered`'s
   `call dword ptr [0x010f0123]`, an `X86_OP_MEM` operand into an IAT slot in a section the
   manifest never declares. Cave space is `[image end, image end + 64 MB)`; the bound is what keeps
   a `0xff32…` byte window from reading as one. Found on decoded operands only (sound), or on a
   4-byte whole blob (exact).
4. **An ambiguous offset** — a dword sensitive under BOTH the absolute and the rel32 reading.
5. **Overlapping refs** — two sensitive dwords whose 4-byte spans intersect; only one rewrite can win.
6. **A sensitive offset the emitted ref set does not cover** — the totality assertion, re-derived
   independently after the refs are built. It cannot fire on correct code, which is precisely why
   `--selftest` fires it by suppressing a ref: a totality check nobody has ever seen go red is not
   evidence.
7. **The F1E refusals, unchanged** — a rel8 leaving its blob, a branch in neither a section nor the
   image, an undecodable section byte.

### `refs_complete` is a manifest property; the applier's reach is a separate axis

`refs_complete=true` says *the relocation metadata is complete* — nothing about whether today's
applier can consume it. Conflating the two would re-create the bug this item kills, one level up.
So the generator emits, separately, the **applier prerequisites** each manifest needs, and the
compile list (`MANIFESTS`) REFUSES an entry whose prerequisites `mh/patch/inmem_patch.h` does not
yet satisfy — parsed out of that header, never hardcoded. Two were open as of F4C, and **both
CLOSED at F4C-COMP (2026-09-13)**:

- `ref_kind::abs32_to_section` was not in the `ref_kind` enum, and `relocate_site` /
  `relocate_section` did not implement it. **Landed:** the kind writes
  `actual_base[target_section] + target` little-endian at `off` — an ADDRESS, where the two rel32
  kinds write a displacement to one. Routing it through the existing rel32 arithmetic would produce
  `base + target - (blob_va + off + 4)`, a number that looks just as plausible, so `patchtest`
  computes that value explicitly and requires it to be ABSENT.
- `inmem_ref::owner` was `uint8_t`. The caphike family needs site indices up to **7,769**, so the
  field had to widen to 16 bits before any of that family could compile in. **Landed** as
  `uint16_t`, at no change in the struct's size (the byte went to what was already padding).
  `patchtest` asserts the width as behaviour: a ref owned by site 300 must reach site 300 and leave
  site 44 — the index it aliased to at 8 bits — untouched.

**What that closure looks like in git**, and it is the interlock being visible rather than a
side-effect: every `prereqs` list in `tools/data/inmem_manifest_audit.json` went to `[]` in one
`--audit` re-run. Widening `MANIFESTS` past its one entry is still F4C-GATE's — the prerequisite
interlock no longer stops it, so the remaining reason is the parity gate, not the applier.

### The corpus, as landed (2026-09-13)

`tools/data/inmem_manifest_audit.json` is the committed extract; `--check-audit` re-derives it from
`src/patcher` and requires byte-identity, so a manifest edit, a generator change, or an applier
header change that opens or closes a prerequisite all surface as a reviewable diff instead of a
silent reclassification. `--selftest` fires every refusal on planted negatives in a temp tree (the
`check_fork_f2_drop` house pattern), including the ref-suppression arm that proves the totality
assertion is not vacuous. Both are lint rows alongside the existing `--check`.

**40 manifests / 46,294 sites → 32 complete (46,282 sites), 0 incomplete, 8 refused (12 sites).**
All 23 manifests F1E emitted `refs_complete=false` — the 46,265-site relocate/caphike/storage
family — are complete. Two things the build surfaced that the F4 measurement had not:

- **F1E's absolute scanner required `base == 0 && index == 0`**, so `mov [edx + 0x0127ccd0], eax`
  did not count as an absolute. That skipped all 15 relocated-array references inside
  `grand_all_caphike_storagecap{,_EN}`'s `.mhcap` cave — a second instance of the same
  silently-accepted class, and one that lifting the whole-blob refusal alone would have shipped as
  `refs_complete=true`. **The totality assertion is what caught it**, on its first run, which is
  the argument for keeping a check whose green state proves nothing. A disp32 counts whatever the
  base and index registers are: this is a non-PIC 32-bit image, and `[reg + abs32]` is exactly the
  form the caphike manifests patch at their own sites.
- **`storage_cap_variable_grand` is composition-dependent, not standalone.** Its 300-byte `.mhcap`
  cave references arrays at `0x0127ccd0` that only exist once `grand_all_*` has relocated them into
  `.mhall`, yet it declares only its own section. On disk that composes because `mhpatch` runs the
  manifests in sequence against one file; in memory, applied alone, it is a cave pointing at a page
  nothing allocates. It is REFUSED, and F4C-GATE's classification field should record it as
  layered-on-grand rather than shippable alone. `grand_all_caphike_storagecap` is the pre-merged
  equivalent and is complete.

The other refusals are the seven `imports` carriers: `recovered` (plus its two `fixups` patches,
each named in the verdict), `sight_redirect` (also `asm`), `example`, and the four zero-site
`*_load` manifests, whose entire content is the `addimport` edit Q7 retires.

## The composition rule — the F4C-COMP build (fork ruling Q6, 2026-09-13)

The question F1E left open: **when an in-memory manifest and a DLL mechanism both want bytes in one
original function body, which one wins and who is told?** Q6's ruling is that the applier registers
each patch site's CONTAINING FUNCTION with the C1 interlock, so a collision is refused BY NAME. This
section records what that means per class, what the existing interlock already gave, and the
measurement that keeps the number honest.

### Why a per-manifest rule was never an option

`src/patcher` holds **46,294 sites over 40 manifests**, and a single one of them
(`grand_all_caphike_storagecap`) lands its 7,750 sites in **619 distinct function bodies**. Authoring
"this manifest excludes that mechanism" at that scale means a hand table nobody can keep in step with
a generator. The registry is the only form the rule can take.

### The disposition, per class

`tools/check_inmem_composition.py` re-derives the classes from committed inputs. Containment is
STRICT (`entry <= va <= end`) — the same predicate `mh::hook::promoted_owner_of` evaluates at
runtime, and the reason the census is reproducible at all.

| class | what it is | sites | disposition |
| --- | --- | ---: | --- |
| **P** | inside a body in `mh::addr::promotable_ranges` | 10,127 | **REFUSE** — the applier refuses the whole manifest when that body is PROMOTED in this run, and a later promotion of a body it already patched is refused too |
| **D** | inside a body the DLL detours at its entry | 4,438 | **REFUSE a jmp install** (the body dies) / **ALLOW a trampoline past the stolen bytes** (the body runs on), with the registration standing |
| P ∪ D | the composition surface | **11,034** | the static upper bound — not an armed-conflict count |
| — of which | reach a body's 8-byte ENTRY WINDOW | **93** | the only sites a trampoline install can actually collide with |
| **V** | inside a `verified_ranges` body this build cannot promote | 26,412 | **ALLOW with registration** — an original body like any other |
| — | inside no function body at all (data, inter-TU gaps) | 468 | **EXEMPT** — no interlock can ever fire on them |

**The 93 is the load-bearing number.** Refusing class D wholesale would disarm working detours to
protect bytes they never touch: an `install_trampoline` steals 8 entry bytes and returns into the
body, so only a site overlapping `[entry, entry+8)` is a genuine byte collision. 93 of 11,034 are.
`no_cd_EN` is the worked example — its site is at `llm_cd_locate_and_open_audio + 0x2f`, the twin in
`seams/standalone.cpp` trampolines that entry, and the two compose *correctly*, which is now stated
by the registry instead of being luck.

### What the existing C1 semantics already gave, and the delta

**Already there at F1E:** `promoted_owner_of(addr)` walks `promotable_ranges` extents and answers by
CONTAINING FUNCTION, not by entry equality — so a manifest site anywhere inside a promoted body was
already refused, by name, before this item started. Class P needed no new mechanism; what it needed
was a test that plants the site at `entry + 0x2f` rather than at the entry, so the generalization is
asserted instead of assumed (`patchtest` arm 6).

**The delta F4C-COMP adds:**

1. **The applier can NAME an arbitrary site's body.** The DLL's only extent table is
   `promotable_ranges` (401 bodies, against the 2,892 `en_functions.json` knows), so the generator
   now emits an `inmem_body[]` per manifest — the same `en_functions.json` join `gen_dll_patches.py`
   already does for the promotable set.
2. **Registration, on the success path only.** `apply()` records each body with the interlock and
   whether any site reaches its entry window. A REFUSED manifest registers nothing: refusing later
   claimants on behalf of bytes that were never written is the mirror image of the silence this
   interlock exists to end.
3. **A fourth refusal reason, `refuse_reason::patched`.** `detour_refusal` asks the registry and
   refuses by manifest + function. It is deliberately NOT gated on `entry_claim`: `rebind` means
   sharing the ENTRY is the design, which says nothing about bytes further into the body — and
   `install_export` promotes through a rebind claim, so reading the claim there would exempt the one
   case that most needs refusing.
4. **The affirmative line.** `report_patched_bodies()` states the registered set once, from
   `MH_InMemPatch_Install` rather than from the arm sequence, so it exists only in a run that asked
   for the patcher and every inmem-OFF arm-order baseline is untouched.

**Direction matters, and the arm order decides it.** The patcher runs inside `MH_Core_Arm` after
`set_owner_table` + `reimpl_probe_install()` and before every byte patch and detour (G68's order).
So promotions are the only claimant that has already happened when the patcher asks, and every
detour is a claimant that has not — which is exactly why the applier REFUSES on the promoted class
and REGISTERS for everything else. Whichever mechanism arrives second is the one refused.

### The one collision class the runtime registry does not answer

`patch_bytes_guarded` writes at arbitrary addresses rather than at entries, so a byte-exact overlap
between a registered DLL patch and a manifest site is not answerable from a body-granular registry.
It does not have to be: it is decidable offline, exactly, and the census does it. Both collisions
the corpus contains are the same benign shape — a manifest and its DLL twin carrying one fix:

- `resync_wait_fix` @ `0x0049c044` ↔ `net_resync_wait_fix_EN`
- `run_without_focus` @ `0x004a04ce` ↔ `run_without_focus_EN`

Which is Q7's "the reimplemented pair should not compile in at all", arrived at from the other
direction and by measurement. An unexplained third row is the thing to look at.

### The number, and why it is 11,034 rather than 11,104

The F4 surface measurement reported 11,104 (10,197 promotable + 4,438 detour). The re-derivation
reproduces the **detour class exactly** — 4,438, which is what identifies the predicate it used: a
body containing a statically-addressed `install_jmp` / `install_trampoline` target or a
`mh/hook/hookpoint.cpp` table row, resolved through `mh_addrs.gen.h` / `mh_export.gen.h`. The
promotable class comes out at **10,127**, 70 short, and neither strict nor loose containment against
`promotable_ranges` reproduces 10,197 — the measurement's exact predicate was not recorded. So the
census pins the predicate the APPLIER evaluates at runtime rather than trying to rediscover a lost
one, and `--check` makes any future movement a reviewable diff in
`tools/data/inmem_composition_census.json` instead of a stale figure in prose. The direction of the
correction does not touch the ruling: 24% either way, and the actionable subset is 93.

## The shipped set — the F4C-GATE build (fork ruling Q7, 2026-09-13)

F1E compiled ONE manifest in and F4C left widening that list to this item, with an instruction
attached: measure the per-manifest cost before promising a gate. This section records what the
corpus turned out to contain, what ships, and what a parity run costs.

### The classification is a manifest field, and the compile list is derived from it

`class` is now a consumed top-level key of every `src/patcher/*.mh.patch.json`, alongside a
`_class_why` documenting the decision. A manifest with no class, or an unrecognised one, is
REFUSED — defaulting would let a new manifest join the corpus without anyone saying whether it
ships, which is the same silence the unread-key rule exists to end. `gen_inmem_manifest.py`'s
compile list is every `shipped` manifest and nothing else; `tools/data/inmem_manifest_audit.json`
carries the class per row, so a change to what mh.dll contains is a reviewable diff.

| class | manifests | what it means |
| --- | ---: | --- |
| `shipped` | 3 | compiled into mh.dll and parity-gated |
| `reimplemented` | 7 | the behaviour already ships as DLL code — a second carrier for one fix |
| `retired-addimport` | 4 | the whole content is an `imports` edit the fork retires |
| `layered` | 1 | appliable only after another manifest (`storage_cap_variable_grand`) |
| `reference` | 25 | kept, never compiled in: the RU siblings + the archaeological/sample ones |

**The shipped set is exactly three, and the EN part of that is a measurement rather than a
preference.** mhpatch refuses a manifest whose `sha256` does not match the target, so of the 40
only 8 apply to the clean EN exe at all (measured 2026-09-13, every manifest built against
`workdir/mh_en_clean/mh.exe`); five of those eight are the reimplemented family and the
`net_load_EN` import edit. What is left is the EN caphike/relocate family:
`grand_all_caphike_storagecap_EN` (7,770 sites, 2 sections, 530 bodies),
`projectile_pool_caphike_EN` (77 sites, 8 bodies) and `projectile_pool_relocate_EN` (75 sites, 8
bodies). Every other caphike/relocate manifest in the corpus is RU-targeted and cannot be an
in-memory subject on this binary.

**They are ALTERNATIVES, not a batch.** Each bakes its cave at the image's next-free VA and their
site sets nest, so a second apply in one process would fail the first one's already-written bytes
at the expected-bytes guard — a correct refusal for a reason that reads like a build mismatch. So
`[patch] manifest=<name>` selects one; with no key and more than one compiled in, the DLL refuses
and lists them rather than picking a default (which would be the 7,770-site limit raise).

**`no_cd_EN` is out of the compile list** (Q7: its behaviour is `seams/standalone.cpp`'s trampoline
over the same body) and the drop changes no shipping behaviour, because `[patch] inmem` ships OFF.
It survives as the offline selftest's FIXTURE — `net_selftest patchtest` needs a manifest that can
be reasoned about completely (one site, one body, an initialised cave whose trailing `jmp` back
into the game is a known address), and the shipped alternatives are 75 to 7,770 sites over up to
530 bodies. The fixture header is generated into `mh_nettest/`, never into the DLL's include tree.

### The finding: in the SHIPPED configuration, every shipped manifest is refused

Measured 2026-09-13 on a standard lane, and it is the load-bearing fact of this section. The
caphike/relocate family patches bodies this tree PROMOTES — 4 of `projectile_pool_relocate_EN`'s 8
containing bodies (`llm_strat_sim_step` among them), 117 of `grand_all_caphike_storagecap_EN`'s
530. Under `[config] mode=brokered` the C1 interlock therefore refuses the manifest BY NAME, and
correctly: those bytes never execute, so a limit raise has to be carried by our implementation
instead. The configuration in which a shipped manifest actually applies is `[config] mode=original`.

That is why the parity lane runs the original engine: parity with mhpatch's on-disk output is a
statement about the ORIGINAL image. Both shapes are committed in the arm-order baselines —
`brokered.json` carries the refusal pair, `original.json` the applied trio — each with its
`--compare` verdict. **The open question this hands on is not a tooling one:** the in-memory limit
raises and the reimplemented spine are today mutually exclusive, and closing that means the
promoted bodies carrying the raised caps themselves.

### The oracle, and what a run costs

`tools/check_inmem_patch_parity.py` iterates the shipped set. Its comparison was generalized off
rel32-only arithmetic to all three ref kinds (F4C-COMP refused `abs32_to_section` outright), and a
ref is now resolved on BOTH sides against that side's own section bases instead of being
pre-resolved against the baked one. It also gained a check that is not about the DLL at all: at
every declared ref, the reference FILE must hold exactly the address the relocation metadata says
that dword names — mhpatch's independent output against the generator's ref set, which catches a
ref with the right offset and the wrong target that masking would otherwise hide on both sides.

**`--offline` is the G180 hand-off, closed.** That trap was a verification tool whose only arm
needed the rig, broken by a refactor for a day with every lint row green. The offline arm builds
each shipped manifest's mhpatch reference (which by itself re-proves every expected-bytes guard
still matches the real clean EN exe), re-derives the refs, runs the baked-value check and drives
the whole comparison over a synthetic dump — 2.5 s for all three, a lint row. It proves the
comparison plumbing, not the DLL, so `--selftest` plants mutations (a byte outside every ref, a ref
dword holding a plausible wrong address, a corrupted cave byte) and requires each to go red.

**Measured cost of the live arm, 2026-09-13** — this is what made the gate unit affordable rather
than a promise:

| manifest | sites | mhpatch ref | lane | compare | verdict |
| --- | ---: | ---: | ---: | ---: | --- |
| `grand_all_caphike_storagecap_EN` | 7,770 | 0.1 s | 0.2 s | 0.3 s | PARITY OK |
| `projectile_pool_caphike_EN` | 77 | 0.0 s | 0.0 s | 0.0 s | PARITY OK |
| `projectile_pool_relocate_EN` | 75 | 0.0 s | 0.0 s | 0.0 s | PARITY OK |

1.3 s wall for all three including lane provisioning. `[patch] dump_exit=1` terminates the process
one instruction after the apply, so a manifest costs one boot-lock window and no render loop — the
per-manifest rig cost the item asked to be measured before a gate unit was promised is a fraction
of a second, not a boot. It is `run_gate`'s `inmem` unit.

A full boot with a manifest armed is a separate, hand-run check (the rig fragment
`tools/uiscripts/ini/inmem_patch.ini`, paired with `rollback_original.ini`): 2026-09-13,
`mp_menu_walk` on the rig with `projectile_pool_relocate_EN` applied booted menu → lobby with all
six captures PASS, the VM's own log carrying the APPLIED line.

### Two caps that moved from comment to check

- `mh::hook::MAX_PATCHED_BODIES` (1024) is public, and every generated manifest header carries a
  `static_assert` of its own body count against it. Widening the compile list past what the C1
  registry can hold is now a BUILD error naming the manifest, not a "TABLE FULL" line in a lane
  nobody is reading. The worst shipped manifest registers 530.
- `MAX_SECTIONS` (4) and `MAX_SITE_LEN` (64) are public for the same reason, and `patchtest` walks
  the whole compiled-in set against them, plus every ref's owner and target-section index. The
  shipped worst case is 2 sections and a 13-byte site.

The cave-space bound is likewise derived now rather than a round number: the top of cave space is
the highest VA any manifest declares a section at, rounded to the 64 KB allocation granularity
(`0x013d0000` today, against F4C's flat `IMAGE_HI + 64 MB` — a 44x smaller false-positive window),
and it is recorded in the audit extract. Every verdict in the corpus is unchanged by the tightening.

### `storage_cap_variable_grand`: layered, NOT deleted

F4C left the disposition here with a precondition: verify that `grand_all_caphike_storagecap`
subsumes it before deleting. **It does, exactly** — all 9 of its sites (VA, `expect` and `bytes`)
are a subset of that manifest's 7,750, and the two `.mhcap` sections are byte-identical at the same
`vaddr`/`vsize`. It is nonetheless KEPT, classed `layered`: it is still a working link in an
on-disk mhpatch SEQUENCE, and deleting a usable RU on-disk manifest to tidy an in-memory refusal
trades something real for nothing. The subsumption is pinned as a `--selftest` arm instead of
asserted in prose, so if it ever stops holding, the file becomes a genuine orphan and says so.

### DLL size

Compiling the three manifests in takes `mh.dll` from 471,552 to 844,800 bytes (+373 KB, +79%),
almost all of it `grand_all_caphike_storagecap_EN`: 7,770 site rows of three pointers each, their
expect/mask/repl arrays, 4,260 refs, 530 body names, and the base relocations for all those
pointers. Compile cost is negligible (0.8 s for the 2.4 MB generated header). A pooled encoding —
one byte array per manifest with `{va, offset, len}` rows instead of three pointers — would remove
most of the relocations and roughly halve it; that is an optimisation nobody has needed yet, and it
is recorded here so the next person does not have to re-measure to decide.
