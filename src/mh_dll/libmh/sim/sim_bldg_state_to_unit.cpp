//
// sim/sim_bldg_state_to_unit.cpp -- see sim_bldg_state_to_unit.h. Translated from the DISASSEMBLY
// (tmp/decomp_sim/llm_strat_bldg_state_to_unit_00471443.asm), not from the Ghidra .c draft (not
// consulted for this file -- see the header banner).
//
#include "sim/sim_bldg_state_to_unit.h"

#include <cstring>
#include <limits>

#include "addr/mh_calls.gen.h" // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"       // ai_say / trace_budget / REF_BLDG_BIT -- shared with the AI domain
#include "state/host_api.h"
#include "state/host_events.h"
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const bldg_state_to_unit_calls &live_bldg_state_to_unit_calls() {
    static const bldg_state_to_unit_calls c = {
        MH_LIBMH_BIND(llm_bldg_calc_placement_corner_from_center_by_type),
        MH_LIBMH_BIND(llm_strat_population_remove),
        MH_LIBMH_BIND(llm_strat_unit_create),
        MH_LIBMH_BIND(llm_strat_mother_reelect_primary),
        MH_LIBMH_BIND(llm_strat_ai_notify_object_removed),
        MH_LIBMH_BIND(llm_strat_bldg_unmap_footprint),
        MH_LIBMH_BIND(llm_strat_unit_purge_unregistered),
        MH_LIBMH_BIND(llm_strat_player_presence_lost),
        mh::state::evt::snd_play_at,
        MH_LIBMH_BIND(llm_strat_fx_anim_spawn),
    };
    return c;
}

namespace {

// mh_map_object_building::anim is `cfg_t_frame_index[12]` flattened to uint8_t[48]
// (addr/mh_structs.gen.h) -- same flattening sim_bldg_state_destroyed.cpp's frame_at()/
// set_frame_at() document and work around; re-derived file-locally per this project's per-TU
// convention (not shared cross-TU). Only the setter is needed here.
void set_frame_at(uint8_t (&anim)[48], int32_t slot, int32_t value) {
    std::memcpy(&anim[slot * 4], &value, sizeof(value));
}

// The tile<-fine conversion (`SAR EDX,0x1f / SHL EDX,0x5 / SBB EAX,EDX / SAR EAX,0x5`), the SAME
// truncating-`/32` idiom sim_order_enqueue.cpp's fine_to_tile() / sim_bldg_state_destroyed.cpp's own
// copy verify; re-derived locally per this project's per-TU convention.
inline int32_t fine_to_tile(int32_t fine) { return fine / 32; }

// RESOLVED (conductor, 2026-08-22): read-memory confirmed DAT_005012dc = -1.0 (hex 00 00 00 00 00 00
// F0 BF). Named+typed in Ghidra as _G_LLM_STRAT_BLDG_TO_UNIT_HEADER_ROW_ENERGY_DELTA (EN v307).
inline constexpr double BLDG_TO_UNIT_HEADER_ROW_ENERGY_DELTA =
    -1.0; // _G_LLM_STRAT_BLDG_TO_UNIT_HEADER_ROW_ENERGY_DELTA, was DAT_005012dc

} // namespace

