//
// mh_minimapbox_export.h -- mods:RES9: THE MINIMAP'S VIEWPORT BOX IS DRAWN OUT OF BOUNDS AT LARGE SIZES.
//
// THE PATH. `llm_strat_minimap_frame` (0x004276b0) sizes the "you are here" box from the view each
// frame: per zoom mode, W = G_WIN_W / (32 / mode) + 1 and H = mode * VIEW_TILES_H - (mode - 1), and
// one linear byte offset from the minimap origin, BOX_OFS = pitch * (mode * row_margin - 1) +
// 2 * (mode * col_margin - 1), where the margins are (128 / mode - view tiles) / 2 -- negative once the
// view holds more tiles than the minimap shows (mode 5 takes mode 1's numbers).
// `llm_strat_minimap_draw_viewport_box` (0x004a2344,
// hand-written asm, no parameters) then draws the outline in 0xDEFF into the side-panel surface
// `_G_LLM_GFX_DRAW_SURFACE` at pitch*ORIGIN_Y + 2*ORIGIN_X + BOX_OFS with no clipping. In zoom mode 4
// with 36 or more tile rows (a render height of 1152+) the box's top edge lands above the surface,
// i.e. in whatever heap block precedes it; a wide view (width > 1184) also smears it across the panel
// left of the minimap. Stock sizes (up to 1024x768) always fit.
//
// THE FIX. The entry's MOV EDI,[0x00603b00] (8B 3D 00 3B 60 00) becomes a JMP to a thunk that draws
// the same outline pixel for pixel, skipping every pixel outside the 128x128 minimap. A box that fits
// draws exactly the stock pixels. Each new box geometry logs one `; RES9:` line to mh_net.log with
// what the stock loop would have written outside the surface and the minimap, what was drawn, and a
// canary: the bytes before the surface are snapshotted around the draw and new 0xDEFF words counted.
// `[ui] minimap_box_clip` (C_DEV): 1 = clipped (default); 0 = stock bytes, nothing installed;
// 2 = the stock body behind the same thunk, measured (the diagnosis arm).
//
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Patch the entry (best-effort: a wrong byte string, a promoted owner or a refused VirtualProtect
// leaves it stock and logs it). Returns 1 when armed. Never fails the process.
int MH_MinimapBox_Install(void);

#ifdef __cplusplus
}
#endif
