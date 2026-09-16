//
// tact/tact_frame.h -- TACT1E: the tactical mission's per-frame pump. Called once per real frame
// while GAME_MODE==6 (llm_frame_dispatch case 6, frontier, not itself a migration member).
//
//   llm_tact_frame @0x00429b1a (0x1265 = 4709 B)
//   void __watcall llm_tact_frame(void)
//
// Re-derived from the DISASSEMBLY (tmp/decomp_tact/llm_tact_frame_00429b1a.asm), not from Ghidra's
// .c: the .c's variable-to-stack-offset naming drifted in the key-event section (see DERIVATION
// step 1 below) even though its CONTROL FLOW matched the opcodes at every point this translation
// cross-checked. The struct/field/calling-convention identifications below were re-derived from the
// raw bytes, not trusted from the .c.
//
// ---------------------------------------------------------------------------------------------
// SHAPE
// ---------------------------------------------------------------------------------------------
//
// One key event is drained per frame (at most). The dequeued scancode then drives, in sequence:
// camera-scroll-held latches, an early "mission ending" branch, ESC/digit/screenshot/debug hotkeys,
// a scroll/mine-blast-transition gate that either runs the main per-frame body or plays a 16-frame
// fade transition, camera panning, the sub-tick calls (render/door/teleport/sidebar/units-and-fx/
// owner-ticks), six independent mouse-click handlers (hover-move-order, left-click-select,
// right-click-attack-or-move, drag-select start/update/release, click-preview-facing paint), and
// an exit-confirm-open overlay (prompt text + two tile-vis-map refresh loops).
//
// ---------------------------------------------------------------------------------------------
// DERIVATION
// ---------------------------------------------------------------------------------------------
//
//  1. @0x00429b22: `utils_assert_stack_capacity(0xc8)` -- inert per translator-brief.md #6, omitted.
//
//  2. @0x00429b32: `llm_tact_ambient_sound_tick()` unconditionally, first thing every frame -- an
//     ALREADY-TRANSLATED sibling (TACT1E), called through its public wrapper.
//
//  3. @0x00429b37-0x00429b4b: THE KEY-EVENT DEQUEUE. `llm_input_key_queue_empty()` returning 0 means
//     NOT EMPTY (i.e. dequeue); a mockable `mh::game::mh_llm_input_key_event` (0x38 B, the SAME type
//     `_G_LLM_INPUT_KEY_EVENTS[128]` uses) is filled by `llm_input_key_dequeue(&event)`, and only its
//     first two dwords are read: `.scancode` (@+0) and `.event_type` (@+4, tested against 0x100 =
//     "key down" and 0x80 = "key up" throughout the function -- Ghidra's own region comment on
//     `_G_LLM_INPUT_KEY_EVENTS` names both bits).
//
//     UNCERTAINTY, not resolved: when the queue IS empty, the ORIGINAL reads UNINITIALIZED STACK at
//     [EBP-0x74]/[EBP-0x70] (the dequeue+copy is skipped entirely) rather than an explicit reset --
//     see this header's own `uncertainties[]` note in the translation report. This translation
//     initializes the scancode/event_type locals to 0 on every call (a fresh local, not a `static`),
//     which means "queue empty this frame" reads as "no hotkey fires this frame" -- the conservative
//     reading, and the one under which every camera-scroll-HELD bit (latched into the persistent
//     store fields below, not through this local) is unaffected. A `static`-across-calls model
//     (mirroring literal stack reuse) is the alternative if review disagrees.
//
//  4. @0x00429b4e-0x00429c2f: camera-scroll-HELD latches -- `_G_LLM_TACT_CAM_SCROLL_{UP,DOWN,
//     RIGHT,LEFT}_HELD` set to 1 on the matching scancode's key-DOWN event (0x100), cleared to 0 on
//     its key-UP event (0x80); a bare key-UP event (0x80, any scancode) also zeroes the scancode
//     local, matching Ghidra's `if ((flags & 0x80) != 0) scancode = 0;`.
//
//  5. @0x00429c36-0x00429c8c: EARLY-EXIT gate -- `active_unit_count()==0` (mission already emptied)
//     OR (`scancode==0x1c` [Enter] AND `exit_confirm_open()==1`, i.e. Enter confirms the pending
//     exit prompt). Resets scroll_cmd/mine_blast_time_end/exit_confirm_open, runs the four
//     already-translated "return to strategic" siblings, and returns -- skipping EVERYTHING below,
//     including the render/door/sidebar/units ticks.
//
//  6. @0x00429c91-0x00429d55: ESC toggle (scancode==1 && key-down: flip `exit_confirm_open()`),
//     digit hotkeys 2..9 (select every owned unit whose `squad_group_id == scancode-2`, unconditional
//     clear-then-scan-then-refresh, NO ctrl-modifier check -- this is a REPLACE, not the
//     ctrl-modified add/replace `selection_clear_unless_ctrl` mediates elsewhere), and F1
//     (scancode==0x1f) -> `llm_tact_save_screenshot()` (frontier, stays original -- not a migration
//     member).
//
//  7. @0x00429d60-0x00429dfd: the scroll/mine-blast gate. `scroll_cmd() < 1` OR (post-increment)
//     `scroll_cmd() < 9` runs the MAIN BODY below (step 8+); otherwise this is a 16-frame scroll-fade
//     TRANSITION: play the mine-blast sound, reset scroll_cmd/mine_blast_time_end, loop 16x
//     {scroll_fade_step(); blink_overlay_clear(); frame_cursor_and_reset();} (all three frontier),
//     then blink_overlay_clear()+vis_map_fill_default(), reset exit_confirm_open, and run the SAME
//     four "return to strategic" siblings as step 5's early exit.
//
//  8. @0x00429dfd-0x00429e3d: mine-blast countdown expiry -- if `time_now() > mine_blast_time_end()`
//     AND `mine_blast_time_end() > 0.0`, bump scroll_cmd() (starts the transition above NEXT frame),
//     zero the scancode local (suppress any hotkey this frame), and zero mine_blast_time_end().
//
//  9. @0x00429e3d-0x00429f1e: four more hotkeys, gated on the (possibly step-8-zeroed) scancode:
//     0x34 -> group_issue_order(0x7f,0,0,0,0) then group_issue_order(4,0,0,0,0) (stop-then-move, the
//     ALREADY-TRANSLATED sibling); 0x35 -> XOR bit 0 of `.active_gun` on every selected unit, then
//     `selection_panel_refresh()` (frontier); 0x33 -> `select_next_unit()` (sibling); 0x32 -> find
//     the FIRST selected unit and `unit_enqueue_command(id,9,0,0,0,0,0)` (mine-arm, sibling), break
//     after one, then zero the scancode local.
//
// 10. @0x00429f1e-0x00429f69: debug hotkey Ctrl+Alt+Shift+W (scancode 0x57, all three modifier
//     bits checked via BOTH byte-mask encodings the game's keystate bytes use -- see the LCtrl gate
//     `llm_tact_selection_clear_unless_ctrl` already documents for the identical two-mask idiom):
//     over the WHOLE 128x128 sub-block (`TACT_MAP_DIM`), `visibility++` and set the tile's flags to
//     EXPLORED (clear FOGGED 0x4000, set EXPLORED 0x8000 -- the exact `flags[1] &=0xbf; flags[1]
//     |=0x80` pair `tact_unit_vision.cpp` already names and comments this way; this site performs it
//     INLINE, not via a call, so it is transcribed inline too, not routed through that TU).
//
// 11. @0x00429fbd... wait -- REORDER NOTE: step 10's tile loop is COLUMN-outer / ROW-inner in the
//     .asm's own stack-offset arithmetic (outer counter x 8 = ELEM_SIZE, inner counter x 0x800 =
//     ROW_STRIDE); re-expressed here as `for (y) for (x) planes().tile_object_at(x, y)` to match
//     `tile_at()`/`tile_object_at()`'s established `(x<<8)|y` indexing (x multiplies by 2048, y by
//     8 -- verified against `mh_map_tile_object_data`'s 8-byte size).
//
// 12. @0x0042a018-0x0042a0c9: debug hotkey Ctrl+Alt+Shift+X (scancode 0x58, same triple-modifier
//     gate): for every selected unit, `character_types[type].energy *= 10` then `unit.hp =
//     character_types[type].energy` (an "instant heal to 10x max HP" cheat).
//
// 13. @0x0042a0c9-0x0042a121: mouse pump (`mouse_delta_pump()`, `mouse_buttons_get()` -> STORE
//     `mouse_buttons_cur()` -- see declared_needs, this is a NEW write path onto an existing
//     view-only region), click_action_taken reset when buttons==0, and cursor sprite selection
//     (`gfx_LoadSprite(2)` when nothing hovered, else `gfx_LoadSprite(1)`).
//
// 14. @0x0042a10a-0x0042a28e: CAMERA PANNING, only when `mouse_buttons_cur()==0 &&
//     scroll_cmd()==0`. Four independent edge/held-key checks (cursor at 0 or the matching
//     SCROLL_*_HELD flag), each either decrementing/incrementing `map_cam_col()`/`map_cam_row()`
//     with a clamp OR calling the matching `view_shift_*` sibling when NOT at the clamp boundary
//     (the clamp-vs-shift choice is genuinely per-axis, not a single combined test). The right/down
//     bounds are computed from `TACT_MAP_WIDTH_CACHE`/`HEIGHT_CACHE` minus `win_w()/32` (shift-divide,
//     algebraically `/32` for this always-nonnegative-after-clamp operand) and `WindowHeight/24`
//     (real IDIV). `cam_follow_selection_tick()` (sibling) runs last, gated on
//     `cam_follow_selection()==1`.
//
// 15. @0x0042a28e-0x0042a2a2: `render_view()`, `door_tick()`, `teleport_zone_scan_tick()`,
//     `sidebar_dispatch()` -- one frontier call, three already-translated siblings.
//
// 16. @0x0042a2a2-0x0042a40d: the "cursor in the game viewport" gate (`cursor_x() < win_w()`) wraps
//     SIX independent click/hover handlers (steps 17-22); none of them run when the cursor is over
//     the sidebar.
//
// 17. @0x0042a2b3-0x0042a40d: HOVER AUTO-MOVE-ORDER. Only when `mouse_buttons_cur()==0`: reset
//     click_scan_scratch/click_action_taken, then for every selected unit within the camera's
//     16x21-tile viewport, compute a dir24 heading from the unit toward the cursor and, if the
//     unit's head command-queue slot is empty OR its `move_retry_wait != 0`, enqueue op=6
//     (turn-then-approach) with that heading as arg0.
//
// 18. @0x0042a40d-0x0042a4af: LEFT-CLICK SELECT ONE. `mouse_buttons_cur()==1 && !drag_select_active
//     && !click_action_taken && hovered_unit_id()>0`, no shift held, hovered unit owned by player:
//     `selection_clear_unless_ctrl()`, select it, `selection_panel_refresh()`, mark
//     click_action_taken.
//
// 19. @0x0042a4af-0x0042a605: RIGHT-CLICK OWN-UNIT COMMAND. Same gate shape with button==2: if the
//     unit is already selected, dispatch on `anim_state` (0/1 -> stop-then-approach op 4; 3 ->
//     stop-then-something op 5; else no enqueue); if NOT selected, just select it. Always marks
//     click_action_taken on this path.
//
// 20. @0x0042a605-0x0042a72c: RIGHT-CLICK GROUP ORDER. Two sibling checks: (a) any mouse button held
//     with a hovered ENEMY unit (owner==1) and `active_unit_count_cached()>0` ->
//     `group_issue_order(2, (buttons-1)^1, 0, world_x, world_y)`; (b) any mouse button held with NO
//     hovered unit, shift held -> the same call but with arg1 forced to 1 when button==3 (both
//     buttons), else `(buttons-1)^1`. (a) and (b) are mutually exclusive by construction
//     (hovered_unit_id()==0 in (b)'s branch), so both are transcribed as-is with no shared state.
//
// 21. @0x0042a72c-0x0042a838: RIGHT-CLICK MOVE OR CAMERA-CENTER (no shift, button==2, no hovered
//     unit, click_action_taken still clear): if any unit is selected (active_unit_count_cached()>0),
//     `group_issue_order(1, world_col, world_row, 0, 0)`; else `camera_center_on_tile(world_col,
//     world_row)` (sibling). Both compute world_col/row from the shift-divide-by-32 /
//     IDIV-by-24 pair already verified in step 14. Marks click_action_taken.
//
// 22. @0x0042a838-0x0042a8f1: CLICK-PREVIEW FACING PAINT. Only when button==2 AND
//     click_action_taken was just set by step 21: for every unit with BOTH status bits 0x1 and 0x40
//     set (selected AND busy/animated), compute a dir24 heading from `move_redirect_col/row` toward
//     the cursor, stamp the OVERLAY byte (`tile_overlay()`, `mh_map_tile_object_data::unit[1]`) at
//     that tile with `heading - 0x40` and the unit's own `click_preview_facing` with the raw heading
//     -- exactly the mechanism `mh_tact_unit_record::click_preview_facing`'s own field comment
//     already documents for this call site.
//
// 23. @0x0042a8f1-0x0042aa00: LEFT-CLICK DESELECT-ALL / DRAG-START. Two more button==1 checks: (a)
//     no shift, some unit selected, no hovered unit -> clear ALL selection bits, refresh panel; (b)
//     (falls through unconditionally from (a)'s own end) no shift, no hovered unit, and
//     `active_unit_count_cached()==0` -> latch drag_anchor_x/y from the cursor and set
//     drag_select_active. (b)'s own gate is independent of (a) actually firing -- both were derived
//     from separate CMP/Jcc chains, not an if/else.
//
// 24. @0x0042aa00-0x0042ac0b: DRAG-BOX (clamp every frame while `drag_select_active()==1`; release
//     when buttons drop to 0 with the box still active and click_action_taken clear): computes the
//     box's four tile-space corners from drag_anchor/cursor via the SAME shift-divide/IDIV pair,
//     sorts each axis independently (col-pair then row-pair, in that order), clears selection,
//     zeroes hovered_unit_id, and for every tile in the box reads `.building` (the occupancy stamp
//     tactical mode repurposes this strategic field for -- see `tile_object_at().building`'s own
//     field comment) as a unit index; if owned (owner==0) and nonzero, selects it. Releases the drag
//     and refreshes the panel.
//
// 25. @0x0042ac24-0x0042ac3f: `update_units_and_fx()`, `unit_owner_tick(1)`, `unit_owner_tick(0)`
//     (three already-translated siblings), then `tile_overlay_refresh()` (frontier).
//
// 26. @0x0042ac3f-0x0042ad70: EXIT-CONFIRM-OPEN OVERLAY. When `exit_confirm_open()==1`: format
//     "ENTER - %s, ESC - %s" (`G_TEXT_PTRS[0x29b]`, `G_TEXT_PTRS[0x2dd]`) into the shared
//     `G_TEXT_TMP` scratch buffer via `w_sprintf__vss`, draw it at the fixed screen position
//     (150,240), then TWO tile-vis-map refresh loops: a 2x20-cell strip at row-base
//     `240/32 = 7` (i.e. cells `[140,160)` and `[160,180)` of the 300-entry array -- the SAME
//     shift-divide-by-32 idiom, applied here to the literal y=240 the draw call just used, so it is
//     algebraically forced to exactly 7 and NOT runtime-variable), and then the WHOLE 300-entry
//     extent unconditionally -- redundant with the first loop's cells, preserved literally per the
//     translator brief's "preserve reset-then-fill / redundant writes" rule.
//
// 27. @0x0042ad70: `frame_cursor_and_reset()` (frontier) on every path that reaches here (both the
//     step-6-through-26 main body and, earlier, the step-7 transition branch call it too, as their
//     own final act before the four "return to strategic" siblings).
//
// PROOF PATH: RIG. `net_selftest.exe tacttest` cannot exercise mouse/keyboard input at all (the
// dequeue/mouse-pump calls are pure frontier reads of the live input ring), so the call-count and
// divergence evidence for this site must come from `test_ui.py --tact-determinism --tact-synth`
// driving a real tactical mission through the DLL-native harness, not from the offline suite alone.
//
#pragma once
#include <cstdint>