namespace detail {

void bldg_state_to_unit(const sim_view &v, sim_store &own, const bldg_state_to_unit_calls &c) {
    building           &b   = own.cur_building(); // _G_LLM_STRAT_CUR_BUILDING dereferenced, read+write
    const uint16_t      bid = b.building_id;      // read ONCE, reused (cfg_buildings has no writer in this closure)
    const cfg_building &cb  = v.cfg_buildings[bid];

    // ---- 0x00471460-0x00471477: no configured equivalent unit type -> stay a building -------------
    // cfg_buildings[bid].equivalent (0x82e) -- see the header's DECLARED NEED on its DTM tag; the
    // disassembly's own usage below (multiplied by sizeof(cfg_unit) and used to index cfg_units[])
    // proves this is a unit-type id despite being tagged `[cfg_t_define_index]`.
    const int32_t equivalent = cb.equivalent;
    if (equivalent == 0) {
        b.state = BLDG_STATE_NO_EQUIVALENT_UNIT; // 1
        return;
    }

    // ---- 0x00471477-0x004714a4: placement corner, TILE-space, from the building's own center ------
    // [EBP-0x20]=corner_x, [EBP-0x1c]=corner_y (verified against llm_strat_unit_create's own EAX=x/
    // EDX=y register mapping downstream, and against llm_bldg_calc_placement_corner_from_center_by_
    // type's committed EAX=type/EDX=center_x/EBX=center_y/ECX=out_x/stack=out_y prototype).
    //
    // MUTABLE ON PURPOSE: see the header's leading uncertainty -- the FAILURE path below overwrites
    // these SAME two locals in place with FINE (pixel) coordinates, mirroring the asm's own stack-
    // slot reuse; a naive pair of differently-named locals would hide that reuse.
    // uint32_t: matches calc_placement_corner_from_center_by_type's committed uint32_t *out_x/out_y
    // (TACT1-P C6, 2026-09-04). The later mother_reelect_primary(int32_t, int32_t, int32_t) call below
    // takes these by value, so the width still round-trips through that implicit conversion.
    uint32_t corner_x = 0, corner_y = 0;
    c.calc_placement_corner_from_center_by_type(bid, static_cast<int32_t>(b.x), static_cast<int32_t>(b.y),
                                                &corner_x, &corner_y);

    // ---- 0x004714ad-0x004714ca: crew departs into the new unit BEFORE it exists --------------------
    // count = cfg_units[equivalent].human -- matches sim_unit_population_remove.h's own documented
    // plate ("crew boards a deployed unit with count = Unit.human").
    c.population_remove(static_cast<uint32_t>(*v.cur_player), v.cfg_units[equivalent].human);

    // ---- 0x004714cf-0x004714f8: create the mobile unit ---------------------------------------------
    const uint32_t new_index =
        c.unit_create(corner_x, corner_y,
                      static_cast<uint16_t>(equivalent), *v.cur_player, UNIT_CREATE_IS_SHIP_SENTINEL);

    if (new_index != 0) {
        // ============================================================================================
        // SUCCESS (0x0047150a-0x004717ce): stamp the new unit from the vacating building's own fields.
        // ============================================================================================
        unit &nu = own.unit_at(*v.cur_player, static_cast<int32_t>(new_index));

        nu.shuttle_slot = b.shuttle_slot;

        // door_approach_route[10] -- see the header UNCERTAINTY: index 10 sits in the field's
        // documented-unconfirmed tail (only index 0 == "facing" is established elsewhere).
        nu.move_heading = cb.door_approach_route[10];

        // Both facing fields are seeded from the SAME source byte (MOVE_MICROSTEPS[heading][step]
        // .facing, read via `move_heading` just stamped above and whatever `move_microstep` unit_
        // create() itself left) -- confirmed by the identical `+2` displacement (into `.facing`,
        // NOT `.x_off`) at both read sites; this is a deliberate "start already facing this way"
        // seed, not a redundant/buggy re-read.
        const move_microstep &ms =
            v.move_microsteps[static_cast<int32_t>(nu.move_heading) * MICROSTEPS_PER_HEADING +
                              nu.move_microstep];
        nu.facing_target  = ms.facing;
        nu.facing_current = ms.facing;
        nu.move_microstep = MOVE_MICROSTEP_FULL; // 0x1f, AFTER the facing lookup above uses the OLD value

        // ---- 0x00471649-0x00471694: elevation, re-derived from an AMBIENT roster slot -------------
        // See the header's UNCERTAINTY: this reads `units[cur_player][cur_index]` -- the SAME
        // ambient index this function uses for the BUILDING roster everywhere else -- NOT the new
        // unit's own slot (`new_index`, used by every other write in this block). Reproduced
        // literally via unit_of(), not "corrected" to `nu`.
        //
        // FIXED 2026-09-07 (SPCAMP A/B/C): the source is `elevation_2` (cfg offset 0x19f), NOT
        // `elevation` (0x19b). The read at 0x00471688 is `MOV EAX,[EAX + 0xe4a237]` with
        // EAX = stale_proto*0x23f; cfg_units' base is 0xe4a098 (cross-checked from this binary's own
        // soldier_count read at 0xe4a2bf against mh_structs.gen.h's `soldier_count == 0x227`), so the
        // offset is 0xe4a237 - 0xe4a098 = 0x19f. mh_structs.gen.h static_asserts elevation at 0x19b
        // and elevation_2 at 0x19f.
        //
        // TWO ADJACENT, IDENTICALLY-TYPED, UNCOMMENTED int32 FIELDS is the whole trap, and the tree
        // had already been here: sim_unit_state_takeoff.h carries a CORRECTION banner establishing
        // ceiling = elevation (0x19b) / floor = elevation_2 (0x19f) "confirmed by two independent
        // landmarks", and sim_map_unit_add.cpp's own stamp-a-new-unit-from-cfg line is
        // `u.elevation = proto.elevation_2;`. This function is the one place that had it backwards --
        // in the .cpp, in the header banner's offset table, AND in the selftest, all three written
        // from the same wrong premise, so they agreed with each other and not with the binary.
        const uint16_t stale_proto = unit_of(v, *v.cur_player, *v.cur_index).unit_proto_id;
        nu.elevation               = v.cfg_units[stale_proto].elevation_2;

        nu.state = UNIT_STATE_JUST_DEPLOYED; // 0x7c

        // ---- 0x004716b3-0x00471734: order, keyed by whether the bound shuttle slot's destination --
        // is THIS planet. Reads `nu.shuttle_slot` (the new unit's just-stamped copy) here; the tail
        // block below re-reads `b.shuttle_slot` (the building's own field) instead -- two literally
        // different expressions in the asm, always equal in practice, preserved as such (see header).
        // WIDTH: dest_planet is compared ZERO-extended (MOVZX), not sign-extended -- see header.
        const prod_shuttle_slot &bound_slot =
            v.prod_shuttle_slots[*v.cur_player * PROD_SHUTTLE_SLOTS_PER_PLAYER + nu.shuttle_slot];
        const bool at_dest_planet = static_cast<uint16_t>(bound_slot.dest_planet) ==
                                    static_cast<uint16_t>(*v.planet_index);
        nu.order = at_dest_planet ? UNIT_STATE_IDLE_SCATTER : UNIT_ORDER_AWAY_FROM_DEST;

        // ---- 0x00471734-0x004717ce: repoint the production/shuttle slot at the NEW UNIT -----------
        // Same dual-use idiom mh_structs.gen.h's `type_ref_id` field comment documents for
        // llm_prod_bldg_depart_finalize's own heli-mothership-departure repoint (a DIFFERENT
        // function, same mechanic) -- see the header OBSERVATION.
        prod_shuttle_slot &slot = own.prod_shuttle_slot_at(*v.cur_player, b.shuttle_slot);
        slot.type_ref_id        = nu.unit_proto_id;
        slot.status             = SHUTTLE_SLOT_STATUS_UNIT_DEPLOYED; // 0xcb
        slot.src_building_index = static_cast<int16_t>(new_index);

    } else {
        // ============================================================================================
        // FAILURE (0x004717d3-0x004718bb): no free roster slot -- the departing unit "dies" instead.
        // ============================================================================================
        // Convert the TILE corner to FINE (pixel) space, wrapped by the map's PIXEL-space masks
        // (general.bw_mask/bh_mask -- NOT width_mask/height_mask, which are the TILE-space pair a
        // different pair of readers use; see sim_state.h's own map_geom field list). REASSIGNS
        // corner_x/corner_y IN PLACE, mirroring the asm's own stack-slot reuse -- see header.
        corner_x = static_cast<int32_t>((static_cast<uint32_t>(corner_x << 5) + 0x10u) & v.geom->bw_mask);
        corner_y = static_cast<int32_t>((static_cast<uint32_t>(corner_y << 5) + 0x10u) & v.geom->bh_mask);

        if (*v.sim_active != 0) {
            // The original offscreen_snd_volume(0x004717f0)+snd_play(0x0047180d) pair, fused into
            // ONE position-carrying record (the LIFT-NOTIFY offscreen conversion): the hosted sink re-runs that
            // exact pair synchronously at emit.
            c.snd_play_at(v.cfg_units[equivalent].sound_explo, fine_to_tile(corner_x),
                          fine_to_tile(corner_y));
        }

        c.fx_anim_spawn(corner_x, corner_y,
                        static_cast<uint32_t>(v.cfg_units[equivalent].anim_explo), *v.game_clock,
                        /*flag=*/1u);

        // Abandon the transfer: clear the shuttle slot's type_ref_id back to "free".
        own.prod_shuttle_slot_at(*v.cur_player, b.shuttle_slot).type_ref_id = 0;
    }

    // ================================================================================================
    // SHARED TAIL (0x004718bb-0x00471aa7): reached by BOTH the success and failure arms above. `new_
    // index` is 0 here on the failure arm (from unit_create's own return), which the mother-unit
    // write below gates on being nonzero anyway (see the comment there).
    // ================================================================================================

    // ---- 0x004718bb-0x004719be: mother-building bookkeeping, roster type re-derivation ------------
    // Re-derives building_id from the ROSTER (buildings[cur_player][cur_index].building_id), NOT via
    // the cur_building pointer -- same asymmetry sim_bldg_state_destroyed.h documents for its own
    // two mothership-type checks.
    const uint16_t roster_bid = building_of(v, *v.cur_player, *v.cur_index).building_id;
    const uint8_t  type       = v.cfg_buildings[roster_bid].type;

    if (type == BUILDING_TYPE_H_MOTHER || type == BUILDING_TYPE_A_MOTHER) {
        // mh_structs.gen.h's own field comments confirm this mechanic independently of the asm:
        // "primary_mother_bldg: tracked primary mother BUILDING index (deployed form)...
        // primary_mother_unit: tracked primary mother UNIT index (mobile heli-mother form); exact
        // dual of primary_mother_bldg - one of the pair is nonzero while the mother lives".
        if (own.profile_at(*v.cur_player).primary_mother_unit[*v.planet_index] == 0) {
            // On the failure arm new_index==0, so this writes 0 into an already-0 slot -- a no-op,
            // matching the asm exactly (both arms share this one expression using whatever value
            // `new_index` holds).
            own.profile_at(*v.cur_player).primary_mother_unit[*v.planet_index] =
                static_cast<int32_t>(new_index);
        }
        if (own.profile_at(*v.cur_player).primary_mother_bldg[*v.planet_index] ==
            static_cast<int32_t>(*v.cur_index)) {
            own.profile_at(*v.cur_player).primary_mother_bldg[*v.planet_index] = 0;
            // See the header's LEADING uncertainty: (corner_x, corner_y) are TILE coords here if the
            // success arm ran, FINE (pixel) coords if the failure arm ran -- reproduced faithfully,
            // not normalized to one coordinate space.
            c.mother_reelect_primary(*v.cur_player, corner_x, corner_y);
        }
    }

    // ---- 0x004719be-0x004719ca: zero the vacating building's own energy (double, two dword stores)
    b.energy = 0.0;

    // ---- 0x004719d1-0x004719ea: per-player roster HEADER ROW scratch bump (slot 0, NOT this building)
    own.building_at(*v.cur_player, 0).energy += BLDG_TO_UNIT_HEADER_ROW_ENERGY_DELTA;

    // ---- 0x004719f0-0x00471a04: AI removal notification, building kind -----------------------------
    c.ai_notify_object_removed(static_cast<uint32_t>(*v.cur_player) | mh::ai::REF_BLDG_BIT,
                               static_cast<uint32_t>(*v.cur_index), /*hard_remove=*/0);

    // NOTE 0x00471a09-0x00471a16: `CMP AX,[PlayerSide]` here is DEAD -- no branch or store reads the
    // flags it sets (the next three instructions are MOVZX/MOVZX/CALL). Omitted: zero observable
    // effect in C++ semantics. See the header OBSERVATION.

    c.bldg_unmap_footprint(*v.cur_player, static_cast<int32_t>(*v.cur_index));

    // ---- 0x00471a29-0x00471a80: roster alive-count + purge/presence-lost, SAME shape as ------------
    // sim_bldg_state_destroyed.cpp's own tail.
    own.profile_at(*v.cur_player).buildings_alive[*v.planet_index] -= 1;
    if (own.profile_at(*v.cur_player).buildings_alive[*v.planet_index] == 0) {
        c.unit_purge_unregistered(*v.cur_player);
        c.player_presence_lost(*v.cur_player, 0);
    }

    // ---- 0x00471a80-0x00471a9a: the building itself goes to RUBBLE_SIGHT_DECAY, anim[0] cleared ----
    b.state = BLDG_STATE_RUBBLE_SIGHT_DECAY; // 4, already-named sim_state.h constant
    set_frame_at(b.anim, 0, 0);
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

void bldg_state_to_unit() {
    sim_state st = state();
    detail::bldg_state_to_unit(st.read, st.own, live_bldg_state_to_unit_calls());
}


} // namespace mh::sim
