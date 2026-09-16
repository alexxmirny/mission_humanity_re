# The state boundary — freeze layout, free the base

**The decision record for `RI-STATE`.** Taken by the user in the 2026-08-22 planning session,
written down here 2026-08-23 (`SB3`). The reimplementation plan states Law 1 and this
document is what makes it *reopenable on evidence*: each rejected option is recorded with the
measured number that rejected it and the tool that produced that number, so a later session can
argue with the measurement instead of with the taste.

Companion material: the reimplementation plan (the five laws, the M6 exit),
the libmh pivot (why the state has to become ours eventually),
the state-flow adjudication (the measured island/shared split),
and the hardcoded-limits catalog (the cap-raise patch pipeline this constrains).

---

## The decision

### D1 — Freeze the LAYOUT. Free the BASE.

A state region has two independent properties, and the migration is allowed to buy exactly one of
them:

- **Layout** — stride, field order, container kind. **Frozen for this whole ledger.** Every original
  accessor reads the array by an offset formula baked into its instruction stream as a `disp32`;
  there is no seam that survives changing it while one such accessor exists. Container redesign
  returns only on the far side of `LIB-FORK`, where the forked project has no original code binding
  it (`LIB-CONTAINER`).
- **Base** — where the bytes live. **Free**, once every ref site is enumerated and rewritable. That
  is the runtime rebase seam (`SB4`). This is what lets `libmh` own the *storage* while the
  representation stays the original's: **"libmh holds the state" means allocation and lifetime, not
  representation.**

### D2 — One declared owner per region.

Every region in `tools/data/state_regions.json` carries an `owner_subsystem` + a `why` in
`tools/data/region_ownership.json`, and an original writer of another subsystem is either a
re-attribution or a recorded, class-tagged fact. Shipped as `SB2`; enforced by rule 8 in
`gen_state_registry.py --check` and by the cross-write gate in `gen_region_accessors.py --check`.

Without it, "own all the writers of sim state" is not a finite sentence — 32 of the external writers
on sim-written regions are presentation/render and 7 are input/selection, so the claim reads as a
mandate to migrate the renderer.

### D3 — The release condition, and it is a check rather than a clause.

> **Re-scoped the same day by `D4` below** — the reader clause moves to the standalone build. Read
> D4 before acting on the reader half of this.

A region may be marked `owned_by_dll` **iff**:

1. its external **writer** set is empty,
2. every external **reader** is covered by a declared `reader_seam`, and
3. it has an `l1_serializer` if it appears in the save format.

`gen_state_registry.py --check` refuses the mark otherwise (rules 4–7, `SB1`), including for an
**unmeasured** region — "nothing measured it" and "nothing accesses it" are different sentences —
and refuses any claim made over a census that is stale in the unsafe direction. This replaces the
clause `SIM1-P` used to carry, which quantified over the set of regions declared moved and therefore
passed while that set was empty.

---

## The rejected options, and the number that rejected each

Every number below is reproducible from a committed input by the named tool; none of them is quoted
from prose. Measured **2026-08-23**, against a census refreshed after that day's renames.

### (a) Own the READERS too, per region — rejected on 305 functions / 117 KB

`python tools/gen_region_accessors.py --all` (census `tools/data/region_accessors.json`, built by
`--refresh` from the Ghidra state matrix).

Scope: the **212** regions the migrated sim/AI writes.

| | functions | code |
| --- | ---: | ---: |
| external **writers** on those regions | 172 | 115 171 B |
| external **readers** on those regions | 305 | 117 009 B |
| **total external accessors** | **477** | **232 180 B** |
| of the readers: render / UI / tactical / input / sound | **71** | **36 836 B** |

Reader subsystems: sim 150 · UI 48 · AI 30 · unnamed 20 · map 17 · tactical 16 · boot 10 · net 7 ·
gfx 5 · sound 2.

**Why that rejects it:** the readers are half again as many functions as the writers and cost the
same order of bytes, and 71 of them are in exactly the subsystems the M6 exit declares non-goals
(render, UI, platform). Owning the readers means reimplementing the renderer to move an array. The
writers are the half that actually blocks anything; the readers need a **seam** (`SB4`) and
legibility (`RD-READY`), not reimplementation — which is why D3 asks for a `reader_seam` rather than
an empty reader set.

*(For scale, over all 554 registry regions rather than the sim-written subset: 866 external
accessors, 393 222 B.)*

### (b) A layout-changing accessor seam — rejected on 42.2% of 7 770 patch sites

`python -c` over the committed manifest `src/patcher/grand_all_caphike_storagecap_EN.mh.patch.json`,
classifying each patch's `_note` with the same `role_of()` rule the patch-surface report applies
(the Ghidra-side script is only needed to attribute sites to *functions*, not to classify them).

| role | sites | share | what it rewrites |
| --- | ---: | ---: | --- |
| `BASE` | 4 493 | 57.8% | a folded `disp32` base+field address, re-encoded per access by Watcom |
| `ROW` | 3 024 | 38.9% | a row-stride immediate (`IMUL …,row`) scaled for a new inner dimension |
| `SHIFT` | 157 | 2.0% | a shift-and-add row-stride splice (strength-reduced multiply) |
| `WBOUND` | 43 | 0.6% | a bound too wide for the immediate, trampolined to a cave |
| `BOUND` | 25 | 0.3% | an in-place loop/scan bound immediate |
| `SIZE` | 24 | 0.3% | a serialization/clear block-size immediate |
| `WALK` | 4 | 0.1% | a count-driven pointer-walk backstop trampoline |
| **non-`BASE`** | **3 277** | **42.2%** | |

**Why that rejects it:** a "seam" that permits layout change would have to reproduce, per site, what
this manifest does by hand. **42.2% of the shipped sites are not address rewrites at all** — they are
stride immediates, strength-reduced multiplies, loop bounds and block sizes, each decoded from the
instruction stream. Even the 57.8% `BASE` majority is not a single pointer swap: Watcom folds
`base+field` into each access, so "the base" appears 4 493 times.

**Correcting the figure this decision was originally recorded with:** `SB3`'s opening entry
attributed the 42% to the `SHIFT/BOUND/WALK/SIZE` roles. Those are **210 sites, 2.7%**. The 42.2% is
non-`BASE` *including* `ROW`. The conclusion is unchanged — `ROW` is a decoded stride immediate, not
a uniform delta — but the attribution was wrong and is fixed here rather than left to be discovered.

*(The RU manifest `grand_all_caphike.mh.patch.json` measures 7 748 sites / 47.4% non-`BASE`; EN is
the primary and is the row quoted.)*

### (c) Permanent total freeze — rejected on 218 regions / 277 KB already free, and on `LIB-REF`

`python tools/gen_region_accessors.py` (summary line).

- **218 of 554 regions have no original accessor at all — 277 441 B of state.** Law 1 never froze
  them; nobody had asked. A permanent freeze abandons that set for no benefit.
- It forecloses the direction already decided in the libmh pivot §3, where a closure
  is *lib-complete* only when it "owns or imports the state it touches rather than reaching original
  writers". Under a permanent freeze `LIB-REF` — the headless front-end that proves `libmh` does not
  need `mh.exe` — is not merely unbuilt, it is impossible: the state would live forever in an
  address space only `mh.exe` provides.

---

## Named consequences

**Container redesign is out of the ledger.** Not "later" — `LIB-CONTAINER` (deferred, `deps:
[LIB-FORK]`) is where it lands, with its own `done_when`. Nothing in this ledger may change a
region's stride or field order.

**Cap raises keep working, and the mechanism is the same one D1 licenses.** A cap raise *does* change
layout — `ROW` sites are stride rewrites — but it changes it **on both sides at once**, by rewriting
every ref site in the original binary (7 770 of them today). That is precisely the "enumerate and
rewrite every ref site" condition D1 puts on relocation, applied to stride instead of base. The two
pipelines must not collide, and the interlock already exists: **rule 3** in
`gen_state_registry.check_ownership` refuses an `owned_by_dll` region that the **patch** manifest
reaches at all, because a patch ref site is a constant baked into the original instruction stream and
cannot follow a move — a serializer does not rescue it. So a region cannot be both *moved out of the
binary* and *cap-raised by `mhpatch`*; whichever comes first, the check names the other.

**`libmh` gets storage, not representation.** `ST2`'s live-base table already resolves every consumer
through `live_base(rid)`; `SB4` makes the rebase real by rewriting original ref sites at load. What
`libmh` never gets, before `LIB-FORK`, is its own struct layout for a shared region.

**`SIM1-P`'s clause is discharged by name.** Its Law-1 clause used to read "no original accessor of a
migrated roster remains for any region declared moved" — true of the empty set. It now names
`STRAT_PROJECTILE_POOL` (181 500 B) and demands the check be green *with the mark in place*. As
measured today that region has **0 external writers and 2 external readers** —
`llm_strat_projectile_draw` and `llm_strat_render_view`, both render — so under D3 it is releasable
exactly when `SB4` gives it a `reader_seam`. That is a concrete blocker, not a clause.

---

---

## D4 — The HOST binds the bases. Nothing moves. (added 2026-08-23)

