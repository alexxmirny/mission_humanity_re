// libmh's side of the event-channel surface (LIFT-EVQ; docs/libmh-abi.md R7) -- the emitters
// module code calls, over the C contract in libmh/include/libmh_host_events.h. Same layering
// as host_api.h: this lives in the `state` module, INSIDE libmh, and knows nothing about what
// the host does with a record (mh.dll's sink routes it onto the original fine thunk at the
// original instant; a poll host drains at frame edge; no host at all -> the ring absorbs it).
//
// Emitters are void and cheap by contract (R8: nothing here may feed hashed sim state), so a
// call site never branches on the host's presence -- the calls-struct member a site binds is
// a plain `void()`/`void(args)` lambda over one of these.
#pragma once

#include <cstdint>

#include "../../libmh/include/libmh_host_events.h"

namespace mh::state {

// Emit one record: synchronous sink dispatch when bound, else ring (drop-newest + counter on
// overflow). The one emit path -- every channel helper below funnels here.
void emit_event(uint16_t channel, uint16_t kind, int32_t a = 0, int32_t b = 0, int32_t c = 0,
                int32_t d = 0);

// Channel helpers, so call sites read as what they mean.
inline void emit_invalidate(uint16_t kind, int32_t a = 0, int32_t b = 0) {
    emit_event(LIBMH_EVC_INVALIDATE, kind, a, b);
}
inline void emit_screen(uint16_t kind, int32_t a = 0, int32_t b = 0) {
    emit_event(LIBMH_EVC_SCREEN, kind, a, b);
}

// The SCREEN channel's answer slots (R9, libmh_host_events.h): what the host pushed for
// `answer_id`, or -1 if nothing is pending. The read is NON-destructive and the clear is
// explicit, so a call site consumes an answer in the same read-then-clear shape whether it is
// reading this slot (standalone) or the game's own global through the hoist ops (hosted).
int32_t screen_answer(uint32_t answer_id);
void    screen_answer_clear(uint32_t answer_id);

// Emit one composed display string over the TEXT surface (libmh_host_events.h): synchronous sink
// delivery of the caller's own pointer, else a truncating copy into the text ring. `text` is UTF-16.
void emit_text(uint16_t kind, const void *text);

// ---- named emit ADAPTERS (LIFT-NOTIFY) -----------------------------------------------------
//
// One per converted fine entry, signature-identical to the entry it replaced, so a calls-struct
// binder line swaps `mh::host().<entry>` for `mh::state::evt::<name>` and nothing else moves --
// the member type, the sites, and the suites' stubs all stay put. Grouped here rather than
// scattered as per-TU lambdas so each conversion exists ONCE and reads next to its kind.
namespace evt {

inline void snd_play(int32_t sound_id, int32_t volume) {
    emit_event(LIBMH_EVC_EVENT, LIBMH_EVK_SND_PLAY, sound_id, volume);
}
inline void snd_ambient_tick() { emit_event(LIBMH_EVC_EVENT, LIBMH_EVK_SND_AMBIENT_TICK); }
inline void snd_ambient_planet_clone(uint32_t planet_id) {
    emit_event(LIBMH_EVC_EVENT, LIBMH_EVK_SND_AMBIENT_CLONE, static_cast<int32_t>(planet_id));
}
inline void snd_stop_all_channels() { emit_event(LIBMH_EVC_EVENT, LIBMH_EVK_SND_STOP_ALL); }
inline void snd_race_alert() { emit_event(LIBMH_EVC_EVENT, LIBMH_EVK_SND_RACE_ALERT); }
inline void snd_zone_play(int32_t sound_id) {
    emit_event(LIBMH_EVC_EVENT, LIBMH_EVK_SND_ZONE_PLAY, sound_id);
}
inline void snd_fx_play(int32_t fx_type, int32_t volume_pct, int32_t pan) {
    emit_event(LIBMH_EVC_EVENT, LIBMH_EVK_SND_FX_PLAY, fx_type, volume_pct, pan);
}
// Slice-3 pair: NOT signature-identical to a replaced entry -- these two fuse a returning host
// query into a void positional record (the R3 cleanup), so their sites and calls-struct members
// changed shape with them.
inline void snd_play_at(int32_t sound_id, int32_t tile_col, int32_t tile_row) {
    emit_event(LIBMH_EVC_EVENT, LIBMH_EVK_SND_PLAY_AT, sound_id, tile_col, tile_row);
}
inline void fx_debris_burst(int32_t tile_col, int32_t tile_row, int32_t energy_max) {
    emit_event(LIBMH_EVC_EVENT, LIBMH_EVK_FX_DEBRIS_BURST, tile_col, tile_row, energy_max);
}

// -- the TEXT surface's adapters (the string-payload notify family) --------------------------------
//
// Composition (sprintf/concat into the shared scratch) STAYS at the call site -- it is libmh's own
// string work on non-hashed scratch -- and only the PRINT is the emit. The _u32/_i32 variants exist
// because most calls-struct members mirror the replaced entries' int-returning signatures; the
// constant 0 stands in for a host return every site provably discards (the one capture,
// sim_player_presence_lost's `ret`, feeds only that function's own ignored return value).
inline void     text_print(void *text) { emit_text(LIBMH_EVTK_TEXT_MAIN, text); }
inline uint32_t text_print_u32(void *text) {
    emit_text(LIBMH_EVTK_TEXT_MAIN, text);
    return 0;
}
inline void    text_float_red(void *text) { emit_text(LIBMH_EVTK_TEXT_FLOAT_RED, text); }
inline int32_t text_float_red_i32(void *text) {
    emit_text(LIBMH_EVTK_TEXT_FLOAT_RED, text);
    return 0;
}
inline void text_float_cyan(void *text) { emit_text(LIBMH_EVTK_TEXT_FLOAT_CYAN, text); }

// -- text/progress/tails + dirty marks -------------------------------------------------------
inline void text_queue_id(int32_t text_id) {
    emit_event(LIBMH_EVC_EVENT, LIBMH_EVK_TEXT_QUEUE_ID, text_id);
}
inline void text_game_speed() { emit_event(LIBMH_EVC_EVENT, LIBMH_EVK_TEXT_GAME_SPEED); }
inline void text_race_alert() { emit_event(LIBMH_EVC_EVENT, LIBMH_EVK_TEXT_RACE_ALERT); }
inline void progress_unit_available(uint16_t player, uint16_t unit_proto) {
    emit_event(LIBMH_EVC_EVENT, LIBMH_EVK_PROGRESS_UNIT_AVAILABLE, player, unit_proto);
}
inline void player_set_color(int32_t player_idx, uint32_t color_index) {
    emit_event(LIBMH_EVC_EVENT, LIBMH_EVK_PLAYER_SET_COLOR, player_idx,
               static_cast<int32_t>(color_index));
}
inline void msg_queue_clear_all() { emit_event(LIBMH_EVC_EVENT, LIBMH_EVK_MSG_QUEUE_CLEAR); }
inline void cam_jump_queue_clear() { emit_event(LIBMH_EVC_EVENT, LIBMH_EVK_CAM_JUMP_QUEUE_CLEAR); }
inline void menu_placement_pending_clear() {
    emit_event(LIBMH_EVC_EVENT, LIBMH_EVK_MENU_PLACEMENT_CLEAR);
}
// THE ONE PAIR ON THIS SURFACE THAT WRITES BEFORE IT EMITS -- the R3b hoist, and the reason is
// worth carrying next to it. `llm_strat_spawn_enemy_landing` reads `*v.cam_col` back to derive
// `strat_players.landing_x` when the map has no free landing spot (sim_landing_spot.cpp:147-150),
// and that read is HASHED sim state. A poll-mode host drains at frame edge, so it would read the
// PREVIOUS frame's camera -- a real divergence, on the one same-frame route that exists
// (llm_strat_frame -> input_update -> llm_debug_console_dispatch -> deploy_starting_squad emits,
// then the same frame's sim_tick -> ... -> spawn_enemy_landing reads).
//
// So libmh owns the value: it writes the region ITSELF, synchronously, and the record becomes
// "the camera moved" rather than "please move the camera". R3b's own second remedy, and the
// cheaper one -- the alternative was pulling both entries back onto the host table.
//
// THE DOUBLE WRITE IS IDEMPOTENT BY MEASUREMENT, not by assumption (R2). llm_map_cam_set_col
// @0x0044af33 is `mov [0x00e58138], eax` VERBATIM -- no clamp, no transform -- followed by
// `_G_LLM_STRAT_CAM_SCROLL_OFFSET_COL = col << 5`. So the host's later write stores the same
// value we just stored, and the only thing that stays deferred is the derived scroll offset,
// which NO translated body reads (zero hits for CAM_SCROLL_OFFSET across src/mh_dll).
// Defined in host_events.cpp -- they touch the region registry, which this header does not.
void        cam_set_col(int32_t col);
void        cam_set_row(int32_t row);
void        view_zoom_scale(double zoom_x, double zoom_y); // bit-packs -- host_events.cpp
inline void view_minimap_zoom_for_size() {
    emit_event(LIBMH_EVC_EVENT, LIBMH_EVK_VIEW_MINIMAP_ZOOM);
}
inline void inv_viewport() { emit_invalidate(LIBMH_EVK_INV_VIEWPORT); }
inline void inv_tact_vis_map() { emit_invalidate(LIBMH_EVK_INV_TACT_VIS_MAP); }
inline void inv_tact_vis_margin() { emit_invalidate(LIBMH_EVK_INV_TACT_VIS_MARGIN); }
inline void inv_tact_unit_slot(int32_t building_id) {
    emit_invalidate(LIBMH_EVK_INV_TACT_UNIT_SLOT, building_id);
}
inline void inv_tact_group_panel() { emit_invalidate(LIBMH_EVK_INV_TACT_GROUP_PANEL); }
inline void inv_tact_sel_panel_mode_tab(int32_t mode) {
    emit_invalidate(LIBMH_EVK_INV_TACT_SEL_PANEL_MODE_TAB, mode);
}
inline void inv_tact_sel_panel_row_toggle(int32_t which, int32_t row) {
    emit_invalidate(LIBMH_EVK_INV_TACT_SEL_PANEL_ROW_TOGGLE, which, row);
}
inline void inv_tact_order_button(int32_t button) {
    emit_invalidate(LIBMH_EVK_INV_TACT_ORDER_BUTTON, button);
}
inline void inv_tact_sidebar_roster() { emit_invalidate(LIBMH_EVK_INV_TACT_SIDEBAR_ROSTER); }
inline void inv_tact_sidebar_rows() { emit_invalidate(LIBMH_EVK_INV_TACT_SIDEBAR_ROWS); }
inline void inv_tact_sidebar_slot(int32_t unit_id) {
    emit_invalidate(LIBMH_EVK_INV_TACT_SIDEBAR_SLOT, unit_id);
}
inline void inv_tact_player_rows(int32_t selected_row) {
    emit_invalidate(LIBMH_EVK_INV_TACT_PLAYER_ROWS, selected_row);
}
// Whole-body draw scopes, not per-primitive replacements (LIFT-TACT L4b) -- see
// the banner over kinds 10-13 in libmh_host_events.h for why these carry values and not geometry.
inline void inv_tact_char_panel_row(int32_t unit_id) {
    emit_invalidate(LIBMH_EVK_INV_TACT_CHAR_PANEL_ROW, unit_id);
}
inline void inv_tact_active_count_hud(int32_t active, int32_t cached) {
    emit_invalidate(LIBMH_EVK_INV_TACT_ACTIVE_COUNT_HUD, active, cached);
}
inline void inv_tact_sel_panel_bg() { emit_invalidate(LIBMH_EVK_INV_TACT_SEL_PANEL_BG); }
inline void inv_tact_sel_panel() { emit_invalidate(LIBMH_EVK_INV_TACT_SEL_PANEL); }

// The two bodies that left libmh WHOLE (LIFT-TACT slice A). Unlike every adapter above, these
// replace a call to OUR OWN translated body rather than to a fine host entry -- the body is gone
// and the sink calls the original. Both are safe to move as a unit because their measured write
// set is tile_vis_map only, which is not hashed and has no translated reader.
inline void inv_tact_char_panel_row_draw(int32_t unit_idx, int32_t row_slot) {
    emit_invalidate(LIBMH_EVK_INV_TACT_CHAR_PANEL_ROW_DRAW, unit_idx, row_slot);
}
// Two adapters, ONE kind: the side is what distinguishes them, and it belongs in the record rather
// than in two near-identical kinds. `highlight_flag` does not appear because both original sites
// pass the literal 1 -- see the kind's comment.
inline void inv_tact_sidebar_row_unassigned(int32_t unit_id, int32_t row) {
    emit_event(LIBMH_EVC_INVALIDATE, LIBMH_EVK_INV_TACT_SIDEBAR_ROW_HOVER, 0, unit_id, row);
}
inline void inv_tact_sidebar_row_group(int32_t unit_id, int32_t row) {
    emit_event(LIBMH_EVC_INVALIDATE, LIBMH_EVK_INV_TACT_SIDEBAR_ROW_HOVER, 1, unit_id, row);
}
// The selection panel's one-time init draw -- a SPLIT rather than a whole-body move: the caller
// keeps the multi-mode reset and the refresh/redraw/clear tail and emits this for the head.
inline void inv_tact_sel_panel_init() { emit_invalidate(LIBMH_EVK_INV_TACT_SEL_PANEL_INIT); }

// ---- llm_tact_frame's own draw calls (LIFT-TACT slice B) ------------------------------------
//
// Converted IN PLACE: the body keeps its shape and its ordering, and only the draws cross. The
// four INPUT entries and llm_tact_render_view are deliberately NOT here -- input stays required
// under R10, and render_view under R3b, because tact_frame reads the hover state it writes at
// nine sites later in the same frame.
inline void inv_tact_cursor_sprite(int32_t sprite_id) {
    emit_invalidate(LIBMH_EVK_INV_TACT_CURSOR_SPRITE, sprite_id);
}
inline void inv_tact_drawn_map_clear() { emit_invalidate(LIBMH_EVK_INV_TACT_DRAWN_MAP_CLEAR); }
inline void inv_tact_drag_box(int32_t anchor_x, int32_t anchor_y, int32_t cursor_x,
                              int32_t cursor_y) {
    emit_event(LIBMH_EVC_INVALIDATE, LIBMH_EVK_INV_TACT_DRAG_BOX, anchor_x, anchor_y, cursor_x,
               cursor_y);
}
inline void inv_tact_minimap_overlay() { emit_invalidate(LIBMH_EVK_INV_TACT_MINIMAP_OVERLAY); }
inline void screenshot_save() { emit_event(LIBMH_EVC_EVENT, LIBMH_EVK_SCREENSHOT_SAVE); }
// The mine-blast exit wipe. libmh states the intent once and the host owns the sixteen frames --
// the same division SCR_FADE_TRANSITION_RUN makes, for the same reason, and this one absorbs
// llm_tact_scroll_fade_step whole (that loop was its only site).
inline void tact_blast_transition() { emit_screen(LIBMH_EVK_SCR_TACT_BLAST_TRANSITION); }
inline void tact_frame_present() { emit_screen(LIBMH_EVK_SCR_TACT_FRAME_PRESENT); }
// SIMABI-NOTIFY: the STRATEGIC frame's two ends, the sibling pair of tact_frame_present above and
// converted for its reason -- each is the last statement of the body that emits it
// (sim_lt_frame.cpp:67 / :78), so no libmh reader runs after it in that frame.
inline void strat_frame_present() { emit_screen(LIBMH_EVK_SCR_STRAT_FRAME_PRESENT); }
inline void strat_frame_redraw() { emit_screen(LIBMH_EVK_SCR_STRAT_FRAME_REDRAW); }
// SIMABI-DISPLAY: the strategic view's display size. The original returns the APPLIED mode (an OS
// readback); libmh no longer stores it, and the hosted sink performs that store itself -- see
// LIBMH_EVK_SCR_SET_DISPLAY_MODE for the measurement that made a void record legal here.
inline void set_display_mode(int32_t mode) {
    emit_screen(LIBMH_EVK_SCR_SET_DISPLAY_MODE, mode);
}
// The exit-confirm prompt. libmh composes the string into its own scratch (that is its string
// work, on non-hashed memory); only the composed text crosses, and the position and colour stay
// in the sink.
inline void text_tact_exit_confirm(void *text) {
    emit_text(LIBMH_EVTK_TEXT_TACT_EXIT_CONFIRM, text);
}

// ---- the mission START/END one-shots (LIFT-TACT slice B2) ------------------------------------
//
// Converted where they stand rather than merged into an enter/leave pair: libmh writes
// window_width / view_tiles_w / grid_width BETWEEN them and those writes are the inputs the
// metrics-init calls consume, so a merge would move a host call across the state it reads.
inline void scr_tact_mission_media() { emit_screen(LIBMH_EVK_SCR_TACT_MISSION_MEDIA); }
inline void inv_gfx_view_metrics() { emit_invalidate(LIBMH_EVK_INV_GFX_VIEW_METRICS); }
inline void inv_tact_view_metrics() { emit_invalidate(LIBMH_EVK_INV_TACT_VIEW_METRICS); }
inline void inv_tact_view_tile_rows() { emit_invalidate(LIBMH_EVK_INV_TACT_VIEW_TILE_ROWS); }
inline void inv_tact_sprite_banks() { emit_invalidate(LIBMH_EVK_INV_TACT_SPRITE_BANKS); }
inline void inv_all_sprite_banks() { emit_invalidate(LIBMH_EVK_INV_ALL_SPRITE_BANKS); }
inline void inv_sprite_pix_offsets() { emit_invalidate(LIBMH_EVK_INV_SPRITE_PIX_OFFSETS); }

// LIFT-TABLE S3: the per-planet graphics tail of cfg_final_planet_Construct. NOT signature-
// identical to a fine entry, because the record replaces a WHOLE tail (the tlo_index store, the
// 8-way bank_count/bank_id switch, the soldier_sprite_bank_offset shift and the eight
// FillBankData calls) rather than one call -- the sel_panel_init / active_count_hud shape.
inline void inv_planet_gfx_setup(int32_t planet_index, int32_t tlo_index) {
    emit_invalidate(LIBMH_EVK_INV_PLANET_GFX_SETUP, planet_index, tlo_index);
}

// LIFT-TABLE S4: the planet-map palette block. No payload -- nine literal colour triples in a fixed
// order; the record is the INSTANT, not the data.
inline void inv_planet_map_palette() { emit_invalidate(LIBMH_EVK_INV_PLANET_MAP_PALETTE); }

// SIMABI-NOTIFY: the sim table's two no-payload invalidates. Signature-identical to the entries
// they replaced (both void(void)), so each site is a one-line swap and nothing else moves.
inline void inv_player_color_lut() { emit_invalidate(LIBMH_EVK_INV_PLAYER_COLOR_LUT); }
inline void inv_planet_extra_sprite_banks() {
    emit_invalidate(LIBMH_EVK_INV_PLANET_EXTRA_SPRITE_BANKS);
}

// -- the SCREEN channel (LIFT-SCREEN) -------------------------------------------------
//
// Same binder-swap discipline as the families above: each adapter is signature-identical to the
// fine entry it replaced, so a calls-struct binder line changes and nothing else does. The three
// `_i32` variants mirror members the original declared `int`; the constant 0 stands in for a
// return that has NO consumer anywhere -- measured and recorded in
// docs/libmh-abi.md R9 (Watcom uncommitted-return shape; llm_ui_outcome_dialog's forward through
// llm_strat_player_presence_lost is ignored by all 21 of that function's callers).
inline int32_t outcome_dialog_i32(uint8_t outcome) {
    emit_screen(LIBMH_EVK_SCR_OUTCOME_DIALOG, outcome);
    return 0;
}
inline int32_t overlay_dismiss_i32() {
    emit_screen(LIBMH_EVK_SCR_OVERLAY_DISMISS);
    return 0;
}
inline int32_t wait_player_overlay_show_i32(int32_t player_idx) {
    emit_screen(LIBMH_EVK_SCR_WAIT_PLAYER_SHOW, player_idx);
    return 0;
}
inline void    mp_leave_reset_game_mode() { emit_screen(LIBMH_EVK_SCR_MP_LEAVE_RESET); }
inline void    bldg_panel_open() { emit_screen(LIBMH_EVK_SCR_BLDG_PANEL_OPEN); }
inline int32_t planet_select_screen_open_i32(int32_t open_arg) {
    emit_screen(LIBMH_EVK_SCR_PLANET_SELECT_OPEN, open_arg);
    return 0;
}
// NOT signature-identical, deliberately: the entry took a raw table ADDRESS and the record carries
// a LIBMH_SCR_DLGT_* id instead (R4 -- an address cannot cross an abstract boundary). The one site
// changes shape with it.
inline void dlg_build_from_table(int32_t table_id) {
    emit_screen(LIBMH_EVK_SCR_DLG_FROM_TABLE, table_id);
}
// Void, unlike the entry it replaced: the kick modal's answer comes back through
// libmh_submit_screen_answer, not through a return (R9). The site reads the answer BEFORE it
// emits -- see the ordering law at the timekeeper call site.
inline void sync_overlay_show() { emit_screen(LIBMH_EVK_SCR_SYNC_OVERLAY_SHOW); }

// ---- the tutorial's choreography ------------------------------------------------------------
//
// llm_tutorial_step_driver's presentation tail. Signature-identical to the entries they replace so
// each is a one-line binder swap, EXCEPT where the entry returned something: those return a
// constant every libmh site provably discards, the same shape the LIFT-NOTIFY text adapters used.
inline void tut_uistate_restore() { emit_screen(LIBMH_EVK_SCR_TUT_UISTATE_RESTORE); }
inline void cursor_menu_draw() { emit_screen(LIBMH_EVK_SCR_CURSOR_MENU_DRAW); }
inline void present_flip() { emit_screen(LIBMH_EVK_SCR_PRESENT_FLIP); }

// Returns 0: llm_ui_menu_bg_redraw_cb's int return is discarded at its one libmh site (the asm
// stores it nowhere either).
inline int32_t menu_bg_redraw() {
    emit_screen(LIBMH_EVK_SCR_MENU_BG_REDRAW);
    return 0;
}

// Returns nullptr: llm_gfx_font_desc_for_flags hands back a descriptor pointer that BOTH libmh
// sites discard (dead stack stores at 0x004bad0f and 0x004bad41). A descriptor pointer is also
// exactly the kind of address that may not cross the boundary (R4), so the value staying host-side
// is the point, not a shortcut.
inline void *font_desc_for_flags(uint32_t style_flags) {
    emit_screen(LIBMH_EVK_SCR_FONT_DESC_FOR_FLAGS, static_cast<int32_t>(style_flags));
    return nullptr;
}

// R4 again: the originals take a widget-list ADDRESS, the records carry a LIBMH_SCR_WGTL_* id.
inline void wgtl_center(int32_t list_id) { emit_screen(LIBMH_EVK_SCR_WGTL_CENTER, list_id); }
inline void wgtl_draw(int32_t list_id) { emit_screen(LIBMH_EVK_SCR_WGTL_DRAW, list_id); }

// Ask the host to run the already-armed screen fade to completion. Replaces a `while (tick() == 0)`
// spin on a host entry: libmh states the intent once, the host decides what completing it costs.
inline void fade_transition_run() { emit_screen(LIBMH_EVK_SCR_FADE_TRANSITION_RUN); }

// Lay out the tutorial hint widget from a UI sprite's metrics. The two metric queries and the four
// assignments they feed live host-side together -- libmh never reads the geometry back.
inline void tut_hint_layout(int32_t sprite_id) {
    emit_screen(LIBMH_EVK_SCR_TUT_HINT_LAYOUT, sprite_id);
}

} // namespace evt

// Dispatches performed through a bound sink since process start (monotonic) -- the seam
// report's arm-time evidence that the hosted config really consumes at emit.
uint32_t event_sink_dispatch_count();

} // namespace mh::state
