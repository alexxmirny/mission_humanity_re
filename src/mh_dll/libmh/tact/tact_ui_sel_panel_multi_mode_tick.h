//
// tact/tact_ui_sel_panel_multi_mode_tick.h -- TACT1E: per-frame input tick for the tactical
// unit-selection sidebar panel while it is in MULTI-SELECT mode (ui_sel_panel_multi_mode != 0).
//
//   llm_tact_ui_sel_panel_multi_mode_tick @0x004361b5 (0x80c)
//   void __watcall llm_tact_ui_sel_panel_multi_mode_tick(void)
//
// Re-derived from the DISASSEMBLY (tmp/decomp_tact/llm_tact_ui_sel_panel_multi_mode_tick_004361b5.asm)
// address-by-address, not trusted from Ghidra's `.c` -- the `.c` happened to agree with the opcodes
// here (cross-checked independently), but per the translator brief the asm is the spec regardless.
//
// GATE (@0x004361cd-0x004361d4): the entire body is skipped when `sidebar_ui_hit_code() > 0` (a
// pending hit from a PRIOR frame that hasn't been consumed yet by whichever handler owns that hit
// code -- see tact_sidebar_dispatch.h/.cpp, the sibling that clears hit codes on button release).
//
// ONE, in order, once the gate passes:
//
//  1. MODE-TAB CLICK (@0x004361da-0x0043627d): if the LEFT button is down and
//     (0xa8 < sidebar_mouse_y < 0xc0) and (sidebar_mouse_x >= 0x50) -- the multi-select tab hot-zone
//     -- set ui_sel_panel_multi_mode = 1, redraw icon slot 4 at (0,0xa8), refresh the panel, stamp
//     hit_code = 2, and RETURN. Otherwise fall through to 2.
//
//  2. GROUP ASSIGN/RECALL (@0x00436282-0x004363cd): if the mouse is over the unit-list roster rect
//     (0,0xc0,0xa0,0xd8) with any button held: `group_row = sidebar_mouse_x / 0x14` (IDIV, genuine
//     signed divide, not a shift form) becomes the new `sidebar_active_group_id`. RIGHT button
//     (==2): for every owner-0, non-empty, SELECTED unit (slots 1..0x80 inclusive), stamp
//     `squad_group_id = (uint8_t)group_row` ("assign to group"), then
//     `llm_tact_squad_roster_refresh()`. Any OTHER button: `llm_tact_selection_clear_unless_ctrl()`,
//     then for every owner-0, non-empty unit whose `squad_group_id == group_row`, OR selection bit 1
//     into `status` ("recall group" -- no early break, every match gets selected). Either arm then
//     falls into the SAME tail: `selection_panel_refresh()`, `ui_draw_player_row_list(group_row)`,
//     hit_code = 0xa. This block does NOT return -- execution continues into 3.
//
//  3. HIGHLIGHT-ON-HOVER (@0x004363d7-0x00436445): only when NO button is held, over the row-list
//     rect (0,0xf0,0xa0,WindowHeight): compute `row = (sidebar_mouse_y-0xf0)/0x30` (genuine IDIV) and
//     latch `sidebar_highlighted_unit_id = sidebar_slot_unit_ids[row+sidebar_slot_scroll]` IFF that
//     slot holds a positive unit id (a zero/negative slot leaves the previous highlight untouched).
//
//  4. FOUR SCROLL-BUTTON HOT-ZONES, cascading if/else-if, each over sel_panel_icon_slot_state[N] > 0
//     as the "this icon is currently enabled" gate ANDed with a click on its rect and the LEFT
//     button held and the slot NOT already latched (bit 0 clear) -- OR the icon in, set/adjust
//     sidebar_slot_scroll, refresh, hit_code = 1 (@0x00436445-0x00436602):
//       slot 0, rect (0,0xd8,0x28,0xf0)  -> scroll = 0                      (scroll-to-top)
//       slot 1, rect (0x28,0xd8,0x50,0xf0) -> scroll -= 1                   (scroll up)
//       slot 2, rect (0x50,0xd8,0x78,0xf0) -> scroll += 1                   (scroll down)
//       slot 3, rect (0x78,0xd8,0xa0,0xf0) -> scroll = selected_count - DECLARED-NEED[1] (scroll-to-bottom;
//         `selected_count` is a fresh count of every unit with status bit 0 set, slots 1..0x80)
//     If NONE of the four hot-zones fire, falls into 5.
//
//  5. ROW-LIST CLICK (@0x00436607-0x0043692e), only reached when none of the 4 scroll buttons fired:
//     LEFT button, over (0,0xf0,0xa0,WindowHeight): `row=(sidebar_mouse_y-0xf0)/0x30`,
//     `unit_idx = sidebar_slot_unit_ids[row+sidebar_slot_scroll]`; if `unit_idx > 0`:
//       a. rect (0x84,row*0x30+0xf7,0x9a,row*0x30+0x10d) hit -> DEFENSE-STANCE TOGGLE: redraw icon
//          slot 0x26 at (0x84,row*0x30+0xf7); cycle `unit[unit_idx].def_stat` 0->1->2->3->4->0 (wrap
//          at 4, INC otherwise); if the NEW value is 4, `llm_tact_unit_enqueue_command(unit_idx, 4,
//          0,0,0,0,0)` (op 4 = "kneel" per mh_tact_unit_record::op's doc comment -- no Ghidra enum,
//          see declared_needs); if the NEW value is 3, restamp
//          `wander_check_time = time_GetCurrentTime()`; hit_code = row+100; stamp FIVE consecutive
//          TILE_VIS_MAP cells dirty (value 2) on BOTH of two rows, `row*2+11` and `row*2+12`
//          (@0x0043675b-0x0043679d, columns 0..4) -- these are the SAME `_G_LLM_TILE_VIS_MAP` array
//          tact_store::tile_vis_map_at() already binds (RID_TILE_VIS_MAP, base 0x00713d20), just
//          reached through the auto-labelled `_G_LLM_TILE_VIS_MAP_M6` alias 6 bytes BEFORE that base
//          (0x00713d1a = base-6; see tact_sidebar_dispatch.h's own banner, which derives the same
//          fact for its M4/M5/M6 writes) -- so this translation expresses it as
//          `tile_vis_map_at(row_index*view_tiles_w()+col-6)`, NOT a new accessor. RETURN.
//       b. else rect (0x44,row*0x30+0x101,0x5a,row*0x30+0x117) hit -> GUN TOGGLE: redraw icon slot
//          0x27 at (0x44,row*0x30+0x101); `unit[unit_idx].active_gun ^= 1`; hit_code = row+0x78.
//          RETURN.
//       c. else -> CAMERA RECENTER (falls through, no return): center the strategic camera on
//          `unit[unit_idx]`'s tile, clamped to the map extent --
//          `map_cam_col = pos_col - win_w/0x40`, clamped to `[0, map_width - win_w/0x20]`;
//          `map_cam_row = pos_row - view_tiles_h/2`, clamped to `[0, map_height - view_tiles_h]`
//          (note the ASYMMETRY: the column center subtracts `win_w/0x40` but the column clamp
//          subtracts `win_w/0x20`, while the row center subtracts `view_tiles_h/2` but the row
//          clamp subtracts the FULL `view_tiles_h` -- both verified bit-for-bit against the raw
//          SAR/SBB sequences at @0x00436835-0x0043691a, preserved literally, not "fixed" to match).
//          Then `llm_tact_vis_map_fill_default()`, hit_code = 1. Falls into 6 (does NOT return).
//     If the outer LEFT-button/rect/unit_idx>0 gate fails at any point, jumps straight to 6.
//
//  6. MIDDLE-CLICK DESELECT (@0x0043692e-0x004369b7): only when the MIDDLE button is held, over
//     (0,0xf0,0xa0,WindowHeight): `unit_idx = sidebar_slot_unit_ids[(sidebar_mouse_y-0xf0)/0x30 +
//     sidebar_slot_scroll]`; if `unit_idx > 0`, clear its selection bit, `selection_panel_refresh()`,
//     hit_code = 1. Then RETURN (end of function).
//
// THE SIGNED DIVISIONS BY A POWER OF TWO (win_w/0x40, win_w/0x20, view_tiles_h/2 in step 5c) are
// compiled as SAR+SBB/ADD bit tricks, not IDIV -- verified they are exactly the standard
// truncate-toward-zero compiler idiom for `x / (power of two)` (worked the bit algebra by hand for
// both the SBB and the ADD forms), so plain C++ `/` reproduces them exactly; this is NOT the
// "implement a signed divide as shifts, preserve the shift form" hazard the brief warns about,
// because C++'s own `/` on signed ints already lowers to the identical instruction shape.
//
// PROOF PATH: RIG (net_selftest.exe tacttest is the offline half). No RNG. Floats: ONE FSTP
// (wander_check_time restamp) and the time_GetCurrentTime() call feeding it; no FP comparisons.
//
#pragma once
#include <cstdint>