**Decided 2026-08-23 (user's call), one day after D1–D3, and it changes what D1's second half is
*for*.** D1 said layout is frozen and the base is free "once every ref site is enumerated and
rewritable" — the runtime rebase seam, `SB4`. That premise contained an unexamined assumption: that
the live base has to **differ** from the stock base *while running inside `mh.exe`*. It does not.

> **D4. The host tells `libmh` where the state is.** A bind table, one entry per region, carrying
> `(base, size, count)`. Inside `mh.exe` the host answers with the stock `.bss` address, so nothing
> moves, nothing is rewritten, and the original code's readers keep reading the live bytes we wrote.
> In a standalone host the answer is whatever that host allocated. Same layout, same code, one
> pointer apart.

This is **Law 3 taken to its terminus** — *reach state through the registry's live base, never a
hardcoded VA* — exactly as the libmh pivot was Law 4's terminus.

### What it changes

| | before (D1–D3) | after D4 |
| --- | --- | --- |
| moving a region's bytes | `SB4`: rewrite every original ref site under expect-guards | **not needed** — the bytes stay put; `SB4` is deferred |
| external **readers** | each needs a declared `reader_seam` before a region may be released | **not a gate at all** in the hosted configuration: they read the live bytes in place |
| the first ratchet | `SB5`: mark the 218 accessor-free regions `owned_by_dll`, one at a time | `SB-HOSTFREE`: prove the **whole registry** relocates without changing a hash |
| `owned_by_dll` | "the bytes have physically left the binary" | **"`libmh` binds this region"** |
| release condition | writers empty **+** readers seamed **+** serializer | **writers empty** (plus the serializer where the save format needs it) |

### Why it is not a weakening

The reader clause was never protecting correctness *in place* — it was protecting against a region
that had moved out from under its readers. Remove the move and the clause has nothing to protect.
D3's writer clause is untouched and is still the whole boundary: **no original function may write a
region `libmh` owns** (`SB-SOLE`).

And the interlock is not wasted, it changes host. In a **standalone** build every region genuinely is
at a non-stock address, so `SB1`'s rules 1–3 (no manifest may reach an owned region by address) and
the `reader_seam` rule become that build's gate rather than dead code. That is `LIB-REF`'s
compile-time contract.

### The seam already exists and is already proven

Measured 2026-08-23, not assumed:

- `src/mh_dll/mh/addr/mh_regions.gen.h` carries `live_table{base,size,moved}` with
  `rebase()`/`unrebase()`/`live_base()`/`live_size()`/`ptr<T>()` (ST2), and **every dereferencing
  consumer resolves through it** — the determinism hash, `region_view`'s emit/fill/poke, the save
  driver's `state_io::resolve`, the shadow region sets, `mh::sim::state()`, `mh::ai::state()`.
- `net_selftest statetest` already proves the both-direction mutation on a rebased region: poking the
  relocated copy changes the hash, poking the abandoned bytes does not, and a control re-rebase shows
  that same poke *is* seen at home.
- A sweep of `mh/{sim,ai,orders,net}` for baked state addresses found **zero** violations.

So `SB-BIND` is not "build a seam". It is "promote the seam to the ABI".

### The three gaps that make it work, and one that it fixes for free

1. **Caps are compile-time.** Only five counts derive from `live_size` (`unit_slots`, `bldg_slots`,
   `done_slots`, `tick2_slots`, `cfg_count`). `QUEUE_CAP 300`, `PENDING_CAP 1000`, `STAGING_CAP 300`,
   `MAX_PLAYERS 8`, `UNITS_PER_PLAYER 100` are constants mirroring the exe's own `CMP` bounds.
   **Consequence, and it is a live defect rather than a future one: the DLL and the shipped
   500-buildings cap-raise build do not compose today** — precisely what Law 3 exists to prevent
   ("compose, don't compete"). Forwarding `(base, size, count)` fixes it as a side effect, which is
   why the bind table carries counts and not just pointers.
2. **Pointer-valued slots.** `_G_LLM_STRAT_CUR_UNIT` and `_G_LLM_STRAT_CUR_BUILDING` hold a live
   `unit*`/`building*` **into** the `units`/`buildings` regions (`sim_state.cpp`).
   Harmless while both sides share a base; they must be translated the moment a host binds a different
   one. `SB-BIND` enumerates them with a tool rather than assuming there are two.
3. **Save blocks that overrun their region's tail.** The original writer's
   `write(&array[0].field, sizeof(array))` idiom runs past the named array into whatever sits next in
   `.bss` — `UPGRADES` into `PROD_SHUTTLE_SLOTS` by 20 B, `MESSAGE_TIME` into `ANIM` by 16 B
   ([save-format.md](save-format.md) "Blocks do not align to symbols", which states the consequence
   outright: *in our allocation those bytes are past its end, and the load direction is an OOB
   write*). Two independent measurements agree here — those are exactly the two pairs the region
   registry's `extent` field reports as partial overlaps. Hosted: harmless. Relocated: `SB-HOSTFREE`
   must handle each or list it.

**What the save FILE format does not do, checked because it would have been fatal:** it stores no
absolute pointers. Every block is `(address-constant, size)` resolved through `state_io::resolve`, the
map-region graph serialises `neighbors` as **indices**, `docked_units` are unit indices, and order
records are index-based. A standalone host therefore needs no pointer fixup pass on load.

### Items

`SB-BIND` (the ABI) → `SB-HOSTFREE` (the proof) → `LIB-REF` (standalone). `SB-SOLE` is the writer
boundary that D3's surviving clause becomes. `SB4` and `SB5` are **deferred**, each with the
condition that would revive it recorded in its tracker entry.

## D5 — `STRAT_PROJECTILE_POOL` stays where it is. Deferred BY NAME, with the counts. (added 2026-09-05)

`SIM1-P` used to be titled "… + move the projectile island", and its Law-1 clause demands that the
release condition be discharged **by name** rather than over a possibly-empty set — either the region
carries `owned_by_dll` with the registry check green, or the record names which of **D3**'s three
conditions it fails and with what count. This is that record, and on it the user **removed the
island-move half from `SIM1-P` on 2026-09-05** as superseded rather than deferred. **It fails two of
the three conditions, and that is not what settles it — the paragraph after the table is.**

Measured 2026-09-05 with `gen_region_accessors.py --region STRAT_PROJECTILE_POOL` — a fresh census, not
the 2026-08-23 figure the clause quotes, because a census can go stale and this page's own closing
paragraph says to re-measure:

| | | |
| --- | --- | --- |
| region | `_G_LLM_STRAT_PROJECTILE_POOL` @ `0x00e1db98` | 181 500 B (`llm_strat_projectile[1500]`, 121 B/entry) |
| ours | 2 writers, 1 reader | |
| **external writers** | **0** | **D3 condition 1: PASSES** |
| **external readers** | **2** — `llm_strat_projectile_draw`, `llm_strat_render_view`, 1 698 B of code | **D3 condition 2: FAILS** — both are RENDER, and neither is covered by a declared `reader_seam` |
| **save format** | present in `tools/data/save_block_table.json` (two blocks) | **D3 condition 3: FAILS** — it is serialized, and no `l1_serializer` is declared for it |

**And the deeper reason, which is why this is a decision rather than a wait.** D4 above settled that
nothing has to move: the host binds the bases, libmh writes **in place** at the stock `.bss` address,
layout is frozen by D1, and the original readers therefore read the live bytes. `SB4` — the runtime
rebase that would supply the `reader_seam` — is **deferred on exactly that reasoning**, not on effort.
So the two render readers are not an obstacle to be cleared before a move; they are evidence that a
move is unnecessary, and `ST0` had already retired the DLL-hosted-pool experiment (kept the export,
dropped the hosting) before this measurement was taken.

**The consequence for `SIM1-P`, stated so nobody re-derives it:** the item's second half was
SUPERSEDED by D4, not blocked by it, **and was removed from the item on 2026-09-05 on the user's
call** — title, `scope` and the Law-1 `done_when` clause all now read as promote-only. Owning every
accessor (which `SIM1-P` clause 2's rebind does) is the whole of what "the sim owns the pool" can mean
under a frozen layout and a host-bound base. What would revive a move is what `SB4`'s entry already
names — a need to move bytes while ORIGINAL code still reaches them — and the two render readers are
precisely that need NOT arising, since they read the live bytes wherever the host says they are.

**The one number that can reopen this:** external WRITERS. D4's argument is that the original readers
read the live bytes, which holds only while ours is the sole writer. `SIM1-P`'s surviving Law-1 clause
is therefore a drift check, not a task — re-run `gen_region_accessors.py --region
STRAT_PROJECTILE_POOL` and require **0** external writers. A nonzero count reopens the move question;
a change in the reader count does not.

## D6 — What the pre-flight re-measurement of `SB-BIND` / `SB-HOSTFREE` found (added 2026-09-06)

**Both items were re-measured before starting, per the tracker's "sanity-check `done_when` before
you begin" rule, and the check earned its keep: five of their own statements were wrong, unfireable,
or stale.** Three of the five would have let a green acceptance run mean nothing. Recorded here
rather than quietly fixed, and the corrected numbers are the ones the two items now carry.

Every figure below is reproducible from a committed input; the join keys are the VA, never the name
(symbol names and region names disagree — `cfg_G_TEXT_PTRS` is region `G_TEXT_PTRS`).

### D6.1 — The `SB-BIND` gate clause could not go red, and the gate pulls the wrong way

`SB-BIND` asked for "a build-time check [that] fails if any libmh TU names a state VA outside a
**named shim**". But `lint_libmh_layering.py:11,103` *defines* the named shim as `addr/*.gen.h` — so
`mh::addr::X` already **is** the sanctioned route and the clause was vacuous.

Worse, the two address gates pull in opposite directions. `check_sim_addresses.py` forbids `ptr<`
**outside** a module's binder and permits `mh::addr::` **everywhere**. So "only `turn_engine.cpp`
goes through the registry" is the gate's *design*, not drift: every non-binder TU is structurally
pushed toward the exact form `SB-BIND` exists to remove.

**And that gate has a hole worth fixing on its own merits.** Its literal-VA window is
`0x00A00000–0x01100000`, on the stated premise that everything below is "small constants (masks,
strides, enum values)". That is **false for 583 of the registry's 823 regions (71%)** — every
`.data`/`.rdata` region, down to base `0x453e2d`. A raw hex literal for `_G_LLM_NET_SEND_BUF`
(`0x5d55cc`) passes today. Derive the window from `state_regions.json` instead of hardcoding it.

### D6.2 — The named-VA census, redone

The 2026-09-03 figure ("~152 raw-VA accesses across 29 files") does not reproduce under any scope.
"29 files" matches the *file* count of the `mh::addr::_G_` data-global subset, whose total is **317**;
the per-file figures (timekeeper 43, rx_dispatch 37, net_session 25, tx_emit 15, resync 11) reproduce
exactly. Counting `mh::addr::<sym>` with line comments stripped, joined against
`dll_addr_manifest.json` and `state_regions.json` by VA:

| scope | uses | files |
| --- | ---: | ---: |
| the seven libmh modules | **272** | 21 |
| … data symbols resolving to a **registry region** — the routable set | **189** | |
| … data symbols with **no registry row** — add rows | 8 | |
| … **code** VAs (`w_sprintf` ×5, qsort comparators) — call shim, never the bind table | 11 | |
| … generated dispatch-table constants in `sim/` — these *are* gap (1) | 64 | |
| `seams/` (the injection harness, deliberately outside libmh) | 458 | |

Per module: lockstep **169** (156 registry-backed, 92%), sim 66, save **32** (100%), ai 4, orders 1.
The 8 registry-less globals are all in `lockstep/overlay_hoist.cpp` — the four `LIB-ABI` stage-E
globals (`OVERLAY_RESULT`, `WAIT_PLAYER_IDX`, `GAME_MODE_SAVED`, `WGT_LIST_LOCKSTEP_SYNC`) — a file
that **postdates** the measurement it is missing from.

Both tranche modules are pre-ledger islands, and this is now verified rather than asserted: nine
`tools/data/*_migration.json` manifests exist and **neither lockstep nor save has one**.

### D6.3 — The cap list named the wrong caps

The compose defect against the shipped 500-buildings cap-raise build is real. The five caps `SB-BIND`
named were mostly not it:

- **`QUEUE_CAP` / `PENDING_CAP` / `STAGING_CAP` are not violated.** The order regions
  `0xbb4ed0` / `0xbb9e80` / `0xbca920` appear **zero times** in the 7 770-patch manifest
  `grand_all_caphike_storagecap_EN` — the order container is untouched by the cap-raise. They are a
  *standalone-host* concern (`SB-HOSTFREE` / `LIB-REF`), not a compose one.
- **`MAX_PLAYERS` is not violated either** — `player_data`'s 166 140 stride appears nowhere as an
  immediate (a cap raise relocates the array), so a player-count raise has no stride class
  and all eight rosters keep 8 rows.

The caps that **are** violated, and which the item had not named: **eight per-player caps, sixteen
definitions across five files** (`ai_state.h`, `sim_state.h`, `issue_state.h`, `order_queue.h`,
`lt_reload_snapshot_resync.cpp`) — `UNITS`/`BUILDINGS`/`SOLDIERS` 100→500, `STORAGE`/`LABS` 25→127,
`TURRETS`/`MINES` 32→127, `PRODUCTIONS` 8→127.

**Not one of them is a C++ array dimension** — every roster is reached through a `T*` bound from the
registry, so no type changes are needed. The hard part is that **44 sites are address arithmetic, not
iteration** (33 `UNITS_PER_PLAYER` row-strides, 11 `MAX_PLAYERS` `peer_state[p*8+q]` matrix strides),
where a wrong runtime count *skews addressing* rather than truncating a loop — reproducing from
outside the exact "correct only for player 0" bug the manifest's `SHIFT` splices fix inside the exe.

Three traps for a name-based sweep: `mh::lockstep::PENDING_CAP` is **48** (a desync sample ring) and
`net_transport`'s `QUEUE_CAP` is **256** (the DLL's own socket inbox) — unrelated homonyms; and
`desync_watch.h:260-261` *is* a genuine static array dimension that must **not** be converted. Also
`lt_reload_snapshot_resync.cpp:55` `static_assert`s the vanilla row stride and must be revisited.

