//
// tact/tact_sidebar_dispatch.h -- TACT1E: the tactical sidebar's per-frame input dispatcher.
//
//   llm_tact_sidebar_dispatch @0x00435972 (0x378)
//
// Translated from the DISASSEMBLY (tmp/decomp_tact/llm_tact_sidebar_dispatch_00435972.asm), not from
// Ghidra's .c -- the .c had already drifted from the assembly for this function (see the translator
// brief). Two things happen here, gated independently:
//
//   1. @0x0043598a-0x004359e3: IF the cursor is over the sidebar (cursor_x >= win_w), latch
//      sidebar_mouse_x/y from (cursor_x - win_w, cursor_y), clear the hovered-unit latch, and run
//      exactly one of the three per-mode per-frame ticks (minimap / multi-select-panel /
//      single-select-panel), chosen by mouse_y vs the 0xa8 minimap-height boundary and (above it)
//      by ui_sel_panel_multi_mode. This block is SKIPPED ENTIRELY when the cursor is left of the
//      sidebar -- no mouse_x/y write, no per-mode tick, in that case.
//   2. @0x004359e3-0x00435ce0: on a pending, just-RELEASED sidebar hit (sidebar_ui_hit_code > 0 AND
//      mouse_buttons_cur == 0), dispatch on the hit code:
//        - ==2                    : consumed with no action (falls through to the checks below with
//                                   hit_code now 0, which never matches any of them).
//        - in [0x3c,0x46)         : redraw the group-assign icon panel via llm_ui_set_draw_surface,
//                                   then stamp the three TILE_VIS_MAP cells immediately covering it.
//        - ==0xa                  : llm_tact_ui_draw_player_row_list(-1).
//        - in [0x14,0x28)         : ASSIGN one unit (looked up through the icon-slot LUT) to the
//                                   active squad group -- squad_group_id = active_group_id.
//        - in [0x28,0x3c)         : UNASSIGN one unit (looked up through the roster table) --
//                                   squad_group_id = 0xff. Both LUT and roster branches finish with
//                                   selection_panel_refresh() + draw_player_row_list(-1).
//        - otherwise, single-select mode (ui_sel_panel_multi_mode == 0):
//            - ==1                : toggle bit 1 of every non-negative sel_panel_icon_slot_state
//                                   slot (4 of them), redraw if any toggled.
//            - in [0x64,0x78) / [0x78,0x8c): look up a unit via sidebar_slot_unit_ids (offset by
//                                   sidebar_slot_scroll) and draw its char-panel row.
//            - otherwise          : no-op.
//        - otherwise, multi-select mode (ui_sel_panel_multi_mode != 0):
//            - ==1                : same toggle as above but over sidebar_scrollbtn_state (8 slots).
//            - otherwise          : no-op.
//
// THE LUT-VS-ROSTER ADDRESSING (@0x00435abb-0x00435b01) -- read this before touching either branch:
//
//   Both branches read a raw little-endian int32 (a unit INDEX, then multiplied by the tact_unit
//   record stride 0x5f4 to reach that unit's squad_group_id at +0x5f0) from a byte-addressed base,
//   NOT a properly bounds-matched array index:
//
//     hit_code <  0x28: unit_idx = *(int32*)(sidebar_icon_slot_unit_lut + hit_code*4)
//     hit_code >= 0x28: unit_idx = *(int32*)(unassigned_unit_roster + 0x60 + hit_code*4
//                                             + (active_group_id << 8))
//
//   Neither constant (0 for the LUT branch, 0x60 for the roster branch) was invented -- both are the
//   literal immediates the compiled instructions embed relative to each region's OWN declared base
//   (_G_LLM_TACT_SIDEBAR_ICON_SLOT_UNIT_LUT @0x0086fd34, _G_LLM_TACT_UNASSIGNED_UNIT_ROSTER
//   @0x0086fd84 -- see mh_regions.gen.h). Two things fall out of that:
//
//   (a) The LUT branch's own hit_code range here is [0x14,0x28), i.e. byte offsets [0x50,0x9c) from
//       the LUT's base -- past the LUT's OWN declared 40-byte extent (10 int32 slots) and squarely
//       inside the ADJACENT _G_LLM_TACT_UNASSIGNED_UNIT_ROSTER's 256 bytes instead. This function
//       never reads the LUT's first 40 declared bytes at all. Preserved literally (declared_needs:
//       the LUT's true extent/shape doesn't match what this function demonstrably touches).
//   (b) The roster branch's `(active_group_id << 8)` term, for active_group_id in [0,7], walks the
//       address PAST unassigned_unit_roster's own 256-byte extent and into the immediately-following
//       _G_LLM_TACT_GROUP_UNIT_ROSTER region (2048 B = 8 rows of 256 B each) -- confirmed by exact
//       arithmetic: unassigned_unit_roster + 0x60 + 0x28*4 + 0*256 == _G_LLM_TACT_GROUP_UNIT_ROSTER's
//       own base address precisely. Only active_group_id == -1 (the documented "unassigned" sentinel
//       on mh_tact_unit_record::squad_group_id) keeps the access inside unassigned_unit_roster
//       itself (the <<8 of -1 subtracts exactly one 256-byte row). declared_needs: there is no
//       `group_unit_roster` view member for the non-sentinel case; this file reaches it via literal
//       byte arithmetic from `unassigned_unit_roster` instead of a byte offset invented from nothing,
//       per the translator brief's exception for a hazard already called out in the assignment brief.
//
// THE TILE_VIS_MAP_M4/M5/M6 WRITES (@0x00435a4c-0x00435a76) are NOT three independent globals --
// they are the three bytes immediately preceding the real _G_LLM_TILE_VIS_MAP array (base
// 0x00713d20; the three writes land at base-6, base-5, base-4). Expressed here as three consecutive
// tile_vis_map_at() cells starting at view_tiles_w()*7 - 6.
//
// PROOF PATH: RIG (net_selftest.exe tacttest is the offline half; the rig is the arm target per
// tools/data/tact_migration.json). No floats, no RNG, no CRT calls.
//
#pragma once
#include <cstdint>

