/* libmh_host_events.h -- the DESIGNED notify surface of the libmh host ABI (LIFT-EVQ,
 * docs/libmh-abi.md R7). HAND-AUTHORED on purpose, unlike libmh_host_api.gen.h: the generated
 * table is the mechanical translation-frontier surface that still moves; this envelope is the
 * stable designed contract the lift shrinks it into. C89-includable, same as the table.
 *
 * THE MECHANISM (user decision 2026-09-03, poll-queue generalized to callback form):
 * libmh emits fixed-size typed records. If the host bound a sink (libmh_set_event_sink), the
 * sink is invoked SYNCHRONOUSLY at emit -- the hosted config routes each record onto the
 * original fine thunk at the original instant, which is what keeps the R5 bit-identity oracle
 * exact. With no sink bound, records accumulate in an internal FIFO ring and the host drains
 * libmh_poll_events() at frame edge (the fork-side shape). Same emit sites, two consumptions.
 *
 * ORDER IS PART OF THE CONTRACT: records are delivered (sink) or drained (poll) in emit order.
 * The ring is fixed-size; on overflow the NEWEST record is dropped and counted
 * (libmh_event_overflow_count) -- every record on this surface is notify-class by
 * construction (R8: nothing here may feed hashed sim state), so a drop degrades presentation,
 * never the sim. Single-threaded by design: emits happen on the sim/frame thread only.
 *
 * KINDS grow per LIFT-NOTIFY/SCREEN conversion; each kind documents its a/b/c/d payload here.
 * Adding a kind BUMPS LIBMH_HOST_EVENTS_VERSION (hand-maintained -- bump on any change to
 * this contract: record layout, channel/kind meaning, ordering/overflow semantics). */
#ifndef LIBMH_HOST_EVENTS_H
#define LIBMH_HOST_EVENTS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LIBMH_HOST_EVENTS_VERSION 0x00010015u

/* channels (R7) */
#define LIBMH_EVC_EVENT      1u /* game events: text/sound/alert/progress */
#define LIBMH_EVC_SCREEN     2u /* screen/modal orchestration requests */
#define LIBMH_EVC_INVALIDATE 3u /* dirty marks: presentation caches to refresh */

/* kinds, per channel -- payload documented per kind */
/* LIBMH_EVC_INVALIDATE: */
#define LIBMH_EVK_INV_TACT_VIEW_TILES 1u /* the tactical view tile cache (whole viewport);   \
                                          * a/b/c/d unused. Replaces the fine entry          \
                                          * llm_tact_mark_view_tiles_dirty (the LIFT-EVQ     \
                                          * pilot conversion, 2026-09-03). */
/* LIBMH_EVC_INVALIDATE -- LIFT-NOTIFY slice 2 (2026-09-03). */
#define LIBMH_EVK_INV_VIEWPORT            2u /* strat viewport (llm_map_cam_mark_viewport_dirty) */
#define LIBMH_EVK_INV_TACT_VIS_MAP        3u /* (llm_tact_vis_map_fill_default) */
#define LIBMH_EVK_INV_TACT_VIS_MARGIN     4u /* (llm_tact_vis_map_clear_right_margin) */
#define LIBMH_EVK_INV_TACT_UNIT_SLOT      5u /* a=building_id (llm_tact_unit_refresh_ui_slot) */
#define LIBMH_EVK_INV_TACT_SIDEBAR_ROSTER 6u /* (llm_tact_ui_sidebar_roster_refresh) */
#define LIBMH_EVK_INV_TACT_SIDEBAR_ROWS   7u /* (llm_tact_ui_sidebar_draw_rows) */
#define LIBMH_EVK_INV_TACT_SIDEBAR_SLOT   8u /* a=unit_id (llm_tact_ui_sidebar_redraw_unit_slot) */
#define LIBMH_EVK_INV_TACT_PLAYER_ROWS    9u /* a=selected_row (llm_tact_ui_draw_player_row_list) */

/* LIBMH_EVC_INVALIDATE -- LIFT-TACT slice 1, the L4b calibration batch (2026-09-03).
 *
 * THESE ARE A DIFFERENT SHAPE FROM THE NINE ABOVE, and deliberately so. Kinds 1-9 each replaced
 * ONE fine entry. Kinds 10-13 each replace a whole body's DRAW SEQUENCE -- `set_draw_surface` +
 * `pack_rgb16` + `sprintf` + `ansi_to_wide` + `text_draw`... -- with a single scope id carrying the
 * VALUES that changed. The reason is that those primitives are arbitrary blits, not notifications:
 * a per-primitive kind would put pixel geometry (x/y/clip/packed colour) on an abstract boundary
 * R4 exists to keep it off. One scope per body is the cut R1 asks for; the literals stay behind in
 * mh.dll's sink, which reproduces the original call sequence at the original instant.
 *
 * CONSEQUENCE, stated rather than discovered later: the geometry assertions those bodies' offline
 * selftests used to make (x=0x8a, clip_h=0x18, the packed colour, the text-buffer trace) no longer
 * have anything in libmh to assert against -- they are the host's business now. The offline suites
 * keep the STATE and RECORD checks; the pixels are proven by the hosted oracle (UI capture diff).
 * That is a real narrowing of offline coverage for these bodies, and it is the intended effect of
 * the lift, not an oversight. */
#define LIBMH_EVK_INV_TACT_CHAR_PANEL_ROW 10u /* a=unit_id -- one character-panel row's readout    \
                                                * changed (llm_tact_ui_char_panel_row_refresh) */
#define LIBMH_EVK_INV_TACT_ACTIVE_COUNT_HUD \
    11u /* a=active_unit_count, b=active_unit_count_cached -- the two-line HUD readout of how many  \
         * squad members are alive / selected. The COUNTS cross, not the formatted strings: the     \
         * host formats and places them. Sink reproduces the original icon blit + two formatted     \
         * text draws (llm_tact_active_unit_count_hud_draw's draw tail). */
