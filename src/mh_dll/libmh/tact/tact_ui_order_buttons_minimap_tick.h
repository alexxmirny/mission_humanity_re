//
// tact/tact_ui_order_buttons_minimap_tick.h -- TACT1E: the tactical HUD's group-order button row
// (4 buttons: CLEAR / STOP+HOLD / MOVE-preview-clear+FIRE / ATTACK-preview-clear+FIRE) and the
// minimap click-to-recenter-camera handler.
//
//   llm_tact_ui_order_buttons_minimap_tick @0x00435cea (0x4cb)
//   void __watcall llm_tact_ui_order_buttons_minimap_tick(void)
//
// Re-derived from the DISASSEMBLY (tmp/decomp_tact/llm_tact_ui_order_buttons_minimap_tick_00435cea.asm),
// cross-checked field-by-field against Ghidra's own .c draft (which, unusually for this cluster, turned
// out to already match the assembly -- every register/stack argument order, branch, and constant below
// was re-derived independently from the opcodes and only THEN compared to the .c; no divergence found).
//
// GATE (@0x00435d02-0x00435d09): the whole function is a no-op once a sidebar hit is already pending
// (`sidebar_ui_hit_code() > 0`).
//
// FOUR BUTTON BRANCHES, mutually exclusive (each is only reached if every earlier one's hit-rect test
// failed), each gated the same way -- `mouse_in_rect(...) == 1 && *mouse_buttons_cur > 0 &&
// sidebar_ui_hit_code() == 0` -- and each doing the same three things: redraw its icon via
// `llm_ui_set_draw_surface`, stamp the 3 TILE_VIS_MAP cells immediately covering the button row dirty,
// then one or two `llm_tact_group_issue_order` calls, then stamp a distinct `sidebar_ui_hit_code`:
//
//   1. @0x00435d0f-0x00435dc3: rect (1,0x8e)-(0x15,0xa2), icon slot 0x23 (35). order 0x46 (CLEAR).
//      hit_code <- 0x3c.
//   2. @0x00435dc8-0x00435e8e: rect (0x14,0x8f)-(0x25,0xa4), icon slot 0x24 (36). orders 0x46, 0x40.
//      hit_code <- 0x3d.
//   3. @0x00435e93-0x00435f5e: rect (0x25,0x8f)-(0x36,0xa4), icon slot 0x28 (40). Also calls
//      `llm_tact_move_path_preview_clear()` before issuing orders 0x47, 0x47 (twice, literally --
//      not a typo in the original). hit_code <- 0x3f.
//   4. @0x00435f63-0x0043602e: rect (0x36,0x91)-(0x4a,0xa5), icon slot 0x25 (37). Also calls
//      `llm_tact_move_path_preview_clear()` before issuing orders 7, 0x47. hit_code <- 0x3e.
//
// THE TILE_VIS_MAP_M6/M5/M4 WRITES in every branch above are the SAME pattern
// tact_sidebar_dispatch.cpp's header already documents: NOT three independent globals, but the three
// bytes immediately preceding the real _G_LLM_TILE_VIS_MAP array (base 0x00713d20; the writes land at
// base-6, base-5, base-4). Expressed here, as there, as three consecutive `tile_vis_map_at()` cells
// starting at `view_tiles_w() * 7 - 6`.
//
// llm_tact_group_issue_order IS A SIBLING MIGRATION MEMBER (libmh/tact/tact_group_issue_order.{h,cpp}),
// not a frontier callee -- its own header banner names THIS function as one of its 13 call sites. Per
// the translator brief's 3b same-set exception, every call to it below goes through its own public
// wrapper `mh::tact::group_issue_order(...)`, not through this file's `_calls` struct.
//
// icon slot indices (0x23/0x24/0x28/0x25) are byte offsets into _G_LLM_TACT_SEL_PANEL_ICON_GFX
// (base 0x00558f04, void*[41]) divided by 4: (0x00558f90-0x00558f04)/4=0x23=35,
// (0x00558f94-...)/4=0x24=36, (0x00558fa4-...)/4=0x28=40, (0x00558f98-...)/4=0x25=37. Each slot holds
// a pointer to sprite data whose first two `ushort`s (offsets 0 and 2) are read as the redraw's
// clip_w/clip_h -- read through `tact_store::sel_panel_icon_gfx_at()` (a write-oriented accessor,
// read-only here; same pattern as tact_sidebar_dispatch.cpp's own use of the same accessor).
//
// THE MINIMAP BRANCH (gate @0x00436033-0x0043608b, body @0x00436090-0x004361a6, reached only if none
// of the 4 buttons hit): gated on
// `sidebar_ui_hit_code() == 0 && *mouse_buttons_cur == 1` and the mouse being strictly inside the
// 0x80 x 0x80 box anchored at (_G_LLM_TACT_UI_MINIMAP_ORIGIN_X, _G_LLM_TACT_UI_MINIMAP_ORIGIN_Y) --
// declared_needs: THESE TWO GLOBALS (0x0050ffb7/0x0050ffbb, already named in Ghidra per
// docs/symbols.md but absent from mh_addrs.gen.h/tact_view -- read-only in this function, no writer
// found). On a hit, recenters _G_LLM_MAP_CAM_COL/_G_LLM_MAP_CAM_ROW on the click position (offset by
// half the visible viewport, itself computed via G_WIN_W/64/32 and WindowHeight/64/24), clamps both to
// [0, extent - viewport), and calls `llm_tact_vis_map_fill_default()`.
//
// THE SIGNED-DIVISION IDIOM (@0x0043609d-0x004360b3 and the 3 similar sites): the compiler's
// shift-form of C++ truncating division by a power of two -- `SAR sign,0x1f` / `SHL sign,shift` /
// `SBB acc,sign` / `SAR acc,shift` -- reproduced here as `(x + (sign & ((1<<shift)-1))) >> shift`
// with `sign = x >> 31`, the same idiom already established in mh/sim (sim_bldg_completion_dispatch.cpp,
// sim_tile_delta_wrapped.cpp: `(x - (x>>31)) >> 1` for /2; this is its /64 and /32 generalization).
// Hand-verified bit-exact against the raw SAR/SHL/SBB/SAR sequence for both signs -- see uncertainties[].
// G_WIN_W is read/computed twice independently by the /32 term (@0x004360ce and @0x004360f6) and
// WindowHeight/24 twice by the IDIV term (@0x0043615a and @0x00436181); both are CSE'd to one local
// here since neither global has a writer between the two reads in this function (same reasoning as
// sim_path_group_steps.cpp's own `half_w`/`half_h` CSE). WindowHeight/24 (@0x0043616d/0x00436194) is a
// genuine `IDIV`, not the shift form -- reproduced directly as `/`.
//
// PROOF PATH: the minimap branch is RIG-or-OFFLINE (every callee it reaches -- mouse_in_rect,
// set_draw_surface, vis_map_fill_default -- is mocked via `_calls`). The 4 button branches are
// RIG-ONLY: each calls the sibling `mh::tact::group_issue_order`, whose OWN callee
// `llm_tact_unit_enqueue_command` is a frontier function reached through a real game VA
// (`mh::call::...`), unmapped inside `net_selftest.exe` -- same caveat tact_sidebar_dispatch.h already
// states for its own sibling call to `ui_char_panel_row_draw`.
//
#pragma once
#include <cstdint>