#include "tact/tact_state.h"

namespace mh::tact {

// Mockable frontier-callee seam (translator brief 3b): every callee below is a FRONTIER function
// (not itself a migration member), so this body reaches them only through this struct, never
// through mh::call:: directly. The sibling-migration-member exception this banner used to record
// is GONE with LIFT-TACT slice A: llm_tact_ui_char_panel_row_draw was the one member called
// through its own public wrapper, and it has left libmh entirely -- it is now the
// `char_panel_row_draw` scope below, an ordinary member like every other.
struct sidebar_dispatch_calls {
    void (*minimap_tick)();     // llm_tact_ui_order_buttons_minimap_tick
    void (*multi_mode_tick)();  // llm_tact_ui_sel_panel_multi_mode_tick
    void (*single_mode_tick)(); // llm_tact_ui_sel_panel_single_mode_tick
    // LIFT-TACT slice A: the group-assign panel's blit became a nullary on_invalidate scope
    // (LIBMH_EVK_INV_TACT_GROUP_PANEL). Its geometry was all literal, so nothing crosses.
    void (*group_panel)();
    void (*draw_player_row_list)(int32_t selected_row); // llm_tact_ui_draw_player_row_list
    void (*selection_panel_refresh)();                  // llm_tact_selection_panel_refresh
    // LIFT-TACT slice A: the selection panel's contents redraw is the on_invalidate scope
    // LIBMH_EVK_INV_TACT_SEL_PANEL (kind 13), which llm_tact_selection_panel_refresh's own
    // conversion already established -- this site was the straggler still holding the fine entry.
    void (*sel_panel_draw)();
    // LIFT-TACT slice A: llm_tact_ui_char_panel_row_draw went host-side WHOLE. Its entire write
    // set was tile_vis_map dirty bytes -- not a TACT_HASH_REGIONS member, no translated reader --
    // so there was nothing to hoist above the cut and nothing to keep. What crosses is which unit
    // in which row: LIBMH_EVK_INV_TACT_CHAR_PANEL_ROW_DRAW.
    void (*char_panel_row_draw)(int32_t unit_idx, int32_t row_slot);
};

namespace detail {

// llm_tact_sidebar_dispatch @0x00435972. See this header's own banner for the full control-flow
// map; the body comments below cite the specific address ranges.
void sidebar_dispatch(const tact_view &v, tact_store &own, const sidebar_dispatch_calls &c);

} // namespace detail

void sidebar_dispatch();

// Declared here per the module convention; DEFINED in tact_sidebar_dispatch.cpp, CALLED from
// install_shadow() by the conductor (not this TU).
namespace detail {
}

} // namespace mh::tact
