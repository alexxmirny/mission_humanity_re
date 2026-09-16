# The libmh host ABI — architecture rules and cut decisions (LIB-ABI-LIFT)

**STATUS: APPROVED (user, 2026-09-03).** R1–R6 approved as drafted; R7–R10 carry the same
session's decisions (R7 poll-queue-generalized-to-callback and R10 input-pushed were the
user's own calls). **R9's modal decision was TAKEN 2026-09-03 at LIFT-SCREEN, on the
measurement it was deferred for — CONVERT** (the rule carries the numbers). **LIFT-TACT's
two deferred shape decisions were TAKEN 2026-09-03 on their measurements — tact_frame
CONVERT IN PLACE (8 of 9; `render_view` exempt) and mission load/parse KEEP IN LIBMH** —
and the same review added **R3b** and the tact/core table split (§3). The epistemics clause
below still governs: every application of a rule to a distinct task gets its own sanity
check. As of the LIB-ABI-LIFT close (2026-09-10) no undecided marker remains in §3 —
every once-deferred row carries its recorded resolution (the sel-panel icon-pointer array
dissolved host-side at LIFT-TACT slice A; the tact/core split row carries the 2026-09-09
reversal and the 2026-09-10 duplication correction, executed as LIB-IFACE-SPLIT §7).

**R3b is what the epistemics clause is for.** The plan listed `llm_tact_render_view` among
the tact-frame conversions; the per-task sanity check found it writes hashed sim state and
that `tact_frame` reads its output nine times later in the same frame. No rule in R1–R10
would have stopped that conversion. Mechanical rule application would have shipped it.

Companion to the libmh pivot (the direction) and to the endgame plan's D-E3/D-E6 stages (the
sequencing). The measured evidence behind
every decision here is `tools/data/hostapi_callers.json` — regenerate with
`python tools/gen_hostapi_callers.py` (per-entry direct/transitive translated callers +
source sites; rerun after each lift to watch the surface shrink).

## 0. Plan epistemics (user, 2026-09-03 — governs this whole document)

This plan is a **working hypothesis, not established truth**. Stages L0–L2 (LIFT-DOC /
LIFT-EVQ / LIFT-NOTIFY / LIFT-SCREEN) are approved as scoped; L3+ (LIFT-RESID, LIFT-TACT,
LIFT-TABLE) are directionally agreed and **must be re-reviewed when reached** — we did not
hold the whole context when they were drafted. Every application of a rule below to a
distinct task gets its **own sanity check against the actual code**; this concerns
**especially any classification of an entry as REQUIRED or of work as out-of-scope** —
those are per-task decisions with evidence, never mechanical rule application.

### 0a. The direction of travel (user, 2026-09-09)

Stated when the LIFT-TABLE re-review drifted into edge-removal, and governing every decision
below it:

> **Not just "remove edges" — move host-related code to the host.** Graphics data and graphics
> code are not sim. **The sim-facing interface must be clear of gfx calls.**

Two consequences the re-review had gotten backwards. (a) An entry is not disposed of by
*internalizing* its arithmetic into libmh — that moves a display concern INTO the deterministic
core to save a table row. R1 already says a census hit is the LOWER BOUND for a cut; the first
question is always which node ABOVE the leaf is the cut, and for a gfx leaf the answer is usually
that its caller is presentation and leaves too. (b) Data reached only by renderers does not belong
in the determinism hash merely because it shares a region with sim fields — the region is the unit
of address bookkeeping, not of adjudication.

Not yet promoted to a numbered rule; it reads as R1+R2's intent stated plainly, and the numbered
set was approved as a whole at LIFT-DOC.

## 1. Why the lift exists

The mechanical 114-entry table (LIB-ABI stages A–D) sits at the **translation frontier** —
an entry exists wherever migration happened to stop, not where a host contract belongs.
Measured (2026-09-03): **74 of 114 entries have exactly one direct translated caller**
(single-TU private dependencies); most render-notify entries are the original's internal
render API a Godot host cannot meaningfully implement; meanwhile `llm_snd_play` fans in
from 25 direct / 184 transitive callers across 4 domains. The lift moves the boundary to
designed cut points. **The mechanical table is not discarded** — it survives as mh.dll's
internal routing layer while the libmh-facing surface shrinks.

## 2. The rules

- **R1 — cut point.** A callback sits at the HIGHEST node where everything beneath it
  writes no sim state after hoisting and the semantics are host-meaningful. A census hit
  is the LOWER BOUND for the cut, never automatically the cut.
- **R2 — state-in / visuals-out, subtree form.** Every sim write inside a host-owned
  subtree is hoisted into libmh above the cut; hosted double-execution is verified
  idempotent per lift.
- **R3 — notify entries are void, fire-and-forget.** No sim-consumed return. An entry
  whose return the sim reads is required-class, or is refactored so libmh computes the
  value itself.
- **R3b — no same-frame side-effect readback across a notify channel.** R3 covers a
  consumed *return*. This covers the other half, found at LIFT-TACT (2026-09-03): an entry
  that returns nothing but whose **writes libmh reads back in the same frame**. R7 gives a
  NULL-callback host frame-edge drainage, so libmh would read the *previous* frame's value —
  a real divergence, not a cosmetic one. Such an entry is **required-class**, or the
  readback is refactored so libmh owns the value. The check is per entry: what does its
  subtree write, and does any translated body read that before the frame ends?
  **Found instance: `llm_tact_render_view`** (see §4) — the rule exists because the plan
  had already listed it for conversion and nothing in R1–R10 would have caught it.
- **R3c — a notify record may not be parameterised by state libmh then mutates.** R3 covers a
  consumed *return*; R3b covers a *readback*. This covers the third shape, found at LIFT-R3B
  (2026-09-09): a record that names shared mutable state rather than carrying its values, where
  the emitter changes that state before the host can consume it. `cam_jump_queue_clear` asked the
  host to drain a queue and `sim_fx_debris_burst` then refilled it, so a frame-edge drain would
  consume the *new* queue — a different action, not a stale value. Either the record carries the
  values (R4), or **libmh performs the state half itself** and the record becomes pure
  presentation. Note this one is invisible to the R3b gate by construction: the gate asks what
  libmh READS BACK, and here libmh reads nothing.

- **R4 — the host reads state; libmh doesn't push it.** Callbacks carry identities,
  deltas, positions — not data snapshots. SB-BIND gives the host the state view (the
  Godot per-frame binding shape).
- **R5 — the hosted config is the oracle.** mh.dll routes each abstract entry onto the
  original thunks; determinism + UI suite bit-identical before/after every lift.
- **R6 — frontier adjudication asks "where is the cut"**, not just "what is the class".
  A new outward callee may belong inside an existing abstract entry's subtree.
- **R7 — three notify channels**, typed records, void, queue-backed:
  `on_event` (game events: text messages, sounds, alerts, progress),
  `on_screen` (screen/modal orchestration: open/dismiss requests),
  `on_invalidate` (dirty marks: scope id + optional index).
  Mechanism (user decision 2026-09-03): libmh emits into an internal ring. Hosted config
  binds the callbacks and mh.dll dispatches **synchronously at emit** onto the original
  fine thunks — the original effect happens at the original instant, keeping R5 exact. A
  NULL-callback host accumulates and drains `libmh_poll_events()` at frame edge. Same
  emit sites, two consumptions.
- **R8 — determinism direction.** A required entry may feed only boot/asset bytes and
  pacing. Nothing a host returns may reach hashed sim state. All per-tick interaction is
  void events out or host-pulled views.