#define LIBMH_EVK_INV_TACT_SEL_PANEL_BG \
    12u /* the selection panel's background plate (the icon-1 blit at the head of                   \
         * llm_tact_selection_panel_refresh). SEPARATE from kind 13 on purpose -- see below. */
#define LIBMH_EVK_INV_TACT_SEL_PANEL \
    13u /* the selection panel's contents (llm_tact_ui_sel_panel_draw).                             \
         *                                                                                         \
         * WHY 12 AND 13 ARE TWO KINDS AND NOT ONE. "One scope per body" is the rule; this body     \
         * needs two records because its two draw points are NOT contiguous -- the background blit  \
         * runs first, then squad_roster_refresh + a roster/rows invalidate (real work, and libmh's \
         * own), and only then the panel contents. Merging them would move one draw across that     \
         * work, and under the hosted sink's synchronous dispatch the record fires where it is      \
         * emitted, so the merge would reorder the original. Order is the contract (R5). */

/* LIFT-TACT slice A. Same whole-body shape as kinds 10-13, and the same reason: the four order
 * buttons' draws are ONE arbitrary-blit primitive repeated with a different icon slot and a
 * different x/y literal, so a per-primitive kind would put pixel geometry on the boundary R4
 * exists to keep it off. What crosses instead is WHICH BUTTON was pressed, which is the thing a
 * host can actually act on -- a Godot host draws its own affordance for that button wherever it
 * likes, and never learns that the original put CLEAR at (1, 0x8f). The slot/x/y table lives in
 * mh.dll's sink. Exactly one of the four fires per tick (each arm returns). */
#define LIBMH_EVK_INV_TACT_ORDER_BUTTON \
    14u /* a = 0..3, the order button whose icon lights up: 0 CLEAR, 1 CLEAR+HOLD, 2 STOP,         \
         * 3 MOVE+STOP (llm_tact_ui_order_buttons_minimap_tick @0x00435cf1) */

/* The group-assign icon panel, redrawn when a group hit code (0x3c..0x45) is consumed
 * (llm_tact_sidebar_dispatch @0x00435a21). All-literal geometry, so nothing needs to cross. */
#define LIBMH_EVK_INV_TACT_GROUP_PANEL 15u

/* The selection panel's MODE TAB. Both mode ticks draw the same tab at the same origin and differ
 * only in which icon they use, so what crosses is the mode the panel just switched TO -- which is
 * the thing that happened, and the thing a host with its own tab widget can act on.
 * a = 0 single-select (llm_tact_ui_sel_panel_single_mode_tick @0x00436a3c, icon slot 3),
 *     1 multi-select  (llm_tact_ui_sel_panel_multi_mode_tick  @0x00436217, icon slot 4). */
#define LIBMH_EVK_INV_TACT_SEL_PANEL_MODE_TAB 16u

/* A per-unit-row toggle button in the multi-select panel lighting up. b is the ROW, not a y
 * coordinate: the original derives y from it (row*0x30 + a per-toggle offset) and that arithmetic
 * is panel layout, so it stays with the sink.
 * a = 0 defense stance (@0x00436396, icon slot 0x26), 1 active gun (@0x00436420, slot 0x27). */
#define LIBMH_EVK_INV_TACT_SEL_PANEL_ROW_TOGGLE 17u

/* LIFT-TACT slice A, the two PURE-PRESENTATION bodies that left libmh WHOLE (2026-09-09).
 *
 * A DIFFERENT SHAPE AGAIN from kinds 10-17, and the difference is worth stating because it is the
 * point of the lift rather than a detail of it. Kinds 10-17 kept the translated body in libmh and
 * moved only its draw sequence out. These two kinds replace a body that is GONE: the whole of
 * llm_tact_ui_char_panel_row_draw and llm_tact_ui_sidebar_row_draw_right is presentation --
 * measured, not assumed, their entire write set is tile_vis_map dirty bytes, which is NOT a
 * TACT_HASH_REGIONS member and has no translated reader at all -- so there was nothing to hoist
 * above the cut (R2) and nothing left in libmh once the draws crossed. What crosses is the roster
 * identity the original computed: which unit, in which row. The sink calls the original bodies. */
#define LIBMH_EVK_INV_TACT_CHAR_PANEL_ROW_DRAW \
    18u /* a = unit_idx, b = row_slot -- the character panel redraws one sidebar roster row after a \
         * click on it (llm_tact_ui_char_panel_row_draw @0x00434322, from                          \
         * llm_tact_sidebar_dispatch's two hit-code arms). Absorbs the five fine entries the body   \
         * called: set_draw_surface, pack_rgb16, text_draw_rgb16, char_panel_ammo_draw,             \
         * unit_draw_hp_bar_slot. */

/* One roster row lighting up under the cursor. The two sides are ONE kind because they are one
 * gesture with a side to it -- the original split them into two functions, and only by translation
 * order was one of them ours and the other a host entry (llm_tact_ui_sidebar_row_draw_left was
 * never translated). `highlight_flag` does NOT cross: both call sites in
 * llm_tact_ui_sel_panel_single_mode_tick pass the literal 1, so it is the original's
 * parameterisation and not information about what happened; the sink passes it.
 * a = 0 the UNASSIGNED list (left, @0x0043468d), 1 the ACTIVE-GROUP list (right, @0x004348bf)
 * b = unit_id, c = row */
#define LIBMH_EVK_INV_TACT_SIDEBAR_ROW_HOVER 19u

