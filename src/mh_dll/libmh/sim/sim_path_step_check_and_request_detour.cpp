#include "sim/sim_path_step_check_and_request_detour.h"

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"        // ai_say / trace_budget -- the shared trace sink, not AI state
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const path_step_check_and_request_detour_calls &live_path_step_check_and_request_detour_calls() {
    static const path_step_check_and_request_detour_calls c = {
        MH_LIBMH_BIND(llm_strat_facing24_to_delta),
        MH_LIBMH_BIND(llm_strat_unit_path_detour),
    };
    return c;
}

namespace detail {

int32_t path_step_check_and_request_detour(const sim_view                                 &v,
                                           const path_step_check_and_request_detour_calls &c,
                                           uint32_t src_x, uint32_t src_y, int32_t dst_x, int32_t dst_y) {
    // 0x0049a25e-0x0049a28f: the SRC tile's occupancy record. `.building` is read here as the
    // occupying UNIT's index and `.class_owner`'s low nibble as that unit's owning player -- the SAME
    // per-class field reuse sim_combat_kill_credit.cpp's ring-damage class_hi==0x80 arm documents
    // ("Reads the SAME `.building` field, not `.unit`"): mh_map_tile_object_data.building holds a
    // building id OR a unit index depending on what class_owner's high nibble says is actually there.
    // This unconditional read (no class-marker gate, unlike the DST read below) is how the function
    // learns WHICH unit is taking this step -- it is given only coordinates, not a unit id; the caller
    // is trusted to pass the coordinates of an actual unit.
    const tile_object &src_tile     = tile_at(v, (int32_t)src_x, (int32_t)src_y);
    const uint32_t     src_unit_idx = src_tile.building;
    const uint32_t     src_player   = (uint32_t)(src_tile.class_owner & 0xfu);

    // 0x0049a29b-0x0049a2be: fast clear-tile path. `v.passable`/`tile_at()` share the same
    // (tile_x<<8)|tile_y indexing (sim_state.h's own note on passable_at()/tile_at()). Return "no
    // detour needed" ONLY when dst is BOTH passable AND has no occupant recorded -- any other
    // combination (impassable, or occupied) falls into the detailed occupant analysis below, whether
    // the obstruction turns out to be a building, terrain, or a unit.
    const uint32_t dst_flat_index = ((uint32_t)dst_x << 8) | (uint32_t)dst_y;
    if (v.passable[dst_flat_index] != 0 && tile_at(v, dst_x, dst_y).building == 0) {
        return 0;
    }

    // 0x0049a2ca-0x0049a2df: only a UNIT-class occupant (class_owner high nibble 0x80, same idiom as
    // sim_combat_kill_credit.cpp's `class_hi == 0x80u` arm and sim_bldg_link_to_network.cpp's
    // `(player|0x40)` building-class check for the same byte) is something this function can try to
    // route around. Anything else -- a building, or dst not itself unit-marked despite failing the
    // clear-tile test above -- is reported blocked outright, with NO detour call.
    const tile_object &dst_tile = tile_at(v, dst_x, dst_y);
    if ((dst_tile.class_owner & 0x80u) == 0) {
        return 1;
    }

    // 0x0049a2e5-0x0049a316: the occupying unit's identity, same {.building=index,
    // .class_owner&0xf=player} reuse as the SRC read above.
    const uint32_t dst_player   = (uint32_t)(dst_tile.class_owner & 0xfu);
    const uint32_t dst_unit_idx = dst_tile.building;

    const unit     &occ       = unit_of(v, dst_player, (int32_t)dst_unit_idx);
    const cfg_unit &occ_proto = v.cfg_units[occ.unit_proto_id];

    // 0x0049a319-0x0049a385: two "occupant isn't really an obstacle" states, checked before the
    // move-step prediction below. Zero-extended 32-bit equality, matching sim_order_dispatch.cpp's
    // established idiom for the identical state-vs-move_op_* comparison ("both a MOVZX'd 16-bit state
    // and a MOVZX'd 8-bit cfg byte"). No llm_strat_unit_state enum backs this domain (see the header's
    // DECLARED NEED) -- move_op_arg is the occupant's OWN class's resting/arg state (0xa ground / 0xb
    // air, per mh_cfg_final_struct_Unit::move_op_arg's field comment); 0xc is an as-yet-unnamed
    // literal state with no cfg or plate backing at this call site.
    if ((uint32_t)occ.state == (uint32_t)occ_proto.move_op_arg) return 0; // 0x0049a354-0x0049a35f
    if ((uint32_t)occ.state == 0xcu) return 0;                            // 0x0049a374-0x0049a385

    // 0x0049a38a-0x0049a3c7: is the occupant actively stepping in ITS OWN class's move state? If not,
    // skip straight to requesting a detour (falls through to the call below).
    if ((uint32_t)occ.state == (uint32_t)occ_proto.move_op_code) {
        // 0x0049a3cd-0x0049a41a: predict the occupant's NEXT tile from its CURRENT path waypoint's
        // heading. `path_buffers[player][slot][entry]`, the SIM1-G1 established indexing (see
        // sim_unit_state_move_walker.cpp's identical `v.path_buffers[player * PATH_WAYPOINTS_PER_PLAYER
        // + slot * PATH_WAYPOINTS_PER_SLOT + entry]` form) -- player=dst_player (the OCCUPANT's own
        // player, not src_player), slot=occ.path_slot_id, entry=occ.path_cursor (both read fresh from
        // the occupant record, matching the asm's own two MOVZX/MOV reads at 0x0049a3e3/0x0049a409).
        const path_waypoint &wp =
            v.path_buffers[dst_player * PATH_WAYPOINTS_PER_PLAYER +
                           (uint32_t)occ.path_slot_id * PATH_WAYPOINTS_PER_SLOT + (uint32_t)occ.path_cursor];

        int32_t dx = 0, dy = 0;
        c.facing24_to_delta(wp.heading, &dx, &dy); // 0x0049a413-0x0049a41a

        // 0x0049a41f-0x0049a43e: step (dst_x,dst_y) by (dx,dy), wrapped by the map's TILE-space masks
        // (map_width_mask()/map_height_mask(), general.width_mask/height_mask -- NOT the pixel-space
        // bw_mask/bh_mask pair sim_unit_predict_coords.cpp's header warns is a different field of the
        // same record).
        const uint32_t wrapped_x = map_width_mask(v) & (uint32_t)(dst_x + dx);
        const uint32_t wrapped_y = map_height_mask(v) & (uint32_t)(dst_y + dy);

        // 0x0049a441-0x0049a458: if the occupant's predicted next tile is NOT our own src tile, assume
        // it is vacating dst on its own -- no detour needed.
        if (wrapped_x != src_x || wrapped_y != src_y) {
            return 0;
        }
        // else: falls through to the detour request below (occupant would step onto OUR src tile).
    }

    // 0x0049a45a-0x0049a472: request a detour for the ACTING unit (identified from the SRC tile in
    // step 0 above), offering the dst tile's occupant as the bounded-recursion alt candidate.
    //
    // 0x0049a45a-0x0049a461 (`MOVZX EAX,word ptr [PlayerSide]; CMP EAX,[EBP-0x2c]`) computes a
    // PlayerSide-vs-src_player comparison whose flags are never consulted: EAX/EDX/EBX are all
    // reloaded with the call's own arguments before any conditional jump, so no branch anywhere reads
    // this CMP's result. Confirmed dead -- see uncertainties[]; not reproduced here since it has zero
    // effect on any output, tracked state, or the call that follows.
    c.unit_path_detour((int32_t)src_player, (int32_t)src_unit_idx, (int32_t)dst_unit_idx); // 0x0049a46d
    return 1;                                                                              // 0x0049a472
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

int32_t path_step_check_and_request_detour(uint32_t src_x, uint32_t src_y, int32_t dst_x, int32_t dst_y) {
    const sim_view v = state().read;
    return detail::path_step_check_and_request_detour(
        v, live_path_step_check_and_request_detour_calls(), src_x, src_y, dst_x, dst_y);
}


} // namespace mh::sim
