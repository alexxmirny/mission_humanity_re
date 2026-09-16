//
// ai/ai_resource_sites.cpp -- see ai_resource_sites.h. Translated from the DISASSEMBLY
// (tmp/decomp_a5/llm_strat_ai_bldg_scan_resource_site_candidates_004e6396.asm), NOT from Ghidra's C:
// the decompile's `extraout_EDX` at the cfg_buildings width/height read is the same __cdecl-clobber
// artifact ai_state.h documents for the two window-test callees -- EDX still holds
// `building_type * 0x842` from the earlier IMUL, and the decompile's
// `*(byte*)((int)(Building[0].area + -1) + extraout_EDX + 9/8)` is that live value read back through
// a pointer expression Ghidra invented. It resolves to the plain field reads used below
// (cfg_buildings[building_type].height / .width), confirmed against the raw offsets +9/+0xa off the
// 0xd9ec80 Building[] base at 0x004e649c/0x004e64a4.
//
#include "ai/ai_resource_sites.h"


namespace mh::ai {
namespace detail {

void bldg_scan_resource_site_candidates(const ai_view &v, const ai_store &own, const ai_calls &gc,
                                        uint32_t player, int32_t building_type) {
    const cfg_building &cb = v.cfg_buildings[building_type];

    // Both window-test callees and the footprint scan want a non-const grid/table pointer even
    // though this function never writes through it itself -- same reasoning as
    // ai_turret_threat.cpp's own_threat_grid and ai_construction_sites.cpp's footprint/passable
    // casts. `footprint_mask` is cfg_buildings[building_type].area, tested as a flat 10x10 span --
    // &cb.area[0][0] is &cfg_buildings[type] + 0xb, matching the .asm's `ADD EBX,0xb` off the
    // 0xd9ec80 Building[] row base.
    uint8_t *own_grid  = &own.players[player].ai_tile_flags_grid[0];
    uint8_t *footprint = const_cast<uint8_t *>(&cb.area[0][0]);
    uint8_t *passable  = const_cast<uint8_t *>(v.passable);

    // UNSIGNED compare against the count, and the count is RE-READ every iteration in the original
    // (the .asm recomputes the player_data row address at every loop-test, never hoisting the
    // count into a register across the loop) -- matched here by reading it fresh in the loop
    // condition rather than caching it in a local before the loop.
    for (uint32_t i = 0; i < (uint32_t)v.players[player].ai_resource_site_count; ++i) {
        // Writable handle: a hit stamps build_tile_x/_y, exhaustion stamps status. Neither write
        // happens on the "status already non-open" or "below threshold" paths.
        resource_site &site = own.players[player].ai_resource_sites[i];

        if (site.status != (int16_t)RESOURCE_SITE_STATUS_OPEN) continue;

        // COARSE quarter-tile -> fine tile: fine = grid * 4 + 2. The original zero-extends the
        // stored 16-bit field (MOVZX, not MOVSX) before the shift/add -- reproduced with the
        // (uint16_t) cast rather than a plain sign-extending widen of the int16_t field.
        const int32_t fine_x =
            (int32_t)(uint16_t)site.grid_x * RESOURCE_SITE_COORD_SCALE + RESOURCE_SITE_COORD_BIAS;
        const int32_t fine_y =
            (int32_t)(uint16_t)site.grid_y * RESOURCE_SITE_COORD_SCALE + RESOURCE_SITE_COORD_BIAS;

        // 0 skips this deposit entirely -- no status write on this path, unlike the "disc
        // exhausted" path below.
        if (!gc.resource_site_meets_threshold((int32_t)player, building_type, fine_x, fine_y))
            continue;

        // Radius-5 spiral disc around (fine_x, fine_y). Offsets are SIGNED bytes (the .asm uses
        // MOVSX, not MOVZX -- unlike the coarse-coordinate reads above).
        const uint32_t ring_count = v.spiral_ring_cell_counts[SITE_SCAN_SPIRAL_RADIUS];
        bool           placed     = false;
        for (uint32_t j = 0; j < ring_count; ++j) {
            const int32_t x = (int32_t)(((uint32_t)(fine_x + v.spiral_offsets[j].dx)) &
                                        *v.map_width_mask);
            const int32_t y = (int32_t)(((uint32_t)(fine_y + v.spiral_offsets[j].dy)) &
                                        *v.map_height_mask);

            // Three gates, fixed order, all short-circuit. NOTE the stencil variant: the
            // 0x40/0x1f "all near unthreatened" test, NOT the target-byte match the other two
            // scanners use.
            if (!gc.grid_stencil_all_near_unthreatened(own_grid, *v.map_width, *v.map_height,
                                                       footprint, FOOTPRINT_SPAN, FOOTPRINT_SPAN, x,
                                                       y))
                continue;
            if (!gc.footprint_scan_for_blocked_cell(passable, *v.map_width, *v.map_height,
                                                    footprint, FOOTPRINT_SPAN, FOOTPRINT_SPAN, x, y))
                continue;
            // width = cfg_buildings[type].width (+0xa), height = cfg_buildings[type].height
            // (+0x9) -- height is the LOWER offset, matched here by the struct's own field names
            // rather than by re-deriving the byte order.
            if (!gc.bldg_check_placement_encloses_neighbors((int32_t)player, x, y, cb.width,
                                                            cb.height))
                continue;

            // Append the ONE candidate this call will ever produce and stop -- there is no 0x1000
            // break-at test anywhere in this function, unlike its two siblings.
            const int32_t n               = *own.site_candidate_count;
            own.site_candidates[n].tile_x = x;
            own.site_candidates[n].tile_y = y;
            own.site_candidates[n].kind   = SITE_KIND_RESOURCE;
            own.site_candidates[n].dist_sq =
                gc.toroidal_dist_sq(v.players[player].ai_home_tile_x,
                                    v.players[player].ai_home_tile_y, x, y);
            ++*own.site_candidate_count;

            site.build_tile_x = (int16_t)x;
            site.build_tile_y = (int16_t)y;

            placed = true;
            break;
        }

        if (placed) return; // JMP straight to the epilogue -- at most one candidate per call.

        // Disc exhausted with no hit: invalidate this deposit and move on to the next one. This is
        // the ONLY path in the whole function that writes `status` to non-open.
        site.status = (int16_t)RESOURCE_SITE_STATUS_INVALID;
    }
}

} // namespace detail

void bldg_scan_resource_site_candidates(uint32_t player, int32_t building_type) {
    const ai_state st = state();
    detail::bldg_scan_resource_site_candidates(st.read, st.own, live_calls(), player, building_type);
}


} // namespace mh::ai