/* The selection panel's ONE-TIME INIT draw, at mission start (llm_tact_ui_sel_panel_init
 * @0x00433e94). This one is a SPLIT, not a whole-body move, and the split line is the thing worth
 * recording: everything up to and including the two labels crosses -- the icon bank's load and
 * 565->555 conversion, the three fixed background panels (icon slots 0, 1 and 3; slot 2 is skipped,
 * and that is the original's own base+offset arithmetic, not a tidy-up), both font selections and
 * both label draws -- while the `ui_sel_panel_multi_mode = 0` reset and the refresh/redraw/clear
 * tail STAY in libmh, because libmh reads that latch every tick and the tail re-enters libmh's own
 * converted bodies.
 *
 * THE ICON POINTER ARRAY GOES WITH THE DRAWS, which is what settles the deferral docs/libmh-abi.md
 * section 4 opened. Every consumer only ever blitted the pointer or read its (w,h) header as the
 * clip for that same blit -- never for a decision -- so once the draws are host-side, no translated
 * body reads or writes the array at all. The one corroborating detail: its deallocator
 * llm_tact_ui_sel_panel_free_gfx @0x00434051 was never translated and never a host entry, so libmh
 * was allocating into an array it could not free. It is the host's now, both ends.
 * a/b/c/d unused. */
#define LIBMH_EVK_INV_TACT_SEL_PANEL_INIT 20u

/* LIFT-TACT slice B -- llm_tact_frame's own draw calls, converted IN PLACE (docs/libmh-abi.md
 * section 3). The deeper input/sim/render split was measured and rejected: it removes the SAME
 * entries, so it buys zero surface delta while restructuring the one body that is promoted and
 * cannot be shadow-armed. `llm_tact_render_view` is NOT here -- it stays REQUIRED under R3b,
 * because tact_frame reads the hover state it writes at nine sites later in the same frame. */
#define LIBMH_EVK_INV_TACT_CURSOR_SPRITE   21u /* a = cursor sprite id (2 = default, 1 = over a unit). An ASSET ID, not a bitmap: the               * host owns which cursor that names. Note the id is chosen from LAST frame's                       * hovered_unit_id -- render_view updates it later, at step 15 -- and that is the                   * original's own ordering, preserved. (gfx_LoadSprite) */
#define LIBMH_EVK_INV_TACT_DRAWN_MAP_CLEAR 22u /* the blink/drawn-cell overlay cache                                                               * (llm_tact_blink_overlay_clear) */
#define LIBMH_EVK_INV_TACT_DRAG_BOX        23u /* a=anchor_x, b=anchor_y, c=cursor_x, d=cursor_y -- the selection rectangle being                  * dragged. THE ONE RECORD ON THIS SURFACE THAT CARRIES RAW COORDINATES, and                       * deliberately: the rectangle IS the state, there is no coarser identity to substitute,            * and R4 admits positions alongside identities. The original's only accumulator                    * underneath is a marching-ants dash-phase byte, so a host animates the outline from               * these four numbers alone. (llm_tact_drag_box_clamp) */
#define LIBMH_EVK_INV_TACT_MINIMAP_OVERLAY 24u /* the minimap overlay layer                                                                        * (llm_tact_tile_overlay_refresh) */

/* LIFT-TACT slice B2 -- the mission START/END one-shots. THE CLUSTER THE 2026-09-03 OPEN MEASURED
 * AND THEN LEFT WITHOUT A DECISION ROW; taken into LIFT-TACT 2026-09-09 and decided per entry.
 *
 * WHY NINE KINDS AND NOT TWO. "Prepare the tactical presentation" / "restore the strategic one"
 * is the cut R1 would reach for, and it is wrong here: the calls are NOT contiguous. libmh writes
 * window_width / view_tiles_w / grid_width BETWEEN them, and those writes are the INPUTS the
 * metrics-init calls read -- so merging would move a host call across the state it consumes.
 * Order is the contract (R5), so each converts where it stands.
 *
 * These are presentation CACHES being rebuilt, which is what puts them on the invalidate channel
 * rather than the screen one: a host with its own renderer reloads its own art. */
#define LIBMH_EVK_INV_GFX_VIEW_METRICS    25u /* recompute the render view's metrics from the window/tile size libmh has just written             * (llm_gfx_view_metrics_init). Its output _G_LLM_GFX_PANEL_ROW_SKIP had a translated               * reader in the same call chain until slice A moved ui_sel_panel_init's draw head                  * host-side; with that gone the R3b readback dissolved and it converts plain. */
#define LIBMH_EVK_INV_TACT_VIEW_METRICS   26u /* the tactical view's own metrics + tile buffers (llm_tact_view_metrics_init). It                  * RE-ALLOCATES the tile_vis_map buffers, so a stale pointer would be a dangling one                * rather than an old value -- which is why the disposition is a measurement and not an             * argument: its only translated readers are tact_view_shift's four, reached solely from            * llm_tact_frame, while llm_tact_mission_start is reached solely from                              * llm_strat_try_enter_tactical_mission. They are structurally in different frames, so a            * frame-edge drain lands before the first tactical frame reads anything. */
#define LIBMH_EVK_INV_TACT_VIEW_TILE_ROWS 27u /* the tactical tile-row pointer table                                                              * (llm_tact_gfx_view_tile_rows_init) */
#define LIBMH_EVK_INV_TACT_SPRITE_BANKS   28u /* load the TACTICAL sprite banks                                                                   * (llm_tact_gfx_load_banks_alt) */
#define LIBMH_EVK_INV_ALL_SPRITE_BANKS    29u /* reload the strategic sprite banks on the way out                                                 * (llm_gfx_load_all_sprite_banks) */
#define LIBMH_EVK_INV_SPRITE_PIX_OFFSETS  30u /* rebuild the sprite pixel-offset table                                                            * (llm_gfx_sprite_pix_offsets_init) */

