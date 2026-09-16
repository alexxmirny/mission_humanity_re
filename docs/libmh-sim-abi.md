# The sim host API reduction — from 37 mirrored entries to a designed interface

**STATUS: EXECUTED (2026-09-10) — all eight slices done; the sim table is the designed surface
§3 describes (24 entries, version `0xFD4BC3F8`). The register below is now a RECORD: each row carries
its verdict, its final emitted name and what its execution learned.** This is
LIB-IFACE-SPLIT step 3, the follow-on the split existed for: the sim table
(`libmh_host_api`, 37 entries after the 2026-09-10 split) reduced to an ABSTRACT host API a
standalone host can implement without the original binary. The tact table
(`libmh_tact_host_api`, 16) is **FROZEN** and out of scope — duplication (an entry in both
tables) is what makes that legal: a sim-side reshape never constrains tact's copy.

Companion to [libmh-abi.md](libmh-abi.md) (the rules R1–R10 and the lift history — its §0
epistemics clause GOVERNS this plan too: every disposition below gets its own sanity check
against the actual code at execution, and none is mechanical rule application) and
the endgame plan's D-E3/D-E6 stages. Evidence: `tools/data/hostapi_callers.json` +
the four-reviewer fan-out of 2026-09-10 (read-only, per-entry site reads), with every
LOW/MEDIUM-confidence claim re-verified centrally against the Ghidra DB or the source before
it landed here.

## 0. What "abstract" means here (the acceptance test)

An entry survives into the abstract API only if **the LIB-REF headless host can implement it
from its contract sentence alone** — no original binary, no knowledge of Watcom internals.
Everything else either leaves the surface (DELETE / INTERNALIZE / CONVERT to a channel) or is
explicitly stamped **pre-fork-only** (DEFER-BLOB: superseded fork-side by LIB-BOOT/LIB-WORLD).
The hosted configuration stays the oracle throughout (R5): mh.dll binds every surviving entry
over the original thunks and every slice must leave run_selftests + the UI suite green, with
determinism runs for any slice touching hashed state.

Disposition vocabulary (used in §2):

| verdict | meaning |
| --- | --- |
| KEEP-ABSTRACT | stays, with a designed name + contract a host implementor reads |
| DELETE | no host contract needed at all; libmh simply stops calling out |
| INTERNALIZE | libmh implements it itself (a translation or an owned constant) |
| CONVERT | becomes a typed record on an existing R7 channel (event/screen/invalidate/text) |
| RESHAPE | merges into a designed multi-entry service (the vfs question) |
| DEFER-BLOB | pre-fork keep as-is; fork-side superseded by the world/boot blob |

## 1. The measured surface (2026-09-10, post-split)

37 entries at the open: render-notify 3, hook 6, display 1, input 1, chat 2, io 8, map-io 7, net 2,
string 4, time 1, fatal 1, platform 1 — plus 3 of these shared with the tact table
(`GetResourseFilePtr`, `llm_view_set_size_mode`, `utils_abort`). **31 since SIMABI-HOOKS
(2026-09-10): the whole `hook` group left. 27 since SIMABI-NOTIFY the same day: the whole
`render-notify` group left too, and `map-io` went 7 → 6. 25 since SIMABI-CHAT, same day again:
the whole `chat` group left. 24 since SIMABI-DISPLAY, same day a fourth time: the `display` group
left too, and the tact overlap went 3 → 2. STILL 24 after SIMABI-VFS, same day a fifth time: the io
group's 8 entries were RESHAPED rather than reduced — the seek/tell retirement that would have made
it 6 was measured and refused (§2b) — and the tact overlap went 2 → 1 when the sim side of
`GetResourseFilePtr` became `asset_read`. STILL 24, and the VERSION CONSTANT UNMOVED at
`0x523B7A40`, after SIMABI-STRING (same day, a sixth time): a count-zero slice that changes only
contract PROSE and adds oracles — the generator hashes signatures and membership, not reason
comments, so a contract correction is by construction not an ABI change. STILL 24 and the version
STILL `0x523B7A40` after SIMABI-MAPIO (a seventh, same day): the two boot parses gained DEFER-BLOB
contracts, likewise all prose. STILL 24 after SIMABI-NAMES (the eighth and last, same day), which is
the one slice whose whole content IS the version move: `0x523B7A40` → `0xFD4BC3F8`, fourteen entries
renamed to their designed names, membership untouched, and the tact overlap 1 → 0 when sim's
`utils_abort` became `fatal` (tact keeps the original spelling — §2a).** Regenerate the numbers with
`python tools/gen_libmh_hostapi.py` (membership) and `python tools/gen_hostapi_callers.py`
(per-entry callers/sites); never trust this paragraph over the generators.

## 2. The disposition register

One row per entry. `conf` is the reviewer's confidence after central re-verification;
anything below HIGH is a lead the executing slice must settle first, per §0's epistemics.

### 2a. Settled by review + central verification (hooks, render-notify, chat, net, time, fatal, input)

**The six DELETEs are EXECUTED (SIMABI-HOOKS, 2026-09-10, table 37 → 31).** Each body was
re-verified empty in the DB that day (stack probe + `return`, nothing between) and each libmh site
was read before removal: all void, unconditional, no readback, no side channel. Two things the
execution learned that the register did not say. (1) `llm_net_lockstep_hook_stub`'s single site was
the ONLY statement of the timekeeper's self-match branch — the branch is load-bearing (it is what
keeps a self-match out of the leader/removal path) and stays, empty, with the reason written at it.
(2) Ten offline-suite checks used these calls as BRANCH WITNESSES (`teardowns == 1` and the like);
every one of them had a sibling assertion on the same branch (`returns`, `presence_lost`,
`commits`, `menu_force_return`), so the branches are still covered — but a hook deletion is not
free of test churn, and a slice whose stub is a branch's only witness would have to add one first.
The rows keep a new TERMINAL ledger class `deleted`: exempt from `gen_libmh_calls`'s orphan gate,
and its own gate fails if a migrated module ever calls one of the six again.