**Why this is urgent rather than tidy.** Nothing calls `rebase()` in production today except
`ai_state.cpp:1419`'s `island_move` (gated on `[promote] ai=1`). Against a cap-raised exe the registry
therefore still reports the **stock** bases, so `hash_base()` would hash 218 400 bytes of *abandoned*
`.bss` the patched game never writes — every peer agreeing on a hash of static zeros while the sims
diverge. **The desync detector goes blind, not red.**

### D6.4 — Four pointer-valued slots, not two; 23 pointer-typed binds

Dereferenced at bind time (`*ptr<T*>(RID_)`), each pointing **into** another registry region, each
with a matching `T**` store slot written from 13 live call sites (`sim_step.cpp` ×10,
`sim_advisor_tick.cpp` ×3): `CUR_UNIT` (`sim_state.cpp:592`), `CUR_BUILDING` (`:631`),
`CUR_PROJECTILE` (`:688`), `CUR_FX_ANIM` (`:694`). All four break under a relocated bind.

The earlier claim that this was "the only sim-view member that dereferences at
bind time" — was true on 2026-08-11 and is now **wrong by 4×**; correcting it is part of `SB-BIND`.

Nineteen further regions are pointer-typed but not dereferenced at bind. Four of them
(`MAP_REGION_LIST_HEAD` / `_POOL_FREE_HEAD` / `_BY_INDEX` / `_ROUTE_BFS_QUEUE`) hold
`llm_map_region*` into `0x420`-byte mallocs that `save_driver.cpp:879` already special-cases with a
two-pass neighbour resolve — a second pointer-graph hazard on the same surface.

The registry cannot be the enumeration key: `state_regions.json` records **no type** (its eight fields
are `name`/`base`/`size`/`extent`/`owner`/`manifests`/`size_claims`/`id`). The tool keys on our C++.

### D6.5 — Bind `reach`, not `size` (user's call, 2026-09-06)

`translate()` ([region_runtime.h:42](../src/mh_dll/libmh/state/region_runtime.h)) returns `nullptr` once
a region is marked *moved* and `delta + size` exceeds `live_size`. Binding `size` would make the ten
overrunning save blocks (D6.6) return null **the instant `libmh_bind_regions()` runs, even at stock
bases** — the no-op arm would go red for a reason unrelated to relocation. Binding `reach` keeps the
hosted identity bind clean.

The cost is **57 pairwise reach-aliasing pairs** (measured **0** by declared `size`), which only bite
when regions relocate *independently* — i.e. they land on `SB-HOSTFREE`, where they already were.

The table is otherwise genuinely flat: **823 regions, 0 overlaps by size, 0 duplicate bases, 15
zero-size regions** to name-and-exclude, 7 388 832 bytes total. (The 2026-08-23 "1 interior region"
no longer reproduces — it is 0 today, and the registry has grown from 554 to 823 since.) The ABI
struct is already declared and unimplemented with zero callers: `libmh_region_bind{region_id, base,
size, count}` + `libmh_bind_regions()`, `src/mh_dll/libmh/include/libmh.h:40,47`.

### D6.6 — `SB-HOSTFREE`'s own instrument goes blind, and the save overruns are 5× larger

**The instrument first, because it invalidates everything measured with it.** Of the 56
`HASH_REGIONS` slices, exactly one overruns its covering region: **`time_globals` @ `0x00e587b1`,
length 48 over `CURRENT_GAME_TIME` whose declared size is 8 — +40 bytes spanning five further
regions** (`LAST_GAME_TIME`, `TOTAL_GAME_TIME`, `_G_LLM_STRAT_SIM_STEP_INTERVAL`, `GAME_TIME_DELTA`,
`game_speed`). Relocate those five independently and `hash_slice()` reads 8 live bytes plus 40 bytes
of arena padding: **the determinism hash silently stops observing the clock family.** A green
3 000-step run would prove nothing about it. Fix it before the relocation arm exists. The same scan
over `mh_shadow.gen.h`'s 1 953 slice entries finds **zero** overruns — the shadow side is clean.

**The save overruns are ten, not two, and four intrude into live state rather than into a gap.**
Computed by joining the 68 fixed-address `STEP_BLOCK`s of `save_table.gen.h` against
`state_regions.json`; the registry's own `extent` field agrees in all ten.

| block | over region (size) | overrun | into live regions |
| --- | --- | ---: | --- |
| `_G_LLM_GAME_SESSION_MODE` @`0x00e58344` len 1623 | same (4) | **+1619** | **49** — `Players`, `PlayerSide`, `CurrentSystem`, `G_PLANET_STATUS`, `planet_time`, the clock family, the `NET_LOCKSTEP_*` set |
| `MESSAGE_TIME` @`0x00ae4b90` len 240 | same (8) | +232 | 1 (`Anim`, 16 B) |
| `Upgrades` @`0x00bcf8d0` len 10400 | same (10296) | +104 | 1 (`PROD_SHUTTLE_SLOTS`, 20 B) |
| `_G_LLM_CLICK_SELECT_TARGET_ID` @`0x00e1629c` len 4 | same (2) | +2 | 1 (`..._TARGET_FLAGS`) |
| six others (`FLOATING_MSG_COLOR` +58, `PROD_SHUTTLE_SLOTS` +20, `MOUSE_BUTTONS_PREV` +16, `player_data` +16, `ANIM_PLACE_DENIED` +4, `UI_SELECTED_BLDG_INDEX` +4) | | | **0** — gap bytes only |

The `GAME_SESSION_MODE` block is the largest by two orders of magnitude and was **never named
anywhere**, though the tactical closure already depends on its shape.

**That split decides the remedy.** [save-format.md](save-format.md)'s zero-filler substitution is
justified by "the overrun span reads all-zero in a loaded game" — true of the six, **false of the
four**: zero-filling those would corrupt real state and arena-filling them would break the
round-trip. `save-format.md` already names the right answer (decompose a block into
`(region, offset, length)` slices plus gap slices, "and `ST3` is where it becomes a lint") — **that
lint does not exist**; nothing in `save_driver.cpp` slices a block by region.

### D6.7 — Tactical mode: extend the replay (user's call, 2026-09-06)

`SB-HOSTFREE` demanded an explicit choice — exclude tactical mode or extend the replay — and said
silence was not available. **Extend**, because the measurement says the replay side is nearly free
and the exclusion would leave a hole a whole game mode wide: `test_ui.py --tact-determinism` is a
local lane-33 vehicle at ~3 s per arm with no rig, `TACT_HASH_REGIONS` already resolves through
`live_base`, `tact_selftest.cpp:213-232` already rebases all 15 tactical regions into a heap arena at
once, and `TILE_OBJECTS`/`PASSABLE` are already hash slices 12 and 13. The tactical arm is blocked on
nothing but the relocation arm the strategic side needs anyway.

Two corrections to the hazard as recorded: it is **five** regions, not four — add
`_G_LLM_STRAT_PATH_SLOT_FLAGS` and `G_PLANET_STATUS`, without which the "23 functions" count is not
correct — and the exception lives in `region_ownership.json` as `cross_write_host_config` with the
literal field `fails_in: ["relocated-bind"]` on all five. The "23 original tactical writers" figure is
from 2026-08-24 and has since **shrunk** as writers migrated to ours (`TILE_OBJECTS` and
`PATH_SLOT_FLAGS` are at zero per-writer entries today); re-run
`gen_region_accessors.py --writers "Tactical mode"` rather than quoting it.