/* LIFT-TABLE S3 (2026-09-09): cfg_final_planet_Construct SPLITS -- its sim writes stay in libmh,
 * its per-planet GRAPHICS tail (0x0045b8fe to the end of the body) becomes this one record. The
 * tail's only inputs are the tileset index and the planet slot; its outputs -- Planets[].tlo_index,
 * Planets[].soldier_sprite_bank_offset and the eight cfg_final_planet_FillBankData calls -- are
 * read by renderers and bank loaders only, and LIFT-TABLE S2 has just taken all of them out of the
 * determinism hash, which is what makes a host-owned writer legal here.
 *
 * WHY libmh STILL COMPUTES tlo_index AND THE RECORD CARRIES IT. cfg_GetTloIndex reads
 * map_header.tlo_name -- a STACK LOCAL of the emitting body, filled one line earlier by the host's
 * own cfg_ReadMapFile entry. Passing that pointer would work under mh.dll, whose sink dispatches
 * synchronously at emit, and would DANGLE under a NULL-callback host, which drains at the frame
 * edge with the frame long gone. So the record carries the resolved index (an identity, R4) and
 * the raw VA edge to cfg_GetTloIndex stays where it already belongs, in LIB-BOOT's cfg-snapshot
 * set. */
#define LIBMH_EVK_INV_PLANET_GFX_SETUP \
    31u /* a=planet_index, b=tlo_index -- per-planet graphics setup (the tail of                   \
         * cfg_final_planet_Construct @0x0045b69c) */

/* LIFT-TABLE S4 (2026-09-09): the planet-map PALETTE block leaves libmh. R1, not R3's internalize
 * branch (section 0a): llm_gfx_pack_rgb16's only libmh caller was the nine-call block at the end of
 * llm_strat_planet_map_session_init, filling nine MF_VIEW/OWN_ISLAND cells -- presentation sitting
 * in the sim store because migration put it there. The cut goes ABOVE the leaf: the block leaves
 * with its cells and the colour-packing entry disappears from the table without anyone
 * reimplementing colour packing inside the deterministic core.
 *
 * NO PAYLOAD, because the block takes none: nine calls with nine literal colour triples, in a fixed
 * order, at a fixed instant. MEASURED, and this is the part worth carrying: all nine cells are
 * WRITE-ONLY IMAGE-WIDE -- one WRITE xref each and no reader anywhere, in libmh, in the seams or in
 * the original -- so the UI capture section 6 named as this stage's oracle would have been vacuous
 * (no pixel depends on them). The oracle is the reader census plus the hosted arm. */
#define LIBMH_EVK_INV_PLANET_MAP_PALETTE \
    32u /* the strategic planet-map's two small RGB palettes (the tail of                          \
         * llm_strat_planet_map_session_init @0x004dc65a) */

/* SIMABI-NOTIFY (2026-09-10): the two no-payload invalidates of the sim table's four CONVERTs.
 *
 * NO PAYLOAD IN EITHER CASE, and for the INV_PLANET_MAP_PALETTE reason rather than by omission:
 * each names an INSTANT at which a presentation cache must be rebuilt from state the host can
 * already read (R4 -- the host reads state, libmh does not push it). Neither takes an argument in
 * the original either. */
#define LIBMH_EVK_INV_PLAYER_COLOR_LUT \
    33u /* every player's display colour re-derived from their configured colour index             \
         * (llm_strat_player_apply_all_colors @0x00454c04, a load's apply phase).                  \
         *                                                                                         \
         * WHAT IT ACTUALLY WRITES, measured rather than named: the body is a 0..7 loop over        \
         * llm_strat_player_set_color(i, _G_LLM_STRAT_PLAYERS[i].color_index), and that callee's    \
         * ONLY write is _G_LLM_STRAT_PLAYER_COLOR_LUT[player_idx] @0x00ae1948 (16 B, eight         \
         * packed RGB565 words). That address is in NEITHER hash table -- checked against every     \
         * HASH_REGIONS and TACT_HASH_REGIONS extent in mh_regions.gen.h -- and no translated body  \
         * reads the LUT at all, which is the same measurement LIBMH_EVK_PLAYER_SET_COLOR's own     \
         * R3b disposition already carries for the same global.                                    \
         *                                                                                         \
         * IT READS strat_players[].color_index, WHICH *IS* HASHED (HIDX_STRAT_PLAYERS), so the     \
         * R3c question is live and answered at the site: nothing between the emit                  \
         * (save_live.cpp:814) and the end of that call writes a colour index -- libmh's only       \
         * writer of the field is llm_strat_player_profile_init (sim_player_init.cpp:74), a         \
         * session-begin path the load's apply phase does not reach. A poll host re-reads the same  \
         * eight indices it would have read at emit. */
#define LIBMH_EVK_INV_PLANET_EXTRA_SPRITE_BANKS \
    34u /* the current planet's extra sprite banks (50..99, BANK_%02d.BNK) reloaded                 \
         * (llm_gfx_load_planet_extra_sprite_banks @0x004657ac).                                    \
         *                                                                                         \
         * PURE PRESENTATION SINCE LIFT-TABLE S2/S3, which is what makes a gfx leaf leaving the     \
         * table legal (section 0a): the bank[] table it walks is host-owned and out of the         \
         * determinism hash. It sat in the `map-io` group only because it reaches the disk the      \
         * same way, and its notify:false was the S1 class-string trap that group inherited -- the  \
         * entry is void, its one libmh site is unconditional, and no return or out-param exists    \
         * to consume. Its subtree is a SUBSET of LIBMH_EVK_INV_ALL_SPRITE_BANKS's (the original    \
         * llm_gfx_load_all_sprite_banks calls it), so the sprite-table readback carries the same   \
         * later-frame disposition that scope already measured. */

/* LIBMH_EVC_EVENT -- the sound family (LIFT-NOTIFY slice 1, 2026-09-03). Each kind names the
 * fine entry it replaced; the hosted sink routes it onto that entry's thunk. */
