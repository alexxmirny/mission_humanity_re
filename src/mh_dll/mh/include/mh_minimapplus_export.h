//
// mh_minimapplus_export.h -- mods:MM1-MM3: THE STRATEGIC MINIMAP AT ITS WIDEST ZOOM.
//
// THE FLAG. `llm_strat_minimap_frame` (0x004276b0) sets `_G_LLM_STRAT_MINIMAP_FULLMAP_FLAG` (0x005586f2) to 1
// when the zoom (0x005586ee) equals 128 / map width, or is 1, else 0, and calls `llm_strat_minimap_render`
// (0x004a17b3). That one flag gives the widest zoom three stock differences, all removed here:
//
//   MM1  RESOURCES. Four `cmp dword [0x005586f2],1 ; je` gates (the je at 0x004a191c zoom 5, 0x004a1b15 zoom 1,
//        0x004a1ce6 zoom 2, 0x004a20ca zoom 4) jump over the per-pixel resource-marker block. Each je becomes
//        two NOPs, i.e. exactly the path the other zooms take.
//   MM2  ROUND MASK. At the render exit 0x004a22e9 `cmp [flag],1`, 0x004a22f0 `je 0x004a1a1b` runs a pass that
//        paints the area outside a disc (table 0x00d16c80) and ends in `jmp 0x004a22fb`.
//   MM3  VIEW BOX. The same je skips the only `call llm_strat_minimap_draw_viewport_box` (0x004a22f6).
//
// MM2 and MM3 share that je, so it becomes a JMP to `gate_thunk`, which takes the stock branch only when the
// mask is wanted; and the mask pass's closing `jmp 0x004a22fb` (0x004a1aad) becomes a JMP to `end_thunk`, which
// draws the box (after the mask, never under it) when the flag is 1 and the box is wanted. Both thunks are
// no-ops for flag != 1. The box still goes through RES9's clipped draw (ui_minimap_box.cpp).
//
// Render only: the patched code writes the side-panel surface, nothing hashed, no RNG.
//
// SETTINGS. `[ui] minimap_plus` (user, default 1, restart) gates all three. C_DEV `[ui] minimap_res_all_zooms`,
// `minimap_top_no_mask`, `minimap_top_box`: -1 = follow minimap_plus, 0/1 force one part, so each is provable
// alone. Boot line: `; minimap: resources at every zoom ON|off, top-level mask off|ON, top-level box ON|off`
// (the mask word is the state of the MASK: off = removed, ON = stock).
//
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Best-effort: a wrong byte string, a promoted owner or a refused VirtualProtect leaves that site stock and
// logs it. Returns the number of sites armed. Never fails the process.
int MH_MinimapPlus_Install(void);

#ifdef __cplusplus
}
#endif