### D6.8 — The replay vehicle, settled so nobody re-derives it

An offline ≥3 000-step golden run **exists and needs no rig**: `test_ui.py --soak --soak-golden`, one
local lane, per-step per-region hash vector plus clock stream and a fingerprint, naming the first
diverging **step and region**. Two caveats: *offline* means no **peers**, not no game (it provisions a
lane with `mh.exe` + packs); and the true recorded-**order** replay (`order_mode=2`,
`harness.cpp:411`) has **no Python driver** — `mp_run.py` only ever records — while `net_selftest` has
no replay mode at all. The mutation arm also already exists: `harness.cpp:4764`'s
`region_poke_only`/`region_poke_at` pokes through `mh::state::mutate_target` (the *live* base) and
reports where it actually wrote.

The relocation arm is the only genuinely new machinery, and it has a precedent at 48/823 scale —
`mh::ai::island_move` (`ai_state.cpp:1330-1428`) copies 48 regions into one arena, rebases, claims an
L1 provider, and poisons the abandoned `.bss` with `0xCD`. Scaling it needs a real arena (7.39 MB over
808 sized regions, not a static buffer), the 15 zero-size regions named and excluded rather than
silently skipped, and an interlock with `island_move`, which refuses a double rebase.

### D6.9 — `MAX_PLAYERS` stays a constant, and why (researched 2026-09-06)

**Decision: `MAX_PLAYERS` is OUT of `SB-BIND` T2** (user's call, 2026-09-06, on the research below).
T2 derives the eight per-player *roster* caps from the bound region size; the player count does not
move. Recorded here because the reasoning is not obvious, the naive reading points the other way,
and a later session would otherwise re-derive it.

**Why it looked like T2's business.** D6.3 notes that `MAX_PLAYERS` is not violated by the cap-raise
because `player_data`'s 166 140 stride "appears nowhere as an immediate" — re-verified 2026-09-06,
`find-constant-uses(0x288fc)` returns **0**. And the count *is* derivable:
`live_size(RID_PLAYER_DATA) / 0x288fc` is exactly 8, and floor division survives D6.5's `reach` bind
(1 329 136 / 166 140 → 8). So the mechanism is available and nearly free.

**Why it is nonetheless wrong to use.** Three findings, in increasing order of how much they settle:

1. **8 is in the RECORD LAYOUT, not just the outer dimension.** `game_player_data` contains four
   `[8]`-dimensioned sub-arrays — `ai_intel_seen_count` (`+0x28488`), `ai_intel_flags` (`+0x284a8`),
   `ai_player_relation` (`+0x284c8`), `ai_opponent_assessments` (`+0x286d8`) — and the session player
   record (`_G_LLM_STRAT_PLAYERS`, 8 × 1856) carries `relation byte[8]`. A player raise therefore
   changes two record strides. That is the case the roster cap-raise explicitly excluded:
   the hardcoded-limits rule — "element strides … change only if the record layout
   does (**reimplementation territory**)".

   **This corrects that page's conclusion**, which reads "a player-count raise has no stride class at
   all … player_data needs only base refs + bounds". The premise (no stride *immediates*) is true;
   the conclusion (no stride *work*) does not follow, because the ×166140 multiply is a shift-add
   chain, not an `IMUL`. Measured at `llm_strat_init_human_player_data` `0x004dd93b`: `SHL 2 / ADD /
   SHL 7 / SUB / SHL 2` → 2556·p in EDX, `SHL 6` → 163584·p in EAX, then a single
   `[EDX + EAX + disp32]`. 2556 + 163584 = 166 140. This is the same class that the existing
   IMUL-only scanner missed for `buildings` (157 sites / 58 functions) and that produced the
   "correct only for player 0" AI crash.

2. **The cost in the exe is ≥ the roster raise the project already ships.** Estimate 6 000–9 000
   patch sites against the shipped manifest's 7 770, on an array (`player_data`, ~1 650 refs across
   130 functions — and **0 of the 7 770 shipped patches land in `[0xe6dec0, 0xfb26a0)`**, measured)
   that has never been relocated, plus ≥ 36 save/load size immediates (18 player-dimensioned blocks
   × save + load). The one place it is *cheaper* is loop bounds: capacity loops are
   `CMP r/m32, imm8` (byte-verified at `0x0045539d` and `0x0049e413`), so 8→16 is a free one-byte
   edit where 100→500 needed trampolines. Nobody is going to build that exe.

3. **The apparent fidelity defect is not one.** The original genuinely distinguishes *capacity* from
   *active count*: `llm_strat_ai_players_tick` (`0x004db905`) bounds five loops on
   `_G_LLM_STRAT_AI_ACTIVE_PLAYER_COUNT` (`0x6616a8`, 31 reads / 4 writes), and the netcode branches
   on `_G_LLM_NET_ACTIVE_PLAYER_COUNT` (`0x5d54c4`), recomputed by a capacity-bounded scan. It is
   tempting to conclude that every C++ site reading `MAX_PLAYERS` where the original reads a count
   global is a live defect. **Measured, it is not:** `mh/ai` has **31 uses of `active_player_count`
   across 12 files** against **two** actual `MAX_PLAYERS` loop bounds (the other 7 textual hits are
   comments), and both survive scrutiny — `ai_player_tick.cpp:121` records "Loop bound is `EBX < 8`,
   SIGNED (CMP/JL)" from the disassembly, and `ai_shortage_state.cpp:34` loops the in-record
   `ai_intel_flags[8]` (capacity, correctly) while *the same function* uses the count global for its
   active sweep at line 45. The translators already made this distinction per site.

**The split that would have to happen first, if anyone ever revisits this.** `MAX_PLAYERS` today
names two different quantities, and deriving it from the bound region would silently corrupt the
second:

| use | correct source |
| --- | --- |
| outer array capacity — `units[8][100]`, `players[8]`, `peer_state[8×8]` | derivable from the bound region size |
| **in-record sub-array dimension** — `ai_intel_flags[8]` and its three siblings | **pinned to the struct mirror**; a derived 16 here overruns `mh_game_player_data` |

Until those are two names, `MAX_PLAYERS` must not be derived. The sharp edge in any such audit is the
11 `peer_state[p * MAX_PLAYERS + q]` sites over `_G_LLM_NET_LOCKSTEP_PEER_STATE` (64 B = 8×8), where
a wrong value skews addressing rather than truncating a loop, plus `desync_watch.h:260-261` — a
genuine static array dimension that must not be converted at all.

**What the lobby says, checked because an independent ceiling would have changed the answer:** it is
the same 8, not a separate number. `_G_LLM_LOBBY_SLOTS` is 456 B = 8 × 57, the peer-horizon arrays
are `[8]`, and the slot array feeds `_G_LLM_STRAT_PLAYERS[8]`, which is what `count_active_players`
scans. The lobby does carry a second, *smaller* runtime bound — the map's declared player count
(`current_map_data.field2_0x8`) — which is already dynamic.

**Not established, and deliberately left so:** the number of ×`0x288fc` shift-add chain sites (no
scan has been run; the zero `IMUL` hits prove only that none are immediates), the total count of
`CMP …,8` player loops, and whether restored MP works above 3 peers.

### D6.10 — What T4/T5 actually drained, and the gate hole they exposed (2026-09-06)

The named-VA ratchet went **191 → 0**: `libmh/lockstep/` 160, `libmh/save/` 31. What the count hides is
that draining it was two changes per binder, and either one alone leaves the module exactly as
broken.

Lockstep kept its state in twelve structs, each bound in its own translation unit as
`static const X st = { reinterpret_cast<T *>(mh::addr::NAME), ... }`.

- **The address** is the stock `.bss` VA. A host binding a relocated region moves the bytes and
  every read still comes from the abandoned copy — succeeding, returning stale values, and agreeing
  with itself.
- **The `static`** runs its initializer once. Converting the addresses to `ptr<T>(RID)` and stopping
  there resolves them at whatever the bind was on *first call* and caches that forever: the same
  stale pointer with better syntax, and harder to see.

So the twelve binders moved into `lockstep/lockstep_state.cpp` and every accessor returns **by
value**, re-resolving per call — `mh::sim::state()` and `mh::ai::state()` have done exactly this
since SIM0/AI0 and are called from the per-step hot path, so the shape is measured, not hoped for.
`libmh/save/` got the same treatment in `save/save_state.cpp` for the state its live arm reaches for
*around* the blocks (the block addresses already came from `save_table.gen.h`).

**`check_sim_addresses.MODULES` now names two binders for each of those modules.** `overlay_hoist.cpp`
qualifies beside `lockstep_state.cpp` on `libmh/orders/`'s two-file precedent: every function in it is a
one-line read or write of one game global handed to the engine as an op, so there is no translated
logic there for a bound struct to keep away from an address.

**THE GATE HOLE.** `gen_state_registry.py --check` re-derived the *header* from
`tools/data/state_regions.json` and compared that. It never compared the data file against the five
manifests the data file is a merge **of**, so a stale data file was invisible to the only gate that
watches the registry. The four globals `overlay_hoist.cpp` binds reached the addr manifest at
LIB-ABI stage E on **2026-09-03** and were still unregistered three days later, with `--check`
printing *"matches its inputs"* throughout. A region nothing registers is a region no bind can move,
so the hole ate precisely the property `SB-BIND` exists to establish. The new upstream arm runs
`merge()` (~0.2 s) on every `--check` and names what differs by class. Registry 823 → **827**.

**Two stale facts found while draining, both older than this item and both invisible to every gate:**

- `tx_emit_chat.cpp` bound `_G_LLM_CHAT_TARGET_MASK` as a bare literal `0x00e5898b` under a
  2026-07-29 comment saying the symbol was absent from the manifest and a follow-up must add it. The
  follow-up landed; nothing pointed the literal at it, **because a resolved TODO in a comment is not
  something any gate re-reads.**
- The five `w_sprintf` sites cast `mh::addr::w_sprintf` to a hand-rolled `__cdecl` pointer,
  explaining that the generator refuses varargs so no generated form could exist. The premise was
  stale: `gen_dll_calls` emits per-arity variants (`__vs`/`__vss`/…), so the calling convention now
  comes from the DB rather than from that comment.