#define LIBMH_EVK_SND_PLAY          1u /* a=sound_id, b=volume (llm_snd_play) */
#define LIBMH_EVK_SND_AMBIENT_TICK  2u /* per-frame ambient pacer (llm_snd_ambient_tick) */
#define LIBMH_EVK_SND_AMBIENT_CLONE 3u /* a=planet_id (llm_snd_ambient_planet_clone) */
#define LIBMH_EVK_SND_STOP_ALL      4u /* (llm_snd_stop_all_channels) */
#define LIBMH_EVK_SND_RACE_ALERT    5u /* (llm_strat_race_alert_sound_emit) */
#define LIBMH_EVK_SND_ZONE_PLAY     6u /* a=sound_id (llm_tact_zone_sound_play) */
#define LIBMH_EVK_SND_FX_PLAY       7u /* a=fx_type, b=volume_pct, c=pan (llm_tact_fx_play_sound) */
/* LIBMH_EVC_EVENT -- text/progress/tail family (LIFT-NOTIFY slice 2, 2026-09-03). */
#define LIBMH_EVK_TEXT_QUEUE_ID           8u  /* a=text_id (llm_ui_print_queue_text_id) */
#define LIBMH_EVK_TEXT_GAME_SPEED         9u  /* (llm_ui_print_game_speed) */
#define LIBMH_EVK_TEXT_RACE_ALERT         10u /* (llm_strat_race_alert_text_emit) */
#define LIBMH_EVK_PROGRESS_UNIT_AVAILABLE 11u /* a=player, b=unit_proto (llm_progress_notify_...) */
#define LIBMH_EVK_PLAYER_SET_COLOR        12u /* a=player_idx, b=color_index (llm_strat_player_set_color) */
#define LIBMH_EVK_MSG_QUEUE_CLEAR         13u /* (llm_ui_message_queue_clear_all) */
#define LIBMH_EVK_CAM_JUMP_QUEUE_CLEAR    14u /* (llm_cam_jump_queue_clear) */
#define LIBMH_EVK_MENU_PLACEMENT_CLEAR    15u /* (llm_menu_build_placement_pending_clear) */
/* 16/17 are the one pair on this surface that report a state change libmh ALREADY MADE, rather
 * than requesting one. libmh writes the camera column/row itself at the emit site and then sends
 * this record; a host applies whatever its own view needs (mh.dll routes it to the original
 * llm_map_cam_set_col/_row, which re-stores the same value and derives its scroll offset from it).
 * The reason is R3b: llm_strat_spawn_enemy_landing reads the camera back in the same frame to
 * derive the HASHED landing_x/landing_y fallback, so a frame-edge drain would feed it last frame's
 * camera. A host that ignores these records entirely still gets a correct sim. */
#define LIBMH_EVK_CAM_SET_COL       16u /* a=col (llm_map_cam_set_col) -- POST-hoc, see above */
#define LIBMH_EVK_CAM_SET_ROW       17u /* a=row (llm_map_cam_set_row) -- POST-hoc, see above */
#define LIBMH_EVK_VIEW_ZOOM_SCALE   18u /* a,b = bit pattern of double zoom_x (lo,hi);           \
                                        * c,d = zoom_y likewise (llm_map_set_zoom_scale) */
#define LIBMH_EVK_VIEW_MINIMAP_ZOOM 19u /* (llm_map_set_minimap_zoom_for_size) */
/* LIBMH_EVC_EVENT -- position-carrying records (the LIFT-NOTIFY offscreen conversion, 2026-09-03). These two DELETE
 * host entries whose RETURN fed sim call sites (an R3 violation): the host attenuates/scales from
 * ITS camera instead of answering a query. The hosted sink reproduces the original call pair
 * (llm_strat_offscreen_snd_volume -> llm_snd_play; llm_strat_offscreen_fx_scale -> x87 intensity ->
 * llm_strat_spawn_debris_burst) synchronously at emit, bit-identically. */
#define LIBMH_EVK_SND_PLAY_AT     20u /* a=sound_id, b=tile_col, c=tile_row -- positional play;      \
                                      * replaces every offscreen_snd_volume+snd_play pair */
#define LIBMH_EVK_FX_DEBRIS_BURST 21u /* a=tile_col, b=tile_row, c=energy_max -- screen-shake burst \
                                       * for a building death; replaces the offscreen_fx_scale ->  \
                                       * spawn_debris_burst chain */

/* LIFT-TACT slice B. A player pressed the screenshot key; whether that writes a PCX beside the exe,
 * opens a share sheet or does nothing at all is the host's to decide. (llm_tact_save_screenshot) */
#define LIBMH_EVK_SCREENSHOT_SAVE 22u

/* LIBMH_EVC_SCREEN -- screen/modal orchestration (LIFT-SCREEN). A record here
 * is a REQUEST to open, replace or dismiss a screen; it never answers anything. The modal whose
 * answer the sim genuinely consumes (the lockstep kick modal) does not answer through this channel
 * either -- its answer arrives as a pushed input, see R9 in docs/libmh-abi.md.
 *
 * Every kind below replaced a fine entry whose LOAD-BEARING state writes were already hoisted into
 * libmh at the call site by LIB-ABI stage E, so what crosses is the visual remainder only. Three of
 * the replaced entries were declared `int` by the original: those returns are Watcom
 * uncommitted-return artefacts with no consumer anywhere (measured at LIFT-SCREEN), and the
 * adapters that keep those member signatures return a constant 0. */
#define LIBMH_EVK_SCR_OUTCOME_DIALOG     1u /* a=outcome code (4=defeat, 5=victory, 6/8=other,     \
                                         * 7=peer-removed) -- the game-over/outcome modal      \
                                         * (llm_ui_outcome_dialog) */
#define LIBMH_EVK_SCR_OVERLAY_DISMISS    2u /* tear down the lockstep wait/sync overlay            \
                                          * (llm_net_lockstep_overlay_dismiss) */
#define LIBMH_EVK_SCR_WAIT_PLAYER_SHOW   3u /* a=player_idx -- the "waiting for player" overlay   \
                                           * (llm_net_lockstep_wait_player_overlay_show) */
#define LIBMH_EVK_SCR_MP_LEAVE_RESET     4u /* leaving/ending an MP session: restore the saved mode \
                                         * and drop the overlay (llm_net_mp_leave_reset_game_mode) */
#define LIBMH_EVK_SCR_BLDG_PANEL_OPEN    5u /* open the strategic building panel, reset its scratch \
                                          * (llm_ui_bldg_panel_open) */
