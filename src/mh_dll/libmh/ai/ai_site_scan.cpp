//
// ai/ai_site_scan.cpp -- see ai_site_scan.h. Translated from the DISASSEMBLY
// (tmp/decomp_a5/llm_strat_ai_bldg_scan_grid_candidates_004e6591.asm,
// tmp/decomp_a5/llm_strat_ai_scan_build_site_candidates_004e66a4.asm), NOT from Ghidra's .c: both
// decompiles are full of extraout_ECX/extraout_EDX/x2/start_x-style phantom locals, the same __cdecl-
// cspec artifact documented on ai_state.h's grid_match_stencil/grid_stencil_all_near_unthreatened --
// the callees are __cdecl and preserve every register but EAX, so Ghidra's cspec (which assumes
// ECX/EDX are call-clobbered) manufactures a fresh SSA value at every call site instead of recognising
// the pre-call register as still live. `bldg_scan_grid_candidates`'s decompile additionally renders
// the home-tile read as `player_data[0].ai_tile_flags_grid + extraout_EDX + -0x18` -- the same
// artifact one register over (EDX there, not ECX), resolved below the same way.
//
#include "ai/ai_site_scan.h"


namespace mh::ai {
namespace detail {

void bldg_scan_grid_candidates(const ai_view &v, const ai_store &own, const ai_calls &gc,
                               uint32_t player, int32_t building_idx) {
    // Both preserved across every call below (grid_match_stencil and footprint_scan_for_blocked_cell
    // are __cdecl and touch only EAX) -- computed once, exactly as the original computes them once
    // per outer-loop pass (LAB_004e65b3 recomputes ESI from EDI/building_idx every inner iteration in
    // the assembly, but building_idx is loop-invariant, so the value never changes; nothing here
    // caches a value that the original re-derives from something that COULD change).
    uint8_t *grid           = const_cast<uint8_t *>(&v.players[player].ai_tile_flags_grid[0]);
    uint8_t *footprint_mask = const_cast<uint8_t *>(&v.cfg_buildings[building_idx].area[0][0]);
    uint8_t *passable       = const_cast<uint8_t *>(v.passable);

    // x outer / width, y inner / height -- BOTH re-read from memory on every iteration (0x004e6686/
    // 0x004e6693 compare directly against the globals, never a hoisted local); a `for` condition
    // dereferencing *v.map_width / *v.map_height every pass reproduces that.
    for (int32_t x = 0; x < *v.map_width; ++x) {
        for (int32_t y = 0; y < *v.map_height; ++y) {
            const int32_t fits =
                gc.grid_match_stencil(grid, *v.map_width, *v.map_height, footprint_mask,
                                      FOOTPRINT_SPAN, FOOTPRINT_SPAN, x, y, /*target_byte=*/2);
            if (fits == 0) continue;

            // Inverted polarity: nonzero means the footprint FITS (empty).
            const int32_t empty = gc.footprint_scan_for_blocked_cell(
                passable, *v.map_width, *v.map_height, footprint_mask, FOOTPRINT_SPAN, FOOTPRINT_SPAN,
                x, y);
            if (empty == 0) continue;

            // 0x004e662f/0x004e6634: count re-read from memory and shifted into a byte offset for
            // the tile_x/tile_y/kind stores.
            const int32_t idx0               = *own.site_candidate_count;
            own.site_candidates[idx0].tile_x = x;
            own.site_candidates[idx0].tile_y = y;
            own.site_candidates[idx0].kind   = SITE_KIND_GRID_FIT;

            const uint32_t dist_sq =
                gc.toroidal_dist_sq(v.players[player].ai_home_tile_x, v.players[player].ai_home_tile_y,
                                    x, y);

            // 0x004e6665: count re-read from memory a SECOND time for the dist_sq store -- nothing
            // between the two reads can change it (toroidal_dist_sq is pure), so this is
            // observationally the same index as idx0, but transcribed as its own read to match the
            // assembly instruction-for-instruction.
            const int32_t idx1                = *own.site_candidate_count;
            own.site_candidates[idx1].dist_sq = dist_sq;
            ++*own.site_candidate_count;

            // NOT A CAPACITY CHECK -- see ai_state.h / ai_site_scan.h. `==`, after the increment,
            // breaking only the INNER loop; the outer x loop resumes and the next inner pass appends
            // at 0x1001, where this can never match again.
            if (*own.site_candidate_count == SITE_CANDIDATE_BREAK_AT) break;
        }
    }
}

void scan_build_site_candidates(const ai_view &v, const ai_store &own, const ai_calls &gc,
                                int32_t player_idx, int32_t building_idx) {
    uint8_t *footprint_mask = const_cast<uint8_t *>(&v.cfg_buildings[building_idx].area[0][0]);
    uint8_t *grid           = const_cast<uint8_t *>(&v.players[player_idx].ai_tile_flags_grid[0]);
    uint8_t *passable       = const_cast<uint8_t *>(v.passable);

    for (int32_t x = 0; x < *v.map_width; ++x) {
        for (int32_t y = 0; y < *v.map_height; ++y) {
            // Probe 1: raw (x, y).
            if (gc.grid_match_stencil(grid, *v.map_width, *v.map_height, footprint_mask, FOOTPRINT_SPAN,
                                      FOOTPRINT_SPAN, x, y, /*target_byte=*/2) == 0)
                continue;

            // 0x004e6722-0x004e6728: y+1 computed ONCE, UNMASKED, into what the assembly spills to a
            // stack slot ([EBP-0x14]) and re-masks at each of the three uses below. `y` itself (ECX in
            // the .asm / `y` here) is never touched by this -- the footprint test at the end still
            // uses the raw, unmasked (x, y).
            const int32_t y_plus_1_raw = y + 1;

            // Probe 2: (x, (y+1) & height_m).
            const int32_t y2 = (int32_t)(((uint32_t)y_plus_1_raw) & *v.map_height_mask);
            if (gc.grid_match_stencil(grid, *v.map_width, *v.map_height, footprint_mask, FOOTPRINT_SPAN,
                                      FOOTPRINT_SPAN, x, y2, /*target_byte=*/2) == 0)
                continue;

            // Probe 3: ((x+1) & width_m, (y+1) & height_m).
            const int32_t y3 = (int32_t)(((uint32_t)y_plus_1_raw) & *v.map_height_mask);
            const int32_t x3 = (int32_t)(((uint32_t)(x + 1)) & *v.map_width_mask);
            if (gc.grid_match_stencil(grid, *v.map_width, *v.map_height, footprint_mask, FOOTPRINT_SPAN,
                                      FOOTPRINT_SPAN, x3, y3, /*target_byte=*/2) == 0)
                continue;

            // Probe 4: ((x-1) & width_m, (y+1) & height_m).
            const int32_t y4 = (int32_t)(((uint32_t)y_plus_1_raw) & *v.map_height_mask);
            const int32_t x4 = (int32_t)(((uint32_t)(x - 1)) & *v.map_width_mask);
            if (gc.grid_match_stencil(grid, *v.map_width, *v.map_height, footprint_mask, FOOTPRINT_SPAN,
                                      FOOTPRINT_SPAN, x4, y4, /*target_byte=*/2) == 0)
                continue;

            // Footprint test: RAW, unmasked (x, y) -- same as the grid scanner, and inverted polarity
            // (nonzero = fits).
            if (gc.footprint_scan_for_blocked_cell(passable, *v.map_width, *v.map_height, footprint_mask,
                                                   FOOTPRINT_SPAN, FOOTPRINT_SPAN, x, y) == 0)
                continue;

            const int32_t idx0               = *own.site_candidate_count;
            own.site_candidates[idx0].tile_x = x;
            own.site_candidates[idx0].tile_y = y;
            own.site_candidates[idx0].kind   = SITE_KIND_GRID_FIT;

            const uint32_t dist_sq = gc.toroidal_dist_sq(v.players[player_idx].ai_home_tile_x,
                                                         v.players[player_idx].ai_home_tile_y, x, y);

            const int32_t idx1                = *own.site_candidate_count;
            own.site_candidates[idx1].dist_sq = dist_sq;
            ++*own.site_candidate_count;

            // NOT A CAPACITY CHECK -- see the grid scanner above and ai_state.h.
            if (*own.site_candidate_count == SITE_CANDIDATE_BREAK_AT) break;
        }
    }
}

} // namespace detail

void bldg_scan_grid_candidates(uint32_t player, int32_t building_idx) {
    const ai_state st = state();
    detail::bldg_scan_grid_candidates(st.read, st.own, live_calls(), player, building_idx);
}

void scan_build_site_candidates(int32_t player_idx, int32_t building_idx) {
    const ai_state st = state();
    detail::scan_build_site_candidates(st.read, st.own, live_calls(), player_idx, building_idx);
}


} // namespace mh::ai