**Four regions are complete in the address and not yet in the extent**, recorded in
`save_state.cpp` rather than quietly bound: `G_SAVE_MODE_RESET_DWORDS` (size 0),
`G_SAVE_DIR`, `G_SAVE_TEMP_DIR` and `G_LZW_TEMP_DATA` (size 1 each). An instruction scan attributes
a byte to a symbol only where an instruction *names* that byte's address, so a buffer walked by a
pointer the code advances measures as one byte however large it is. `ptr<T>(RID)` ignores the size,
so this is harmless today; it bites when a host sizes an arena off the registry, which is
`SB-HOSTFREE`'s reach work.

**Proven by mutation, not by a green run.** Restoring the `static` on one lockstep binder reddens
exactly three `bindtest` arms; caching `mh::save::binds()` reddens three more. Both read-back arms
are **guarded**, and the guard is not a softening: unguarded, the lockstep mutation crashed the suite
with *no output at all* — a view that did not follow points at an unmapped stock VA — so the three
failures it had already recorded were never printed.

## D7 — `SB-HOSTFREE` H0: the instrument repaired, and what the census says the item can actually prove (added 2026-09-06)

### D7.1 — One hash slice was reading 40 bytes it did not own, and the compile-time check for that was circular

`HASH_REGIONS[]` carried **one** entry, `time_globals`, declared as 48 bytes at `CURRENT_GAME_TIME`
(`0x00e587b1`). `CURRENT_GAME_TIME` is **8** bytes. The other 40 are five separate registry regions —
`LAST_GAME_TIME`, `TOTAL_GAME_TIME`, `_G_LLM_STRAT_SIM_STEP_INTERVAL`, `GAME_TIME_DELTA`, `game_speed`
— adjacent in `.bss` and adjacent nowhere else.

That is fatal **specifically** to this item. Every one of those five is independently bindable; the
arrangement `SB-HOSTFREE` exists to prove is exactly the one that separates them. Under it the slice
reads 8 live bytes and then 40 bytes of whatever now sits past the relocated host, so the
determinism hash keeps printing a value for the clock family and stops observing it. A green
3 000-step relocated run would then prove nothing about the clock — and the clock is what
`llm_strat_time_tick` writes, i.e. the SP oracle's whole subject.

**Why no gate caught it, and this is the part worth carrying.** The generated header does emit a
containment `static_assert` per slice — but against the region's `reach`, and `reach` is
`max(measured size, the furthest byte any manifest claims)`. A hash entry claiming 48 bytes at an
8-byte symbol **raises the reach to 48 and is then measured against it**. The check is circular for
precisely the entries it would have to catch. It had been green over this since the manifest was
extracted on 2026-07-31.

**The repair.** The manifest entry is split into six, one per region: `current_game_time` keeps index
52 (renamed — a name meaning *the six* on a slice covering one is how a desync report comes to name
the wrong thing) and the other five are **appended** at 56–60, because the table order is a wire
contract that `mp_analyze.py` reads positionally. All six stay `state_excluded()`; the state-only
verdict is unchanged, which is deliberate — an instrument repair that also moved the verdict would be
two changes wearing one commit. `SP_TIME_REGIONS` grows from three names to eight, and an SP failure
can now say *which* clock value moved.

**The real gate is `gen_state_registry.check_slice_overruns`**: `offset + len <= size`, on the
measured size, across both slice tables, on every invocation (so it stops the header being written
rather than being complained about afterwards). Committed state: **0 overruns of 76 slices**. It is
mutation-proven from the direction the defect came from — a *manifest* edit, not a registry edit:
restoring the 48-byte window reports one overrun naming all five swallowed regions.

**And the runtime half** is `bindtest` arm M, which is the half that would have caught this in 2026-07.
It binds all six to six separate buffers in **reversed** order (so the adjacency is genuinely gone,
not merely six `rebase()` calls deep), pokes each of the five, and requires each one's *own* slice
hash to move. The counterfactual is asserted directly rather than left to a code reading: the same
48-byte window the pre-H0 entry hashed is recomputed on every poke and **must not move**. Both halves
go red — folding the five back onto `CURRENT_GAME_TIME`'s address reddens the premise and the
five-poke check; binding the six contiguously reddens the counterfactual.

### D7.2 — "a RELOCATED copy of EVERY bindable region" cannot mean every region, and the census says so by name

`SB-HOSTFREE`'s `done_when` asks for the host to bind a relocated copy of **every** bindable region
and for the run to stay hash-identical. Measured against `region_accessors.json`, that is not
achievable while the original binary is in the process, and the reason is Law 1 rather than anything
about the bind:

| | regions |
| --- | --- |
| in the registry | **827** |
| free of any original accessor — **relocatable** | **401** |
| reached by an original writer, reader or **address-taker**, or unmeasured — **blocked** | **426** |
| zero-size (nothing to relocate) | **15** |
| of the relocatable, what a host may actually move — **`movable_count()`** | **380** (**313 300 B**) |

> **Both of the first two numbers were wrong when this section was first written**, and the
> correction came from running the arm rather than from re-reading the data: they were 463 / 364,
> derived from `external_writers + external_readers == 0`. The census **drops** a matrix cell whose
> only counts are `addr_of`/`imm`, so an original body that *passes* a region's address — 
> `push offset X; call CreateMutexA` — left no trace in either list. 69 regions have such a taker,
> and one of them exits the game at step 0 when moved. Schema 5 records them; `measured: false`
> (60 regions, absence of evidence) now blocks too. Trap: "no accessor" is not "nothing touches it" -- the original passes the region's ADDRESS.

An original accessor is a baked `disp32` in an instruction we do not own. It cannot follow a bind, so
relocating a region any of them still reads or writes puts the original half of the game on the
abandoned `.bss` while our half reads the arena. The hash would not report "identical"; it would
report a divergence, or worse, a frozen arena agreeing with itself.

**The item's `7 388 832 B over 808 sized regions` is the all-regions figure and is not the
movable one.** What a host may actually move is 313 300 B — about **4 %** of it.

**It is still not a vacuous proof, and that is the number that decides whether the item survives
this.** **19 of the 51 hash-slice host regions** are movable, so the determinism hash still watches
relocated bytes: **13 of 61 strategic slices and 6 of 15 tactical slices**. That count is unchanged
by the schema-5 correction above — the 62 regions it withdrew include none the hash reads, which is
the one way that correction could have hollowed the item out and did not. The relocatable set includes the whole order pipeline (`QUEUE`, `QUEUE_COUNT`,
`STAGING`, `STAGING_COUNT`, `PENDING`, `PENDING_COUNT`), the lockstep barrier tables
(`PEER_HORIZON`, `PEER_HORIZON_PENDING`, `COMMITTED_HORIZON`, `PEER_STATE`, `PEER_TIMING`), the frame
ring and fps estimate, and six tactical tables. Those are the regions `LIB-REF` and `SB-SOLE` are
about.

So the clause needs restating, not abandoning: **every region the census says no live original
accessor reaches**, with the blocked set named and counted rather than skipped — which is what the
existing clause already demands of the exclusions ("the 15 zero-size ones, **and any other**"). The
six clock regions of D7.1 are themselves in the blocked set (`CURRENT_GAME_TIME` has two original
readers), which is why arm M relocates them in the offline fixture, where no original code runs.

### D7.3 — The relocation arm's two self-refusals, and the one that would have corrupted saves silently

Building `bind_relocated()` surfaced a hazard the item had not anticipated, and it is the more
dangerous kind: it would not have diverged.

**Ten registry regions have `reach > size`** — a manifest claims bytes *past* the symbol. They are
the ten overrunning save blocks D6.6 measured. Two separate refusals follow, and only the second
bites today:

1. **The overrunning region itself may not move.** Relocating it copies a window over its
   *neighbours* into the arena and then poisons those neighbours' live storage — corrupting regions
   the bind never claimed. All ten are already blocked by an original accessor, so this branch is
   unreachable now; it is written down because `relocatable` grows as promotion retires accessors,
   and the day one of the ten becomes eligible is a day nobody will be looking for a 1623-byte
   poison.

2. **A region lying UNDER someone else's overrunning window may not move either — and 21 do.**
   `_G_LLM_GAME_SESSION_MODE`'s 1623-byte save block runs over 50 regions, and **21 of them are
   relocatable**. Move those and the block keeps reading the stock address after the bytes have
   left: it saves 0xCD where those regions' live values should be, and restores poison on load.

The second is the one worth carrying, because of *how* it fails — and the mechanism was checked
rather than assumed. `identity_resolve` is `mh::state::translate(addr, size)`, and a block the
registry cannot place returns `nullptr`, which the driver treats as a **hard failure**. So the
instinct is that a spanning block would fail loudly. It does not, and the reason is D6.5: `covering()`
resolves against **`reach`**, and `reach` was extended to 1623 precisely so this block would resolve.
It therefore gets a perfectly valid pointer — to the stock `.bss` — and reads 0xCD for the 21 regions
that moved.

**The decision that made overrunning blocks resolve is the one that makes this silent instead of
loud.** And the damage is confined to bytes *only the save format reads*: the determinism hash reads
each region at its own live base, finds the arena, and reports agreement. A relocated soak would have
been green, every step, while writing corrupt saves — the oracle built to catch relocation faults is
structurally blind to this one.

Both refusals are derived from `REGIONS[]` rather than declared, so a new overrunning block in the
save table starts protecting its neighbours the moment the registry is regenerated — there is no
list to forget. They live in the **generated header**, as `constexpr under_overrunning_window()` and
`is_movable()`, and that placement is load-bearing rather than tidy: the bind and the non-vacuity
counts have to agree about which regions moved, and the first version of this — a private copy of
the predicate in `host_bind.cpp` — had the counts reporting the wider `relocatable` set while the
bind moved the narrower one. The assertion disagreed with reality by exactly the regions the bind
was right to refuse.

So the numbers a host actually moves are **`movable_count() = 380`** — of the 401 the census clears,
minus the 21 under an overrunning window — and the report prints every bucket by name. A live run
consumes **316 676 B** of arena for them (more than the 313 300 B the sizes sum to, because each
region is 16-byte aligned). Both refusals retire when the save blocks are decomposed into
`(region, offset, length)` slices that follow each region separately, which is this item's own
save-block tranche (D6.6).

