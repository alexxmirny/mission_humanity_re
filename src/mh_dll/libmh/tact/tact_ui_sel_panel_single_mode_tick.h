//
// tact/tact_ui_sel_panel_single_mode_tick.h -- TACT1E: the single-select-mode tactical sidebar's
// per-frame input tick.
//
//   llm_tact_ui_sel_panel_single_mode_tick @0x004369c1 (0x56c)
//   void __watcall llm_tact_ui_sel_panel_single_mode_tick(void)
//
// Re-derived from the DISASSEMBLY (tmp/decomp_tact/llm_tact_ui_sel_panel_single_mode_tick_004369c1.asm),
// not from Ghidra's .c -- though for this function the two happened to agree at every branch; the .c
// is transcribed here only as a cross-check, never as the source of truth. Called from
// llm_tact_sidebar_dispatch (tact_sidebar_dispatch.cpp) as `c.single_mode_tick()` whenever the cursor
// is over the sidebar, below the 0xa8 minimap boundary, and `ui_sel_panel_multi_mode() == 0`.
//
// GATE (@0x004369d9-0x004369e0): no-op whenever a sidebar hit is already pending
// (`sidebar_ui_hit_code() > 0`) -- the dispatcher only calls this tick when nothing is latched, but
// the original still re-checks, so this stays a real guard, not dead code.
//
// Once the gate passes, exactly one of the branches below fires, in this priority order (every
// non-final branch is a `return` -- the assembly's own unconditional `JMP 0x00436f23` out of each
// block):
//
//   1. @0x004369e6-0x00436a89 MODE-TAB CLICK: `mouse_buttons_cur == 1 && sidebar_mouse_y > 0xa8 &&
//      sidebar_mouse_x < 0x50 && sidebar_mouse_y < 0xc0` (all four ANDed; the two 0x20/0x1c-slot
//      locals the compiler uses to stage the middle two comparisons carry no behaviour of their
//      own). Resets `ui_sel_panel_multi_mode` to 0 (switching back to single-select), redraws icon
//      slot 3 of `sel_panel_icon_gfx` at (0,0xa8) sized by that icon's own 16-bit width/height
//      header, refreshes the panel, and latches hit code 2.
//
//   2. @0x00436a8e-0x00436aeb PLAYER-ROW-LIST CLICK: `mouse_in_rect(0,0xc0,0xa0,0xd8) == 1 &&
//      mouse_buttons_cur > 0`. Sets `sidebar_active_group_id = sidebar_mouse_x / 0x14` (a literal
//      IDIV, so plain C `/` already reproduces its truncate-toward-zero semantics -- not a
//      shift-form divide needing special handling), redraws the player row list for that group, and
//      latches hit code 10.
//
//   3. @0x00436af0-0x00436e02 THE EIGHT SCROLL BUTTONS: `sidebar_scrollbtn_state[i] > 0 &&
//      mouse_in_rect(rect[i]) == 1 && mouse_buttons_cur > 0 && (sidebar_scrollbtn_state[i] & 1) ==
//      0` -- checked in order i=0..7, each a `return` on match. Every match sets bit 0 of its own
//      `sidebar_scrollbtn_state[i]` (the debounce -- keeps the button from re-firing while still
//      held), refreshes the panel, and latches hit code 1. Buttons 0-3 act on the UNASSIGNED list
//      (rects at y=[0xd8,0xf0), x steps of 0x14 starting at 0); buttons 4-7 act on the ACTIVE-GROUP
//      list (same y band, x steps of 0x14 starting at 0x50):
//        i=0 rect(0x00,0xd8,0x14,0xf0): sidebar_unassigned_scroll_row = 0
//        i=1 rect(0x14,0xd8,0x28,0xf0): sidebar_unassigned_scroll_row -= 1
//        i=2 rect(0x28,0xd8,0x3c,0xf0): sidebar_unassigned_scroll_row += 1
//        i=3 rect(0x3c,0xd8,0x50,0xf0): sidebar_unassigned_scroll_row =
//                                       UNASSIGNED_UNIT_ROSTER.count - sidebar_multi_panel_visible_rows
//        i=4 rect(0x50,0xd8,0x64,0xf0): sidebar_group_scroll_row = 0
//        i=5 rect(0x64,0xd8,0x78,0xf0): sidebar_group_scroll_row -= 1
//        i=6 rect(0x78,0xd8,0x8c,0xf0): sidebar_group_scroll_row += 1
//        i=7 rect(0x8c,0xd8,0xa0,0xf0): sidebar_group_scroll_row =
//                                       GROUP_UNIT_ROSTER[active_group_id].count -
//                                       sidebar_multi_panel_visible_rows
//
//   4. @0x00436e02-0x00436f23 THE TWO ROW-LIST HOVER/DRAW CHECKS: these are NOT mutually exclusive
//      with each other, NOR early returns -- both run unconditionally once every button check above
//      falls through, exactly as the assembly does (falls from LAB_00436e02 straight into
//      LAB_00436e87 with no intervening jump when the first check's own `iVar1 > count` guard
//      fails, and both land on the shared epilogue at 0x00436f23). Only the SECOND check's own
//      out-of-rect / button-not-1 failure short-circuits with an early jump straight to the
//      epilogue -- reproduced here by simply falling out of the function after it, since nothing
//      follows.
//        LEFT  (UNASSIGNED list, x in [0,0x50)):  mouse_buttons_cur==1 &&
//              mouse_in_rect(0,0xf0,0x50,WindowHeight)==1 (own.window_height(), RID_WINDOWHEIGHT --
//              the SAME "WindowHeight" global tact_mission_start.cpp's window_height() binds, per
//              mh_regions.gen.h; there is no separate tact_view member for it, so this reads it
//              through the store like that TU does). row = (sidebar_mouse_y-0xf0)/0x18 (literal
//              IDIV again). If `sidebar_unassigned_scroll_row + row + 1 <=
//              UNASSIGNED_UNIT_ROSTER.count`, draws that row highlighted and latches hit code
//              `row + sidebar_unassigned_scroll_row + 0x15`.
//        RIGHT (ACTIVE-GROUP list, x in [0x50,0xa0)): same shape against
//              GROUP_UNIT_ROSTER[active_group_id], hit code base 0x29 instead of 0x15.
//
// THE ROSTER RECORD LAYOUT (both UNASSIGNED_UNIT_ROSTER @0x0086fd84 and GROUP_UNIT_ROSTER[8]
// @0x0086fe84, stride 0x100 for the latter) is `llm_tact_unit_roster_slot`, already named in Ghidra
// but NOT YET in mh_structs.gen.h (same state tact_sidebar_dispatch.h's own banner already recorded
// for the same two regions) -- so both view members stay raw `const uint8_t *` and this file reaches
// `.count` (offset 0) and `.ids[idx]` (offset 4 + idx*4) by the literal byte immediates the compiled
// instructions embed, exactly as that sibling TU does, not by inventing an offset. GROUP_UNIT_ROSTER's
// per-group row offset (`active_group_id << 8`) is computed as an UNSIGNED shift on an explicit
// `uint32_t` cast before being cast back, so a sentinel `active_group_id == -1` reproduces the
// original SHL's bit pattern (one row back) instead of triggering C++'s undefined negative-left-shift
// behaviour -- same idiom tact_sidebar_dispatch.cpp already uses for the identical hazard.
//
// declared_needs (see the structured report): three globals this function reads/writes are absent
// from tact_state.h -- `sidebar_unassigned_scroll_row`/`sidebar_group_scroll_row` (RID_TACT_SIDEBAR_
// UNASSIGNED_SCROLL_ROW / RID_TACT_SIDEBAR_GROUP_SCROLL_ROW already exist in mh_regions.gen.h, just
// not yet wired into tact_store) and `sidebar_multi_panel_visible_rows`
// (_G_LLM_TACT_SIDEBAR_MULTI_PANEL_VISIBLE_ROWS @0x005590c4 -- already named in Ghidra per
// tools/data/en_symbol_index.json, but has no RID/addr entry at all yet). This TU is written as if
// `own.sidebar_unassigned_scroll_row()`, `own.sidebar_group_scroll_row()`, and
// `v.sidebar_multi_panel_visible_rows` exist; it will not build until the conductor adds them.
//
// PROOF PATH: RIG (net_selftest.exe tacttest is the offline half). No floats, no RNG. The only
// outward call whose side effects escape this closure is llm_ui_set_draw_surface/the two row-draw
// helpers -- all frontier, presentation-only.
//
#pragma once
#include <cstdint>

