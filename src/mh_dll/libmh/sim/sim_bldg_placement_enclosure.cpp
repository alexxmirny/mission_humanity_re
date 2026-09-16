//
// sim/sim_bldg_placement_enclosure.cpp -- see sim_bldg_placement_enclosure.h. Translated from the
// DISASSEMBLY (tmp/decomp/llm_strat_bldg_connectivity_flood_fill_004d77cc.asm and
// tmp/decomp/llm_strat_bldg_check_placement_encloses_neighbors_004d8c7d.asm), not from Ghidra's C
// drafts -- the second draft carries its own "WARNING: Type propagation algorithm not settling"
// banner, and every branch/offset/mask below was re-derived from the listing rather than trusted
// from either draft.
//
#include "sim/sim_bldg_placement_enclosure.h"

#include "ai/ai_state.h" // ai_say / trace_budget -- the shared trace sink, not AI state

namespace mh::sim {

namespace detail {

void connectivity_flood_fill(const sim_view &v, uint32_t player, int32_t exclude_bldg_idx,
                             uint8_t *flag_array) {
    // ---- seed pass: 0x004d77ec-0x004d787f -------------------------------------------------------
    // buildings[player][0].index (offset +0x0, NOT +0x2 building_id) is the live occupied-slot
    // count -- MOVZX word, zero-extended.
    int32_t remaining = (int32_t)(uint16_t)building_of(v, player, 0).index;
    for (int32_t slot = 1; remaining != 0; ++slot) {
        const building &b = building_of(v, player, slot);
        if (b.building_id == 0) continue; // empty slot: no write, no decrement (0x004d7845)

        const uint8_t type = v.cfg_buildings[b.building_id].type;
        flag_array[slot]   = (type == BUILDING_TYPE_A_MOTHER || type == BUILDING_TYPE_H_MOTHER)
                                 ? 1
                                 : 0; // 0x004d7854-0x004d786c
        --remaining;
    }

    // ---- flood pass: 0x004d787f-0x004d7a2d, a do-while(changed) fixpoint --------------------------
    bool changed;
    do {
        changed = false;

        // Live occupied-slot count re-read fresh for this outer iteration (0x004d78a2).
        remaining = (int32_t)(uint16_t)building_of(v, player, 0).index;
        for (int32_t slot = 1; remaining != 0; ++slot) {
            const building &b = building_of(v, player, slot);
            if (b.building_id == 0) continue; // empty slot: skip entirely, no decrement (0x004d78e0)

            // Excluded slot, or not yet flagged: counts toward the total but contributes no window
            // scan (0x004d78e6-0x004d78f7).
            if (slot == exclude_bldg_idx || flag_array[slot] == 0) {
                --remaining;
                continue;
            }

            const cfg_building &cb = v.cfg_buildings[b.building_id];

            // The building's own 31x31 zone: dy outer (height/2 + b.y + dy, masked by height_m),
            // dx inner (width/2 + b.x + dx, masked by width_m) -- 0x004d790a-0x004d7a00.
            for (int32_t dy = -15; dy <= 15; ++dy) {
                const int32_t ty = (int32_t)(((uint32_t)((int32_t)b.y + cb.height / 2 + dy)) & *v.height_m);
                for (int32_t dx = -15; dx <= 15; ++dx) {
                    const int32_t tx =
                        (int32_t)(((uint32_t)((int32_t)b.x + cb.width / 2 + dx)) & *v.width_m);

                    const tile_object &t = tile_at(v, tx, ty);
                    // 0x004d79d1: class_owner must be THIS player's building tag.
                    if (t.class_owner != (uint8_t)(player | ORDER_KIND_BUILDING)) continue;
                    // 0x004d79e2: only newly-discovered slots (flag_array entry still 0) propagate.
                    if (flag_array[t.building] != 0) continue;
                    flag_array[t.building] = 1;    // 0x004d79e7
                    changed                = true; // 0x004d79ea
                }
            }
            --remaining;
        }
    } while (changed);
}

int32_t check_placement_encloses_neighbors(const sim_view &v, sim_store &own, int32_t player,
                                           int32_t x, int32_t y, uint32_t width, uint32_t height) {
    // ---- pass 1 (0x004d8c98-0x004d8ce1): seed the WHOLE map extent from `passable`. -------------
    // Loop bounds are the REAL map extents (`*v.map_width`/`*v.map_height`), not this function's
    // own `width`/`height` parameters (those are the proposed footprint's dimensions, used below).
    for (int32_t tx = 0; tx < *v.map_width; ++tx) {
        for (int32_t ty = 0; ty < *v.map_height; ++ty) {
            own.bldg_enclosure_scratch_at(tx, ty) =
                (v.passable[((uint32_t)tx << 8) | (uint32_t)ty] == 0) ? 0x80 : 0x00;
        }
    }

    // ---- pass 2 (0x004d8ce1-0x004d8d18): OR bit 0x1 over the PROPOSED w x h footprint at (x,y). --
    for (uint32_t fx = 0; fx < width; ++fx) {
        for (uint32_t fy = 0; fy < height; ++fy) {
            const int32_t tx = (int32_t)(((uint32_t)(x + (int32_t)fx)) & *v.width_m);
            const int32_t ty = (int32_t)(((uint32_t)(y + (int32_t)fy)) & *v.height_m);
            own.bldg_enclosure_scratch_at(tx, ty) |= 0x1;
        }
    }

    // ---- pass 3 (0x004d8d18-0x004d8e9b): OR bit 0x40 over every existing building's own 31x31 --
    // zone. Same "buildings[player][0].index is the live occupied count" scan as the sibling
    // function above, no exclude parameter here.
    int32_t remaining = (int32_t)(uint16_t)building_of(v, (uint32_t)player, 0).index;
    for (int32_t slot = 1; remaining != 0; ++slot) {
        const building &b = building_of(v, (uint32_t)player, slot);
        if (b.building_id == 0) continue; // empty slot: skip, no decrement (0x004d8d81)

        const cfg_building &cb = v.cfg_buildings[b.building_id];
        for (int32_t dx = -15; dx <= 15; ++dx) {
            const int32_t bx = (int32_t)(((uint32_t)((int32_t)b.x + cb.width / 2 + dx)) & *v.width_m);
            for (int32_t dy = -15; dy <= 15; ++dy) {
                const int32_t by =
                    (int32_t)(((uint32_t)((int32_t)b.y + cb.height / 2 + dy)) & *v.height_m);

                uint8_t &cell = own.bldg_enclosure_scratch_at(bx, by);
                cell |= 0x40;                        // 0x004d8e58
                if ((cell & 0x41) == 0x41) return 1; // 0x004d8e67-0x004d8e6d, EARLY RETURN
            }
        }
        --remaining;
    }

    // ---- pass 4 (0x004d8e9b-0x004d8fb6): a SEPARATE fixpoint flood over the whole map extent. ---
    for (;;) {
        bool changed = false;

        for (int32_t tx = 0; tx < *v.map_width; ++tx) {
            for (int32_t ty = 0; ty < *v.map_height; ++ty) {
                const uint8_t cell = own.bldg_enclosure_scratch_at(tx, ty);
                // Candidate: bit 0x40 SET and bit 0x80 CLEAR (0x004d8eb7-0x004d8ecb).
                if ((cell & 0x40) == 0 || (cell & 0x80) != 0) continue;

                const int32_t right         = (int32_t)(((uint32_t)(tx + 1)) & *v.width_m);
                const int32_t left          = (int32_t)(((uint32_t)(tx - 1)) & *v.width_m);
                const int32_t down          = (int32_t)(((uint32_t)(ty + 1)) & *v.height_m);
                const int32_t up            = (int32_t)(((uint32_t)(ty - 1)) & *v.height_m);
                const uint8_t all_neighbors = (uint8_t)(own.bldg_enclosure_scratch_at(right, ty) &
                                                        own.bldg_enclosure_scratch_at(left, ty) &
                                                        own.bldg_enclosure_scratch_at(tx, down) &
                                                        own.bldg_enclosure_scratch_at(tx, up));
                // All four cardinal neighbours already have 0x40: nothing to flood from here
                // (0x004d8f1f-0x004d8f23).
                if ((all_neighbors & 0x40) != 0) continue;

                // This tile's OWN 31x31 zone, centred on the tile itself (NOT via a building
                // record, unlike pass 3) -- 0x004d8f25-0x004d8f84.
                for (int32_t dx = -15; dx <= 15; ++dx) {
                    const int32_t bx = (int32_t)(((uint32_t)(tx + dx)) & *v.width_m);
                    for (int32_t dy = -15; dy <= 15; ++dy) {
                        const int32_t by = (int32_t)(((uint32_t)(ty + dy)) & *v.height_m);

                        uint8_t &c = own.bldg_enclosure_scratch_at(bx, by);
                        if ((c & 0x40) == 0) changed = true; // 0x004d8f4d-0x004d8f57
                        c |= 0x40;                           // 0x004d8f61
                        if ((c & 0x41) == 0x41) return 1;    // 0x004d8f72-0x004d8f74, EARLY RETURN
                    }
                }
            }
        }

        if (!changed) return 0; // 0x004d8fac-0x004d8fb4
    }
}

} // namespace detail

// ---- the public surface --------------------------------------------------------------------------

void connectivity_flood_fill(uint32_t player, int32_t exclude_bldg_idx, uint8_t *flag_array) {
    const sim_view v = state().read;
    detail::connectivity_flood_fill(v, player, exclude_bldg_idx, flag_array);
}

int32_t check_placement_encloses_neighbors(int32_t player, int32_t x, int32_t y, uint32_t width,
                                           uint32_t height) {
    sim_state st = state();
    return detail::check_placement_encloses_neighbors(st.read, st.own, player, x, y, width, height);
}


} // namespace mh::sim