### D7.4 — The mutation `done_when` asked for cannot go red, and what replaces it

`SB-HOSTFREE`'s acceptance says: *"binding ONE region back to its stock base while the rest are
relocated makes the run diverge and the report NAMES that region — a green run that could not have
gone red is not a pass."* The instinct is right and the mutation is wrong, which only became
visible by running it.

**Measured.** A 3000-step soak with `relocate_pin=_G_LLM_STRAT_ORDER_QUEUE` reported
`golden MATCHED over 3000 compared steps`. The pin applied correctly — the log line shows
`379 region(s) relocated` instead of 380, `12 strat` hashed slices instead of 13, and names the
pinned region — so this is not a mutation that failed to arm. It is a mutation that cannot diverge,
for two independent reasons, both general:

- **Every consumer follows the registry.** A region answered at its stock base therefore behaves
  *exactly* as it does in an unrelocated run. The pin removes a move; it does not introduce a
  disagreement, and a disagreement is what a divergence is made of.
- **The poison staged at that base is transient.** The first write lands on it and it is gone — and
  for live state the write comes before any read.

The order queue adds a third, specific to it: `mh::orders` L1-serves that region since ST4, so the
hash asks the module for its canonical stream and never touches the address at all. Any of the three
alone is enough.

**What replaces it: `relocate_corrupt=<region>`.** Relocate the region normally, then fill its
**arena** copy with 0xCD before binding. Now a consumer that correctly follows the registry gets
garbage, and the run must diverge naming that region. That is the property the acceptance actually
wants — *the determinism hash is reading the relocated bytes* — stated as something that can fail.

`relocate_pin` is kept, and kept honest: the log now calls it `pinned to stock (no-op arm)` rather
than `MUTATION, expect divergence`. It is still worth having, because "one region answered at stock
while 379 move, and the run is unchanged" is a real property — it says a partially-relocated
arrangement is coherent. It is simply not evidence that the gate can go red.

**The general shape, which is the reason this is written down rather than just fixed.** A mutation
has to make the system *wrong*, not merely *different*. Removing an optimisation, skipping a move,
taking a slower path — these change how the run gets there and not what it computes, so a
correctness oracle is right to stay green. Before trusting a mutation arm, ask what incorrect value
it puts where, and which reader sees it.

### D7.5 — The relocation arm, run: what it proved and what it found

Three 3000-step all-AI soaks against one golden recorded on the stock bind
(`test_ui.py --soak --steps 3000 --soak-golden …`, one local lane, no rig peers).

| arm | `[harness]` | result |
| --- | --- | --- |
| golden | — | recorded, 3000 steps |
| **the claim** | `relocate_state=1` | **`golden MATCHED over 3000 compared steps`** — 380 regions, 316 676 B, 13 strat + 6 tact hashed slices on relocated bytes |
| the no-op arm | `relocate_state=1;relocate_pin=…` | matched (see D7.4 — this is not a mutation) |
| **the mutation** | `relocate_state=1;relocate_corrupt=_G_LLM_STRAT_FRAME_TIME_RING` | **`DIVERGED from golden at step 1, region(s): frame_ring, fps_estimate`** |

The interlock worked on its first live run and said so:
`[ai-island] MOVED 0 region(s) … (48 already relocated by the HOST: claimed, not moved)`. The move
and the ST4 claim are separable, so `island_move` claims what the host already relocated instead of
copying poison into its own store.

**What it found, and this is why the arm was worth building rather than reasoning about.** The first
poisoned run died at **step 0** — `PROCESS-GONE`, no crash, no WER event. The same run with
`relocate_poison=0` reported `golden MATCHED over 3000 compared steps`, which localizes the failure
exactly: the bind and the copy are fine, something still *reads the abandoned address*. It was
`_G_LLM_SINGLE_INSTANCE_MUTEX_NAME`, cleared by the census because the original only ever **takes**
its address (`push offset …; call CreateMutexA`) — see D7.2's correction.

**And a second, quieter one.** With that fixed the run passed — while printing
`master_gate=3452816845` in its own AI banner. That is 0xCDCDCDCD: `harness.cpp` was reading
`_G_LLM_STRAT_AI_ENABLED` from the stock VA. **The instrument was reading dead memory and the run
still said PASS.** `tools/check_movable_addresses.py` gates the whole DLL on the narrow question the
module gate never asked — *does anything spell the address of a region a host may move?* — and it
opened at **26** sites, all in `mh/seams`, the instrumentation layer that gate never covered.

**All 26 are now drained and the ratchet is at zero.** Eight in `harness.cpp` (the order
queue/pending/staging triples and their counts, the per-player sequence table, the AI gate) and 18
across the other seven files: `constexpr uintptr_t ADDR_*` became inline functions, because a
constant cannot follow a bind. `net_session_probe.cpp` got the larger version of the same change —
its 22-row store table carried a `region_id` per row instead of a `uintptr_t va`, resolved through
`live_base()` at read time, because a probe whose job is to say whether the session's opening stores
landed would otherwise have reported every movable row as MISMATCH against poison. (That TU was
deleted at fork F2F; the finding is kept because the same trap applies to any future live read-back
of a movable region.)

The baseline file is kept at an empty object rather than deleted: an absent baseline means an implicit
zero and would be trivially satisfied. The ratchet still has work — `movable` **grows** as promotion
retires original accessors, so a region that becomes movable while some old line still spells its
address turns this red on code nobody has touched.

### D7.6 — The four pointer-valued slots, read-back-proven (2026-09-06)

`done_when` asks that the pointer-valued slots be "shown by READ-BACK to dereference into the
relocated arrays, not asserted — **all four of them**". One was. `bindtest` arm J relocated `units`,
translated a stored stock pointer into it, poked the arena and read the byte back through the
translated pointer — and the other three were left to follow by inference.

That inference is precisely the mistake recorded before: it registered this shape as
having **one** instance, and `tools/data/pointer_slots.json`'s generated enumeration found **four**.
So the arm now runs the same relocate → translate → poke → read-back cycle for each of
`CUR_BUILDING → buildings`, `CUR_PROJECTILE → projectile_pool` and `CUR_FX_ANIM → fx_anims`, each
with its own arena, because each slot points into a *different* registry region.

Mutation-proven: making `translate_slot` the identity reddens 5 checks — the three original
`CUR_UNIT` ones and both new summary checks. `pointer_slots.json`'s `_residual` — which named this
clause as the open question — is closed for all four `deref` entries; the 19 `slot` entries are
unchanged, since what a `T**` points *at* is the deref entry's problem and it is now settled.

### D7.7 — The tactical arm: it passes, and getting it to pass found the last alias (2026-09-06)

**The vehicle is the mode that already existed.** `--tact-determinism` runs two arms of one local
lane through the same `--tactical` entry and compares the per-frame `T`/`TS` hashes. `--tact-relocate`
makes the **second** arm run under `[harness] relocate_state=1`, so the mode's own comparison becomes
in-place vs relocated. Relocating both arms would have compared two relocated runs, which agree for
the same reason two in-place runs do.

| arm | result |
| --- | --- |
| `--tact-relocate` | **`SIM: IDENTICAL over 400 frames`**, **`FULL: IDENTICAL over 400 frames`** — 380 regions relocated, **6 tactical hash slices** on relocated bytes |
| `--tact-relocate-corrupt _G_LLM_TACT_DOOR_TABLE` | **`DIVERGED at frame 1 … regions=['tact_doors']`** |

The shape rules refuse a vacuous pass: the relocated arm's tactical-slice count is read back out of
the DLL's own `[reloc]` banner, and a run that moved none of them fails rather than agreeing for
free.

**Getting there took one real defect, and it was a new class (G138).** The first relocated tactical
run ended at **frame 1** — `TACT SURVIVORS owner0 (none)`, 16 units instead of 24, the player's squad
missing. Re-running with the poison disabled changed *nothing*, which kills the entire "something
reads the abandoned address" family: with poison off a stale reader gets a correct copy.

It was `launch.cpp` filling the squad blackboard through `mh::addr::squad_blackboard` = `0x00e15e60`
— the same 1024 bytes the registry calls **`_G_LLM_SQUAD_STATUS`** and moves. A **write here against
a read there**, which is exactly why the poison switch was irrelevant. Five sibling scalars were
hidden the same way.

**And the gate that was supposed to catch it reported zero**, because it matched the constant's
**name** against the registry's region names — the same question only when two tables spell the bytes
identically, and `mh_addrs.gen.h` and `state_regions.json` do not. The gate is now **address-keyed**:
it resolves each constant to its value and asks whether that address lands *inside* a movable region,
which also catches interior offsets a name join can never see. It reports the alias with the region it
hits (`squad_blackboard -> _G_LLM_SQUAD_STATUS`).

### The five contested regions: restated, not cleared

`done_when` allowed either. The census settles it — every one of the five is refused by the bind
because original accessors still reach it, so `fails_in: ["relocated-bind"]` stands, now **with
names**:

| region | still reached by |
| --- | ---: |
| `TILE_OBJECTS` | 44 (4 live writers, 37 readers, 3 address-takers) |
| `PASSABLE` | 19 (5 / 8 / 6) |
| `G_PLANET_STATUS` | 3 (1 / 2 / 0) |
| `STRAT_PATH_SLOT_FLAGS` | 2 readers |
| `STRAT_PATH_BUFFERS` | 1 reader |

The refusal is **mechanical**, not a judgement: `relocatable` is generated from that census, so each
entry lifts by itself when the last of its accessors is promoted. Each `why` now records the runtime
check that exists, the verdict, and the named accessors that keep the exception alive.

**One caveat, recorded because nothing enforces it.** `relocatable` is derived from a region having a
*translated body*, not from that body being **armed in the current configuration**. `SQUAD_STATUS` is
movable only because `llm_strat_bldg_gather_nearby_squad_status` is ours — and
**G44** every original access to it is *computed* (`&base + i*0x10`), so the census could not see the
original writer even if it ran. A region in that shape has no second line of defence.