#define LIBMH_EVK_SCR_PLANET_SELECT_OPEN 6u /* a=open_arg -- the planet-select screen             \
                                             * (llm_ui_planet_select_screen_open) */
#define LIBMH_EVK_SCR_DLG_FROM_TABLE     7u /* a=LIBMH_SCR_DLGT_* -- build a dialog from a named     \
                                         * layout (llm_ui_dlg_build_from_table). The original    \
                                         * argument is a raw table ADDRESS, which cannot cross an \
                                         * abstract boundary (R4): the record carries an id and   \
                                         * the host owns the table it names. */

#define LIBMH_EVK_SCR_SYNC_OVERLAY_SHOW 8u /* the lockstep stall KICK MODAL: "Player not      \n                                            * responding", with Reset / Disconnect / Quit      \n                                            * (llm_net_lockstep_sync_overlay_show). This is the \n                                            * ONE screen whose answer the sim consumes; the     \n                                            * answer comes back through the submit call below,  \n                                            * never as a return. */

/* ---- the TUTORIAL's choreography (LIFT-RESID slice 2) ---------------------------------------
 *
 * llm_tutorial_step_driver's per-frame tail is pure presentation: restore the tutorial's menu UI
 * state, redraw the backdrop, centre + draw its widget list, and either restore-on-done or draw
 * the cursor and flip. None of it feeds hashed state (R8), so each leaves as a record and the
 * host performs it at the original instant.
 */
#define LIBMH_EVK_SCR_TUT_UISTATE_RESTORE 9u  /* restore the tutorial's saved menu UI state                                                       * (llm_menu_tutorial_uistate_restore) */
#define LIBMH_EVK_SCR_MENU_BG_REDRAW      10u /* repaint the menu backdrop (llm_ui_menu_bg_redraw_cb). The original returns an int that           * every libmh site discards. */
#define LIBMH_EVK_SCR_CURSOR_MENU_DRAW    11u /* draw the menu cursor (llm_gfx_draw_cursor_menu) */
#define LIBMH_EVK_SCR_PRESENT_FLIP        12u /* present the composed frame (llm_gfx_present_flip) */
#define LIBMH_EVK_SCR_FONT_DESC_FOR_FLAGS 13u /* a=style_flags -- select the font description for a widget's style                                * (llm_gfx_font_desc_for_flags). The original returns a descriptor POINTER; every libmh            * site discards it (both are dead stack stores in the asm), so nothing crosses back. */
#define LIBMH_EVK_SCR_WGTL_CENTER         14u /* a=LIBMH_SCR_WGTL_* -- centre a named widget list (llm_ui_widget_list_center) */
#define LIBMH_EVK_SCR_WGTL_DRAW           15u /* a=LIBMH_SCR_WGTL_* -- draw a named widget list (llm_ui_widget_list_draw) */
#define LIBMH_EVK_SCR_FADE_TRANSITION_RUN 16u /* run the ALREADY-ARMED screen fade transition to completion. libmh arms the transition            * by writing its own src/dst screen ids, then asks for it with this record; the host               * decides what "to completion" costs -- mh.dll ticks the original                                  * llm_ui_screen_fade_transition_tick until it reports done, a host with no fade returns            * immediately. THE POINT IS THE POLL LOOP: the original body spun on the tick entry, and           * a libmh loop may not poll a host entry (a host that answers 0 forever would hang the             * library). Blocking is the HOST's business because the host owns the frame loop. */
#define LIBMH_EVK_SCR_TUT_HINT_LAYOUT     17u /* a=ui sprite id -- lay out the tutorial hint widget's geometry from that sprite's                 * metrics (llm_gfx_sprite_width / llm_gfx_ui_sprite_get_header_field2 + the four                   * assignments the original derives from them). The metrics are ASSET queries whose only            * consumer is this layout, and _G_LLM_UI_TUTORIAL_HINT_WIDGET is MF_VIEW-only, unhashed            * and host_free, so widget and arithmetic move host-side together. libmh keeps the                 * widget's .label and reads its .flags; it never reads the geometry back. */

/* LIFT-TACT slice B -- the two tactical frame-boundary scopes.
 *
 * BLAST_TRANSITION IS A COLLAPSED LOOP, and that is the cut R1 asks for rather than a shortcut.
 * The original runs a FIXED sixteen iterations of (scroll_fade_step, blink_overlay_clear,
 * frame_cursor_and_reset) when a mine blast ends the mission, and nothing of libmh's happens
 * between iterations -- so what a host needs to know is "play the exit wipe", not forty-eight fine
 * steps. It also absorbs llm_tact_scroll_fade_step entirely, that loop being the entry's only site.
 * Not a spin, unlike SCR_FADE_TRANSITION_RUN: the bound is a compile-time 16, so no host answer can
 * hang it -- but the frame loop is the host's for the same reason, and mh.dll's sink runs the
 * original sixteen-iteration loop verbatim. */
#define LIBMH_EVK_SCR_TACT_BLAST_TRANSITION 18u
/* Present the composed tactical frame and reset the per-frame cursor/tile state
 * (llm_tact_frame_cursor_and_reset). The tactical sibling of SCR_PRESENT_FLIP, and terminal on
 * every path that reaches it -- which is why it is not an R3b hazard despite its subtree writing
 * the framebuffer and window metrics: nothing of libmh's runs after it in the same frame. */
#define LIBMH_EVK_SCR_TACT_FRAME_PRESENT 19u

/* LIFT-TACT slice B2 -- the mission boundary's three screen-level requests. */
#define LIBMH_EVK_SCR_TACT_MISSION_MEDIA 20u /* the mission's intro media/splash page
                                              * (llm_ui_info_media_draw_p1) */

