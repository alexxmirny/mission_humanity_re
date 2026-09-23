//
// include/mh_tile_dirty.h -- mp:GX1: mark screen tiles DIRTY in the ground layer's DAMAGE MAP.
//
// THE MECHANISM (EN v411, recovered 2026-09-22; the plate on llm_strat_render_ground_tile
// @0x004a5167 carries the full recovery, and the whole of it is restated here).
// `_G_LLM_TILE_VIS_MAP` is one byte per 32x32 SCREEN tile, index `row*_G_LLM_VIEW_TILES_W + col`.
// Retail's own per-tile floor redraw repaints a tile ONLY if bit 0 of its byte is set (or the
// cached fog shade changed) and CONSUMES the bit on repaint -- confirmed in TWO consumers, not
// one: `llm_strat_render_ground_tile` (strategic mode 2, gate `(vis[idx] & 1) == 0` -> skip,
// else clear the WHOLE byte to 0 and blit) and `llm_tact_tlo_tile_blit` (tactical mode 6, gate
// `vis[idx] == 0` -> skip, else DECREMENT and blit -- a counter, not a bit, but `|= 1` still
// guarantees a nonzero value and therefore exactly one repaint, same as the strategic gate).
// Both read the SAME array through the SAME index formula, so one helper serves both modes.
// `_G_LLM_TILE_DRAWN_MAP` is stamped `= 1` in lockstep by both retail consumers at the same
// index; it has no measured reader (2026-09-22 dig), but mimicking it is free and keeps our
// shadow of retail's bookkeeping exact rather than a guess about which half matters -- so this
// helper stamps it too.
//
// Every retail primitive that touches the framebuffer stamps what it touched (the ground blit
// above; `llm_gfx_text_mark_tiles_dirty` for glyph runs; `llm_gfx_draw_text_rgb`'s own internal
// layout->mark->draw). Our own seams that write pixels directly (mh/seams/gfx_overlay.cpp) or
// call the ONE game text wrapper that skips the stamp (mh/seams/ui_net_indicator.cpp, historically)
// did not, so whatever they drew stuck forever once the ground pass had no reason to touch that
// tile again -- the actual "our overlays smear" bug. This header is the one place both seams
// call to do what retail does.
//
// RESOLVED THROUGH THE REGION RUNTIME, NOT A CONSTANT ADDRESS. `_G_LLM_TILE_VIS_MAP`,
// `_G_LLM_TILE_DRAWN_MAP`, `_G_LLM_VIEW_TILES_W` and `_G_LLM_VIEW_TILES_H` are all state-registry
// regions (mh::state::RID_*); `mh::state::live_base`/`ptr<T>` is the SB-HOSTFREE-safe way to read
// one (a movable region read at its stock `mh::addr::` constant would read poison the day
// something relocates it -- check_movable_addresses.py is the gate that would catch a regression
// here). See src/mh_dll/libmh/state/region_runtime.h for the policy this follows.
//
// tooling:TL-TILEMAP-EXTENT (open, filed alongside mp:GX1): the registry currently declares BOTH
// maps at their OLD, short extent -- 300 bytes -- even though the real extent is 1024
// (`_G_LLM_TILE_VIS_MAP`) / 1018 (`_G_LLM_TILE_DRAWN_MAP`) and the game writes index 767 at a
// 1024x768 window. Widening the registry entries is THAT row's job, not this one's: touching it
// here would move the fixture fingerprint out from under a row this lane does not own. So every
// write below is CLAMPED to whatever `mh::state::live_size()` answers right now (300, today) --
// an interim narrowing, not a claim about the map's true size. The clamp becomes unnecessary
// (harmless, since `i < live_size` will simply always hold) the day TL-TILEMAP-EXTENT widens the
// registry entries; nothing here needs to change when that lands.
//
#pragma once
#include <cstdint>

#include "state/region_runtime.h" // mh::state::live_base/live_size/ptr, RID_TILE_VIS_MAP/RID_TILE_DRAWN_MAP/RID_VIEW_TILES_W/RID_VIEW_TILES_H

namespace mh::gfx {

// Mark every ground-layer tile covered by the SCREEN-pixel rect [x, x+w) x [y, y+h) dirty, so
// retail's own floor redraw repaints it (strategic mode 2's llm_strat_render_ground_tile, or
// tactical mode 6's llm_tact_tlo_tile_blit -- whichever mode is actually running; the call is a
// harmless no-op write in every other mode, none of which has a consumer for either map at all).
//
// Rows/cols run x>>5 .. (x+w-1)>>5 and y>>5 .. (y+h-1)>>5 (32x32 tiles, matching retail's own
// `_G_LLM_GFX_TILE_DRAW_ROW/COL` stride), clamped to [0, _G_LLM_VIEW_TILES_W/H) so a caller's
// rect hanging off the window edge cannot walk past either live array's declared length via a
// huge row/col multiplying VIEW_TILES_W into something the clamp below no longer catches.
inline void mark_ground_tiles_dirty(int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return;

    const int32_t view_w =
        *mh::state::ptr<const int32_t>(mh::state::RID_VIEW_TILES_W);
    const int32_t view_h =
        *mh::state::ptr<const int32_t>(mh::state::RID_VIEW_TILES_H);
    if (view_w <= 0 || view_h <= 0) return;

    uint8_t *const vis = mh::state::ptr<uint8_t>(mh::state::RID_TILE_VIS_MAP);
    if (vis == nullptr) return;
    uint8_t *const drawn     = mh::state::ptr<uint8_t>(mh::state::RID_TILE_DRAWN_MAP);
    const uint32_t vis_len   = mh::state::live_size(mh::state::RID_TILE_VIS_MAP);   // TL-TILEMAP-EXTENT clamp
    const uint32_t drawn_len = mh::state::live_size(mh::state::RID_TILE_DRAWN_MAP); // TL-TILEMAP-EXTENT clamp

    int col0 = x >> 5, col1 = (x + w - 1) >> 5;
    int row0 = y >> 5, row1 = (y + h - 1) >> 5;
    if (col0 < 0) col0 = 0;
    if (row0 < 0) row0 = 0;
    if (col1 >= view_w) col1 = view_w - 1;
    if (row1 >= view_h) row1 = view_h - 1;

    for (int row = row0; row <= row1; ++row) {
        const uint32_t base = static_cast<uint32_t>(row) * static_cast<uint32_t>(view_w);
        for (int col = col0; col <= col1; ++col) {
            const uint32_t i = base + static_cast<uint32_t>(col);
            if (i < vis_len) vis[i] |= 1u;
            if (drawn != nullptr && i < drawn_len) drawn[i] = 1u;
        }
    }
}

} // namespace mh::gfx