**That caveat was the cause, and it took an item to find because the arm that tests it runs on one
step in three thousand**. A relocated soak through a real `LOADGAME`
diverged at exactly the load step, in `order_queue` alone. The writer is the ORIGINAL
`map_LoadPlanetFromDisk` @`0x00448107`, which restores that block with
`LZW_ReadCompressedFromFile(0x00bb4ed0, 0x4fb0, fh)` — the destination is an **immediate in its own
instruction stream**. `order_queue` is `relocatable` because we have a translated body for that
function; under the ship defaults that body is not armed, so the original ran and wrote to the
address the state had left.

**The first attempt to test this hypothesis armed the wrong switch.** `[promote] container_load`
installs over `llm_game_load` — the outer `.sav` root, which *calls* the per-planet loader. The
per-planet loader has its own switch, `[promote] load`, and stayed original in both arms; so the run
that was supposed to vary the hypothesis did not vary it, and its null result was read as evidence
against the hypothesis. With `load=1` actually armed, the same soak matches its golden over 3000
steps. D7.9 is the gate that followed.

### D7.8 — The save-block decomposition, and the refusal it lets go (2026-09-06)

The last clause. Ten save blocks run past the symbol they start at; for **four** the overrun lands in
other **live regions** rather than in gap bytes. `save-format.md` proposed a zero-filler and named the
real answer — decompose a block into `(region, offset, length)` slices — and said *"ST3 is where it
becomes a lint"*. It did not exist.

**What was actually wrong, and it is not "a missing check".** `identity_resolve` is
`mh::state::translate()`, which returns `nullptr` for a block the registry cannot place — so the
instinct is that a spanning block fails loudly. It does not: `covering()` resolves against **`reach`**,
and `reach` was widened so exactly these blocks would resolve (D6.5). The block gets a perfectly valid
pointer to the region it *started* in, and under a relocating host reads the other regions out of the
copies they abandoned. Nothing returns null. The save is simply wrong, in bytes only the save format
reads — which is why no determinism run can see it (G135).

**The decomposition.** `gen_save_table_header.py` now walks every block against the **measured**
region sizes — never `reach`, which is the number that caused this — and emits `BLOCK_SLICES` /
`SLICED_BLOCKS` for the blocks that span more than one region:

| block | runs |
| --- | ---: |
| `Upgrades` +10400 | 2 regions + 1 gap |
| `_G_LLM_GAME_SESSION_MODE` +1623 | **50 regions** + 3 gaps |
| `MESSAGE_TIME` +240 | 2 regions + 1 gap |
| `_G_LLM_CLICK_SELECT_TARGET_ID` +4 | 2 regions |

`save_driver` gathers and scatters those four run by run, on the same tri-state seam the ST4 owned
blocks already use. A region run follows **its own** region; a **gap** run — bytes no region describes
— stays at its stock address, which is right because nothing relocates memory no region claims. The
other six blocks are untouched: one region run plus a trailing gap already resolves, since the gap
lies inside the host's `reach` and the host bind copies `reach` bytes, so the gap travels with it.
Emitting those too would have bought a 1.3 MB staging buffer (`player_data`) and changed nothing.

**The run is resolved by REGION, not by address, and that is load-bearing.** `state_io` gains
`resolve_region(ctx, rid, off, len)`. `resolve` cannot answer it: it goes through `covering()`, which
matches on `reach` and returns the *first* region in base order containing the window — so a run
inside `GAME_SESSION_MODE`'s window would resolve back to `GAME_SESSION_MODE`, reproducing the bug the
decomposition exists to fix. `nullptr` is legal and means "fall back to the run's stock address",
which is what `savetest`'s sparse-slab fixture wants; it has no registry at all and still passes
**179 / 0**.

**The lint.** `tools/check_save_block_slices.py`: every block is either one region run (the existing
path is correct) or decomposed in the committed header. Unhandled overruns **0 of 68 blocks**. It
reads `SLICED_BLOCKS` out of the **artefact the DLL compiles against**, so it answers "is what ships
correct", not "would a fresh generation be correct". Four selftest arms, including a *wrong run count*
— an absent entry is the loud error, a mis-tiled one the quiet one.

**And the refusal it lets go.** D7.3's second refusal — the 21 regions under `GAME_SESSION_MODE`'s
window — existed only because those blocks could not follow them. They can now, so `is_movable()`
stops consulting `under_overrunning_window()` and **relocatable rises 380 → 401**. The count is still
reported, relabelled: *moved, and relying on the decomposition*. The first refusal stays — a region
that overruns its **own** symbol would carry a window over its neighbours and poison their storage,
which decomposition does not address.

**The write side is proven offline, run by run.** `savetest` gained a three-file arm: save with
nothing relocated (A); save with one spanned region answered out of a **separate buffer holding the
same bytes** (B) — B must equal A, or the decomposition is not byte-transparent; then change **one
byte** in that buffer and save again (C) — C must differ, or the gather never read the relocated
buffer at all. The middle arm is what stops the third passing for a trivial reason. **187 checks / 0
failures.**

**The live LOAD arm is OPEN.** A relocated soak through a real `LOADGAME` diverges at the load step in
`order_queue`; three explanations have been measured and rejected (the unpromoted loader, a stale
reader, run-to-run noise — the no-relocation control matches over 3000 steps). The known shape is
**G139**. It does not affect the strategic or tactical relocation arms, which carry no `LOADGAME`.

**Lifting the refusal immediately proved the gate that watches for this.** Six more aliases appeared the moment
those 21 became movable — `peer_grace_timer -> _G_LLM_NET_LOCKSTEP_PEER_TIMEOUT_ELAPSED` among them —
because `check_movable_addresses` tightens by itself as the movable set grows. That is the behaviour
it was built for, firing on its first real opportunity rather than on a hypothetical. All six drained.

## The boundary of this decision

This adjudicates the **registry** — the 554 regions the five manifests knew about *when D1–D5 were
taken*. It is **823** as of 2026-09-06 (D6.5); the counts in D1–D5 are as-measured and were not
restated. **2 057 of the
state matrix's 2 596 referenced regions resolve into no registry region at all** and are covered by
none of the numbers above. The registry is not every global in the binary, and no count here should
be read as one.

The census these numbers come from is Ghidra-derived and can go stale; `gen_region_accessors.py
--check` classifies drift in two directions and refuses an ownership claim made over a census stale
in the unsafe direction (`SB1` rule 7). Re-measure before quoting any figure on this page in a new
decision.

### D7.9 — Promotion-conditional movability: the walker gate (2026-09-06)

**The rule.** A region may be relocated only once **every original block-table walker that names its
bytes** has been promoted. Four functions reach state through a list of `(address, size)` blocks
baked into their own instruction streams:

| walker | ini switch | what it is |
| --- | --- | --- |
| `SavePlanetToDisk` | `[promote] save` | the per-planet WRITE |
| `LoadPlanetFromDisk` | `[promote] load` | the per-planet READ |
| `game::SaveGame` | `[promote] container` | the `.sav` root WRITE |
| `llm_game_load` | `[promote] container_load` | the `.sav` root READ |

**Why this is not a census fix.** By the census's own rule all four are **ours** — a translated body
exists — and `ours` never blocks. The census is right; `relocatable` simply never meant *armed*.
Armedness is a property of the running configuration, so it cannot live in a generated flag. The
mask is static (which walkers name these bytes), the host supplies which it promoted, and
`movable_under(r, armed)` is where they meet.

**Shape.** `gen_state_registry.py` derives `SAVE_WALKER_MASK[]` from `save_block_table.json` —
already-extracted data, not a new derivation — and emits `movable_under` /
`movable_count_under(armed)` beside `is_movable`. `relocation_opts::armed_walkers` carries the mask;
`bind_relocated` binds a held region **to its stock base** rather than skipping it, so the original
walker and every registry consumer agree about where the bytes are. That is correct, not merely
undamaged.

**Two guards against the gate going inert,** since a gate that never fires is the failure this page
keeps recording:

- a block table with **no `[promote]` switch** maps to `WALK_OTHER`, which nothing can arm, so a
  newly extracted table makes its regions permanently held rather than silently free;
- `static_assert(WALKER_HELD_COUNT > 0)` in the generated header, plus `bindtest` arm N2, which
  asserts the count, the specific region, that a held region is **bound-to-stock** rather than
  skipped, and that promoting the loader **alone** does not release a region the saver also names.

**And one guard pointing the other way, which is the only unsafe direction this design has.** The
bind runs as the second statement of `MH_Harness_Init`, before any promotion has installed, so it
necessarily reads a **declaration** (the ini) rather than a fact. `report_relocation()` runs after
`MH_Seam_Init` and compares the mask the bind trusted against the one `save_live` actually installed,
reporting a mismatch as a corrupted-run warning. The first build of the reader needed exactly this:
it read `[promote]` out of `mh_harness.ini` when the keys live in `mh_net.ini`, computed 0 in every
configuration, and reported `29 held` on a run that had promoted all four.

**Measured.** Every golden recorded on the same build; the `LOADGAME` fires at step 300 of 3000.