#include "tact/tact_state.h"

namespace mh::tact {

// Mockable frontier-callee seam (translator brief 3b): every callee here is a FRONTIER function, not
// a migration member, so this body reaches them only through this struct.
struct ui_sel_panel_multi_mode_tick_calls {
    // LIFT-TACT slice A: three blits became two scopes -- the mode tab (shared with the
    // single tick, since both draw the same tab and differ only in icon slot) and the
    // per-row toggle, which carries the ROW rather than a y coordinate.
    void (*sel_panel_mode_tab)(int32_t mode);
    void (*sel_panel_row_toggle)(int32_t which, int32_t row);
    void (*selection_panel_refresh)(); // llm_tact_selection_panel_refresh @0x00434af7
    int32_t (*ui_mouse_in_rect)(int32_t rect_x0, int32_t rect_y0, int32_t rect_x1,
                                int32_t rect_y1);          // llm_tact_ui_mouse_in_rect @0x00435909
    void (*squad_roster_refresh)();                        // llm_tact_squad_roster_refresh @0x004356bd
    void (*selection_clear_unless_ctrl)();                 // llm_tact_selection_clear_unless_ctrl @0x0042af6d
    void (*ui_draw_player_row_list)(int32_t selected_row); // llm_tact_ui_draw_player_row_list @0x004354d3
    int32_t (*unit_enqueue_command)(int32_t unit_id, int32_t op, uint8_t interrupt_flag, int32_t arg0,
                                    uint16_t arg1, uint16_t arg2,
                                    uint16_t arg3); // llm_tact_unit_enqueue_command @0x0042b39d
    double (*time_get_current_time)();              // time_GetCurrentTime @0x00427616
    void (*vis_map_fill_default)();                 // llm_tact_vis_map_fill_default @0x0043abfe
};

const ui_sel_panel_multi_mode_tick_calls &live_ui_sel_panel_multi_mode_tick_calls();

namespace detail {

void ui_sel_panel_multi_mode_tick(const tact_view &v, tact_store &own,
                                  const ui_sel_panel_multi_mode_tick_calls &c);

} // namespace detail

void ui_sel_panel_multi_mode_tick();

namespace detail {
}

} // namespace mh::tact