- **R9 — modal-with-result. DECIDED 2026-09-03 (user): CONVERT, both entries.** The class
  was expected to hold two genuine host interactions that would have to stay REQUIRED
  pre-fork. Measured at LIFT-SCREEN, it holds **one**, and that one's async answer path
  **already exists in the original**, so the volume that was supposed to decide this is
  near-zero:
  - `llm_net_lockstep_sync_overlay_show` (`0x004c7eea`, the "Player not responding" kick
    modal) **does not block**. It is `arm_screen(); return poll(OVERLAY_RESULT)`.
    `_G_LLM_NET_LOCKSTEP_OVERLAY_RESULT` (`0x0065679a`) has **5 xrefs total**: the only
    writer carrying an answer is the **5-instruction widget button callback `0x004c7bd4`**
    (`OVERLAY_RESULT = WAIT_PLAYER_IDX`, reached as DATA from the LOCKSTEP_SYNC widget
    list); the only reader is the entry itself, which samples-then-clears. Conversion cost
    is therefore **one `on_screen` OPEN record + making OVERLAY_RESULT libmh-owned state
    that the host writes through a pushed input** (R10's shape) — not a redesign.
    **Ordering law: read → clear → emit**, so under the hosted arm the original re-reads
    the same value and its own clear is the idempotent double-write (the stage-E doctrine).
  - `llm_ui_outcome_dialog` is **not modal-with-result at all**. Its epilogue is
    `MOV EAX,[EBP-0x1c]` — **read at the time as the Watcom uncommitted-return shape, which was wrong (corrected 2026-09-09 by PROTO-RETSCAN's epilogue scan): that slot is written `= 1` twice, at 0x004c6cfc and 0x004c6fdb, the second immediately before the load, so the function really does `return 1`.** The ledger's fake return is a DECOMPILER artifact on an uncommitted-convention function, not an instruction pattern, and citing it for a byte sequence conflated the two. **The R9 verdict is unaffected, because it never rested on that half** —
    the consumer chain is dead, which the scan now confirms mechanically (`consumed=False`: no caller reads EAX after the call): three of its four callers discard the value, and
    `llm_strat_player_presence_lost`, which tail-forwards it, is called from **21 sites
    that all ignore the value**. It converts as a plain void `on_screen` record.

  The fork-side shape stated when this rule was drafted ("libmh emits the screen request;
  the host submits the answer later as an input/order") is thus adopted **pre-fork** as
  well; no entry in this class stays REQUIRED.
- **R10 — input is pushed, never pulled** (generalizes the `llm_strat_input_update`
  wall). The host owns the input pump; the key/mouse entries the tact frame consumes stay
  required pre-fork; no polled-state conversion (it would change the
  at-most-one-hotkey-per-frame semantics).

## 3. Cluster cut-decision register

Every host-callback cluster carries a decision or an explicit deferred-to-stage marker.
"Evidence" = `tools/data/hostapi_callers.json` + the 2026-09-03 digs (day log).

| cluster | entries (approx) | decision | stage |
| --- | --- | --- | --- |
| Sound (snd_play, ambient, fx, race alert, zone, stop_all) | 9 | → `on_event`; `offscreen_snd_volume` DELETED — events carry position, host attenuates from its camera | LIFT-NOTIFY |
| Text/game messages (PrintTextMessage, print_queue_text_id, floating msgs, game speed, race-alert text, progress notify) | ~8 | → `on_event`; `offscreen_fx_scale` deleted the same way | LIFT-NOTIFY |
| Dirty marks (view tiles, vis-map fills, viewport, sidebar/roster/slot refresh family) | ~12 | → `on_invalidate(scope, index)` — measured: all void, no readback anywhere | LIFT-NOTIFY |
| Notify tails (zoom scale, player color, cam col/row, message-queue clears, jump-queue/placement clears) | ~8 | → `on_event` / `on_invalidate` | LIFT-NOTIFY |
| Screens/modals (stage-E visual remainders: lockstep overlay family, outcome dialog, bldg panel; dlg_build_from_table path, planet_select_screen_open) | **9** (measured) | **DONE 2026-09-03** — all nine are `on_screen`, table 87 → 78. R9 decided CONVERT (see the rule). `llm_net_lockstep_extend_ui_enter` was ADDED to the cluster (user, 2026-09-03) — the stage-E pass missed it because it hangs off `sim_order_dispatch`'s calls struct, but it is the same split shape as the four it did take. `llm_ui_dlg_build_from_table` crosses as a table **id** (`LIBMH_SCR_DLGT_*`), not the raw VA `0x00656d6a` — R4 | LIFT-SCREEN |
| Tutorial choreography (fade spins, widget layout, presents, cursor, menu restore) | **11** (measured 2026-09-09) | **DONE in two slices, 2026-09-09 — table 78 → 68.** Slice 2 took `llm_tutorial_step_driver`'s seven (3 left immediately, being its exclusive callees); slice 3 took `llm_game_start_tutorial`'s and released the four they shared. Two conversions are not signature-identical and carry the interesting content: `llm_ui_widget_list_center`/`_draw` cross as a `LIBMH_SCR_WGTL_*` **id** rather than a widget-list address (R4, the `dlg_build_from_table` reshape), and the fade **spin** — `while (tick() == 0)`, the one pattern the ABI actually forbids — became `LIBMH_EVK_SCR_FADE_TRANSITION_RUN`: libmh arms the transition and asks once, the host owns the frame loop and therefore the waiting. `llm_gfx_font_desc_for_flags`, `sprite_width` and `ui_sprite_get_header_field2` were absorbed into `LIBMH_EVK_SCR_TUT_HINT_LAYOUT` (see §4). **The drafted "resumable 3-phase state machine" was NOT built and was not needed** — it assumed a per-frame invoker that does not exist: `start_tutorial`'s only reference in the binary is a DATA store at `0x0064fe23`, the main-menu TUTORIAL widget's action field, so it is a one-shot click handler. The hashed work (the `player_desc` setup and `session_begin_multi`) still runs between the two fade requests in the original order; only the waiting moved. **One entry is KEPT REQUIRED: `utils_wide_to_short_str`** — see §4. The working assumption here and in the misfit row ("str conversions → internalize") did not survive reading it: the codec is `WideCharToMultiByte(CP_ACP, …)`, a locale-dependent Win32 service, and its one libmh site writes the hashed `player_desc[1].name`. So the cluster closes at **10 of 11 converted**, with the eleventh carrying a recorded reason rather than a conversion | LIFT-RESID |
| ~~Session trio (`session_begin_multi`, `planet_session_begin`, `planet_map_session_init`)~~ | **0** | **ALREADY SATISFIED — no work, measured 2026-09-09.** The provisional LIFT-RESID scope named this half; it is done. `session_begin_multi`'s message-queue clear is already `mh::state::evt::msg_queue_clear_all` (LIFT-NOTIFY) and its abort-to-menu `llm_menu_force_return_to_main` is an `MH_LIBMH_BIND` rebind, not a host entry (LIFT-SCREEN took its dialog path). The trio's remaining host calls all belong elsewhere: `map_ReadMap_pre` (map-io, KEPT), `getAsciiVer` + `rng_seed_wallclock_seconds` (misfits, LIFT-TABLE), `llm_gfx_pack_rgb16` (survives on 4 tact callers) | — |
| tact frame draw calls (render_view, overlays, cursor/present, text, screenshot) | **9** (measured) | **DECIDED 2026-09-03 (user): CONVERT IN PLACE, 8 of 9.** The deeper input/sim/render split was measured and REJECTED: it removes the *same* entries (the 8 draws leave either way; the 4 input entries stay required under R10 either way), so it buys **zero surface delta** while restructuring the one body that is promoted and cannot be shadow-armed (`tact_frame.cpp:491-503` — its only oracle is the A/B per-frame hash trajectory). The body also genuinely interleaves: `render_view` fires at step 15 *before* steps 16-24 mutate selection/orders, and `selection_panel_refresh` fires 5× mid-body. "Host owns the frame loop" remains a fork/D1 goal, not an ABI-surface one. **`llm_tact_render_view` is EXEMPT — stays REQUIRED per R3b** (see §4). `drag_box_clamp` → host-derived: its only accumulator is a dashed-outline phase byte, so the host animates the marching-ants from the coordinates alone. **RE-MEASURED AT RESUME 2026-09-09 (slice B):** `tact_frame` has **13** direct entries, not the 12 the open counted, and they partition exactly as the decision predicted — 4 input REQUIRED (R10: `key_dequeue`, `key_queue_empty`, `mouse_buttons_get`, `mouse_delta_pump`), `render_view` REQUIRED (R3b), and the **8 that convert**: `gfx_LoadSprite`, `gfx_draw_text_rgb`, `blink_overlay_clear`, `drag_box_clamp`, `save_screenshot`, `scroll_fade_step`, `tile_overlay_refresh`, `frame_cursor_and_reset`. **DONE 2026-09-09, table 61 -> 53 — all EIGHT, not seven.** `frame_cursor_and_reset` was expected to wait for slice B2 because `mission_start` shares it; converting that ONE site with slice B was a two-line change and the entry left with the rest, so the coupling the plan recorded turned out to cost nothing to break. Two of the eight are not plain one-for-one records. **The mine-blast wipe COLLAPSED**: the original runs a fixed sixteen iterations of (`scroll_fade_step`, `blink_overlay_clear`, `frame_cursor_and_reset`) and nothing of libmh's happens between them, so it became ONE `LIBMH_EVK_SCR_TACT_BLAST_TRANSITION` and **absorbed `scroll_fade_step` entirely** — that loop was the entry's only site, which is also how the one scope the R3b gate cannot measure (its framebuffer writes are pointer-mediated) avoided needing a BLIND disposition. And `draw_text_rgb` went to the **TEXT surface** rather than a record: its one site draws the exit-confirm prompt at a literal (0x96, 0xf0) in literal white, so only the composed string crosses. `drag_box_clamp` is **the one record on the whole notify surface carrying raw coordinates** — the rectangle IS the state, there is no coarser identity, and R4 admits positions. The two present scopes carry R3b dispositions rather than silence: `FRAMEBUFFER` and `G_WIN_W` are `justified-exempt`, and the one site where the terminal argument does NOT hold (`mission_start` reads `gfx_framebuffer` at :151 after emitting at :70) is named in the ledger instead of glossed | LIFT-TACT |
| tact panel/sidebar + unit lifecycle (set_draw_surface, text_draw_rgb16, pack_rgb16, font_select, sel_panel_draw, sidebar_row_draw_left, char_panel_ammo_draw, unit_draw_hp_bar_slot, char_panel_row_refresh) | **10 bodies / 1704 lines** (measured; + 3 R2-relevant siblings with no host calls = 13 files / 2076 lines, matching the plan's "~13 fns / ~2 KLOC" though not its five named examples) | **DECIDED 2026-09-03 (user): one `on_invalidate` scope id per body**, mh.dll's sink calling the fine draw thunks internally — the shape every body in the cluster already ends with (`evt::inv_tact_*`). These are arbitrary-blit primitives and do NOT map onto R7's typed-record model per call. **8 entries leave, 4 survive on non-tact callers.** No GATING body exists in this cluster (measured): every draw entry is void-returning or feeds its value straight into the next argument. `ui_sel_panel_init` converts LAST, gated on the icon-pointer decision in §4. **RE-MEASURED AT RESUME 2026-09-09 (slice A), and both counts moved:** 3 bodies converted at slices 1-2, leaving **7 bodies / 1466 lines** (not the 1366 recorded) — `sidebar_dispatch` 203, `ui_char_panel_row_draw` 271, `ui_order_buttons_minimap_tick` 181, `ui_sel_panel_multi_mode_tick` 242, `ui_sel_panel_single_mode_tick` 215, `ui_sidebar_row_draw_right` 218, `ui_sel_panel_init` 136. The payoff is **7 entries, not 8** — the eighth was `char_panel_row_refresh`, already gone at slice 1 — namely `ui_set_draw_surface`, `ui_text_draw_rgb16`, `tact_ui_sel_panel_draw`, `gfx_font_select`, `char_panel_ammo_draw`, `sidebar_row_draw_left`, `unit_draw_hp_bar_slot`: **68 → 61**. And **3 survive, not 4**: `gfx_pack_rgb16` (`planet_map_session_init`), `GetResourseFilePtr` (`ai_scr_parse` + `load_base_layout_dmp` + `mission_load`), `gfx_convert_pixels_565_to_555` (`mission_load`). Converted in **two batches** (user, 2026-09-09): the six readers, then `ui_sel_panel_init` alone once their leaving has dissolved the icon-pointer question. **SLICE A CLOSED ITS FIRST HALF 2026-09-09, table 68 -> 65.** The four in-place bodies converted at `b4244343`; then the two PURE-PRESENTATION bodies left libmh WHOLE -- `llm_tact_ui_char_panel_row_draw` (deleted, with its promotion and its offline suite) and `llm_tact_ui_sidebar_row_draw_right` (same) -- because their measured write set is **tile_vis_map dirty bytes and nothing else**: not a `TACT_HASH_REGIONS` member, no translated reader, so R2 had nothing to hoist. Two new kinds carry them: `INV_TACT_CHAR_PANEL_ROW_DRAW(a=unit_idx, b=row_slot)` and `INV_TACT_SIDEBAR_ROW_HOVER(a=side, b=unit_id, c=row)`. The second is **one kind for both sides** -- `row_draw_left` was a host entry and `row_draw_right` was ours only by translation order, and `highlight_flag` does not cross because both sites pass the literal 1. Three entries left with them (`char_panel_ammo_draw`, `unit_draw_hp_bar_slot`, `sidebar_row_draw_left`); the remaining four leave with `ui_sel_panel_init`. The R3b gate measured both new scopes independently (14 and 10 reachable functions, 22 and 23 write symbols) and returned **clean**, which is the mechanical half of the same claim. One thing got BETTER rather than narrower: `tact_sidebar_dispatch_selftest.cpp`'s two `hit_code >= 0x64` arms were untestable on their positive side while the callee was a sibling migration member binding real game VAs, and are now covered (T18/T19). **SLICE A CLOSED 2026-09-09, table 68 -> 61 — the full 7-entry payoff.** `ui_sel_panel_init` was the last and it SPLIT rather than moving whole, because it is the one body in the cluster that is not pure presentation: the head (icon-bank load + 565->555 conversion, three background panels, both font selections, both labels) crosses as `INV_TACT_SEL_PANEL_INIT`, while `ui_sel_panel_multi_mode = 0` and the refresh/redraw/clear tail stay in libmh — the latch is read every tick and the tail re-enters libmh's own converted bodies. The four remaining entries left with it: `set_draw_surface`, `text_draw_rgb16`, `tact_ui_sel_panel_draw` (which already had kind 13 — the fine entry survived only on two direct binders) and `gfx_font_select`. The three that survive are the ones predicted: `gfx_pack_rgb16` (sim_resid), `GetResourseFilePtr` (ai + sim + mission_load), `gfx_convert_pixels_565_to_555` (mission_load) | LIFT-TACT |
| tact mission start/end one-shots (mission_start's resolution/sprite-bank/view-metrics setup; mission_end_return_to_strategic's teardown) | **11** (measured 2026-09-09) | **THE ROW THE OPEN MEASUREMENT NEVER GOT, added 2026-09-09 (user).** The 2026-09-03 open enumerated FIVE tact clusters — `tact_frame` 12, panel/sidebar 9, **mission start/end one-shots 10**, mission load/parse 6, sound 1 — and this register carried a decision for four of them. Without a row these would have fallen through to LIFT-TABLE undecided, and they do **not** belong under the mission clause below: that clause is about **load/parse** and its io calls as required data services, whereas these are mode/presentation entries. Live set: `mission_start` (201 lines) calls `gfx_apply_window_resolution`, `gfx_sprite_pix_offsets_init`, `gfx_view_metrics_init`, `tact_frame_cursor_and_reset`, `tact_gfx_load_banks_alt`, `tact_view_metrics_init`, `ui_info_media_draw_p1`; `mission_end_return_to_strategic` (91 lines) calls `gfx_apply_resolution_change`, `gfx_load_all_sprite_banks`, `tact_gfx_view_tile_rows_init`, `view_set_size_mode`. **Taken into LIFT-TACT as slice B2**, its decision to be made on its own measurement at the slice — per §0 every entry gets a per-entry recorded reason, not a rule cite. Note `view_set_size_mode` also has `llm_game_start_tutorial` as a caller and `frame_cursor_and_reset` is shared with `tact_frame`, so B2's payoff is coupled to B's. **DECIDED AND EXECUTED 2026-09-09, table 53 -> 46. The cluster is TWELVE entries, not eleven** — `utils_abort` is a live `mh::host()` member at `tact_mission_start.cpp:111` and the open's count missed it (`hostapi_callers.json` reports `n_direct: 0` for it because `utils_abort` is not a node in the trimmed call graph at all, so the census is silently wrong for that one row; its `site_modules` field, which is a source scan, has it right at 9 TUs). **Eight convert, four stay REQUIRED, and every one of the twelve carries its own reason in `libmh_call_ledger.json` rather than a rule cite.** Converted: `ui_info_media_draw_p1`, `tact_frame_cursor_and_reset` (with slice B — the coupling above cost two lines to break, so it did not wait), `tact_gfx_load_banks_alt`, `gfx_view_metrics_init`, `tact_view_metrics_init`, `gfx_sprite_pix_offsets_init`, `gfx_load_all_sprite_banks`, `tact_gfx_view_tile_rows_init`. **Required: `utils_abort`** (9 TUs, `notify: false`, process termination — LIFT-TABLE's `fatal` group); **`view_set_size_mode`** (R3 — its return IS consumed, at `sim_start_tutorial.cpp:44`, even though this cluster's own site discards it); and **the two resolution changes, which were converted and then REVERTED on the R3b gate's measurement**. That reversal is the epistemics clause working: `gfx_apply_window_resolution`'s closure is 81 functions and `gfx_apply_resolution_change`'s is 107, they re-derive fifteen and sixteen regions translated libmh reads, and each emitter reads its own output back **in the same call** — `mission_start`'s 480-row intro blit takes `gfx_draw_surface` as source and `gfx_framebuffer` as destination at `:150-151`, and `mission_end` calls `map_LoadPlanetFromDisk` on the line after, which reaches the map-region-pool readers. A host reconfiguring the display that libmh then depends on is not a notification. The eight that did convert pass the same two checks the two failed: every reader is reached only from a later frame (`mission_start` ← `strat_try_enter_tactical_mission` only; the readers ← `tact_frame`/`strat_frame` only), and the emitting body reads none of its own output afterwards | LIFT-TACT |
| tact mission load/parse (GetResourseFilePtr/RealSize tact arm, fatal-abort callers, tlo palette/shade/565) | ~6 | **DECIDED 2026-09-03 (user): KEEP IN LIBMH**, io calls classified as required data services. The working assumption (ownership → host, symmetric with cfg and map load) was sanity-checked at execution and the numbers did not support it: it removes **~4 entries, not 6** — `GetResourseFilePtr` survives on ai/sim callers and `llm_fatal_cleanup` survives on `tact_door`/`tact_unit_spawn`, which are not parsers. Against that: re-classing **1384 already-translated, verified lines** (`mission_load` 898 + `mission_parse` 486) out of libmh, and a standalone libmh that cannot start a mission without a LIB-WORLD tact blob. The parser is NOT restructured under either option | LIFT-TACT |
| tact/core host-table split (the interface isolation) | **35 of 68 tact-only by DIRECT caller (51%), re-measured 2026-09-09** (was 36 of 78 at the 2026-09-03 open; LIFT-RESID took 10 entries off the table, so the SHARE went up while the count went down — the premise is stronger, not weaker), + 4 shared (`GetResourseFilePtr`, `gfx_pack_rgb16`, `str_ansi_to_wide_scratch`, `view_set_size_mode`) | **DECIDED 2026-09-03 (user): SPLIT**, as LIFT-TACT's closing slice — `libmh_host_api` (core) + `libmh_tact_host_api`, bound separately. Derivable from `hostapi_callers.json`'s measured `direct_by_domain`, so it is generator work, not hand classification. It also names an asymmetry worth fixing: `libmh.h` exports **no tactical entry point at all** (`libmh_sim_step`/`submit_order`/`state_hash`/`save_planet` are strategic), yet 46% of the callbacks a host must implement exist solely to serve tactical mode. Public-API symmetry (`libmh_tact_*` entries) stays fork/LIB-REF territory. **REVERSED 2026-09-09 AT EXECUTION: NO SPLIT. The premise did not survive the slice that was supposed to enable it**, and the epistemics clause is what caught it. Re-measured after A/B/B2: tact-only by direct translated caller is **13 of 46 (28%)**, not 35 of 68 (51%) — the lift removed the tact-specific render-notify entries, which is exactly what it was for, and in doing so it removed most of the population the split was meant to isolate. **And the 13 are mostly not tactical at all.** Measured against the ORIGINAL call graph rather than the translated one, only **THREE** have no non-tact caller in the binary: `llm_tact_render_view`, `llm_tlo_shade_table_build_tact` and `llm_gfx_apply_resolution_change`. The other ten read as tact-only purely because their strategic and menu callers are not translated yet — `llm_input_key_queue_empty` has SIX non-tact callers in the original (`llm_strat_input_update`, `llm_ui_menu_transition_settle`, `llm_ui_modal_key_pump`, …), `rsr_GetFileRealSize` has TEN. So splitting on `direct_by_domain` would put the input pump, `fatal`, `rsr_GetFileRealSize` and the resolution family into a table named for tactical mode **on the strength of a translation-frontier artefact** — which is the precise defect §1 says this whole item exists to remove. It would freeze the accident into the ABI instead of erasing it. The legibility the split was for is delivered instead by LIFT-TABLE's regroup, which gives every surviving entry a NAMED GROUP (`input`, `io`, `time`, `fatal`, `net`, …) — a host reads what an entry is FOR without a second binding surface, a second unbound walk, a second version handshake, and a new lint class nobody has (nothing today could detect an entry routed through the wrong one of two tables). The asymmetry the row named — `libmh.h` exporting no tactical entry point — is real and unchanged; it is a public-API question and stays fork/LIB-REF territory, where it already was. **AND THE REVERSAL IS NARROWER THAN THIS ROW READS — corrected 2026-09-10 (user).** What it refuted is EXCLUSIVE PARTITIONING by `direct_by_domain`: every argument above turns on an entry being assigned to one table and the assignment being wrong. The user's design allows **DUPLICATION** — tact and sim get different tables and both may contain the SAME callback — and that removes the objection entirely, because nothing is assigned exclusively, so nothing can be mis-assigned; when a strategic caller of a shared entry is translated later it is simply added to the sim table too. Measured that day from `site_modules` (which of OUR TUs call each entry — the question this row kept answering with the original call graph instead): **sim 37, tact 16, both 3, and 13 leave the sim table**, taking the entire input pump, all of `display` bar `view_set_size_mode`, all of `sound` and the tileset/colour converters with them. That is the reduction the split is FOR, and it is what makes the sim surface abstractable; tact stays frozen. Owned by **LIB-IFACE-SPLIT**, a dep of LIB-VA0 — **EXECUTED 2026-09-10, §7** (the measured 37/16/3 landed exactly) | LIFT-TACT |
| File/resource I/O (utils_open/close/read/write, file_seek/tell, resource ptr/size) | **9** (measured) | **DONE 2026-09-09 at S7** — REQUIRED, direct file paths kept (user, 2026-09-03), regrouped as `io`, exactly the nine the row guessed. Each carries a host-implementor reason; the two whose census row reads `0 direct callers` (`file_seek`, `file_tell`) say in theirs that they are LIBMH-AUTHORED calls — our save writer patches a block length back into a header — so the zero is the census's blind spot for calls with no original-graph edge, not a dead entry | LIFT-TABLE |
| map-io (cfg_ReadMapFile, map_ReadMap_pre, regions load/save, tlo load, player-data save) | 7 | ~~KEPT AS-IS pre-fork — superseded fork-side by LIB-BOOT/LIB-WORLD blobs~~ **KEPT AS-IS pre-fork; only TWO of the seven are blob-superseded (corrected 2026-09-10, SIMABI-MAPIO).** **REASONS WRITTEN 2026-09-09 at S7, and they say what the group name does not**: six of the seven are PARSES wearing an io hat, three sit on the LZW block codec libmh already owns byte-verified as `mh::save::read_block`/`write_block`, and `cfg_ReadMapFile` writes real sim geometry (width/height, the torus masks, `pathfinder_params->width_mask`) — a host returning only the header fields it thinks matter breaks pathfinding. The seventh, `llm_gfx_load_planet_extra_sprite_banks`, is pure presentation and is in the group only because it reaches the disk the same way (it left the table at SIMABI-NOTIFY as `INV_PLANET_EXTRA_SPRITE_BANKS`). **THE CORRECTION: "superseded fork-side by the blobs" was written for the whole row and holds for only the two `.MP` BOOT PARSES — `cfg_ReadMapFile` and `map_ReadMap_pre`, which now carry DEFER-BLOB contracts naming the parse OUTPUT a blob must carry.** The other five — `llm_game_save_player_data`, `llm_map_load_regions`, `llm_map_save_regions`, `llm_planet_tlo_load`, and `llm_gfx_load_planet_extra_sprite_banks` before its conversion — are called from the `.sav`/`save%02d.dat` CONTAINER path, and no blob supersedes them, because they are **permanent SAVE-format obligations**. Two recorded sources, combined: (a) **LIB-WORLD is a BOOT fixture, not a save mechanism** — D-E5 defines it as "dump every bound region at step 0", and the endgame plan's D-E5 explicitly records savegame-as-fixture as the *later second* oracle it rejected for now; (b) **the save format is pinned untouched** — LIFT-TABLE's scope (user, 2026-09-09) keeps the SAVE format frozen, and the fork clause in D-E5's stage 3 has the frozen side keeping "save-format byte compatibility forever". A fixture that replaces boot cannot retire an obligation that outlives the fork. Sharpened at execution: even the two that *are* superseded do not retire cleanly on their own — `map_ReadMap_pre` internally calls `llm_planet_tlo_load`, which survives on its own save-path caller, so retiring the parse leaves that entry standing | LIFT-TABLE |
| Time / fatal / net-recv | 3 → **5** | **DONE 2026-09-09 at S6/S7.** Regrouped as scoped, and two of the three groups grew on measurement: `fatal` took `llm_fatal_cleanup` beside `utils_abort` (it survives the mission-load cluster on two TACTICAL callers that are not parsers), and `net` took `llm_net_transport_send` — the one of the seven `dead`-class stubs mh.dll actually REPLACES, so `dead` was not merely imprecise for it, it was backwards. `time` stayed at one, with the monotonicity requirement the link watchdog depends on written into its reason | LIFT-TABLE |
| Input entries (key queue/dequeue, mouse pump/buttons) | 4 → **5** | **DONE 2026-09-09 at S6/S7** — REQUIRED per R10, regrouped as `input`, and `llm_strat_input_update` JOINED THEM. Its class had been `mixed` since 2026-09-01 with a settled reason and an unsettled class, which the routing census reads as "needs a settled class before it can be routed further"; it was the last such row. It is the one required entry whose effects reach hashed sim state — R3's escape hatch, dispatched synchronously by contract, the declared pre-fork wall — and its reason says so rather than leaving a host to discover it | LIFT-TABLE |
| Asset-metric queries (font_desc_for_flags, sprite_width, sprite header field) | 3 | **GONE 2026-09-09 at LIFT-RESID, and the earlier "REQUIRED" was a classification error worth naming.** All three were pure — but purity was never the question. Their only libmh caller was the tutorial pair, and the widget whose layout consumed them (`_G_LLM_UI_TUTORIAL_HINT_WIDGET`) is `MF_VIEW`-only, unhashed and `host_free = true`, so widget, metrics and arithmetic moved host-side together as `LIBMH_EVK_SCR_TUT_HINT_LAYOUT`. The `asset-metrics` group does not survive the lift. **The trap: a REQUIRED verdict derived from an entry's own nature outlives its callers.** Classify from the caller set | LIFT-RESID (done) |
| Misfits (stack_capacity_guard, pack_rgb16, convert_565_to_555, rng_seed_wallclock, getAsciiVer, str conversions, build_media_diag) | ~8 | **DONE 2026-09-09 at S4/S5, and the row's own framing is the thing that did not survive**: "misfit" described how the entries looked in an undifferentiated `platform` list, not a property they shared. `stack_capacity_guard` → libmh-internal (a Watcom `__STK` probe; a host cannot answer a question about our own frame). `rng_seed_wallclock` → init parameter, user's call. `pack_rgb16` → gone at S4 with the block above it. `getAsciiVer` and the str conversions → REQUIRED, `string` group — REFUTED as misfits, they are locale-dependent Win32 codecs and one of them feeds hashed state. `build_media_diag` → REQUIRED with the reason the dig produced (see §6 S5). The 565→555 pair stayed out of scope with tact. So of ~8 "misfits", two internalized, one moved, four are real required services and one was already handled elsewhere | LIFT-TABLE |
| **Planet construct/clone** (`cfg_final_planet_Construct` 0x0045b69c, `llm_strat_scenario_planet_clone` 0x0045ba25, `cfg_final_planet_FillBankData` 0x0045e306, `cfg_ReadMapFile` 0x004a3b9d, `cfg_GetTloIndex` 0x004b9582) | 3 entries + 1 translated body | **DECIDED 2026-09-09 (user), three parts.** (1) **The body SPLITS**: its sim writes stay in libmh, its graphics half (per-planet gfx setup — the `FillBankData` bank fill, the `tlo_index` → `soldier_sprite_bank_offset` chain, whatever else the field census classes gfx) moves HOST-side. Ruling §0a: it "also loads gfx for the planet, that is not actually part of sim". This supersedes the earlier reading that the whole runtime path is libmh's because the runtime CALLER is sim — the caller being sim decides where the sim half lives, not where the gfx half does. (2) **`Planets[].bank[100]` (+0x38d, 100 B/record) LEAVES the determinism hash** — it is written only by `FillBankData` and read only by `llm_gfx_planet_bank_needed` / `llm_gfx_load_banks` / `llm_gfx_load_planet_extra_sprite_banks`, all gfx. Mechanism: an `emit_planets` masked walker in `state/region_view.h` dispatched on `HIDX_PLANETS`, the same shape as `emit_units` (ctrl-group mask) and `emit_tile_objects` (fog mask) — NOT a manifest edit, because `HASH_REGIONS[]` is positional (`IDX_*` + `mp_analyze.py`'s `REGION_NAMES`) and anything but an append silently re-labels every later region. Once bank[] is unhashed, a host-side `FillBankData` is sound; before that it was a `notify:1` entry writing hashed+saved state. (3) The boot half was never libmh's: LIB-BOOT's 2026-08-22 snapshot decision already puts the whole ~65 KB cfg parser (`cfg_ConstructPlanets` and siblings) host-side; only the per-session clone into `Planets[0x1f]` is sim. Exact byte ranges and the per-field reader evidence: the 2026-09-09 field census | LIFT-TABLE |
| **Tactical** (the whole mode's entries) | — | **OUT OF SCOPE for this pass (user, 2026-09-09): "let's not touch tact for now. It should get its own interface."** Note how this relates to LIFT-TACT's closing reversal, because the two are easy to confuse: that reversal refused to DERIVE a tact/core split from `direct_by_domain`, on the measurement that 10 of the 13 apparently-tact-only entries are universal services whose non-tact callers merely aren't translated yet — deriving from the frontier would freeze the accident into the ABI. It did NOT rule against a tactical interface as an architectural object. A tact interface built from MODE SEMANTICS and the original call graph is consistent with that measurement; one built from translation order is the thing it refused. Nothing tact-only is regrouped or converted until then. **THE TABLE SPLIT IS NOW OWNED: `LIB-IFACE-SPLIT` (opened 2026-09-10, a dep of LIB-VA0; EXECUTED the same day — §7)** — two tables with duplication allowed, membership derived from the accessor-site scan (`mh::host()` / `mh::tact_host()`, assigned by the referencing TU's module). It does NOT convert or regroup anything tact-only, so this row's out-of-scope ruling stands as written; what it changes is that the sim table stops carrying tact's 13. The ownership half needs nothing: `sim/`, `ai/`, `lockstep/` and `orders/` contain ZERO `mh::tact::` references, and the only caller of `mh::tact::mission_start()` is `seams/launch.cpp:1173`, harness-side and already outside libmh — so "sim notifies the host, the host switches to tact" replaces a coupling that lives in untranslated original code (`llm_strat_try_enter_tactical_mission`), not one in ours | LIB-IFACE-SPLIT |

Per the epistemics clause: each row above is re-checked against the actual code at its
stage; "REQUIRED" rows get a per-entry recorded reason at LIFT-TABLE, not a rule cite.

## 4. R3/R3b/R9 conflict register (entries whose return — or whose writes — feed something)

| entry | what reads the return | disposition |
| --- | --- | --- |
| `llm_net_lockstep_sync_overlay_show` | lockstep reset/disconnect logic | **RESOLVED 2026-09-03** — a real answer, but polled, not blocking. CONVERTED: `on_screen` OPEN + `OVERLAY_RESULT` becomes libmh state written by a pushed host input. See R9 |
| `llm_ui_outcome_dialog` (via stage E) | GAME_MODE gating | **RESOLVED 2026-09-03** — no consumer at all (Watcom fake return; the `presence_lost` forward is read by none of its 21 callers). CONVERTED void. See R9 |
| `game_ui_PrintTextMessage`, `llm_ui_print_floating_msg_red` | int returns | verify consumers at LIFT-NOTIFY; expected ignorable → void event |
| `llm_strat_offscreen_snd_volume` / `llm_strat_offscreen_fx_scale` | snd_play volume / fx scale | DELETED — position-carrying events; host attenuates/scales |
| `llm_tact_render_view` (**R3b**, no return at all) | its own writes, same frame: `llm_tact_unit_update_anim` is reached ONLY through its camera-viewport tile scan and writes `anim_frame_time`/`frame_index`/`anim_cycle_time`/`frame_interval`/`sprite_id` into `tact_units` = `TACT_HASH_REGIONS[0]` (`mh_regions.gen.h:2897`, comment 2911-2916); it also writes `hovered_unit_id`, which `tact_frame` reads at **9 sites, all after the call at line 286** (299, 316, 327, 347, 351, 365, 397, 406, 430) to drive `unit_enqueue_command`/`group_issue_order` — further hashed-roster writes | **REQUIRED — exempt from the tact-frame conversion (user, 2026-09-03).** On the channel, a NULL-callback host would defer dispatch to frame edge and steps 17-24 would issue orders against **last frame's** hover state. It holds today only because mh.dll is the sole host and always binds a synchronous sink — an accident, not a contract. And no oracle we have would catch it: `TACT_HASH_REGIONS` excludes hover/camera and the render wall is never the OURS arm. Exempting costs ONE entry; converting would require making "always synchronous, never poll-deferred" an explicit tested property of that kind. Consistent with its already-declared presentation-wall status (`tact_effect_classes.json:638-639`) |
| `GetResourseFilePtr` header readback (`tact_ui_sel_panel_init`) | clip_w/clip_h for draws — **and much more than that**: `ui_sel_panel_init.cpp:38-51` stores the **raw returned pointer** into `own.sel_panel_icon_gfx_at(i)` (a `void**` state-registry array, `RID_TACT_SEL_PANEL_ICON_GFX`), which **9 of the 10 L4b bodies** then dereference for geometry or hand to `draw_surface` (15 referencing files) | **DEFERRED within LIFT-TACT (user, 2026-09-03).** The original disposition ("presentation geometry — moves host-side with its cluster") described only this function's own header read; the pointer it produces is threaded through the whole cluster, so the real question — does the icon-pointer array stay libmh-owned at all — is cluster-wide. Converting the 9 *readers* does not need the answer; `ui_sel_panel_init` converts LAST, after it. Note the entry survives on ai/sim callers regardless. **RESOLVED 2026-09-09 at slice A's close: the array goes HOST-SIDE with the draws, and the question dissolved rather than being decided.** Every consumer across all ten bodies used `sel_panel_icon_gfx_at` only to blit the pointer or read its `(w,h)` header as the clip for that same blit — never for a decision — so once the draws are host-side no translated body reads or writes it at either end. The corroborating detail that settles it: the array's deallocator `llm_tact_ui_sel_panel_free_gfx` @`0x00434051` was never translated and was never a host entry, so libmh was allocating into an array it could not free. It is the host's now, both ends |
| `llm_gfx_pack_rgb16`, `llm_gfx_sprite_width`, `llm_gfx_ui_sprite_get_header_field2`, `llm_gfx_font_desc_for_flags` | UI layout math | pure queries — asset-metrics group or internalized; layout largely moves host-side. **`llm_gfx_pack_rgb16` RE-DECIDED 2026-09-09 (user, §0a): NOT internalized.** It is an R3 violation as it stands — `notify:1` (the flag is derived from the ledger CLASS string alone, never from the body), the generated no-op returns `0`, and all 77 sites consume the return, nine of them stored into sim-store cells by `sim_planet_map_session_init.cpp:51-59`. But the fix is R1, not R3's second branch: its only non-harness libmh caller is that palette block, whose nine cells are `MF_VIEW`/`OWN_ISLAND` (`mh_regions.gen.h:1707-1710,1725-1729`) — presentation state sitting in the sim store because migration put it there. **The cut goes above the block**: the host owns the planet-map palette, the block leaves libmh, and the entry disappears without anyone reimplementing colour packing inside the deterministic core. The remaining two colour entries (`convert_pixels_565_to_555`, `tlo_palette_convert_565_to_555`) have exactly one libmh caller between them — `tact_mission_load.cpp` — so they are blocked on the tactical pass and are NOT touched now. **Trap for whoever executes this**: the argument order is `(red, blue, green)` — Ghidra's plate and the asm agree (EDX carries the `&0xFC` 6-bit green field) — while `planet_map_session_init_calls` declares `(r, g, b)`. Nothing is wrong today because every call is positional through one shim; an implementation that trusts the NAMES swaps green and blue on every call, and those cells are `MF_VIEW` only, so no oracle we have would catch it |
| `file_seek` / `file_tell` | our save_live.cpp (no original-graph edge — libmh-authored calls) | required `io` group |
| `llm_str_ansi_to_wide[_scratch]`, `utils_wide_to_short_str` | text rendering paths | **SPLIT, and the second half CORRECTED 2026-09-09 after reading the body.** `llm_str_ansi_to_wide` keeps the original disposition (it survives the lift on its `pre:save` caller regardless). `utils_wide_to_short_str` was recorded here, twice, as an INTERNALIZE case on the strength of its name and its hashed write; both readings were wrong about the mechanism. It is `utils_w_strlen` + `utils_WideStringToAscii` @0x004cf2dc, and that is `WideCharToMultiByte(CP_ACP, 0, src, n, dst, n, <default char @0x00661524>, …)` — a **locale-dependent Win32 codec**, not the truncation the `crt_string` family vendors. **KEPT REQUIRED** (`notify: false`, platform group): a host that no-ops it leaves `player_desc[1].name` unwritten and diverges on the very next hash, so the named trap is the protection and `hostapitest` is what proves a required entry names itself. **This is the one entry in the table whose result feeds HASHED sim state** — worth carrying into LIB-REF: a standalone host must produce the same bytes, not merely something plausible. An ASCII truncation would be exact for the EN build's value at that site (`"Others"`, read off a live run's PLAYERDUMP) and silently wrong for a non-ASCII text table |

## 5. What each lift must prove (uniform)

1. **No-op/queue arm**: with the subtree's entries no-op'd (or drained by the queue-mode
   selftest host), the sim oracles are unaffected — the subtree hid no sim write.
2. **Hosted oracle**: run_selftests (ASan) + UI suite + determinism, bit-identical
   baselines, after every lift (R5).
3. **Rewritten verified bodies** (LIFT-RESID/LIFT-TACT) additionally pass reimpl-verify
   with offline-suite pins mutation-proven.

## 6. LIFT-TABLE execution plan (2026-09-09, from the re-review's five digs)

Ordered by dependency, not by size. Every stage names its oracle and its negative arm, because a
stage that cannot be made to fail is not evidence (G106). §5's uniform proofs apply on top.

**Out of scope, decided rather than deferred by omission:** anything tactical (user, 2026-09-09 —
tact gets its own interface later, §3); the two 565→555 converters, whose only libmh caller is
`tact_mission_load.cpp` and which are therefore blocked on that; re-cutting the `map-io` cluster,
which §3 keeps pre-fork because LIB-BOOT/LIB-WORLD blobs supersede it — S7 gives those seven entries
their per-entry reasons and nothing else; and the SAVE format, untouched (user, 2026-09-09).

**EXECUTED 2026-09-09, S1..S8 — the outcome is recorded stage by stage below, and four stages
changed on their own measurement rather than being applied as written (§0). The table is
**114 → 50**; the VA census is **54 → 24** across the day, **39 → 24** in this pass, with
`dead`, `mixed` and `pre-ledger` all at zero.**

### S1 — class corrections (ledger data only, no code)

`notify` is derived from the ledger CLASS string alone (`gen_libmh_hostapi.py` CATEGORIES), never
from the `reason`, so six entries ship a no-op-able contract their own recorded decision
contradicts. Flip the class; the flag follows.

| entry | why it must be `notify: false` |
| --- | --- |
| `llm_tact_render_view` | R3b's founding instance (§4). Its reason text is still the pre-discovery "view-only state per xref sweep" |
| `llm_gfx_apply_window_resolution` | reason already says "KEPT REQUIRED under R3b"; same-call readback of `GFX_DRAW_SURFACE`/`FRAMEBUFFER` |
| `llm_gfx_apply_resolution_change` | same, plus `MAP_REGION_LIST_HEAD` re-derived on the next line |
| `llm_view_set_size_mode` | R3 — return assigned into hashed `view_size_mode()` at `sim_start_tutorial.cpp:44` |
| `llm_ui_chat_recalc_target_mode` | **new R3b**, adjudicated at LT1D on 2026-09-02, one day before R3b existed (the G111 shape). Sole writer of `_G_LLM_CHAT_TARGET_MODE`, which translated `lt_chat_ally_mask` reads as its top-level gate on every `llm_diplomacy_set_relation` |
| `llm_ui_chat_target_add` | **new R3b**, same LT1D gap. Its mask is read by translated `chat_send` to build every outgoing `MSG_CHAT` |

Neither chat entry touches render or the OS — they are bookkeeping over player state, so the end
state is that libmh owns them. Reclassing is the correctness fix; **translating them is a follow-on
item**, not this stage. **That follow-on landed 2026-09-10 (SIMABI-CHAT):** both bodies are libmh's
(`libmh/lockstep/lt_chat_ally_mask.{h,cpp}`), both rows left the ledger, and the R3b hazard the two S1
flips were protecting is gone by construction rather than by contract — writer and reader are one
synchronous call graph. `llm_view_set_size_mode` is the only S1 row still standing on its own flip
(SIMABI-DISPLAY's gate decides it).

**Oracle:** `run_selftests` + the no-op-host arm (a required entry must name a trap, not no-op).
**Negative arm:** revert one flip; the no-op arm goes red naming that entry.

**DONE 2026-09-09 (commit `9b89e1b6`), exactly as scoped.** All six flipped to a notify:0 class;
the table stayed at 46 entries and the ABI version moved, because category order is struct
layout. `run_selftests` green, which is also the arm: the selftest host now traps BY NAME on all
six instead of returning a silent default, and no suite reaches one.

### S2 — take the planet gfx bytes out of the strategic hash

An `emit_planets` walker in `state/region_view.h` dispatched on `HIDX_PLANETS`, skipping exactly two
windows per 0x427-byte record: **(+0x38d, 100 B)** `bank[]` and **(+0x425, 2 B)** `tlo_index` +
`soldier_sprite_bank_offset` — 102 of 1063 bytes, non-contiguous, with 0x98 bytes of sim between
them. Same shape as `emit_units` / `emit_tile_objects`. The `HASH_REGIONS[]` row is NOT edited: the
table is positional (`IDX_*` + `mp_analyze.py`'s `REGION_NAMES`) and anything but an append silently
re-labels every later region.

Everything else in the record stays hashed, including the strings: `map_name` is the cheapest early
desync signal available, and `tlo_file` (+0x20e) has a RUNTIME writer (`map_ReadMap` @0x004a3361, at
every map load), so excluding it would silence a non-gfx path. `info_txt` and `asteriods` have no
reader at all and stay in for the same reason — *unread* is not *gfx*.

**Oracle:** determinism run identical (the mask is symmetric — both peers run one build).
**Negative arm, mandatory:** a `mask_planets_gfx=false` switch restoring the unmasked walk (the
`mask_ctrl_group` precedent), plus a mutation showing that a change to a SIM field of the record
still trips the verdict. A mask whose unmasked arm cannot be run is a mask nobody can audit.

**DONE 2026-09-09 (commit `8fb6e4cb`).** `mh::state::emit_planets` is the third masked walker;
`statetest` grew 24 checks over both windows' four ends, all four boundaries, both strings, and
the save blob, and was MUTATION-CHECKED twice (local→bytes on the windows takes 5 checks red;
bytes→local on the record head takes the three over-mask checks red).

**THE COST THE STAGE DID NOT KNOW IT HAD, and it is the interesting half.** The two committed
UIREC oracles — `spcamp-solo.oracle.gz` (2026-09-07) and `tutorial-solo.oracle.gz` (2026-09-09)
— store ABSOLUTE per-step `state` hashes extracted from HUMAN recordings, and both predate this
mask. A human recording **cannot be re-derived under a new hash definition**: the log stores the
hash, not the bytes. So `--ui-abc`'s B-vs-C and A-vs-C arms would have gone red from step 1 with
nothing wrong, on the day the fixtures were newest. `ui_write_config` therefore pins
`mask_planets_gfx=0` for every UIREC arm, which is sound only because the UNMASKED walk
reproduces the pre-mask flat hash BIT-FOR-BIT (`hash_sink` carries a partial block across
`raw()` calls, so the chunking is invisible) — asserted in `statetest`, not assumed. This is the
FIRST mask change in the repo's history to meet a live absolute-hash artefact; `mask_soldier_anim`
(2026-08-28) predates the whole `.oracle.gz` mechanism, which is why it is precedent for the
selftest shape and not for this. `--determinism`, `--tact-determinism` and the pixel baselines
are arm-vs-arm within one build and unaffected.

### S3 — the planet gfx tail moves host-side

`cfg_final_planet_Construct`'s graphics work is one **contiguous tail, 0x45b8fe to the end of the
body**, with nothing sim beneath it (census 2026-09-09): `cfg_GetTloIndex` → `tlo_index` → the 8-way
switch → `SHL AL,6` → `soldier_sprite_bank_offset` → the `index == 0x1f` guard → 8× `FillBankData`.
Its only inputs are `map_header.tlo_name` and the planet index; its outputs are read by renderers and
bank loaders only. It becomes ONE host entry (planet gfx setup), replacing `FillBankData` in the
table and removing `cfg_GetTloIndex`'s raw VA edge.

Blocked on S2: before the mask this entry writes hashed and saved state, which is exactly what made
`FillBankData`'s `notify: 1` a real bug rather than a label error.

**Oracle:** R5 hosted arm bit-identical, plus the no-op arm (post-S2 it can hide no sim write).
**Negative arm:** no-op the new entry with S2 reverted — the hash moves, proving the mask is what
makes the move legal rather than the move being harmless on its own.

**DONE 2026-09-09 (commit `336e0832`), table 46 → 45**, as `LIBMH_EVK_INV_PLANET_GFX_SETUP(a =
planet_slot, b = tlo_index)` on the invalidate channel rather than a new table row — a composite
tail has no single original function for `gen_libmh_hostapi` to generate a forwarder from, which
is why every LIFT-TACT composite went the same way. `cfg_final_planet_FillBankData` left the
table with it.

**ONE CLAUSE ABOVE IS REFUTED: the raw VA edge to `cfg_GetTloIndex` CANNOT go, and the reason
generalises.** The lookup reads `map_header.tlo_name`, a STACK LOCAL of the emitting frame.
Handing the host that pointer works under mh.dll, whose sink dispatches synchronously at emit,
and DANGLES under a NULL-callback host, which drains at the frame edge with the frame gone —
R7's two consumptions are exactly what makes the difference invisible in testing. **A record may
not carry a pointer into the emitter's frame**; an identity may. So libmh keeps the 116-byte
constant-table lookup and the resolved index crosses, while the VA edge stays where it already
belonged, in LIB-BOOT's `cfg-snapshot` set. Census unchanged at 39 for this stage.

The R3b gate filed the new scope in the `readback` bucket, because `PLANETS` is one 34016-byte
region and the gate works at region granularity; it carries a `justified-exempt` disposition with
both halves it needs — field disjointness (the scope writes only +0x425/+0x426 and `bank[]`, with
zero translated readers of any of them) and, for `game_SaveGame` which copies the record
wholesale, a STRUCTURAL ordering: `cfg_final_planet_Construct` has exactly two callers image-wide,
`cfg_ConstructPlanets` and `llm_strat_scenario_planet_clone`, both session-entry, while a
queue-mode host drains at the end of that same frame.

### S4 — the planet-map palette block moves host-side, and `pack_rgb16` leaves

R1, not R3's internalize branch (§0a). The nine `pack_rgb16` calls in
`sim/resid/sim_planet_map_session_init.cpp:51-59` fill `planet_map_pal4_*`/`pal5_*` — nine
`MF_VIEW`/`OWN_ISLAND` cells, presentation sitting in the sim store because migration put it there.
The block leaves libmh with its cells; the entry disappears without colour packing entering the core.

**Two traps, both live.** (a) The argument order is `(red, blue, green)` — Ghidra's plate and the asm
agree, EDX carries the `&0xFC` 6-bit green field — while `planet_map_session_init_calls` declares
`(r, g, b)`. Everything is positional through one shim today, so nothing is wrong until someone
implements from the NAMES. Fix the declaration and the two globals whose names inherited the swap
(`PAL5_GREEN`/`PAL5_BLUE`) in the same change. (b) **No hash covers these cells**, so determinism is
not the oracle here.

**Oracle:** a UI capture of the planet-map screen, registered in `tools/test_ui.py` per the standing
rule — this is the stage where a wrong colour is invisible to every other gate.
**Negative arm:** swap two channels deliberately; the registered capture must go red.

**DONE 2026-09-09 (commit `9728f697`), table 45 → 44** — as
`LIBMH_EVK_INV_PLANET_MAP_PALETTE`, no payload: nine literal colour triples in a fixed order, so
the record is the INSTANT, not the data.

**THIS STAGE'S ORACLE DOES NOT EXIST, and that is the finding rather than a shortfall.** The
capture above was specified on the premise that a wrong colour is invisible to every other gate.
Measured at execution: **all nine cells are WRITE-ONLY IMAGE-WIDE** — one WRITE cross-reference
each and no reader in libmh, in the seams, or in the original (reference manager, not a text
search). No pixel depends on them, so a channel-swap negative arm would have passed VACUOUSLY,
and a registered capture would have been a gate that cannot fail. The oracle is that reader
census plus the R5 hosted arm. Recorded, not substituted.

**Trap (a) is closed and it was real.** `pack_rgb16` @0x0043c56b takes `(red, blue, green)` —
verified first-hand off the body, not off the prior note: `param_1 & 0xf8 << 8` → bits 15-11,
`param_2 & 0xf8 >> 3` → bits 4-0 (BLUE), `param_3 & 0xfc << 3` → bits 10-5 (GREEN). Two of the
game's own globals had inherited the swap and are RENAMED in Ghidra (**EN v398**, each carrying
the derivation as an EOL comment): `0x00fe5b64` is fed `pack_rgb16(0,0,0xff)` = pure GREEN and
was named `_..._BLUE`; `0x00fe5b68` is fed `pack_rgb16(0,0xff,0)` = pure BLUE and was named
`_..._GREEN`. RID numbering is positional and unchanged; only the names move, and no behaviour
does — every call is positional, which is precisely why nothing had caught it.

One thing the plan could not have known: **the sink must reach those cells through the region
runtime**, not `mh::addr::`. All nine are RELOCATABLE regions, so under `relocate_state=1` a
constant address writes into the 0xCD poison the region left behind. `check_movable_addresses`
is what said so.

### S5 — the misfits, each on its own evidence

- `llm_stack_capacity_guard_0x20` → **libmh-internal.** The body forwards to the Watcom `__STK`
  probe; no OS or device touch anywhere; all 7 sites are 4 sim pathfinding functions. A host cannot
  implement a stack probe.
- `llm_strat_rng_seed_wallclock_seconds` → **init parameter.** Real `time()`/`_localtime()`, but
  called twice, both from `llm_strat_planet_session_begin`. Also removes an MP divergence hazard
  (two peers seeding from their own clocks).
- `getAsciiVer` → **stays REQUIRED, `string` group. The misfit line is REFUTED**: it is
  `WideCharToMultiByte(CP_ACP, …)`, structurally the `utils_wide_to_short_str` twin, called live from
  9 sites (error dialogs, building construct, lobby map-picker). Not init data.
- `llm_build_media_diag_report` → **unsettled, and the question is one dig**: its blob is appended
  into save files; does anything ever read it back? If nothing does, a host-supplied placeholder
  satisfies the byte layout and it reclasses toward internal/stub; if something does, it stays
  required. Do not dispose of it by rule.
- `pack_rgb16` and the 565→555 pair are handled by S4 / out of scope — **not** by this line, which is
  where the misfit list originally swept them.

**DONE 2026-09-09 (commit `6c20ed62`). Two dispositions held, one was refuted before execution,
and the fourth turned into the opposite of what the dig was expected to license.**
- `llm_stack_capacity_guard_0x20` → libmh-internal, as scoped (`sim/sim_stack_guard.h`). The
  calls-struct MEMBERS stay: its seven sites sit on not-found/impassable arms and the offline
  suites count those calls to prove control flow reached them, so deleting the members would
  delete the evidence along with the entry.
- `llm_strat_rng_seed_wallclock_seconds` → **init parameter (user's call, 2026-09-09)**, the new
  public `libmh_set_session_seed`. Two divergences are DECLARED rather than discovered later: the
  value is sampled when the host chooses rather than at session begin, and the original's two
  independent `tm_sec` draws become one — which is the property the multiplayer fix wants.
  mh.dll pushes `GetLocalTime().wSecond` at bind time, and the FIRST version of that line pushed the
  ORIGINAL function's result instead, so the harness `pin_strat_seed` trampoline would stay in the
  path. That killed every boot: the bind point is inside DllMain and the original runs in mh.exe's
  Watcom CRT, which the loader has not initialised yet (dead-ends **G163** -- the whole offline gate
  was green while every boot was dying, because `net_selftest` has no mh.exe). The pin is preserved
  directly instead, pushed beside `pin_strat_seed`'s own arm, which is the better place for it: our
  promoted body reads the pushed slot, so a trampoline over the original entry would have left the
  promoted arm on the wall clock while the stock arm was pinned. `simtest`
  T1 now pins the new property (both channels take the pushed value; a different pushed value
  moves both) in place of the one it pinned before.
- `getAsciiVer` → REQUIRED, `string` group, as the refutation above said.
- `llm_build_media_diag_report` → **STAYS REQUIRED**, and the dig that was meant to free it is
  what settled the reason instead. Nothing reads the blob: two call sites image-wide (the
  main-menu load discards the return; `game_SaveGame` writes it), and `llm_game_load` stops at
  its last wanted member and never reads another byte — traced to its `utils_close_file` and
  independently recorded in `docs/save-format.md`. So a host placeholder does satisfy the format.
  But **libmh generating that placeholder would make the HOSTED save differ from the original's
  byte-for-byte**, and this entry is exactly the seam that lets mh.dll keep producing the real
  bytes while a standalone host produces any 0x400 it likes. The plan's "reclasses toward
  internal/stub" ran out one step short of that.

### S6 — the folded 2026-09-08 clauses

**save_live.cpp is DONE, 2026-09-09 — all 15 of its sites disposed, VA census 54 → 39.** The user
read the file, counted 15 `mh::call::` sites against the census's 11, and the gap was real: four A/B
harness trampolines (`mh::call::detail::s_u32_*((uintptr_t)g_*_tramp, …)`) name no callee, so the
census's `!= "detail"` filter erased the whole site. Instrument fixed first (LIB-VA0, commit
`6e0b902e`: a `tramp` shape, routed by shape, reconciled with a printed term, three selftest arms).
Then, per the user's rule — rebind if translated, host-API if not:

| population | sites | disposition |
| --- | --- | --- |
| named callees, all with translated bodies | 8 | `MH_LIBMH_BIND(fn)(…)`, the call-through-the-binder form. **Hosted behaviour unchanged** — a bound row's default key is 0, still `mh::call::` at the original entry, so each subsystem's `[promote]` A/B survives. Only standalone changes |
| A/B triggers (`loadgame_now`, `savegame_now`, `save_planet_now`) | 3 | MOVED to `seams/save_triggers.cpp` (S6.1), added to `mh.vcxproj` **and** `mh_nettest.vcxproj`, absent from `libmh.vcxproj`. Still route through the ENTRY — that is what makes them two-armed |
| verify-mode trampolines | 4 | `#ifndef MH_LIBMH_BUILD`. Verify mode is a HOSTED oracle: standalone has no original binary to compare against and the trampoline is installed by the hosted detour path only, so the branch is unreachable there rather than stubbed. Both the body and the reaching `if` are guarded |

Binder rows 695 → 697, deferred traps 12 → 13 (`llm_strat_planet_map_session_init` is sim_resid, so
it names a trap until LIB-REBIND-UI rather than binding — the honest state, and still better than an
invisible VA).

**S6.1's two-arm property RE-DEMONSTRATED ON THE RIG, 2026-09-09** — the clause's own requirement,
and the reason it is a requirement: relocating a function whose entire purpose is to be two-armed
must not be trusted to a build that only proves linkage. Two headless `sp_det.txt` runs, 600 steps,
wall-clock pinned, differing in nothing but three `[promote]` fragments:

```
python tools/ui_test.py sp_det.txt --harness --steps 600 --headless \
    --harness-extra "save_at=300;save_count=1;savegame_at=400;loadgame_at=500;loadgame_name=uitest;pin_wallclock=1" \
    [--extra-ini <a fragment setting [promote] save / container / container_load = 1>]
```

| trigger | stock arm | promoted arm |
| --- | --- | --- |
| `save_planet_now` → `; [save] TRIGGER step=300` | `rc=1 (promoted=0)` | `rc=1 (promoted=1)` |
| `savegame_now` → `; [save] SAVEGAME step=400` | `rc=1 (container promoted=0)` | `rc=1 (container promoted=1)` |
| `loadgame_now` → `; [save] LOADGAME step=500` | `rc=1 (container_load promoted=0)` | `rc=1 (container_load promoted=1)` |

And NON-VACUOUSLY, which `promoted=1` alone does not establish — that flag says the promotion is
installed, not that our body ran. The promoted arm's `mh_net.log` carries
`SavePlanetToDisk replacement served call #1`, `game::SaveGame replacement served call #1` and
`llm_game_load replacement served call #1`; the stock arm has **zero** `[promote] save|container|
container_load` lines at all. One trigger, two implementations, selected from outside — unchanged by
the move.


1. **Three A/B triggers to `seams/`** — `loadgame_now`, `savegame_now`, `save_planet_now`
   (`save_live.cpp` ~:1656/1661/1667). CONFIRMED one-line cast-and-forwards through `mh::call::`, and
   better motivated than the clause knew: `save_live.cpp` compiles into `libmh.vcxproj`, so those raw
   VA calls leak into the standalone build today. **The move must keep them calling the ENTRY** —
   rewriting them to call the promoted body directly destroys the two-arm property that is their
   whole purpose. Re-run the harness after the move to show both arms still selectable.
2. **The four apply-phase sites (`:805/812/814/820`) do NOT cross the `io` group.** The clause is
   refuted for at least three of them, and the body's own comment says "No file I/O":
   `register_bldg_type_callbacks` (sim setup), `time_resync_and_tick` (sim clock),
   `snd_ambient_reseed_planet_event_times` (sound) touch no file; `map_LoadPlanetFromDisk` is a
   re-entry into another promotable root. All four are `[promote]`-gated A/B entry calls, the same
   pattern as (1). Disposition: each routes to its own subsystem, preserving its independent toggle.
   **This overturns a user clause from 2026-09-08 and needs sign-off before execution.**
3. **The 7 `dead`-class seam stubs (14 sites) onto the interface**, each with its group — a seam the
   DLL supplies is not original game logic, so `dead` is the wrong class for a live one.
4. **R9's 22 direct sites**: 12 reach a promoted body (correct hosted via E9, unresolved only
   standalone), 10 → LIB-VA-PROMOTE. Each converted, routed, or KEPT with a recorded reason.
5. **`llm_strat_input_update`'s class settled** — R10 (the host owns the pump) says it stays walled;
   record that as its reason rather than leaving the row classless.
6. **`llm_strat_register_bldg_type_callbacks`' single direct site**, and reconcile
   the libmh rebind notes' resolved-question-2 claim, which says both registrars derive like any
   other row while the tree says neither is a row at all (a direct inline call is invisible to a
   generator that derives rows from calls-struct members).

### S7 — the regroup, with a recorded reason per surviving entry

Named groups: `io`, `time`, `fatal`, `input`, `net`, `asset-metrics`, `string`. No tact/core split
(§3). Every entry that stays REQUIRED carries a one-sentence reason written for a host implementor —
about what the host supplies that libmh cannot compute, never about who calls it. The 2026-09-09 digs
supply drafts for every surviving entry; each is re-checked at apply, per §0's clause that a REQUIRED
classification is a per-task decision with evidence.

Two corrections to fold in while writing them: the `0 direct caller` rows (`file_seek`, `file_tell`,
`utils_abort`) are a census blind spot, not dead entries — all three verified live in `src/mh_dll`;
and six of the seven `map-io` entries are parses wearing an io hat (three of them sitting on the LZW
block codec libmh already owns byte-verified as `mh::save::read_block`/`write_block`), which the
reason text should say plainly even though the cluster stays.

**DONE 2026-09-09 (commit `6c20ed62`). THIRTEEN groups, not the seven drafted**, because the draft
list was written before S1's reclassings and before the hook stubs arrived, and because two of its
seven (`asset-metrics`) had already been dissolved at LIFT-RESID:

| notify | groups |
| --- | --- |
| yes (a headless host may bind a no-op) | `render-notify` 6, `sound` 1, `hook` 6 |
| no (implement, or bind a NAMED trap) | `display` 4, `input` 5, `chat` 2, `io` 9, `map-io` 7, `net` 2, `string` 4, `time` 1, `fatal` 2, `platform` 1 |

`display` is the group the draft had no name for: four entries that are visual and still REQUIRED,
because each either returns a value the sim reads (R3) or reconfigures state libmh reads back in
the same call (R3b). `hook` is the six empty hook points, and its blurb says the thing a host
implementor most needs to know about them — a no-op is not a degradation, it is retail's own
behaviour. `platform` is down to ONE entry, which is the measure of what it had become: it had
been the place every required entry went regardless of what it was for.

Every required entry carries a reason written for a host implementor. The load-bearing ones say
so out loud: `utils_wide_to_short_str` names the exact `WideCharToMultiByte` call to reproduce and
why an ASCII truncation is exact for this build and silently wrong for another;
`llm_time_get_ticks_ms` must be MONOTONIC or the link watchdog credits a stalled peer forever;
`utils_abort` MUST NOT RETURN; `cfg_ReadMapFile` is a PARSE that writes sim geometry, so a host
returning only the header fields it thinks matter breaks pathfinding; `llm_net_transport_recv`
must be non-blocking or the sim stalls.

### S8 — close

Regenerate `gen_libmh_hostapi`; report the entry count against the pre-pass 114; the mechanical table
survives as mh.dll's internal routing layer with the unbound-walk still reporting zero; the census and
generator gates green on the final shape; LIB-VA0's per-class census reports this item's populations
at ZERO with its ratchet green. **Negative arm:** revert one converted site — the census names that
callee and the ratchet goes red.

**DONE 2026-09-09.**

| measure | before this pass | after |
| --- | --- | --- |
| libmh-facing table | 46 (114 pre-LIFT) | **50** |
| VA census sites | 39 | **24** |
| `dead` / `mixed` / `pre-ledger` sites | 13 / 2 / 4 | **0 / 0 / 0** |
| unadjudicated callees | 0 | 0 |
| unbound-walk | 0 | 0 (`hostapitest`) |

**The table went UP, and the number is the honest one.** Seven entries joined it that were not on
it before — the six hook stubs and `llm_strat_input_update` — because they were reaching original
code at a fixed VA from inside libmh while wearing an adjudication (`dead`, `mixed`) that said
nothing about how a host would ever satisfy them. Two left as leaves (`pack_rgb16`,
`FillBankData`), two left as non-services (`stack_capacity_guard`, `rng_seed_wallclock`). A
surface that shrinks while VA calls stay hidden behind an unsettled class is the metric this item
exists to distrust: **24 is the number that moved.**

**Negative arm demonstrated** (`lockstep/tx_emit.cpp:50` reverted to `mh::call::`): the census
names the callee — `THE VA SURFACE ROSE: 24 -> 25 site(s) ... lockstep/tx_emit.cpp:50:
llm_teardown_hook_stub` — and exits 1. It fires twice over, in fact: `gen_libmh_calls` reports
`misrouted 1` in the same breath, because reaching a host-callback-classed callee through
`mh::call::` is a layering error independently of the ratchet.

## 7. LIB-IFACE-SPLIT execution (2026-09-10) — two tables, duplication allowed

The design is the user's three steps (tracker `LIB-IFACE-SPLIT`): (1) tact and sim get DIFFERENT
callback tables; (2) both may contain the SAME callback — duplication is allowed and is the point;
(3) the sim table is then reduced to an abstract API while tact stays frozen (the reduction is a
follow-on item, not this one). Duplication is what survives the §3 reversal: that reversal refuted
EXCLUSIVE partitioning by `direct_by_domain`, and with nothing assigned exclusively, nothing can be
mis-assigned.

**Dispatch: two accessors** (user's call, of two options presented). `mh::host()` keeps returning
the sim table (`libmh_host_api` — struct name, path, and macro names unchanged; the version
recomputed with the membership change, which is the handshake doing its job), and `mh::tact_host()`
returns `libmh_tact_host_api` (`libmh_tact_host_api.gen.h`); the 29 tact binder lines across 11 TUs
respelled mechanically. The rejected alternative — a third, libmh-internal union struct assembled
at bind time — had zero source churn but added a merge step and forced shared entries to bind
identically across tables, i.e. duplication only at the binding surface, not at dispatch.

**Membership is DERIVED, and the spelling cannot defeat it**: `gen_libmh_hostapi.py` scans
`src/mh_dll/mh/**` for both accessors, assigns by the referencing TU's MODULE (tact/ → tact table,
every other migrated module → sim table), and FAILS loudly on (a) an accessor/module disagreement,
(b) an entry with no site anywhere (membership underivable), (c) a scanned name with no ledger row.
The ledger stays the single source of an entry's group/notify/reason; the scan decides only which
table(s) carry it.

| measure | single table (pre) | after the split |
| --- | --- | --- |
| entries | 50 | **sim 37 + tact 16, both 3, union 50** (printed by the generator; union==50 asserted) |
| per-group sim/tact | — | render-notify 3/3, sound 0/1, hook 6/0, display 1/4, input 1/4, chat 2/0, io 8/2, map-io 7/0, net 2/0, string 4/0, time 1/0, fatal 1/2, platform 1/0 |
| version handshakes | 1 | 2 (`LIBMH_HOST_API_VERSION`, `LIBMH_TACT_HOST_API_VERSION`) |
| unbound walks | 1 | 2 (`libmh_host_api_unbound`, `libmh_tact_host_api_unbound`), independent |

The sim table loses exactly the population that made it hard to abstract: the whole input pump,
all of `display` except `view_set_size_mode`, all of `sound`, and the 565→555/tlo converters. The
overlap is `GetResourseFilePtr`, `llm_view_set_size_mode`, `utils_abort`. Both hosts bind both
tables (mh.dll at arm time — the `[hostapi]` seam line prints both counts and both walks;
`net_selftest` in `main`), `hostapitest` grew the tact table's own handshake/holey-table/walk arms
(68 checks), and `hostapi_c_compile.c` proves both structs flat in plain C.

**Mutation-proven** (all three run 2026-09-10, then reverted): (1) adding a
`mh::tact_host().llm_time_get_ticks_ms` site to a tact TU put the entry in BOTH tables with no
hand edit — tact 16→17, both 3→4, union still 50, and only the TACT version constant moved
(0xAE411064→0x3ED2302F; sim's unchanged — per-table handshake independence). (2) Removing the only
site of `llm_snd_play_matching_sample` made the generator fail NAMING the entry rather than
silently shrinking a table. (3) A `mh::host().X` spelling inside tact/ is refused naming the file
and entry. The nulled-entry-per-table walk arms live in `hostapitest` (`utils_open_file` named by
the sim walk, `llm_input_key_dequeue` by the tact walk, and the sim walk proven to stay zero while
the tact table carries the hole).

## 8. The inbound surface inventory (host → libmh) — the FIRST measurement, superseded as the source

> **STATUS: HISTORICAL. Membership now lives in `tools/gen_libmh_inbound.py` (LIB-REF-IN, landed
> 2026-09-11) and is re-derived on every lint run.** Read this section for how the question was
> first asked and what it found; run the generator for the answer. The design that came out of it
> is `src/mh_dll/libmh/include/libmh_host_in.h` (the hand-authored contract) plus
> `libmh_host_in.gen.h` (the derived half: order ids, entry ids, the computed version).
>
> **This section does not reproduce, and that is the whole reason the generator exists.** Replaying
> §8's own model against the ledgers and call graph *as they stood at its own commit* `4d34326a`
> gives **47 rows / 120 edges / 94 callers**, not the 41/88/111 printed below. The gap is the 22
> **pruned roots** (`tools/data/reconciliation_pruned_roots.json`): the closure withdrew
> provably-dead host functions as seeds, so the numbers cannot be re-derived by anyone who does not
> also reproduce the prune. Two further drifts were measured the same way: `game_SetEvent` is **35**
> front-end sites, not 34 (`FUN_004bb944`, a widget `action_cb` reached only through a DATA
> reference — exactly the class a hand count misses), and §8c's TRANSLATE-LATER list is still 19
> rows but **not the same 19**.
>
> The generator answers a deliberately *different* question, and the difference is not a refinement
> but a correction. Section 5 of `report_promotion_reconciliation.py` asks a **pre-fork** question —
> which owned bodies can the original binary still walk into as originals — and answers it with
> every installed `MH_EXPORT_REPLACE` **cut** from the graph, so its count moves whenever a
> promotion is installed (it prints 4 today). An ABI's membership cannot depend on what happens to
> be installed the day it is read. At the fork there are no promotion seams at all, so the
> generator walks the graph **cut-free**, over **all six** per-function ledgers rather than `sim`
> alone, and classifies every original→owned edge by what the fork does with its caller.
>
> What it prints today: **139 candidate rows / 274 caller edges / 158 distinct callers → 103 covered
> by an inbound entry, 36 excluded with a written reason, 0 uncovered**, plus the hosted-routing
> census. It refuses on nine classes of gap — an undispositioned row, an unclassified caller, a
> stale adjudication row, an entry a row derives that the header does not declare, a declaration no
> row derives, an order parameter that cannot ride an `int32_t` argv, an unrouted seam, a
> mispacked `argv`, and a stale generated output — and it is wired into `lint_repo.py`.
>
> So: **keep this section, do not update it.** Editing the table below to match the tree would
> recreate precisely the failure it now documents — a hand list that reads as authoritative and
> cannot be re-derived. Its own instruction, "reproduce, do not re-type", is discharged by the
> generator, not by maintenance here.

**What follows is that first measurement, 2026-09-10, taken during SIM-HOSTREACH.** It is not a
proposed API and never was. It is the list of places where original front-end code calls a body we
own, taken from the reconciliation's own fixpoint over `tools/data/call_graph_no_crt.json`.

### 8a. Why these call sites are the inbound candidates

A call site here rides a PROMOTION SEAM today: original front-end code calls the original body, and
where we have installed an `MH_EXPORT_REPLACE` the entry is an E9 into ours. **That seam does not
exist at the fork.** A standalone host is not patching an image — it links `libmh.lib` and calls it,
so every front-end call site that today reaches an owned body by falling through a redirect needs a
designed public entry instead, `libmh_submit_order`-shaped. The promotion seams stay correct
pre-fork; the point is that they are the mechanism the fork removes.

The classification is per CALLER EDGE, not per row — a row can carry both classes:

- **(a) FORK-REPLACED** — the reaching caller is original front-end/UI/menu/render/input/camera code
  the fork's host reimplements. Its call into an owned body is a **host-inbound-API candidate**.
- **(b) TRANSLATE-LATER** — the reaching caller is original sim/logic code a future translation
  batch absorbs. The edge dissolves with the translation and implies **no inbound API**.

**Measured: 41 owned sim bodies, 88 distinct host callers, 111 edges — 84 FORK-REPLACED, 27
TRANSLATE-LATER.** 22 of the 41 rows carry at least one FORK-REPLACED edge; the other 19 are
TRANSLATE-LATER-only. (The 41 are the "topmost" half of section 5's 70 — the rows a host root calls
directly. The remaining 29 are reached only THROUGH those, so they imply no inbound entry of their
own and are not listed here.)

### 8b. The FORK-REPLACED candidates (22 rows, 84 call sites)

`game_SetEvent` is the canonical case and dwarfs the rest: **34 of the 84** front-end call sites
reach it, from every HUD button, panel tick, dialog and mode change in the game. It is the row
G167 was found on. Whatever the inbound API becomes, a `libmh_post_event`-shaped entry next to
`libmh_submit_order` is the thing 34 call sites are asking for.

| owned sim body | front-end call sites | what the host is doing there |
| --- | --- | --- |
| `game_SetEvent` | 34 | raise a game event on a user action (HUD buttons, panel ticks, dialogs, display/view mode changes, chat submit, debug console, return-to-menu) |
| `llm_strat_pixel_delta_wrapped` | 10 | wrapped-map pixel delta for drawing (unit/shadow/docked sprites, overlays, placement cursor, drag rect) |
| `llm_strat_unit_ctrlgroup_remove_member` | 6 | remove a unit from a control group (HUD row, drag-select) |
| `llm_strat_ctrl_group_contains_unit` | 5 | membership query behind the HUD's group edits |
| `llm_strat_unit_ctrlgroup_add_member` | 4 | add a unit to a control group (drag-select, HUD row, click-select) |
| `llm_strat_order_ctrlgrp_flash_member` | 3 | flash a group's members on selection |
| `llm_strat_order_ctrlgrp_select_member` | 3 | select a group member from the HUD |
| `llm_strat_tile_delta_wrapped` | 3 | wrapped tile delta for offscreen fx scale / sound attenuation / tile draw |
| `llm_strat_unit_get_coords` | 2 | unit position for the camera and the strategic draw |
| `llm_unit_state_is_boarding` | 2 | boarding state for selection markers and the unload button |
| `llm_bldg_finish_current_order` | 1 | the "finish this order now" button |
| `llm_bldg_footprint_is_clear` | 1 | per-frame placement check in the input poll |
| `llm_game_player_set_human` | 1 | debug: make every player human |
| `llm_prod_planet_distance_factor` | 1 | planet-selector tooltip |
| `llm_strat_planet_distance` | 1 | planet-selector tooltip |
| `llm_strat_bldg_get_coords` | 1 | pan the camera to a building |
| `llm_strat_bldg_shuttle_slot_is_free` | 1 | shuttle-slot query for the panel |
| `llm_strat_bldg_uses_workers` | 1 | building-panel draw |
| `llm_strat_locate_active_port` | 1 | colonist-status popup |
| `llm_strat_storage_purge_dead_docked` | 1 | storage-building panel draw |
| `llm_strat_tile_dist_wrapped` | 1 | camera snap to group 0 when offscreen |
| `llm_strat_unit_ctrl_group_assign` | 1 | assign a unit to a group from a HUD row |

Read the shape, not only the rows: most of these are **queries** the front-end makes about sim state
(coords, distance, membership, "is this slot free", "is it boarding") plus a small number of
**commands** (`game_SetEvent`, finish-order, group add/remove/select). A designed inbound surface is
likely a read-model plus a command entry, not 22 exported functions — but that is LIB-REF's call and
this section deliberately stops short of making it.

### 8c. The TRANSLATE-LATER-only rows (19, no inbound API implied)

Each is reached only from original sim/logic code, so the edge disappears when that caller is
translated. Listed so a later batch can see what it closes, and so nobody mistakes them for API:

`llm_diplomacy_set_relation` (`llm_diplomacy_restore_relations`) ·
`llm_map_merge_small_regions` (`llm_map_build_regions`) ·
`llm_map_region_pick_smaller` (`llm_map_assign_remaining_tiles_to_regions`) ·
`llm_resource_add` (`llm_strat_bldg_apply_cfg_resources`, `llm_strat_bldg_grant_resource_bonus`) ·
`llm_strat_bldg_clear_staffed_flag` + `llm_strat_bldg_set_staffed_flag` (`llm_strat_bldg_staffed_flag_toggle`) ·
`llm_strat_bldg_find_mothership_position` + `llm_strat_prod_spawn_arrived_unit` (`llm_strat_prod_shuttle_slot_spawn_arrival`) ·
`llm_strat_bldg_sprite_anchor_offset` (`llm_strat_bldg_init_defaults`) ·
`llm_strat_dir_from_to` (`llm_strat_dir_from_to_is_octant_sector`, `llm_tact_move_find_approach_tile`) ·
`llm_strat_facing24_to_delta` (the three pathfind compactors + `llm_strat_group_move_member_step_blocked`) ·
`llm_strat_fow_remove_sight` (`llm_strat_fow_remove_sight_r12`) ·
`llm_strat_path_attach_slot` (`llm_tact_move_commit_pending_path`) ·
`llm_strat_revoke_invention` (`SwitchToPlanet`) ·
`llm_strat_target_class` (`llm_strat_unit_approach_weapon_range`) ·
`llm_strat_unit_select_weapon` (`llm_strat_group_issue_attack_order`) ·
`llm_strat_unit_unlink_tile` + `map_unit_PutOnMap` (`llm_strat_unit_move_to_tile`) ·
`map_fow_UpdateFoWPlus` (`llm_map_fow_reveal_here`).

**Reproduce, do not re-type.** The measurement is `report_promotion_reconciliation.py` section 5
plus a caller classification; the classification of the 88 callers is the only hand-made part, and
it is the same kind of claim as the RESIDUE table in that tool — a name here needs a reason. If the
numbers above and section 5's disagree, section 5 is right and this section is stale.
