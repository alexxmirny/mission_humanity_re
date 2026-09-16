//
// sim/sim_storage_get_approach_tile.cpp -- see sim_storage_get_approach_tile.h. Translated from the
// DISASSEMBLY (tmp/decomp/llm_strat_storage_get_approach_tile_0048b37c.asm), not from the Ghidra .c
// draft.
//
#include "sim/sim_storage_get_approach_tile.h"


namespace mh::sim {

namespace {

// The `SAR EDX,0x1f / SHL EDX,0x5 / SBB EAX,EDX / SAR EAX,0x5` idiom (0x0048b458-0x0048b46c and its
// three siblings below) is the SAME instruction sequence -- same shift-by-5, same divisor 32 -- that
// sim_order_enqueue.cpp's fine_to_tile(), sim_bldg_state_destroyed.cpp/.h's own re-derivation,
// sim_prod_shuttle_complete.h's re-derivation, and sim_unit_state_predicates.cpp's re-derivation have
// each independently verified equals plain C truncating `/ 32` (IDIV-equivalent, not floor -- they
// differ on negatives, but this shift form and `/ 32` do NOT differ, which is exactly what those four
// derivations each confirmed against the same raw bytes this function also uses). Re-derived locally
// per this project's per-TU convention rather than shared across files.
inline int32_t fine_to_tile(int32_t fine) { return fine / 32; }

} // namespace

namespace detail {

void storage_get_approach_tile(const sim_view &v, uint16_t player, uint16_t unit_index,
                               uint32_t *out_fine_x, uint32_t *out_fine_y, uint32_t storage_idx) {
    // 0x0048b39d-0x0048b3c8: resolved_slot = (storage_idx != 0) ? storage_idx
    //                                                            : units[player][unit_index].home_storage_slot
    const uint32_t resolved_slot =
        (storage_idx != 0) ? storage_idx
                           : static_cast<uint32_t>(unit_of(v, player, unit_index).home_storage_slot);

    // 0x0048b3cb-0x0048b3f5: unit_proto_id read once (cached, matching the original's single stack
    // local); Unit[unit_proto_id].move_op_code == 0x11 (heli mover class) selects the elevator-offset
    // branch below.
    const uint16_t unit_proto_id = unit_of(v, player, unit_index).unit_proto_id;

    if (v.cfg_units[unit_proto_id].move_op_code == 0x11) {
        // 0x0048b3fb-0x0048b42b: building_type_id = buildings[player][unit_storage[player][resolved_
        // slot].b_index].building_id -- computed ONCE, held in a stack local, and referenced by BOTH
        // the X- and Y- blocks below (see header banner: this part of the original genuinely IS
        // cached, unlike the elevation diff).
        const int32_t  b_index = storage_of(v, static_cast<uint32_t>(player), static_cast<int32_t>(resolved_slot)).b_index;
        const uint16_t building_type_id =
            building_of(v, static_cast<uint32_t>(player), b_index).building_id;

        // DECLARED NEED (see header banner): `facing` does not exist on cfg_building yet -- it is
        // absorbed into `_pad_0x78e[160]`. Written as if it already exists; this line is why the file
        // will not compile until the conductor splits the field out.
        const uint8_t          facing = v.cfg_buildings[building_type_id].door_approach_route[0];
        const dir_step_offset &off    = v.dir_step_offsets[facing];

        // ---- X block (0x0048b3fb-0x0048b4d5): independently re-derives the elevation diff --------
        {
            const int32_t elevation   = v.cfg_units[unit_proto_id].elevation;                // 0x0048b458/0x0048b45e
            const int32_t elevation_2 = v.cfg_units[unit_proto_id].elevation_2;              // 0x0048b47b/0x0048b481
            const int32_t diff        = fine_to_tile(elevation) - fine_to_tile(elevation_2); // 0x0048b464-0x0048b492

            const int32_t exit_x = storage_of(v, static_cast<uint32_t>(player), static_cast<int32_t>(resolved_slot)).exit_tile_x; // 0x0048b4c2
            *out_fine_x          = static_cast<uint32_t>(exit_x - diff * off.dx) & map_width_mask(v);                             // 0x0048b4c8-0x0048b4d5
        }

        // ---- Y block (0x0048b4d7-0x0048b55e): independently re-derives the SAME diff -------------
        // Deliberately NOT sharing the X block's `diff`/`elevation` locals -- see header banner: the
        // original re-reads Unit[unit_proto_id].elevation/elevation_2 from memory again here rather
        // than reusing the X block's registers, so this transcription re-reads too.
        {
            const int32_t elevation   = v.cfg_units[unit_proto_id].elevation;                // 0x0048b4e1/0x0048b4e7
            const int32_t elevation_2 = v.cfg_units[unit_proto_id].elevation_2;              // 0x0048b504/0x0048b50a
            const int32_t diff        = fine_to_tile(elevation) - fine_to_tile(elevation_2); // 0x0048b4ed-0x0048b51b

            const int32_t exit_y = storage_of(v, static_cast<uint32_t>(player), static_cast<int32_t>(resolved_slot)).exit_tile_y; // 0x0048b54b
            *out_fine_y          = static_cast<uint32_t>(exit_y - diff * off.dy) & map_height_mask(v);                            // 0x0048b551-0x0048b55e
        }
    } else {
        // 0x0048b562-0x0048b59c: not a heli-vs-elevator approach -- the storage slot's own exit tile,
        // with NO wrap-mask AND applied (unlike the branch above).
        const unit_storage &st =
            storage_of(v, static_cast<uint32_t>(player), static_cast<int32_t>(resolved_slot));
        *out_fine_x = static_cast<uint32_t>(st.exit_tile_x);
        *out_fine_y = static_cast<uint32_t>(st.exit_tile_y);
    }
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

void storage_get_approach_tile(uint16_t player, uint16_t unit_index, uint32_t *out_fine_x,
                               uint32_t *out_fine_y, uint32_t storage_idx) {
    const sim_view v = state().read;
    detail::storage_get_approach_tile(v, player, unit_index, out_fine_x, out_fine_y, storage_idx);
}


} // namespace mh::sim


namespace mh::sim {
} // namespace mh::sim