| entry | verdict | target shape | conf | key evidence |
| --- | --- | --- | --- | --- |
| `llm_teardown_hook_stub` | DELETE | libmh-internal no-op | high | body verified empty (stack probe + ret); 5 unconditional sites |
| `llm_teardown_hook_stub_b` | DELETE | same | high | same shape, 1 site |
| `llm_net_lockstep_hook_stub` | DELETE | same | high | empty @0x0049bc88; timekeeper.cpp:194 |
| `llm_debug_log_msg_stub` | DELETE | same (the string was never emitted in retail either) | high | ai_state.cpp sites; retail body emits nothing |
| `llm_lobby_map_recv_step_stub` | DELETE | same | high | body re-verified empty in DB 2026-09-10; the same-name lobby-bootstrap loop is untranslated lobby code, not our path |
| `llm_strat_session_begin_empty_stub` | DELETE | same | high | body re-verified empty in DB 2026-09-10 (stack probe + ret, 1 call edge); intent unknown, emptiness fact |
| `llm_strat_player_apply_all_colors` | CONVERT | `on_invalidate` `INV_PLAYER_COLOR_LUT` (no payload — the record is the instant) | high | save_live.cpp:814; LUT not in any hash region; same shape as INV_PLANET_MAP_PALETTE |
| `llm_strat_render_present` | CONVERT | `on_screen` `SCR_STRAT_FRAME_PRESENT` | high | terminal call of `detail::frame()` (sim_lt_frame.cpp:67); no same-frame readback; SCR_TACT_FRAME_PRESENT precedent |
| `llm_strat_render_view` | CONVERT | `on_screen` `SCR_STRAT_FRAME_REDRAW` | high | terminal call of `frame_redraw_behind_dialog()`; render-read only (state-boundary.md D5) |
| `llm_ui_chat_recalc_target_mode` | INTERNALIZE — **EXECUTED 2026-09-10 (SIMABI-CHAT)** | translated into `libmh/lockstep/lt_chat_ally_mask.{h,cpp}` (287 B, pure recompute, idempotent); R3b readback dissolved | high | R5 story CORRECTED at central check: 7 original callers exist — 5 translated lockstep, 2 UNTRANSLATED UI (`chat_target_remove`, via add) — so this is shared-storage dual-writer pre-fork, NOT sole ownership; ops idempotent, cells region-registered. Re-verified at execution: the caller set and the MODE/MASK xref sets are exactly as stated |
| `llm_ui_chat_target_add` | INTERNALIZE — **EXECUTED 2026-09-10 (SIMABI-CHAT)** | same TU (55 B, bitmask OR + the recalc tail, idempotent) | high | same; untranslated original caller `llm_ui_diplomacy_apply_and_resume` @0x004c7fc6 keeps its own path pre-fork. Its OTHER original caller `llm_diplomacy_init_multiplayer` is ours, and its site now binds our body |
| `llm_net_transport_recv` | KEEP-ABSTRACT — **RENAMED `transport_recv` 2026-09-10 (SIMABI-NAMES)** | `transport_recv(sender_id_out, buf, len)` non-blocking poll (the signature is the original's, unchanged — this slice renamed only) | high | contract already host-meaningful; mh.dll REPLACES send in retail |
| `llm_net_transport_send` | KEEP-ABSTRACT — **RENAMED `transport_send` 2026-09-10** | `transport_send(buf, len)` reliable in-order broadcast | high | 16 direct callers; the restored-MP seam |
| `llm_time_get_ticks_ms` | KEEP-ABSTRACT — **RENAMED `ticks_ms` 2026-09-10** | `ticks_ms()` monotonic, pacing-only (R8 contract) | high | lockstep watchdog sites |
| `utils_abort` | KEEP-ABSTRACT — **RENAMED `fatal` 2026-09-10, SIM SIDE ONLY** | `fatal(status)`, must not return. Tact's frozen copy keeps `utils_abort`, so the printed overlap goes 1 → 0 — duplication working as designed, the same shape as `GetResourseFilePtr`/`asset_read` | high | shared with tact (tact copy frozen) |
| `llm_strat_input_update` | KEEP-ABSTRACT — **RENAMED `apply_frame_input` 2026-09-10** | `apply_frame_input()` — ONE opaque wall entry: "supply this frame's strategic input effects, synchronously, once" | high | 10,785-byte fused pump/dispatch, SIM-RESID-W walled, unshadowable; a replay host dispatches recorded ORDERS here, no typed-event API (R10's typed shape is tact's, not this) |

**The four CONVERTs are EXECUTED (SIMABI-NOTIFY, 2026-09-10, table 31 → 27, tact byte-identical).**
Kinds `SCR_STRAT_FRAME_PRESENT` 21, `SCR_STRAT_FRAME_REDRAW` 22, `INV_PLAYER_COLOR_LUT` 33,
`INV_PLANET_EXTRA_SPRITE_BANKS` 34 (the fourth's row is in §2b); events version `0x00010013` →
`0x00010014`. **The `render-notify` group is now EMPTY on the sim side** (0 sim / 3 tact) — predicted
by the register, but it cost one test edit the register did not foresee: `hostapitest`'s "a notify
entry no-ops silently where a required one traps by name" check had no sim entry left to name, and now
makes that class assertion against the FROZEN tact table. That line has now been rewritten by three
separate conversions; it is the price of naming an entry in a test that is really about a class.

Four things the execution learned. **(1) For `apply_all_colors` the live rule was R3c, not R3b.** Its
write is trivially safe — the body is a 0..7 loop over `llm_strat_player_set_color`, whose only store
is `_G_LLM_STRAT_PLAYER_COLOR_LUT[player_idx]` @`0x00ae1948`, and "not in any hash region" was
re-derived by scanning every `HASH_REGIONS` and `TACT_HASH_REGIONS` extent rather than by citing the
row. What the loop READS is hashed (`strat_players[].color_index`), so the record names shared mutable
state: answered at the site, since nothing between the emit (`save_live.cpp:814`) and the end of the
container load writes a colour index — libmh's only writer of that field is
`llm_strat_player_profile_init`, on the session-begin path. **(2) The R3b gate found five candidate
readbacks the register predicted none of**, and all five were settled off the bodies rather than by
citing the precedent rows that share their writers: `SPRITE_PIX_OFFSETS` is element-disjoint (the frame
closures' only writer, `llm_gfx_build_bar_sprite_rle`, stores ONE fixed slot `[0x12]`, verified at its
line 92); `STRAT_SOLDIERS` has **two** writers, not the one the 2026-09-04 sweep's row named
(`llm_strat_render_tile_object` **and** `llm_strat_render_bldg_docked_unit`), both touching only
`anim_change_count`, which `emit_soldiers` already masks out of the verdict; `FRAMEBUFFER` and
`G_WIN_W`/`G_WIN_H` reuse the destination-only and idempotent-copy arguments with the identical writer
functions. **(3) The general argument is ORDER, and it is stronger than "terminal":** both frame bodies
run `sim_tick` and only *then* emit, so every reader runs before the emit in frame N and after the drain
in frame N+1 — both consumptions hand a reader the same bytes. **(4) `SCR_STRAT_FRAME_REDRAW` has a
SECOND emit site the scope did not mention.** `sim_tutorial_step_driver.cpp:299` calls
`MH_LIBMH_BIND(llm_strat_frame_redraw_behind_dialog)` **mid-body**, so "terminal call of its frame body"
is true of the body and not of every site; what follows it there is presentation records plus two
own-state reads, grepped. Same shape as `SCR_TACT_FRAME_PRESENT`'s own non-terminal second site, named
rather than glossed for the same reason.

**The two INTERNALIZEs are EXECUTED (SIMABI-CHAT, 2026-09-10, table 27 → 25, tact byte-identical).**
Both bodies live in `libmh/lockstep/lt_chat_ally_mask.{h,cpp}` beside the mask rebuild that reads their
output — one state set, one set of traps, no second TU restating them. The offline oracle is
`libmh_selftest.exe libtranstest` t18/t19 (20 checks over a `chat_target_state` of plain locals): no rig, and
no shadow site, because a body that is pure over its state gets the cheap parameterised test rather
than a T3 arm. Four things the execution learned that the register did not say.

**(1) The MCP decompile hides a fourth local, and it is dead.** `[EBP-0x1c]` is written three times
(init 1; cleared at `0x0049d6e7`/`0x0049d70d` off `Players[PlayerSide].relation[i] == 1`) and read
zero times — dead computation Ghidra silently eliminates, which is why the exported `.c` shows three
locals and the `.asm` shows four. It is the reason `chat_target_state` binds no `player_desc` even
though the disassembly reads that table. Read the `.asm`, not the decompile: this was the one place
the two disagreed, and only the `.asm` says why.

**(2) The real branch is `selected > 1`, not "everything selected".** `CMP [EBP-0x24],1 / JG` at
`0x0049d725`: with exactly ONE eligible target, selected, `all_selected` still holds and the answer is
still PARTIAL (2), never ALL (0). A reimplementation that reads that arm as "all selected → 0" passes
a three-target case and fails only at the boundary, so t18 carries a case whose whole job is to
separate the two.

**(3) `recalc` writes 0, 2 and 3 — and the ally-chat gate the mask rebuild tests for is 1.**
`_G_LLM_CHAT_TARGET_MODE` has six xrefs total and no `disp32` writer of 1 anywhere in the image, so
whatever arms ally chat does it through a path the xref set cannot see. Recorded rather than "fixed":
inventing a fourth value here would silently arm the rebuild.

**(4) The two ledger rows LEFT rather than reclassing.** `deleted` would have been a false claim —
libmh does make a call in their place (its own), and the originals stay live for the untranslated UI
callers — and no `host-callback`/`event-channel` class describes a body we own. So both rows exit
`libmh_call_ledger.json` the way the vendored sprintf family did on 2026-09-08, with the reasoning
moved into the owning header's banner; `gen_libmh_calls` reports pool 6 / unadjudicated 0 / orphaned 0
/ misrouted 0. An INTERNALIZE has no terminal class of its own, and after this slice it still does not
need one.

### 2b. io + map-io (15 entries, reviewed 2026-09-10)

| entry | verdict | target shape | conf | key evidence |
| --- | --- | --- | --- | --- |
| `utils_open_file` | RESHAPE — **EXECUTED 2026-09-10 (SIMABI-VFS)** | `vfs_open(path, mode_enum)` — mode is only ever "rb"/"wb"; paths stay verbatim (the 2026-09-03 decision holds) | high | **6** open sites (save_live.cpp:366,426,597,618,740,786), 3 "wb" / 3 "rb" — the register said "all 8 sites", which was the io group's ENTRY count, not this entry's site count |
| `utils_read_from_file` | RESHAPE — **EXECUTED** | `vfs_read(h,dst,n)→bytes` — `elem_size` is always the literal 1; short read REPORTED | high | save_live.cpp:101,608, both `(dst, 1, size, ctx)` |
| `utils_write_to_file` | RESHAPE — **EXECUTED** | `vfs_write(h,src,n)→bytes, negative on failure` — `param_2` (the fwrite COUNT, not elem_size) always 1; must not transform (save byte-identity) | high | save_live.cpp:85,623, both `(fh, 1, data, size)` |
| `utils_close_file` | RESHAPE — **EXECUTED** | `vfs_close(h)`, now VOID — the status was discarded at all 5 sites and by the original | high | save_live.cpp:399,441,629,753,805 |
| `file_seek` / `file_tell` | **KEEP-ABSTRACT — the retirement is REFUSED, gate settled negative** | `vfs_seek(h,off,whence)` / `vfs_tell(h)` | high (was medium) | see "the seek/tell gate" below |
| `GetResourseFilePtr` | RESHAPE — **EXECUTED (sim side only)** | `asset_read(name,dst,dst_cap)→full length, -1 absent; dst may be null with dst_cap 0 = the size query` — a COPY, closing the measured CROSS-HEAP FREE HAZARD: two sim sites free the returned pointer through the vendored CRT (ai_scr_parse.cpp:49, sim_load_base_layout_dmp.cpp:184; :72 is the GET, not a free). mh.dll's binder does ptr→copy→the game's own free internally. Tact's copy keeps the pointer-returning shape (`tact_frozen`) | high | the header's "never frees" claim is CONTRADICTED at 2 sim sites — confirmed |
| `llm_res_bank_get_file_size` | RESHAPE — **EXECUTED** | `asset_size(name)` — the second param is a register-convention artifact (always the filename pointer twice) | high | ai_scr_parse.cpp:42-43, confirmed at the one and only site |
| `cfg_ReadMapFile` | DEFER-BLOB — **CONTRACT STAMPED 2026-09-10 (SIMABI-MAPIO); NAME DELIBERATELY KEPT at SIMABI-NAMES** | pre-fork unchanged (a REQUIRED synchronous parse; fills the caller's header PAST its declared size — 0x189 high-water of 0x17c, a buffer contract any reimpl must keep); fork-side LIB-WORLD carries `width`/`height` + `current_map_data.tlo_name` (+0x11c) — and NOT the masks or the tlo index, both of which are derived (see the correction below) | high | endgame-plan D-E5 names exactly this pair; field list re-verified at the body and both sites |
| `map_ReadMap_pre` | DEFER-BLOB — **CONTRACT STAMPED, FIELD LIST WAS AN UNDERCOUNT; NAME DELIBERATELY KEPT at SIMABI-NAMES** | same; writes hashed `Planets.tlo_file` (+0x20e) at runtime — geometry, not IO — **but it installs the whole planet**: `map_FillDefaults`'s ~25-region wipe, then the six .MP planes. See below | high | session-begin sites read nothing back — confirmed, both are void with an index in |
| `llm_game_save_player_data` | KEEP-ABSTRACT — **RENAMED `save_player_data` 2026-09-10** | shape unchanged — a PERMANENT save-format obligation, NOT blob-superseded (see the correction below) | high | save_driver.h:140-144 also corrects the ledger: the original fires a status side effect beside its block writes |
| `llm_map_load_regions` | KEEP-ABSTRACT — **RENAMED `map_load_regions` 2026-09-10** | shape unchanged. The codec-reshape ("give me bytes, libmh parses") is BLOCKED pre-fork by graph OWNERSHIP, not file format: the decoded node list hangs off original global 0x0051de7c and untranslated pathfinding walks it directly; both A/B arms call the ORIGINAL today | high | save_live.cpp:475-477, save_driver.h:225-227 |
| `llm_map_save_regions` | KEEP-ABSTRACT — **RENAMED `map_save_regions` 2026-09-10** | shape unchanged, same reason | high | |
| `llm_planet_tlo_load` | KEEP-ABSTRACT — **RENAMED `planet_tlo_load` 2026-09-10** | shape unchanged; the one R3-consumed return in the group — it gates an early exit (save_live.cpp:455) | high | |
| `llm_gfx_load_planet_extra_sprite_banks` | CONVERT — **EXECUTED 2026-09-10 (SIMABI-NOTIFY, §2a)** | `on_invalidate` `INV_PLANET_EXTRA_SPRITE_BANKS` (no payload). Pure presentation since S2; its `notify:false` is the S1 class-string trap uncaught for this row. §0a's direction verbatim: a gfx leaf leaves | high | save_live.cpp:462 — void, unconditional, no readback; its `SPRITE_PIX_OFFSETS` readers are the ones `INV_ALL_SPRITE_BANKS` already measures (the original `load_all_sprite_banks` calls this entry), re-measured for the load boundary: `bldg_sprite_anchor_offset`'s two callers image-wide are boot stage 7 and `turret_fire` inside the sim tick |

**The io reshape is EXECUTED (SIMABI-VFS, 2026-09-10, table 24 → 24, io 8 → 8, overlap 2 → 1,
tact ABI byte-identical).** Sim table version `0xC508FD64` → `0x523B7A40`; the tact table's version
constant and struct are unchanged, and its only diff is the `tact_frozen` reason comment (which the
generator excludes from the version hash by design). **The count did not fall, and that is the
outcome, not a shortfall**: the group's 8 entries became 8 *designed* entries, and the retirement
that would have made it 6 is refused below. Five things the execution learned.

**(1) THE SEEK/TELL RETIREMENT IS UNSOUND, and the gate the scope asked for never had to be run.**
The scope gated the retirement on the mixed A/B promotion arm still working. The premise fails one
level above that: the retirement was to thread the byte count `map_SavePlanetToDisk` had just written
into the container embed, and **the container embeds EVERY VISITED PLANET, not the current one**
(`game_SaveGame`'s plate: "appends each visited/current planet's per-planet save file,
size-prefixed"; `members_write_streaming` walks `member_included` over all of them — a real run
measured **three** members). `save%02d.dat` has **three original writers** — `game_SaveGame`
@0x0044719a, `SwitchToPlanet` @0x0044cece and `llm_strat_try_enter_tactical_mission` @0x0044d38f
(xrefs re-read in the DB) — and the last two are UNTRANSLATED, fire at arbitrary earlier instants,
and in earlier process runs. At most ONE member's length is ever knowable from a writing path; every
other one is a file on disk whose length only the file knows. So the pair is a permanent host
obligation, and the A/B question is moot rather than unanswered. Recorded on the entries themselves,
not only here, because a future reader meets the entry before the register.

**(2) THE GENERATOR COULD NOT EXPRESS A DESIGNED SHAPE AT ALL.** Every entry's name and signature
came from the ledger key plus the committed Ghidra prototype — which is precisely why the mechanical
table mirrored the translation frontier. A row may now carry an `abi` block (name / ret / params, and
an optional `bind` naming a HAND-WRITTEN mh.dll binder that fills the slot instead of the generated
one-line forward). The ledger KEY stays the original callee, so the census, the adjudication ledger
and `hostapi_callers.json` keep resolving; `gen_hostapi_callers` maps the emitted name back through
`abi` before it touches the EN call graph, without which the whole io group would have silently
measured zero direct callers.

**(3) THE `asset_size`/`asset_read` PAIR IS NOT ONE LOOKUP, and assuming it was would have been a
real bug.** `llm_res_bank_get_file_size` @0x004ef24c walks `DAT_0066f464` with a 0x20-byte stride
(size at +0x1c); `GetResourseFilePtr` @0x004cf069 walks `rsr_GetFileEntry`'s 0x40-byte
`rsr_file_entry`. Two different bank systems. So the DMP loader — which needed a length it never
needed before, because it used to walk the host's own buffer — could NOT be given `asset_size`
without changing which lookup answers for a `.DMP`. It gets the size from `asset_read`'s query mode
instead, i.e. from the same entry that supplies the bytes. The binder answers that query with
`rsr_GetFileRealSize` @0x004cf15e, which is **the game's own** "how big is the buffer
GetResourseFilePtr returned": 14 call sites, nearly all of them literally that pair on one name
(`llm_tact_mission_load`, `llm_planet_tlo_load`, `cfg_ReadMapFile`, `llm_ui_text_viewer_open`,
`cfg_Init`, `llm_snd_load_sound_cfg`), several sizing a `mem_realloc` from it.

**(4) A RESHAPE CAN SMUGGLE IN A BEHAVIOUR CHANGE THROUGH ITS RETURN TYPE, and this one nearly did.**
`vfs_write` was first written to return the byte count with 0 for failure. `fwrite` writes zero ITEMS
when the item size is zero, so the original answers 0 for an n==0 write — a FAILURE to every caller,
and the thing `write_extension`'s zero-length guard exists for. A 0-means-failure byte count makes
`bytes == n` true for n == 0 and turns that failure into a success. The entry returns a NEGATIVE
value on failure for this reason alone; caught by reading `savetest`'s `mem_write` stub, whose
comment had already recorded the CRT behaviour.

**(5) THE CROSS-HEAP-FREE RULE IS NOW A GATE, not a sentence.** `gen_libmh_hostapi` fails if any
sim-side `io` entry returns a pointer — the shape that made the hazard possible — scoped to `io`
because other groups legitimately return host static storage libmh only reads
(`llm_build_media_diag_report`, `llm_str_ansi_to_wide_scratch`). Negative case exercised: flipping
`asset_read`'s return to `uint8_t *` fails the generator by name. The complementary grep is clean —
every surviving `utils_free` in a libmh sim module (`ai_scr_parse.cpp:125`,
`sim_load_base_layout_dmp.cpp:184`, `sim_pathfind_route_leg_group_and_sort.cpp:154`) releases a
buffer libmh itself allocated.

**The byte-identity evidence, and why it is not the before/after file diff the item asked for.** A
rig-produced `.sav` is **not reproducible across runs**: two identical command lines on ONE build
gave different files (1182367 vs 1182597 bytes), even with `pin_wallclock=1;fixed_step=1` — the
property SIM1-P clause 10 already records. A
before/after diff of one is therefore vacuous, and worse, the save promotions all ship OFF
(`SHIP_PROMOTE_SAVE`/`LOAD`/`CONTAINER`/`CONTAINER_LOAD` = 0), so an unpinned run does not even
execute the reshaped entries. The intended oracle — the in-call A/B, `[promote] save_verify=1`, which
writes ours, renames it aside and calls the original on the same state — **is broken at HEAD**:
PROCESS-GONE at 233 steps, reproduced identically on a stashed HEAD build, so it is a pre-existing
failure and not this slice's (tracked with the tooling work). What was proven instead, both arms
world-independent:

- With all four save roots promoted, a whole `.sav` written through the reshaped entries: 1,183,818
  bytes, `container promoted=1`, rc=1. `libmh_selftest.exe savetest` over it: **287 blocks, 287 decoded,
  287 re-encoded BYTE-IDENTICALLY, ended at 1183818 of 1183818**; through the reimplemented drivers
  **ROUND-TRIP IDENTICAL**, ver=5, 19 container blocks, 3 members (1,153,109 bytes), tail 234, every
  member IDENTICAL. The member section parsing at all is `vfs_seek`/`vfs_tell` being right.
- The container READ path A/B, which depends only on the input file and not on the world: the same
  `.sav` loaded with `container_load` promoted (ours, through `vfs_read` + `vfs_open(WRITE)` +
  `vfs_write` + `vfs_close`) and unpromoted (retail's own CRT calls) extracts **byte-identical**
  member files — `save01.dat` 314047 / `save02.dat` 454920 / `save03.dat` 384142, all three SHA-256s
  equal across arms. That is also the load-interop half: retail's reader accepts what our writer
  produced.

**The correction that rewrites libmh-abi.md §3's map-io row**: only the two `.MP` boot
parses (`cfg_ReadMapFile`, `map_ReadMap_pre`) are LIB-WORLD-superseded. The five entries
called from the `.sav`/`save%02d.dat` container path are SAVE-format obligations — LIB-WORLD
is a boot fixture, not a save mechanism, and LIFT-TABLE's scope pinned "the SAVE format,
untouched (user, 2026-09-09)". Combining those two recorded facts is the reviewer's
inference, confirmed by the conductor against both sources. **LANDED 2026-09-10 (SIMABI-MAPIO)**
in that row, with both sources cited there.

**The two DEFER-BLOB contracts are STAMPED (SIMABI-MAPIO, 2026-09-10, count 24 → 24, version
`0x523B7A40` unmoved — the ledger reasons are header comments and the generator hashes signatures
and membership, so a contract is by construction not an ABI change; the tact header regenerated
byte-identical and the sim header's diff is comment-only, verified by stripping comments from both
sides).** Re-verifying the register's field list against the bodies was the slice's whole value,
and **it was wrong in both directions**.

**(1) THREE OF THE FOUR NAMED FIELDS ARE NOT BLOB FIELDS AT ALL.** `llm_map_setup_dimensions`
@0x004989c2 derives `general.{width_mask,height_mask,big_width,big_height,bw_mask,bh_mask}` and
`pathfinder_params->width_mask` from `width`/`height` — and **libmh already owns that body**,
translated and shadow-armed (`sim/libtrans/sim_lt_map_setup_dimensions.cpp`), and already re-invokes
it on the load path (`save/save_live.cpp:466`). So the blob carries the two dimensions and the
importer re-derives seven values. For the pathfinder byte it *could not* do otherwise: the storage
is a malloc'd 0x20-byte block reached through `general.pathfinder_params` (+0xc) — heap, not a bound
region, so a "dump every bound region" fixture cannot capture it, and a blob that tried would carry
a stale pointer. The fourth, the **resolved tlo index**, is not this entry's output either: the
parse produces the tileset *name* (`tlo_name` +0x11c) and libmh's own body resolves it through
`cfg_GetTloIndex` (`sim_lt_cfg_planet.cpp:113`), after which S3 sends the index **host-side** inside
`LIBMH_EVK_INV_PLANET_GFX_SETUP`, landing in no region. What both libmh sites actually read back out
of the header is `tlo_name` and nothing else, and both discard the return — the `< 0` check lives
only in the untranslated lobby callers.

**(2) `map_ReadMap_pre`'s FIELD LIST WAS AN UNDERCOUNT BY THE WHOLE MAP.** The register had it
writing hashed `Planets.tlo_file` and nothing more. Read down the chain, it *installs the planet*:
`map_FillDefaults` @0x0045603e bulk-zeroes ~25 per-planet regions (units, buildings, tile_objects,
soldiers, projectiles, order queue, resources, passable, fog_of_war, productions/mines/turrets/labs/
unit_storage, path buffers) **including what its recorded storage_stats/death_anim_table
PRESERVE-BUG leaves behind**; then `map_ReadMap` @0x004a3251 sets width/height, strcpy's the .MP's
tileset name into `Planets[index].tlo_file`, and loads **six planes** — main terrain →
`tile_objects` (HIDX 21, hashed), passable → `passable` **plus `llm_map_build_regions`, which builds
the same nav decomposition `llm_map_load_regions` decodes on the save path**, half-res →
`_G_LLM_MAP_HALFRES_GRID`, the f2 object list → `_G_LLM_MAP_OBJECTS` with chain heads stamped back
into `tile_objects`, the yield plane → `resources` (scaled by `Planets[idx].source_mul`), and the
start spots → `_G_LLM_STRAT_LANDING_SPOTS`. Every one of those is a **bound region**, which is
exactly why D-E5's fixture shape retires this entry and cannot retire the save five.

**(3) THE ENTRY DOES NOT RETIRE ALONE.** `map_ReadMap_pre` internally calls `llm_planet_tlo_load` —
its own table entry, which survives on its save-path caller (`save_live.cpp:463`, the group's one
R3-consumed return) — and `llm_gfx_load_planet_extra_sprite_banks`, which already left the table at
SIMABI-NOTIFY as `INV_PLANET_EXTRA_SPRITE_BANKS`. The original still calls both originals; that is a
fact about the pre-fork binary, not a double-fire in libmh, and it is written at the entry so the
next reader does not have to re-derive it.

**(4) NO RIG ARM IS OWED**, by SIMABI-STRING's rule below: the slice changes what the ledger *says*,
not what libmh *writes* — no non-comment line of generated code moved, so a determinism run would
have compared two identical binaries.

### 2c. string + platform + display (6 entries, reviewed 2026-09-10)

| entry | verdict | target shape | conf | key evidence |
| --- | --- | --- | --- | --- |
| `getAsciiVer` | KEEP-ABSTRACT, STRICT contract — **EXECUTED 2026-09-10 (SIMABI-STRING); RENAMED `wide_to_local_bytes` at SIMABI-NAMES** | `wide_to_local_bytes(wsrc)` — must reproduce Win32 CP_ACP bytes. **LEDGER CORRECTION: this entry feeds hashed state TOO** — its output lands in `own.profile_at(player).name`, inside `strat_players` = `HIDX_STRAT_PLAYERS` (region 12, 14848 B hashed RAW, no mask; verified in mh_regions.gen.h). "utils_wide_to_short_str is THE one" is wrong; there are two. **The hop that hid it is an ORIGINAL**: `llm_strat_player_profile_init` @0x00454985 is what copies the returned bytes into `_G_LLM_STRAT_PLAYERS[player].name` (decompiled at execution — an unrolled-by-2 strcpy at the top of the body), so no libmh site shows the write. Now MEASURED: `simtest` T12 | high | sim_planet_session_begin.cpp:118-132 (`c.get_ascii_ver` → `c.player_profile_init`) → the original @0x00454985 |
| `utils_wide_to_short_str` | KEEP-ABSTRACT, STRICT — **EXECUTED 2026-09-10; RENAMED `wide_to_local` at SIMABI-NAMES** (the register named only `wide_to_local_bytes`, and both codecs run this direction — see the SIMABI-NAMES notes) | shape unchanged — a hash-feeding codec (`players` = HIDX 55, verified by address overlap), but no longer "THE one". Now MEASURED: `simtest` T10 | high | sim_start_tutorial.cpp:125 (`c.wide_to_short_str(..., p1.name)`) |
| `llm_str_ansi_to_wide` | KEEP-ABSTRACT, LOOSENED contract — **EXECUTED 2026-09-10; RENAMED `ansi_to_wide` at SIMABI-NAMES** | `ansi_to_wide(dst,src)` — "any correct local-codepage→UTF-16"; neither site is hashed (scenario name = MF_VIEW; MESSAGE_QUEUE = MF_VIEW\|MF_SAVE, pre-v5 legacy-save upgrade only — re-verified at execution by ADDRESS OVERLAP against every `HASH_REGIONS`/`TACT_HASH_REGIONS` extent, not by citing the flag). **LEDGER CORRECTION: not allocating** — dst is caller-supplied throughout; the "allocation must outlive" wording described buffer persistence, not a heap contract | high | sim_scenario_planet_clone.cpp:97, save_live.cpp:678-682 |
| `llm_str_ansi_to_wide_scratch` | KEEP-ABSTRACT, LOOSENED — **EXECUTED 2026-09-10; RENAMED `ansi_to_wide_scratch` at SIMABI-NAMES** | same, transient shared buffer; every site feeds an already-converted on_event/floating-text path (presentation only), all through `G_TEXT_TMP`, which overlaps no slice in either hash table. **THIS ROW UNDERCOUNTED ITS SITES: four, not two** — the two sim ones below plus `lockstep/rx_dispatch.cpp` (chat speaker + body) and `lockstep/timekeeper.cpp` (player-left alert); `hostapi_callers.json` had them all along and the row was written from the sim pair. The verdict is unchanged — none of the four is hashed — but a LOOSENED contract argued from half the sites was luck, not method. Candidate for a later push-bytes-into-the-event-payload reshape — flagged, not verdict | high | sim_diplomacy_set_relation.cpp:61-68, sim_player_presence_lost.cpp:180/227, rx_dispatch.cpp:276-277, timekeeper.cpp:90 |
| `llm_build_media_diag_report` | KEEP-ABSTRACT — **RENAMED `media_diag_block` 2026-09-10** | shape unchanged. A standalone host returns any fixed 0x400 block (savetest asserts shape, never content — verified in save-format.md); it CANNOT be internalized, because the hosted arm must keep producing retail's real CD-fingerprint bytes (R5) | high | save_live.cpp:692-696; save-format.md:650-659 |
| `llm_view_set_size_mode` | **CONVERT (sim table only)** — **EXECUTED 2026-09-10 (SIMABI-DISPLAY)** | `on_screen` `LIBMH_EVK_SCR_SET_DISPLAY_MODE` (kind 23, `a=size_mode`); libmh stops storing the return and the hosted sink performs that store itself. **LEDGER CORRECTION, the consequential one: the cell is NOT hashed** — `RID_VIEW_SIZE_MODE` is MF_VIEW only, absent from BOTH hash tables (re-verified at execution by ADDRESS OVERLAP against every `HASH_REGIONS` and `TACT_HASH_REGIONS` extent, not by citing this row), and game-modes.md documents it as a persisted per-machine user preference (setup.dat), not sim state. The R3 "consumed into the hashed cell" premise is overclaimed. libmh also cannot honestly compute the value (it is the OS-granted resolution readback — gfx logic §0a keeps out of the core). Tact's frozen copy is untouched (it discards the return at its own site anyway); overlap 3 → 2 | high after the gate (was medium-high) — the live run is below | decomp of 0x0044e47d; sim_start_tutorial.cpp:43-44; tact_mission_end_return_to_strategic.cpp:39-42; mh_regions.gen.h:1519 |

**The CONVERT is EXECUTED (SIMABI-DISPLAY, 2026-09-10, table 25 → 24, overlap 3 → 2, tact header
byte-identical).** Kind `LIBMH_EVK_SCR_SET_DISPLAY_MODE` 23; events version `0x00010014` →
`0x00010015`. **The `display` group is now EMPTY on the sim side** (0 sim / 4 tact). Four things the
execution learned that the register did not say.

**(1) THE GATE WAS VACUOUS AS THE ROW SPECIFIED IT, and the fix was a second arm.** The row asked
for a fixed-return-0 notify. But at the one site the real return is provably 0 already: mode 0 →
`llm_gfx_apply_window_resolution(640,480)` → `llm_gfx_set_window_resolution` snaps `WindowWidth` to
640 (and falls back to 640 on an unsupported mode), so `799 < WindowWidth` is false and the body
returns 0; the early-out arm returns the cache, also 0. A fixed-0 arm therefore could not have
diverged whatever the cell's hashedness. So the gate ran TWICE with
`test_ui.py --ui-abc tutorial_solo`, **18,317 compared steps** each, arm B = all-original
(return-consuming) and arm C = the committed recording: once as the row's fixed-return-0 notify, and
once with libmh storing **2** — a value the entry cannot return here. Both green, 0 mismatches on
every required channel. The second run is the one that carries the claim.

**(2) THE R3 PREMISE WAS WRONG BUT R3b WAS NEVER ASKED, and R3b is where the real work was.** The
row's whole argument is about the RETURN. Converting the entry creates a notify scope, and
`gen_notify_readback` immediately refused it: the closure is 132 functions and re-derives **18**
regions translated libmh reads. A first pass called that closure saturation and was WRONG — re-walked
with `llm_strat_frame_sim_only` cut, **every one of the 18 has a writer inside the uncut subtree**,
because the entry really does tear the display down and rebuild it. The disposition
(`notify_readback_dispositions.json`, `LIBMH_EVK_SCR_SET_DISPLAY_MODE`) answers all five groups at
the site; the load-bearing one is `G_WIN_W`/`G_WIN_H`, written by `llm_gfx_view_metrics_init` inside
the subtree and read AFTER the emit by `land_players_on_planet`, reached from this same body through
`session_begin_multi`. **The standing idempotent-copy precedent had to be REFUSED there**: its own
wording exempts a copy because `WindowWidth` moves only at boot and at explicit resolution-change
entry points — and this scope IS one of those entry points. It is exempt instead on use: both
readers feed the metrics to nothing but `cam_set_col`/`cam_set_row`, and the camera cells are
MF_VIEW|MF_SHADOW|MF_SAVE and in neither hash table. This is the same shape as the two resolution
changes SIMABI-NOTIFY REVERTED, and it survives where they did not for a measured reason: their
emitters read the surface/framebuffer pointers back in the same call, this one's post-emit readers
are the camera and the mode, and every framebuffer/surface reader in the set is `llm_tact_*` while
the sole emitter is strategic.

**(3) THE GENERATOR COULD NOT EXPRESS "CONVERTED SIM-SIDE ONLY", and the split was supposed to.**
§1 says duplication makes a sim-side reshape legal because tact's copy is independent — but
`gen_libmh_hostapi` derives both tables from ONE ledger class per name, so reclassing the row broke
the frozen tact table's membership and its printed reason. This is the first shared entry ever
converted on one side, so the defect had never been reached. Fixed in the generator rather than by
reclassing tact's copy too (which would have re-frozen a tact decision into a sim-side reshape — the
exclusivity defect the split exists to prevent): a row may carry a `tact_frozen` block with its own
`class` and `reason`, and the accessor scan now checks each site against the table ITS accessor
selects, so a sim module reaching a converted entry through `mh::host()` fails by name. The tact
header came out byte-identical, diffed.

**(4) THE SINK PERFORMS THE ORIGINAL CALLER'S STORE, and that is routing, not an extra.** The cell
has untranslated original readers (`llm_ui_outcome_dlg_open`, `llm_ui_menu_close_to_hud`,
`llm_strat_input_update`) and a TRANSLATED tact one (`tact_mission_end_return_to_strategic.cpp:39`),
so in the hosted config it must keep being written at this instant or R5's bit-identity oracle
stops holding. A headless host with no window binds a sink that does nothing there.

**The CP_ACP question (both STRICT codecs)**: vendoring a fixed codec into libmh would fix
the real MP hazard (two peers with different Windows locales hash different bytes — retail's
own behaviour) but would break R5's hosted premise (retail genuinely calls the locale
codec). The precedent is `rng_seed_wallclock_seconds`: an MP-divergence fix is an EXPLICIT,
user-approved, declared divergence taken at its own slice — never a silent vendor.
**SETTLED 2026-09-10 (SIMABI-STRING): both entries stay STRICT pre-fork, and the CP_ACP hazard
is a NAMED DEFERRED DECISION owned by `LIB-FORK`** — the item whose scope already is "what the
fork may change and what the frozen side guarantees", which is exactly the shape of this
question. It **first bites at `LIB-REF`**, whose headless host must either implement the locale
codec or take the divergence, and whose own `done_when` compares its lockstep hash against the
in-binary run — a different codec moves `strat_players`/`players` and fails it. Both items carry
the note; neither the ledger nor libmh vendors anything in the meantime.

**RULED 2026-09-12 (F0-CPACP, user): KEEP CP_ACP.** Both strict codecs stay retail-faithful —
they reproduce the machine's own Win32 CP_ACP bytes, preserving original-vs-ours equivalence on
any single machine; MP carries the documented same-locale requirement (retail's own behaviour).
Nothing is vendored. **The post-fork direction (user, same ruling): move to UTF-8 text entirely
with multilanguage support** — a full text-pipeline modernization under the brokered full-rewrite
program, recorded in the fork plan as a named post-F5 direction and a planned,
declared divergence when it lands (it retires this whole question rather than re-answering it).

**Missing oracle found — and CLOSED at the same slice.** Neither hash-feeding codec had a
positive mutation arm (flip a returned byte → assert the hash slice moves); only the selftest
host's no-op trap existed, which proves an entry is REACHED and says nothing about where its
bytes go. Both arms now live at their own sim sites, in `simtest`, permanent:
`sim_start_tutorial_selftest` **T10** (`utils_wide_to_short_str` → `players`) and
`sim_planet_session_begin_selftest` **T12** (`getAsciiVer` → `strat_players`). Each runs the body
three times — baseline, one flipped byte of the codec's returned buffer, reverted — hashing
through the runtime's own `hash_slice()`/`emit_slice()` with the region rebased onto the
fixture's array, and prints its compared/differing byte counts. Measured: `strat_players`
14848 B compared / 1 differing, `92a638af7e687320` → `29bf3f1a7e687320` → back; `players` 416 B
compared / 1 differing, `cf66c30d593bd99b` → `55da560d593bd99b` → back. **The third run is not
decoration**: without it the arm cannot tell a hashed codec output from a noisy hash.

**The one thing an offline arm has to buy honestly** is `getAsciiVer`'s extra hop. Its bytes
reach `strat_players` through `llm_strat_player_profile_init`, an UNTRANSLATED original, so T12's
mock reproduces that copy rather than assuming it — transcribed from the decompile @0x00454985
(an unrolled-by-2 strcpy that breaks on the terminator in either half, so `strcpy` is byte-exact),
with the >12-character truncate-to-9-plus-ellipsis branch deliberately not modelled and the arm
asserting its names are short enough that the original would not take it either.

### 2d. The rename pass (SIMABI-NAMES, 2026-09-10 — the last slice)

**EXECUTED: sim 24 → 24, version `0x523B7A40` → `0xFD4BC3F8` (ONE constant move), tact 16 /
`0xAE411064` and its header BYTE-IDENTICAL (diffed), overlap 1 → 0.** Fourteen entries took the
designed name the register's target-shape column had been carrying since the plan opened; the
signatures are reproduced verbatim from the pre-slice header, so the ABI descriptor's ONLY change is
the names (and the member ORDER they sort into, which the same single version move covers). The full
map, ledger key → emitted name:

| group | old (ledger key = the original callee) | new |
| --- | --- | --- |
| input | `llm_strat_input_update` | `apply_frame_input` |
| io | (the eight `vfs_*`/`asset_*`, already designed at SIMABI-VFS) | unchanged |
| map-io | `llm_game_save_player_data` / `llm_map_load_regions` / `llm_map_save_regions` / `llm_planet_tlo_load` | `save_player_data` / `map_load_regions` / `map_save_regions` / `planet_tlo_load` |
| map-io | `cfg_ReadMapFile`, `map_ReadMap_pre` | **KEPT** (see (2) below) |
| net | `llm_net_transport_recv` / `llm_net_transport_send` | `transport_recv` / `transport_send` |
| string | `getAsciiVer` / `utils_wide_to_short_str` / `llm_str_ansi_to_wide` / `llm_str_ansi_to_wide_scratch` | `wide_to_local_bytes` / `wide_to_local` / `ansi_to_wide` / `ansi_to_wide_scratch` |
| time | `llm_time_get_ticks_ms` | `ticks_ms` |
| fatal | `utils_abort` | `fatal` (sim only) |
| platform | `llm_build_media_diag_report` | `media_diag_block` |

Four judgment calls the register did not settle, recorded here rather than in a commit message.

**(1) THE REGISTER NAMED ONE `wide_to_local_bytes` AND THERE ARE TWO WIDE→LOCAL CODECS.** §2c gives
`getAsciiVer` the designed name `wide_to_local_bytes` and leaves `utils_wide_to_short_str`'s
target-shape column reading "unchanged", which cannot mean its NAME — the slice's scope renames every
survivor. Taking the register's name for both would have been a member-name COLLISION, so the second
one is `wide_to_local`, cut on the shape a host implementor actually has to distinguish: `wide_to_local`
converts into the CALLER's buffer and returns it, `wide_to_local_bytes` returns the converted bytes in
storage the host owns. That is the same distinction the other direction already spells
`ansi_to_wide` / `ansi_to_wide_scratch`, and the asymmetric suffixes (`_bytes` vs `_scratch`) are the
price of keeping the register's blessed name rather than re-cutting it here.

**(2) THE TWO DEFER-BLOB ROWS DELIBERATELY KEEP THEIR ORIGINAL NAMES, and that is the decision, not an
omission.** A designed name in this table is a promise: *this is a permanent obligation a standalone
host implements from the contract sentence*. `cfg_ReadMapFile` and `map_ReadMap_pre` are the two rows
that explicitly do NOT make that promise — they are pre-fork-only, scheduled to leave the surface when
LIB-WORLD's blob carries the outputs §2b lists. So their original callee names are left standing as the
PRE-FORK STAMP: a reader meeting `cfg_ReadMapFile` beside `map_load_regions` can see which rows the
interface designed and which it is merely still carrying. Written at both entries too, not only here.
Consequence worth naming: the sim table is 22 designed names + 2 stamps, which is where §3's
"predicted end state" number and the printed count differ in kind rather than in count.

**(3) THE OVERLAP WENT 1 → 0, and no share was lost.** `utils_abort` is the last entry both tables
held. Renaming the sim side to `fatal` while tact's frozen copy keeps `utils_abort` means the two
tables no longer share a MEMBER NAME — `gen_libmh_hostapi` now prints `in both tables: none` and the
union 39 → 40 — but both still bind the same original body in the hosted configuration. Same mechanism
as `GetResourseFilePtr`/`asset_read` at SIMABI-VFS: a `tact_frozen` block with `"abi": null`, carrying
its own copy of the reason text so the tact header's comment does not move either. The generator's
per-accessor check is what keeps this honest — a sim module reaching `mh::host().utils_abort` now fails
BY NAME, which is exactly how the 25 call sites were found.

**(4) NO RIG ARM IS OWED, by SIMABI-STRING's rule, and the negative arm is the one that matters.** The
slice changes no libmh behaviour: every renamed member forwards to the same `mh::call::` thunk and the
generated binder diff is names only. What it DOES change is the handshake, so that is what was
exercised: a host built against the pre-rename constant `0x523B7A40` is REFUSED —
`libmh_set_host_api rc=-1 (expect -1), current 0xFD4BC3F8`, with the binding intact afterwards (run as
a temporary `hostapitest` arm against the old literal and then reverted; the standing arms are
`LIBMH_HOST_API_VERSION ± 1`, which do not rot). The trap-by-name arm moved with its entry and now
reads `REQUIRED entry ticks_ms called with no host impl`.

## 3. Slices (the tracker items this plan proposes)

Ordering principle: cheapest, least-contested surface reductions first; the decisions that
need design or a gate (vfs, seek/tell, display, codec contracts) as their own items;
naming/reshape LAST as one deliberate version bump, never bundled with membership changes
(each slice = its own version-constant move, so the negative-arm history stays legible —
the S1–S8 model).

| slice | contents | surface delta |
| --- | --- | --- |
| SIMABI-HOOKS | the 6 hook DELETEs — **DONE 2026-09-10** | 37 → 31 ✅ |
| SIMABI-NOTIFY | the 4 settled CONVERTs: `SCR_STRAT_FRAME_PRESENT`, `SCR_STRAT_FRAME_REDRAW`, `INV_PLAYER_COLOR_LUT`, `INV_PLANET_EXTRA_SPRITE_BANKS` — **DONE 2026-09-10** | 31 → 27 ✅ |
| SIMABI-CHAT | the 2 INTERNALIZEs (normal reimpl-loop discipline: translate, verify, promote; dual-writer R5 story — untranslated UI keeps its own path into shared region-registered cells; ops idempotent) — **DONE 2026-09-10** | 27 → 25 ✅ |
| SIMABI-DISPLAY | `view_set_size_mode` CONVERT, gated on its live hash-mutation run (§2c) — **DONE 2026-09-10** | 25 → 24, overlap 3 → 2 ✅ |
| SIMABI-VFS | the io reshape: `vfs_*` 6 + `asset_size`/`asset_read` 2 (closing the cross-heap free hazard); the seek/tell retirement REFUSED on evidence (§2b) — **DONE 2026-09-10** | 24 → 24, io 8 designed, overlap 2 → 1 ✅ |
| SIMABI-STRING | contract work, count 0: strict-vs-loosened contract text per §2c; the CP_ACP hazard recorded as a named deferred fork decision (owner: `LIB-FORK`); ADD the missing positive hash-mutation arms for both strict codecs — **DONE 2026-09-10** | 24 → 24, version `0x523B7A40` unmoved ✅ |
| SIMABI-MAPIO | count 0 pre-fork: stamp the DEFER-BLOB contracts (what LIB-WORLD must carry) on the two boot parses; correct libmh-abi.md §3's map-io row per §2b's save-container correction — **DONE 2026-09-10** | 24 → 24, version `0x523B7A40` unmoved ✅ |
| SIMABI-NAMES | the abstract renaming pass over every survivor (`ticks_ms`, `transport_send/recv`, `fatal`, `apply_frame_input`, the vfs/asset names, …) — one version bump, no membership change — **DONE 2026-09-10** | 24 → 24, version `0x523B7A40` → `0xFD4BC3F8`, overlap 1 → 0 ✅ |

**END STATE, REACHED 2026-09-10 (all eight slices done): 24 entries**, sim table version
`0xFD4BC3F8`, printed by `python tools/gen_libmh_hostapi.py` (24 was 22 in the original prediction,
until SIMABI-VFS measured the seek/tell retirement and refused it). Every one carries a contract a
headless host implements from the sentence alone, and 22 of the 24 carry a DESIGNED NAME; the two
exceptions are the DEFER-BLOB boot parses, whose original names are kept deliberately as the
pre-fork stamp (§2d (2)). The final roster, in struct order:

| group | n | entries |
| --- | --- | --- |
| input | 1 | `apply_frame_input` |
| io | 8 | `asset_read`, `asset_size`, `vfs_close`, `vfs_open`, `vfs_read`, `vfs_seek`, `vfs_tell`, `vfs_write` |
| map-io | 6 | `cfg_ReadMapFile`†, `map_ReadMap_pre`†, `map_load_regions`, `map_save_regions`, `planet_tlo_load`, `save_player_data` |
| net | 2 | `transport_recv`, `transport_send` |
| string | 4 | `ansi_to_wide`, `ansi_to_wide_scratch`, `wide_to_local`, `wide_to_local_bytes` |
| time | 1 | `ticks_ms` |
| fatal | 1 | `fatal` |
| platform | 1 | `media_diag_block` |

† pre-fork-only (DEFER-BLOB), name kept as the stamp. **Overlap with tact: 0** — it was 3 at the
split, 2 after SIMABI-DISPLAY, 1 after SIMABI-VFS (`GetResourseFilePtr` vs `asset_read`), and 0 after
SIMABI-NAMES (`utils_abort` vs `fatal`). Both remaining pairs still bind the same original body in
the hosted configuration; a table that no longer shares a member name with tact's is duplication
working exactly as designed, not a lost share.

Each slice's uniform oracle: `gen_libmh_hostapi` regenerated with the count printed
before/after; both drift gates green; hostapitest arms updated in the same slice (a
converted entry's record gets a kind check; a deleted entry's absence is the generator's
business); `run_selftests` + the UI suite green; a determinism run for SIMABI-CHAT,
SIMABI-DISPLAY and SIMABI-STRING (they touch hashed state or its writers); and a mutation
per new mechanism. Per §0's epistemics clause every slice re-verifies its own rows against
the code at execution — the register above is the reviewed hypothesis, not a license.

**SIMABI-STRING's rig clause was DISCHARGED OFFLINE, deliberately, and the reason generalises.**
The slice changed no shipped behaviour: the ledger reasons are header COMMENTS, the generated
version constant did not move, and libmh's four codec call sites are byte-identical — so a
determinism run would have compared two identical binaries and proved nothing about the claim the
slice actually makes, which is *where a codec's output bytes go*. A rig arm can only show that; it
cannot localise it. The two `simtest` arms hash the NAMED slice with one codec byte flipped and
again with it restored, which is the same evidence with the ambiguity removed and at ~0 rig cost.
Rule for the remaining count-zero slices: a determinism run is owed when a slice CHANGES what
libmh writes, not when it changes what the ledger says about it.

## 4. What this plan does NOT do

- Touch anything tact-only, or tact's copies of the 3 shared entries (the split's whole point).
- Add a public `libmh_tact_*` entry point (LIB-REF territory, as libmh-abi.md §3 records).
- Force "host owns the frame loop" — the frame-present CONVERTs keep synchronous dispatch in
  the hosted config; the restructure stays fork/D1 (the tact precedent's wording).
- Decide the vfs and codec questions by rule — both are measured decisions for their slices,
  taken to the user with the evidence (§2b/§2c).
