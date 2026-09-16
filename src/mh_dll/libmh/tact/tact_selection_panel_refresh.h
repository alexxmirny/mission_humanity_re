//
// tact/tact_selection_panel_refresh.h -- TACT1E: redraws the tactical selection-panel UI after any
// selection/roster change.
//
//   llm_tact_selection_panel_refresh @0x00434af7 (0x71)
//   void __watcall llm_tact_selection_panel_refresh(void)
//
// Re-derived from the DISASSEMBLY (tmp/decomp_tact/llm_tact_selection_panel_refresh_00434af7.asm),
// not trusted from Ghidra's .c per the translator brief -- though every operand below was
// independently re-derived from the raw opcodes and the .c's reading of them happened to agree.
//
// DERIVATION (straight-line, one gate, five outward calls, all frontier):
//
//   1. @0x00434b0f-0x00434b35: llm_ui_set_draw_surface(surface, pitch, x, y, clip_x, clip_y, clip_w,
//      clip_h, src_bitmap), same nine-param prototype already bound and used at every other TACT1E
//      call site in this module (tact_sidebar_dispatch.cpp, tact_ui_sel_panel_init.cpp, ...):
//        surface    = EAX = *_G_LLM_GFX_DRAW_SURFACE      (tact_view::gfx_draw_surface)
//        pitch      = EDX = _G_LLM_GFX_PANEL_ROW_SKIP      (tact_view::gfx_panel_row_skip)
//        x          = EBX = 0                              (XOR EBX,EBX)
//        y          = ECX = 0xf0                            (MOV ECX,0xf0)
//        clip_x     = 0       -- stack arg, pushed LAST (closest to the CALL), i.e. the first
//        clip_y     = 0x48       stack-passed parameter after the four register ones -- reconstructed
//        clip_w     = 0xa0       from the five PUSH instructions in right-to-left source order
//        clip_h     = 0xf0       (standard x86 push order: first PUSH = rightmost/last argument).
//        src_bitmap = _G_LLM_TACT_SEL_PANEL_ICON_GFX[1]   (tact_store::sel_panel_icon_gfx_at(1),
//                     pushed FIRST -- the rightmost, last parameter.)
//   2. @0x00434b3a: llm_tact_squad_roster_refresh() -- unconditional, no arguments.
//   3. @0x00434b3f-0x00434b54: if _G_LLM_TACT_UI_SEL_PANEL_MULTI_MODE == 0,
//      llm_tact_ui_sidebar_roster_refresh(); else llm_tact_ui_sidebar_draw_rows().
//   4. @0x00434b54-0x00434b63: llm_tact_ui_sel_panel_draw() then
//      llm_tact_active_unit_count_hud_draw(), both unconditional, no arguments.
//
// All five callees are FRONTIER (none is a translated migration member of this batch), so the body
// reaches every one of them through the calls-struct per the translator brief's 3b -- including
// llm_tact_selection_panel_refresh's OWN two existing callers in this module
// (tact_select_next_unit.cpp, tact_sidebar_dispatch.cpp), which still call it as
// `mh::call::llm_tact_selection_panel_refresh`; this translation does not touch those call sites.
//
// PROOF PATH: RIG (net_selftest.exe tacttest is the offline half). No float math, no RNG, no roster
// mutation -- every outward effect is one of the five calls above, in this exact order.
//
#pragma once
#include <cstdint>

#include "tact/tact_state.h"

namespace mh::tact {

// Mockable frontier-callee seam (translator brief 3b): every callee here is a FRONTIER function, not
// a migration member of this batch.
struct selection_panel_refresh_calls {
    // The body's TWO draw points are two scope emits (LIFT-TACT L4b), and they
    // stay two because they are not contiguous -- real work runs between them and the hosted sink
    // dispatches where the record is emitted, so merging them would reorder the original (R5).
    void (*sel_panel_bg)();               // was llm_ui_set_draw_surface @0x004a8b73 (icon-1 plate)
    void (*squad_roster_refresh)();       // llm_tact_squad_roster_refresh @0x004356bd
    void (*ui_sidebar_roster_refresh)();  // llm_tact_ui_sidebar_roster_refresh @0x00434b68
    void (*ui_sidebar_draw_rows)();       // llm_tact_ui_sidebar_draw_rows @0x00434ceb
    void (*ui_sel_panel_draw)();          // was llm_tact_ui_sel_panel_draw @0x00435078
    void (*active_unit_count_hud_draw)(); // llm_tact_active_unit_count_hud_draw @0x00434dc2
};

const selection_panel_refresh_calls &live_selection_panel_refresh_calls();

namespace detail {

// llm_tact_selection_panel_refresh @0x00434af7. See this header's own banner for the full
// derivation. Takes `own` in addition to `v`: the src_bitmap argument
// (_G_LLM_TACT_SEL_PANEL_ICON_GFX[1]) and the MULTI_MODE gate are both store-only bindings (no view
// member exists for either -- see tact_state.h's own accessor comments), read here without being
// written; `v` supplies the two gfx globals that DO have a view binding.
void selection_panel_refresh(const tact_view &v, tact_store &own,
                             const selection_panel_refresh_calls &c);

// The inv 6 R3b hoist: llm_tact_ui_sidebar_roster_refresh @0x00434b68's THREE STATE STEPS -- the
// visible-slot rebuild, the scroll clamp, and the four scroll-arrow icon states -- run in libmh
// above the INV_TACT_SIDEBAR_ROSTER emit, because llm_tact_sidebar_dispatch reads all three back
// later in the SAME frame and turns the slot id into a hashed tact_units.status write. Exposed
// rather than kept file-local so tacttest can pin it directly; the .cpp carries the derivation.
void roster_slots_rebuild(const tact_view &v, tact_store &own);

} // namespace detail

void selection_panel_refresh();

// Declared here per the module convention; DEFINED in tact_selection_panel_refresh.cpp, CALLED from
// install_shadow() by the conductor (not this TU).
namespace detail {
}

} // namespace mh::tact