/* SIMABI-NOTIFY (2026-09-10): the STRATEGIC frame's two presents -- the sibling of
 * SCR_TACT_FRAME_PRESENT, and they convert for that precedent's reason.
 *
 * Both are the LAST STATEMENT of the body that emits them (sim_lt_frame.cpp:67 and :78, the
 * translated llm_strat_frame / llm_strat_frame_redraw_behind_dialog), so nothing of libmh's runs
 * after them in that frame and there is no same-frame readback to defer (R3b). Neither returns
 * anything (R3), and neither names state libmh then mutates (R3c) -- the frame is over.
 *
 * Note what is NOT converted with them: llm_strat_input_update stays REQUIRED at the head of the
 * same body (R10, the declared pre-fork input wall). The frame's two ends are a host's; its input
 * pump is a contract. "Host owns the frame loop" remains a fork/D1 goal -- these records keep the
 * hosted config's synchronous dispatch at the original instant (R5/R7). */
#define LIBMH_EVK_SCR_STRAT_FRAME_PRESENT \
    21u /* present the composed strategic frame -- the view render plus the flip                   \
         * (llm_strat_render_present @0x0044e578, = llm_strat_render_view + llm_frame_present) */
#define LIBMH_EVK_SCR_STRAT_FRAME_REDRAW \
    22u /* repaint the strategic scene BEHIND an open dialog, without the flip                     \
         * (llm_strat_render_view @0x0044e5a4). Distinct from kind 21 rather than folded into      \
         * it: the redraw path deliberately does not present, and which of the two the original    \
         * ran is the thing that happened. */
/* THE TWO RESOLUTION CHANGES ARE NOT HERE, and their absence is a decision rather than an
 * oversight. llm_gfx_apply_window_resolution and llm_gfx_apply_resolution_change were converted
 * in this slice and then REVERTED on the R3b gate's measurement: their closures are 81 and 107
 * functions and they re-derive fifteen and sixteen regions that translated libmh reads, with a
 * same-call readback in each emitter (mission_start's intro blit reads gfx_draw_surface and
 * gfx_framebuffer at :150-151; mission_end calls map_LoadPlanetFromDisk, which reaches the
 * map-region-pool readers, on the line after). They stay REQUIRED. A host reconfiguring the
 * display that libmh then depends on is not a notification.
 *
 * THE THIRD ONE IS, and the difference is measured rather than felt. SIMABI-DISPLAY (2026-09-10)
 * converts llm_view_set_size_mode, whose closure reaches the same window teardown/recreate the two
 * above do -- but its libmh consumption is ONE assignment, and that is what R3 is about. See kind
 * 23. */
#define LIBMH_EVK_SCR_SET_DISPLAY_MODE \
    23u /* a=size_mode (0 = 640x480, 1 = 800x600, 2 = 1024x768) -- ask the host to apply a          \
         * strategic-view display size (llm_view_set_size_mode @0x0044e47d).                        \
         *                                                                                         \
         * A NOTIFY DESPITE A NON-VOID ORIGINAL, which is the whole content of this row. The        \
         * original returns the APPLIED mode -- an OS-granted resolution readback: it calls         \
         * llm_gfx_apply_window_resolution and then re-derives the mode from WindowWidth, which     \
         * llm_gfx_set_window_resolution snaps to one of 1024/800/640 and falls back to 640 on an   \
         * unsupported mode. libmh cannot honestly compute that value (gfx logic is out of the      \
         * core), and it does not need to: its ONE site stored the return into                      \
         * _G_LLM_VIEW_SIZE_MODE, which is RID_VIEW_SIZE_MODE, MF_VIEW-only and in NEITHER hash     \
         * table -- checked by address overlap against every HASH_REGIONS and TACT_HASH_REGIONS     \
         * extent, not by citing a row. The game-modes research doc records the cell as a per-machine  \
         * user preference (setup.dat), not sim state.                                              \
         *                                                                                         \
         * THE HOST STILL OWNS THE CELL. The hosted sink performs the original caller's assignment  \
         * (it stores the real return into RID_VIEW_SIZE_MODE), so retail behaviour is unchanged    \
         * for the untranslated original readers -- llm_ui_outcome_dlg_open, llm_ui_menu_close_to_  \
         * hud, llm_strat_input_update -- and for tact, which READS the cell (tact_mission_end_     \
         * return_to_strategic.cpp:39). libmh no longer WRITES it; a headless host with no window   \
         * simply leaves it alone.                                                                  \
         *                                                                                         \
         * GATED LIVE BEFORE IT MOVED (the register's own caveat, docs/libmh-sim-abi.md 2c).        \
         * `test_ui.py --ui-abc tutorial_solo`, 18,317 compared steps, twice: with the entry bound  \
         * as a fixed-return-0 notify, and again with libmh storing a value the entry CANNOT return \
         * here (2). Both green on every required channel against arm B (all-original, return-      \
         * consuming) and arm C (the committed recording). The second run is the one that carries   \
         * the claim: at this site the real return is provably 0 (mode 0 -> WindowWidth 640 ->      \
         * 799 < 640 is false), so the fixed-0 arm alone would have been a vacuous green.           \
         *                                                                                         \
         * TACT KEEPS ITS OWN ENTRY. The frozen tact table's copy is untouched -- its site          \
         * discards the return anyway -- so the shared-entry overlap goes 3 -> 2. */

/* dialog-layout ids for LIBMH_EVK_SCR_DLG_FROM_TABLE. One member so far -- the only call site in
 * libmh is llm_menu_force_return_to_main's, which passes 0x00656d6a (llm_ui_dlg_table_00656d6a). */
#define LIBMH_SCR_DLGT_MAIN_MENU_QUIT 1u

/* widget-list ids for LIBMH_EVK_SCR_WGTL_CENTER / _DRAW. Same R4 reasoning as the dialog tables
 * above: the original passes a raw widget-list ADDRESS and an address cannot cross an abstract
 * boundary, so the record names a list and the host owns the address. Both members are the
 * tutorial's own lists; llm_tutorial_step_driver is the only libmh caller today, and it reaches
 * DONE only through a slot it assigned on the line before, so both sites are provably constant. */