| configuration | relocated | held | hashed slices | verdict |
| --- | --- | --- | --- | --- |
| relocated, no promotion (the arm that failed) | 372 | 29 | 11 strat + 6 tact | `golden MATCHED over 3000 compared steps` |
| relocated, all four promoted (mask `0x0f`) | 401 | 0 | 15 strat + 6 tact | `golden MATCHED over 3000 compared steps` |
| relocated, `relocate_corrupt=…FRAME_TIME_RING` | 372 | 29 | 11 + 6 | `DIVERGED at step 1`, naming it |
| strategic soak, no `LOADGAME` (D7.5's arm) | 372 | 29 | 11 + 6 | `golden MATCHED over 3000 compared steps` |
| tactical `--tact-relocate` (D7.7's arm) | 372 | 29 | 11 + 6 | SIM and FULL `IDENTICAL over 400 frames` |

So the gate costs 29 regions and 4 hashed strategic slices at ship defaults, and **promoting a walker
earns its regions back in the same run** — the arm is conditional, not a permanent refusal.


## D8 — `SB-SOLE` closed: the writer boundary, and the `owned_by_dll` / `relocated` split (added 2026-09-10)

**`SB-SOLE`'s release condition is met.** Refreshed census, printed rather than implied:

| | 2026-08-28 (the item's opening snapshot) | 2026-09-10 |
| --- | --- | --- |
| regions `libmh` writes (`ours_writers` non-empty) | 342 | **434** |
| ...clean — no LIVE external writer | 203 | **310** |
| ...still holding a live external writer | 139 | **124** |
| distinct live external writers over them | 190 | **227** |
| ...UNDISPOSED | 190 | **1** |
| regions marked `owned_by_dll` | 0 | **342** (310 of them new) |

The population **grew** while it was being drained — 342 → 434 regions, 190 → 227 writers — which is
the intended dynamic and the reason the item was re-keyed to `ours_writers` in the first place: every
promotion adds regions `libmh` writes. What went to zero is the *undisposed* count.

### The residue, and what disposed it

`LIB-BOOT` (12) and `TACT-WRITERS` (16) were the two scopes still carrying undisposed writers. Both
are now `checked: true` in `tools/data/writer_dispositions.json`, on the **existing** reason classes:

- **`load-path` × 11** — the `cfg_*` config-table constructors. Every one is reached only through
  `ConstructObjects ← cfg_Init ← InitSafe ← llm_boot_stage_tick`, the single process-boot config
  pass; the two second callers are named rather than hidden (`cfg_anim_Construct` also runs on a
  cursor-set/menu-screen load, `cfg_final_planet_FillBankData` also at session start via
  `llm_strat_scenario_planet_clone`). The class's host-config block gained a second arm for them.
- **`init-only` × 1** — `llm_cfg_text_ptrs_reset`, a session-entry reset from `llm_strat_mode_init`,
  classed apart from the loaders rather than flattened into them.
- **`permanent-wall` × 13** — the tactical presentation writers. 11 already carried a
  `presentation` verdict in `tools/data/tact_walls.json`; the two that did not
  (`llm_tact_gfx_init_view_surfaces` / `_tile_ptrs`) are absent from that file only because they are
  reached from boot and from a window-resolution change, not from the tactical roots.
- **`dead-code` × 3** — `llm_tact_move_find_approach_tile` / `_find_reachable_dest` /
  `_flood_reachable_tile`. The first two are the tactical closure §9's proven dead
  pair; the third is dead **by transitivity** — those two are its only callers and its address is
  taken nowhere in the image.

**`TACT-WRITERS`'s "already adjudicated in the per-writer ratchet" was half true, and that is the
finding worth keeping.** The ratchet in `region_ownership.json` only sees **cross-subsystem** writes:
`--writers "Tactical mode"` covers 10 (region, writer) pairs over 8 regions, so tactical writers of
**tactical-owned** regions (`TACT_MOVE_*`, `TACT_SIDEBAR_*`, `TILE_VIS_MAP_M4/M5`) carried no reason
anywhere. Same structural blind spot the writer-attribution census was built for -- it asks who
owns each ORIGINAL writer of a region, which is the question a per-region view cannot pose.

### The ONE region that keeps an unexcepted writer

`PLAYERS` — `llm_ui_diplomacy_apply_and_resume`, owned by open MP item **`U39`**,
which NOPs the direct relation write so the order handler is the sole writer. It is deliberately
**not** excepted: an exception would accept a double-write on a hashed cell.

### External READERS are not required to be zero, and this is the paragraph that says why

In the hosted configuration the host answers the bind table with the **stock `.bss` base**, so an
original reader of an owned region reads **the live bytes `libmh` just wrote**. There is nothing
stale to read. Owning the readers as well was measured and rejected on its own numbers — 305
functions / 117 KB, option (a) above — and the `reader_seam` clause was never protecting correctness
*in place*; it was protecting against a region that had **moved** out from under its readers. So it
now keys on `relocated`, where it is the standalone build's gate rather than dead code. Measured:
**89** of the 310 new marks would be refused if that rule had stayed on `owned_by_dll`.

### The split, which `SB-BIND` promised and did not land

`D4` redefined `owned_by_dll` from *"the bytes have physically left the binary"* to *"`libmh` binds
this region"*. `SB-BIND`'s `done_when` asked for that to reach `region_ownership.json`'s `_comment`
and `gen_state_registry`'s rule text; **it never did** — both still carried the old sentence.
Measured 2026-09-10: marking the 310 qualifying regions produced **666 interlock violations, all 666
of them rules 1, 3 and 6** — exactly the three `D4` retired for the hosted configuration. Rules 4 (an
original writer remains) and 5 (unmeasured) fired **zero** times, which is the same sentence as
"`SB-SOLE`'s boundary holds". The rules were not wrong; they were keyed to the wrong claim.

So the stronger claim got its own field:

| field | means | arms |
| --- | --- | --- |
| `owned_by_dll` | `libmh` binds it **and no original function writes it** | rules 4, 5, 9 |
| `relocated` | ...**and its bytes are somewhere else** — the old sentence | rules 1–3, 6 |

They are a **ladder**, not alternatives: rule 9 refuses `relocated` without `owned_by_dll`. The 48 AI
island regions carry both (`ai::island_move()` really does move them); nothing else carries
`relocated`, so rules 1–3 and 6 stay armed and inert, the shape `LIB-REF`'s standalone build turns
live. The generated `region` struct's flag is renamed `owned` → `relocated` to match, and
`save_driver.cpp`'s `static_assert` follows it — keying that build failure on the weaker claim would
have failed the build for stating a true thing.

### Rule 10, the kept-writer ratchet

The other legal outcome per region needed a home that cannot rot. A region `libmh` writes that keeps
a live external writer now carries `kept_writers` (the exact list) + `kept_writers_why` in
`region_ownership.json` — **124** of them — and `gen_state_registry --check` refuses a missing
record, a list that disagrees with the census, a stale one, one alongside `owned_by_dll`, and an
empty reason. The per-writer adjudication is **not** duplicated there: it stays in
`writer_dispositions.json`, one reason class per function, gated by the writer-attribution census
that derives the population it must cover.

**The negative arm**, run rather than argued: un-owning ONE migrated function
(`llm_strat_bldg_gather_nearby_squad_status`, `sim_resid` row `verified` → `in_progress`) and
refreshing the census turns all seven regions it writes non-zero, and both gates go red — 7 ×
`ORIGINAL WRITER ON AN OWNED REGION` + 2 × `LIBMH-WRITTEN REGION WITH AN UNRECORDED WRITER` (exit 1),
and `gen_writer_attribution --check` names it as newly undisposed (exit 1). Restored: both exit 0.

## The interior-pointer disposition (fork F1F, user-ratified 2026-09-12)

The G_TEXT_PTRS fault exposed the class: dwords inside a carried block pointing INTO another
bound region's stock span, invisible to the base-equality census (`region_head_ptrs`). The
whole-blob screen is now a committed instrument — `tools/scan_interior_ptrs.py`, inputs are the
committed fixture blob + `mh_regions.gen.h`, three reproduction self-checks (blob-parse
totality; base-equality subtotal == the blob's own census; G_TEXT_PTRS reproduced
entry-for-entry: 806 = 721 carried + 85 `.rdata` + 0 zero + 0 unadjudicated) — and it re-derives
**41 holders / 4913 dwords exactly** (11 base-equality, so 4902 interior over 35 holders),
identically across all three fixtures: the population is template data, not live session state.

**The ruling: density-threshold scoping.** Density ≥ 25% AND interior hits ≥ 32 selects
**G_TEXT_PTRS alone** (89.5%, 719 interior) — and the separation is a **gap, not a tuned
constant**: the densest holder clearing the hit floor sits at 4.868%, so any density cut in
[5%, 89%] selects the identical set. Everything below is dispositioned **noise-by-class**, each
class mechanically characterised (`--partition` re-derives all of this):

| Class | Holders | Dwords | The evidence |
| --- | ---: | ---: | --- |
| Base-equality only (already censused) | 6 | 7 | value == a region base; spine.cpp's "eleven" |
| UTF-16 text aliasing | 11 | 2671 | 97–100% wide-text; the misaligned-phase control equals the aligned count (lift 1.0) |
| Offset / packed-delta tables | 6 | 1125 | `SPRITE_PIX_OFFSETS` is a monotone byte-offset ramp — the generated header's consumer expression `GFX_BANK_PIXELS + SPRITE_PIX_OFFSETS[frame]` settles it; dir tables hold packed byte deltas |
| Live/template record arrays | 12 | 378 | alignment lift ≤ 1.0, near-zero run share, scattered targets |

**Three riders, part of the ruling:**

1. **Four UI-widget holders carry MEASURED real pointers, filed as safe — not as noise.**
   `UI_WGT_LIST_MP_LOCAL_BROWSER`/`MP_MAP_PICKER` (+64/+68), `UI_WGT_MENU_OK`/`UNDEF_0065076B`
   (+56) hold per-widget label pointers into `G_TEXT_BLOCK` at consistent struct offsets
   (N = 1–3 each). Their safety argument is the headless-replay one — the UI widget class never
   runs in a standalone strategic replay — the same argument spine.cpp makes for the
   base-equality heads. A density record alone would have mislabeled measured pointers as noise.
2. **TLO_REGISTRY: carried verbatim, never dereferenced standalone — safe by a stated guard; do
   not derive.** The boot blob derives this block *because* its live bytes are image pointers;
   the world blob carries it raw *because* the world policy is verbatim-or-the-oracle-moves.
   Both are right; `materialise_tlo()`'s rebased-bind refusal (`state/boot_snapshot.cpp`)
   reconciles them, the consumer degrades to the original's own Jungle default, and the region
   is `MF_VIEW`-only so the pointer values never reach the determinism hash. Deriving it in the
   world policy would change the world schema and force re-recording all three fixtures to fix
   a hazard two guards already prevent. The sole-reader claim is a **gate**
   (`scan_interior_ptrs --check`, wired into `lint_repo`): a new `RID_TLO_REGISTRY` referencer
   outside `boot_snapshot.cpp` + its selftest is red.
3. **The instrument's blind spot, on the record.** The screen sees only pointers into registry
   regions — 66% of its own address window (7,710,311 of 11,676,038 bytes); the blob's loose
   counter says ~112k dwords land in inter-region gaps invisibly, and pointers into
   non-registry `.bss`, `.rdata`, or heap are invisible entirely. G_TEXT_PTRS was caught only
   because `G_TEXT_BLOCK` is a registry region. The live replay remains the standing
   enforcement instrument: under per-region arena binding a real stale pointer faults loud or
   moves a hashed region.