#include "tact/tact_state.h"

namespace mh::tact {

// Every callee here is a FRONTIER function (not itself a migration member), reached only through
// this struct per the translator brief's 3b.
struct ui_sel_panel_single_mode_tick_calls {
    // LIFT-TACT slice A: the mode-tab blit became LIBMH_EVK_INV_TACT_SEL_PANEL_MODE_TAB,
    // shared with the multi tick -- both draw the same tab and differ only in icon slot.
    void (*sel_panel_mode_tab)(int32_t mode);
    void (*selection_panel_refresh)(); // llm_tact_selection_panel_refresh
    int32_t (*mouse_in_rect)(int32_t rect_x0, int32_t rect_y0, int32_t rect_x1,
                             int32_t rect_y1);          // llm_tact_ui_mouse_in_rect
    void (*draw_player_row_list)(int32_t selected_row); // llm_tact_ui_draw_player_row_list
    // LIFT-TACT slice A: the two roster-row draws became ONE scope with a side,
    // LIBMH_EVK_INV_TACT_SIDEBAR_ROW_HOVER. They were never two things -- the original split one
    // gesture into two functions, and only translation order made `_left` a host entry and
    // `_right` a translated body of ours. `highlight_flag` is gone from the signatures because
    // both sites passed the literal 1: it is the original's parameterisation, not information
    // about what happened, and the sink passes it.
    void (*sidebar_row_hover_unassigned)(int32_t unit_id, int32_t row);
    void (*sidebar_row_hover_group)(int32_t unit_id, int32_t row);
};

const ui_sel_panel_single_mode_tick_calls &live_ui_sel_panel_single_mode_tick_calls();

namespace detail {

// llm_tact_ui_sel_panel_single_mode_tick @0x004369c1. See the header banner for the full derivation.
void ui_sel_panel_single_mode_tick(const tact_view &v, tact_store &own,
                                   const ui_sel_panel_single_mode_tick_calls &c);

} // namespace detail

void ui_sel_panel_single_mode_tick();

// Declared here per the module convention; DEFINED in tact_ui_sel_panel_single_mode_tick.cpp, CALLED
// from install_shadow() by the conductor (not this TU).
namespace detail {
}

} // namespace mh::tact