#define LIBMH_SCR_WGTL_TUTORIAL_STEP    1u /* _G_LLM_UI_WGT_LIST_TUTORIAL_STEP @0x00653b17 */
#define LIBMH_SCR_WGTL_TUTORIAL_DONE    2u /* _G_LLM_UI_WGT_LIST_TUTORIAL_DONE @0x00653baf */
#define LIBMH_SCR_WGTL_TUTORIAL_INTRO   3u /* _G_LLM_UI_WGT_LIST_TUTORIAL_INTRO @0x00653acb */
#define LIBMH_SCR_WGTL_TUTORIAL_WELCOME 4u /* _G_LLM_UI_WGT_LIST_TUTORIAL_WELCOME @0x00653b63 */

/* ---- the SCREEN channel's answer path (R9) --------------------------------------------------
 *
 * A screen record is a REQUEST and never answers. Exactly one screen's answer is consumed by the
 * sim -- the lockstep kick modal -- and it arrives PUSHED, the same direction as input (R10): the
 * host submits it whenever the viewer presses the button, libmh holds it in its own slot, and the
 * sim reads that slot at the point the original polled the game's global.
 *
 * The value SURVIVES until libmh consumes it or the host overwrites it, because the sim samples
 * on its own schedule (once per stall-retry tick, not once per frame) -- a submit that only lived
 * for one frame would be lost between samples. Submitting the "none" value (-1) withdraws an
 * unconsumed answer. Not a determinism input in the R8 sense: it is a local player's UI action,
 * and what it drives (a peer-removal request) is broadcast through the normal lockstep path. */
#define LIBMH_SCR_ANS_LOCKSTEP_KICK 1u /* value = the player index to disconnect, -1 = none */

void libmh_submit_screen_answer(uint32_t answer_id, int32_t value);

typedef struct libmh_event {
    uint16_t channel;    /* LIBMH_EVC_* */
    uint16_t kind;       /* LIBMH_EVK_*, meaning scoped by channel */
    int32_t  a, b, c, d; /* kind-specific payload (ids / indexes / positions), else 0 */
} libmh_event;

typedef void (*libmh_event_sink)(const libmh_event *e);

/* Bind (or clear, with NULL) the synchronous sink. Returns 0 on success, -1 on a version
 * mismatch (the working binding, if any, is kept). Binding a sink does not flush the ring;
 * poll any backlog first if emits could have preceded the bind. */
int libmh_set_event_sink(libmh_event_sink sink_or_null, uint32_t events_version);

/* Drain up to `max` queued records into `out`, in emit order; returns the count written.
 * Only meaningful with no sink bound (a bound sink consumes at emit and the ring stays
 * empty). */
uint32_t libmh_poll_events(libmh_event *out, uint32_t max);

/* Records dropped to ring overflow since process start (monotonic). */
uint32_t libmh_event_overflow_count(void);

/* ---- the TEXT surface (LIFT-NOTIFY, the offscreen conversion's sibling for string payloads) ----
 *
 * Some notify traffic is a COMPOSED DISPLAY STRING, not a scalar tuple: chat lines arrive as wire
 * bytes, the upgrade announcements are open-ended clause chains, and one dead debug path XOR-decodes
 * a cheat string. Those cannot ride the 20-byte record, so strings get their own emit path with the
 * same two consumptions: a bound sink receives (kind, text) SYNCHRONOUSLY at emit -- `text` is the
 * emitter's own buffer and is valid ONLY for the duration of the call -- and with no sink bound the
 * text is COPIED into an internal ring (LIBMH_TEXT_RING_CAP slots) that the host drains with
 * libmh_poll_texts at frame edge. Copy-at-emit is what makes poll mode safe: every emitter composes
 * into one shared scratch, so a deferred pointer would read whatever was composed last.
 *
 * Text is UTF-16 (the game's native wide format), NUL-terminated. A text longer than
 * LIBMH_EVT_TEXT_MAX-1 units is delivered whole to a sync sink but TRUNCATED in the queue copy;
 * ring overflow drops the newest and counts (libmh_text_overflow_count). Same ordering and
 * threading contract as the record surface. */
#define LIBMH_EVTK_TEXT_MAIN       1u /* the main message line (game_ui_PrintTextMessage) */
#define LIBMH_EVTK_TEXT_FLOAT_RED  2u /* red floating HUD line (llm_ui_print_floating_msg_red) */
#define LIBMH_EVTK_TEXT_FLOAT_CYAN 3u /* cyan floating HUD line (llm_ui_print_floating_msg_cyan) */
/* LIFT-TACT slice B: the tactical exit-confirm prompt ("ENTER - <yes>, ESC - <no>"), composed by
 * libmh into its own scratch and drawn at a literal (0x96, 0xf0) in literal white -- so only the
 * STRING crosses and the position and colour stay in the sink, the same division the three lines
 * above already make. (llm_gfx_draw_text_rgb) */
#define LIBMH_EVTK_TEXT_TACT_EXIT_CONFIRM 4u
#define LIBMH_EVT_TEXT_MAX                512u /* queue-copy capacity per text, UTF-16 units incl. NUL */
#define LIBMH_TEXT_RING_CAP               8u

typedef void (*libmh_text_sink)(uint16_t kind, const uint16_t *text);

/* Bind (or clear, with NULL) the synchronous text sink. Same version handshake and keep-on-refusal
 * semantics as libmh_set_event_sink. */
int libmh_set_text_sink(libmh_text_sink sink_or_null, uint32_t events_version);

/* Drain up to `max` queued texts, in emit order; kinds_out[i] pairs with texts_out[i]. Returns the
 * count written. Only meaningful with no sink bound. */
uint32_t libmh_poll_texts(uint16_t *kinds_out, uint16_t (*texts_out)[LIBMH_EVT_TEXT_MAX],
                          uint32_t  max);

/* Texts dropped to ring overflow since process start (monotonic). */
uint32_t libmh_text_overflow_count(void);

#ifdef __cplusplus
}
#endif

#endif /* LIBMH_HOST_EVENTS_H */