#include "tact/tact_state.h"

namespace mh::tact {

// Every outward call this body makes that is NOT an already-translated sibling (Law 3b): frontier
// callees only, indirected for offline testability. The already-translated siblings
// (ambient_sound_tick, group_issue_order, select_next_unit, unit_enqueue_command, view_shift_*,
// cam_follow_selection_tick, door_tick, teleport_zone_scan_tick, sidebar_dispatch,
// update_units_and_fx, unit_owner_tick, squad_sync_hp, scroll_target_proximity_tick,
// mission_end_return_to_strategic, units_reset_hp_for_active, camera_center_on_tile, calc_dir24,
// selection_clear_unless_ctrl) are called through their OWN public wrappers instead, exactly as
// tact_update_units_and_fx.cpp calls unit_weapons_tick/fx_update_projectile.
struct frame_calls {
    int32_t (*key_queue_empty)();                                           // llm_input_key_queue_empty @0x004d1018
    void (*key_dequeue)(uint32_t event_ptr);                                // llm_input_key_dequeue @0x004d1030
    double (*time_now)();                                                   // time_GetCurrentTime @0x00427616
    void (*selection_panel_refresh)();                                      // llm_tact_selection_panel_refresh @0x00434af7
    void (*save_screenshot)();                                              // LIBMH_EVK_SCREENSHOT_SAVE
    void (*mouse_delta_pump)();                                             // llm_input_mouse_delta_pump @0x00426645
    int32_t (*mouse_buttons_get)();                                         // llm_input_mouse_buttons_get @0x00426712
    void (*gfx_load_sprite)(int32_t sprite_id);                             // LIBMH_EVK_INV_TACT_CURSOR_SPRITE
    void (*render_view)();                                                  // llm_tact_render_view @0x0042d237
    void (*drag_box_clamp)(int32_t x1, int32_t y1, int32_t x2, int32_t y2); // LIBMH_EVK_INV_TACT_DRAG_BOX
    void (*tile_overlay_refresh)();                                         // LIBMH_EVK_INV_TACT_MINIMAP_OVERLAY
    void (*zone_sound_play)(int32_t sound_id);                              // llm_tact_zone_sound_play @0x0042f229
    // LIFT-TACT slice B: the sixteen-iteration mine-blast wipe collapsed to ONE scope,
    // LIBMH_EVK_SCR_TACT_BLAST_TRANSITION -- nothing of libmh's happens between its iterations, so
    // what a host needs is "play the exit wipe", not forty-eight fine steps. The three members it
    // replaced (scroll_fade_step, blink_overlay_clear, frame_cursor_and_reset) are gone from this
    // struct in their looped role; the last two keep members for their OTHER, standalone sites.
    void (*blast_transition)();
    void (*blink_overlay_clear)();  // LIBMH_EVK_INV_TACT_DRAWN_MAP_CLEAR
    void (*vis_map_fill_default)(); // llm_tact_vis_map_fill_default @0x0043abfe
    void (*frame_present)();        // LIBMH_EVK_SCR_TACT_FRAME_PRESENT
    // DECLARED NEED: w_sprintf__vss already exists in mh_calls.gen.h (arity confirmed).
    int32_t (*sprintf_vss)(void *dst, const wchar_t *format, const wchar_t *a0, const wchar_t *a1);
    // LIFT-TACT slice B: LIBMH_EVTK_TEXT_TACT_EXIT_CONFIRM. Only the composed STRING crosses --
    // the position (0x96, 0xf0) and the white are literals and stay in the sink, the same division
    // the other text-surface adapters already make.
    void (*draw_exit_confirm_text)(void *text);
};

const frame_calls &live_frame_calls();

namespace detail {

// llm_tact_frame @0x00429b1a. See the header banner for the full derivation.
void frame(const tact_view &v, tact_store &own, const frame_calls &c);

} // namespace detail

void frame();

// Declared here per the module convention; DEFINED in tact_frame.cpp, CALLED from install_shadow()
// by the conductor (not this TU).
namespace detail {
}

// The PROMOTED arm's installer is declared in tact/tact_promote.h with its four siblings, not here
// -- same arrangement as tact_character_parse.cpp's. See tact_frame.cpp for why this function is
// promoted rather than shadow-armed.

} // namespace mh::tact