#include "tact/tact_state.h"

namespace mh::tact {

// Mockable frontier-callee seam (translator brief 3b). llm_tact_group_issue_order is deliberately
// NOT a member here -- it is a sibling migration function, called through its own public wrapper
// (see this header's banner).
struct ui_order_buttons_minimap_tick_calls {
    int32_t (*mouse_in_rect)(int32_t rect_x0, int32_t rect_y0, int32_t rect_x1,
                             int32_t rect_y1); // llm_tact_ui_mouse_in_rect
    // LIFT-TACT slice A: the four button icon blits became ONE on_invalidate scope carrying
    // the button identity (0..3). The slot/x/y table and the blit itself live in mh.dll's
    // sink now -- see LIBMH_EVK_INV_TACT_ORDER_BUTTON. The offline suite can no longer assert
    // the pixel geometry, which is the intended effect of the lift and not an oversight; the
    // hosted UI capture is what proves the pixels.
    void (*order_button)(int32_t button);
    void (*move_path_preview_clear)(); // llm_tact_move_path_preview_clear
    void (*vis_map_fill_default)();    // llm_tact_vis_map_fill_default
};

const ui_order_buttons_minimap_tick_calls &live_ui_order_buttons_minimap_tick_calls();

namespace detail {

// llm_tact_ui_order_buttons_minimap_tick @0x00435cea. See this header's own banner for the full
// derivation; the body comments cite the specific address ranges.
void ui_order_buttons_minimap_tick(const tact_view &v, tact_store &own,
                                   const ui_order_buttons_minimap_tick_calls &c);

} // namespace detail

void ui_order_buttons_minimap_tick();

// Declared here per the module convention; DEFINED in tact_ui_order_buttons_minimap_tick.cpp,
// CALLED from install_shadow() by the conductor (not this TU).
namespace detail {
}

} // namespace mh::tact
