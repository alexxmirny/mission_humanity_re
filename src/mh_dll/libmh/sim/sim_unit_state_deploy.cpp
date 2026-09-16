//
// sim/sim_unit_state_deploy.cpp -- see sim_unit_state_deploy.h. Translated from the DISASSEMBLY
// (tmp/decomp_sim/llm_strat_unit_state_deploy_approach_0048117c.asm,
// tmp/decomp_sim/llm_strat_unit_state_deploy_to_building_00481a6b.asm) -- see the header banner for
// the two out-pointer-wiring CORRECTIONs and the construction_complete register-leak derivation.
//
#include "sim/sim_unit_state_deploy.h"

#include "addr/mh_calls.gen.h" // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"       // ai_say / trace_budget -- the shared trace sink, not AI state

#include <cstring>
#include "state/host_api.h"
#include "state/host_events.h"
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const unit_state_deploy_approach_calls &live_unit_state_deploy_approach_calls() {
    static const unit_state_deploy_approach_calls c = {
        MH_LIBMH_BIND(llm_strat_tile_neighbor_in_dir),
        MH_LIBMH_BIND(llm_bldg_calc_placement_corner_from_center),
        MH_LIBMH_BIND(llm_bldg_footprint_is_clear),
        MH_LIBMH_BIND(llm_map_bldg_footprint_clear_passable),
        MH_LIBMH_BIND(llm_strat_unit_unlink_tile),
        MH_LIBMH_BIND(llm_strat_fow_remove_sight),
        MH_LIBMH_BIND(map_unit_PutOnMap),
        MH_LIBMH_BIND(map_fow_UpdateFoWPlus),
        MH_LIBMH_BIND(llm_strat_ctrl_group_contains_unit),
        MH_LIBMH_BIND(llm_strat_unit_ctrlgroup_remove_member),
        MH_LIBMH_BIND(game_SetEvent),
        MH_LIBMH_BIND(llm_strat_unit_set_state),
        MH_LIBMH_BIND(llm_strat_unit_set_state_order),
    };
    return c;
}

const unit_state_deploy_to_building_calls &live_unit_state_deploy_to_building_calls() {
    static const unit_state_deploy_to_building_calls c = {
        MH_LIBMH_BIND(llm_bldg_calc_placement_corner_from_center),
        MH_LIBMH_BIND(llm_bldg_construct_finalize),
        MH_LIBMH_BIND(llm_strat_bldg_update_charge_pips),
        MH_LIBMH_BIND(llm_strat_mother_reelect_primary),
        MH_LIBMH_BIND(llm_strat_bldg_construction_complete),
        mh::state::evt::snd_play_at,
        MH_LIBMH_BIND(llm_strat_fx_anim_spawn),
        MH_LIBMH_BIND(llm_strat_unit_unlink_tile),
        MH_LIBMH_BIND(llm_strat_fow_remove_sight),
        MH_LIBMH_BIND(llm_strat_unit_teardown),
    };
    return c;
}

namespace {

// llm_strat_unit_state value with no backing Ghidra enum (rule 17a); same value as
// sim_unit_apply_damage.h's own UNIT_STATE_DEPLOY_TO_BUILDING. File-local (NOT at namespace mh::sim
// scope, unlike this project's usual per-TU-constant convention) because mh_nettest's selftest TU
// includes multiple sim/*.h headers together, and two identically-named `inline constexpr` symbols at
// the same namespace scope collide (C2374/C2086) the moment both headers land in one TU.
constexpr uint16_t UNIT_STATE_DEPLOY_TO_BUILDING = 0x17;

// mh_cfg_final_struct_Building::anim is `cfg_t_frame_index[12]` flattened to `uint8_t[48]` -- same
// flattening sim_bldg_state_destroyed.cpp's frame_at()/set_frame_at() already document and work
// around; reused here in the same shape, file-local per this project's "write only your own new
// files" convention (not shared cross-TU).
int32_t frame_at(const uint8_t (&anim)[48], int32_t slot) {
    int32_t value;
    std::memcpy(&value, &anim[slot * 4], sizeof(value));
    return value;
}

// Same truncating-toward-zero `/32` idiom sim_bldg_state_destroyed.cpp's own fine_to_tile() documents
// (that file's comment records the SAR/SBB-vs-C-division verification); re-derived locally per this
// project's per-TU convention rather than shared cross-TU.
inline int32_t fine_to_tile(int32_t fine) { return fine / 32; }

} // namespace

namespace detail {

void unit_state_deploy_approach(const sim_view &v, sim_store &own,
                                const unit_state_deploy_approach_calls &c) {
    unit          &u          = own.cur_unit(); // _G_LLM_STRAT_CUR_UNIT dereferenced.
    const uint16_t player     = *v.cur_player;
    const uint16_t unit_index = *v.cur_index;

    // 0x0048119b-0x004811bf: the tile one step ahead of the unit in its current move_heading.
    int32_t neighbor_col = 0, neighbor_row = 0;
    c.tile_neighbor_in_dir(u.x, u.y, u.move_heading, &neighbor_col, &neighbor_row);

    // 0x004811c4-0x004811da: the building-footprint placement corner AROUND THAT NEIGHBOR TILE (see
    // the header CORRECTION -- this is a DIFFERENT (col,row) pair from the one above, not a rename of
    // it).
    // corner_col/corner_row stay int32_t (they feed footprint_is_clear/footprint_clear_passable's
    // int32_t params below); calc_placement_corner_from_center's committed out-params are uint32_t *
    // (TACT1-P C6, 2026-09-04) -- same 32-bit quantity, cast at this minority site.
    int32_t corner_col = 0, corner_row = 0;
    c.calc_placement_corner_from_center(u.unit_proto_id, neighbor_col, neighbor_row,
                                        reinterpret_cast<uint32_t *>(&corner_col),
                                        reinterpret_cast<uint32_t *>(&corner_row));

    const int32_t equivalent = v.cfg_units[u.unit_proto_id].equivalent;

    // 0x004811df-0x00481208: is the footprint at that corner clear (viewer = the unit's own player)?
    const int32_t clear =
        c.footprint_is_clear(corner_col, corner_row, equivalent, static_cast<uint32_t>(player));

    if (clear == 0) {
        // 0x004813c1-0x004813cb: blocked -- scatter and retry later. (A dead `CMP cur_player,
        // PlayerSide` follows in the assembly at 0x004813d0-0x004813d6 with no consuming branch --
        // not reproduced, see the header NOTE.)
        c.unit_set_state_order(UNIT_STATE_IDLE_SCATTER, UNIT_STATE_IDLE_SCATTER);
    } else {
        // 0x0048120e-0x00481276: clear the footprint's passability, unlink the unit's current tile,
        // and remove fog sight at the unit's OLD position (before it moves).
        c.footprint_clear_passable(corner_col, corner_row, equivalent);
        c.unit_unlink_tile(player, unit_index);
        c.fow_remove_sight(player, u.x, u.y, v.cfg_units[u.unit_proto_id].sight);

        // 0x00481281-0x00481293: move onto the NEIGHBOR tile (tile_neighbor_in_dir's raw output, NOT
        // the footprint corner -- see the header CORRECTION).
        u.x = static_cast<uint8_t>(neighbor_col);
        u.y = static_cast<uint8_t>(neighbor_row);

        c.map_unit_put_on_map(player, unit_index, u.x, u.y);
        c.map_fow_update_fow_plus(player, u.x, u.y, v.cfg_units[u.unit_proto_id].sight);

        // 0x00481304-0x00481385: local-player-only UI cleanup -- either drop the unit from ctrl group
        // 0, or (for some OTHER player's unit) clear a stale click-select target that pointed at it.
        if (player == static_cast<uint16_t>(*v.player_side)) {
            // 0x0048130d-0x0048133d.
            if (c.ctrl_group_contains_unit(unit_index, v.ctrl_groups[0].count, 0) != 0) {
                c.unit_ctrlgroup_remove_member(unit_index, &own.ctrl_group_at(0).count, 0);
                c.game_set_event(EVENT_INFO_REFRESH);
            }
        } else if (*v.click_select_target_flags == static_cast<uint16_t>(player | 0x80u) &&
                   own.click_select_target_id() == unit_index) {
            // 0x0048134b-0x00481380.
            own.click_select_target_id() = 0;
            c.game_set_event(EVENT_INFO_REFRESH);
        }

        // 0x0048138a-0x004813ba: remap move_heading via the dir-remap table (heading
        // smoothing/snapping, not a facing write -- see the batch context's dir_remap_table note),
        // reset the sub-tile microstep counter, and enter DEPLOY_TO_BUILDING.
        u.move_heading   = static_cast<uint8_t>(v.dir_remap_table[u.move_heading].step_primary);
        u.move_microstep = 0;
        c.unit_set_state(UNIT_STATE_DEPLOY_TO_BUILDING);
    }

    // 0x004813dd-0x004813e2: every path ends here.
    c.game_set_event(EVENT_INFO_REFRESH);
}

void unit_state_deploy_to_building(const sim_view &v, sim_store &own,
                                   const unit_state_deploy_to_building_calls &c) {
    unit          &u          = own.cur_unit();
    const uint16_t player     = *v.cur_player;
    const uint16_t unit_index = *v.cur_index;

    // 0x00481a91-0x00481ac4: step_cost = cfg_units[proto].step_speed[player] * 2.0 -- a compile-time
    // immediate (constructed on the stack from two MOVs in the asm, not a `DAT_` memory read), so
    // written as the literal `2.0` directly.
    const double step_cost = v.cfg_units[u.unit_proto_id].step_speed[player] * 2.0;

    if (own.tick_budget() < step_cost) {
        // 0x00481ac6-0x00481aeb: insufficient budget -- standard carryover idiom, straight to return.
        u.activity_clock -= own.tick_budget();
        own.tick_budget() = 0.0;
        return;
    }
    own.tick_budget() -= step_cost; // 0x00481af0-0x00481af9

    // 0x00481aff-0x00481b25: descend one elevation step; if not low enough yet, nothing else happens
    // this tick (a THIRD gate the batch hazard note omitted -- see the header banner).
    u.elevation -= 1;
    if (u.elevation > v.cfg_units[u.unit_proto_id].elevation_2) {
        return;
    }

    // 0x00481b2b-0x00481b53: the building-footprint placement corner AROUND THE UNIT'S OWN CURRENT
    // POSITION (unlike deploy_approach, no neighbor-tile probe precedes this).
    // corner_col/corner_row stay int32_t (used in the masked_x/masked_y arithmetic and passed to
    // mother_reelect_primary's int32_t params below); calc_placement_corner_from_center's committed
    // out-params are uint32_t * (TACT1-P C6, 2026-09-04) -- same 32-bit quantity, cast at this site.
    int32_t corner_col = 0, corner_row = 0;
    c.calc_placement_corner_from_center(u.unit_proto_id, u.x, u.y,
                                        reinterpret_cast<uint32_t *>(&corner_col),
                                        reinterpret_cast<uint32_t *>(&corner_row));

    const int32_t equivalent = v.cfg_units[u.unit_proto_id].equivalent; // read once, reused below --
                                                                        // see the header banner's
                                                                        // "read once" note.

    // 0x00481b58-0x00481b83: llm_bldg_construct_finalize(param_1=0, y_b=corner_row, player,
    // param_4=2, x_b=corner_col, building_id=equivalent) -- register/stack layout matches the
    // callee's own committed prototype exactly (see header banner).
    const uint32_t building_id = static_cast<uint32_t>(
        c.construct_finalize(0u, corner_row, player, static_cast<char>(2),
                             static_cast<uint32_t>(corner_col), static_cast<uint32_t>(equivalent)));

    if (building_id != 0) {
        // ---- arm 1 (0x00481b95-0x00481e0b): SUCCESS -- the roster slot was allocated. ----
        building &b = own.building_at(player, static_cast<int32_t>(building_id));

        // 0x00481b95-0x00481be8.
        b.energy = (u.energy / v.cfg_units[u.unit_proto_id].energy) * v.cfg_buildings[equivalent].energy;
        c.bldg_update_charge_pips(player, building_id); // 0x00481bee-0x00481bfd

        // 0x00481bfd-0x00481c2b: bind the shuttle slot (0 = unbound, propagated from the unit).
        b.shuttle_slot     = u.shuttle_slot;
        const uint8_t slot = b.shuttle_slot;
        if (slot != 0) {
            // 0x00481c34-0x00481ca6.
            prod_shuttle_slot &s = own.prod_shuttle_slot_at(player, slot);
            s.status             = 0xca;
            s.src_building_index = static_cast<int16_t>(building_id);
            s.type_ref_id        = static_cast<uint16_t>(b.building_id);
        }

        // 0x00481cad-0x00481db0: heli-mothership primary-election bookkeeping. Read via the ROSTER
        // ARRAY expression (units[player][cur_index]), not the cur_unit pointer -- see the header
        // banner's "asymmetric read" note (provably the same value here, reproduced literally anyway).
        const uint32_t proto_type_for_mother =
            v.cfg_units[unit_of(v, player, unit_index).unit_proto_id].type;
        if (proto_type_for_mother == UNIT_TYPE_A_HELI_MOTHER ||
            proto_type_for_mother == UNIT_TYPE_H_HELI_MOTHER) {
            player_profile &prof = own.profile_at(player);
            if (prof.primary_mother_bldg[*v.planet_index] == 0) {
                prof.primary_mother_bldg[*v.planet_index] = static_cast<int32_t>(building_id);
            }
            if (static_cast<int32_t>(unit_index) == prof.primary_mother_unit[*v.planet_index]) {
                prof.primary_mother_unit[*v.planet_index] = 0;
                c.mother_reelect_primary(player, corner_col, corner_row); // reuses the SAME corner
                                                                          // pair construct_finalize
                                                                          // already consumed.
            }
        }

        // 0x00481db0-0x00481dfc: population bookkeeping.
        own.population_at(player).human += v.cfg_units[u.unit_proto_id].human;
        own.population_at(player).human_in_field -= v.cfg_units[u.unit_proto_id].human;

        // 0x00481dfc-0x00481e06: param_3/param_4 here are register-liveness leaks in the ORIGINAL (not
        // explicit arguments), but the callee is PROVEN to never read either (see the header banner's
        // RESOLVED note + the plate comment on llm_strat_bldg_construction_complete) -- so any value
        // is behaviorally correct; this passes a deterministic value for param_3 and 0 for param_4.
        const uint32_t param3_deterministic = player * (slot != 0 ? 0x1f18u : 0x6aa4u);
        c.bldg_construction_complete(player, building_id, param3_deterministic, /*param_4=*/0u);
    } else {
        // ---- arm 2 (0x00481e10-0x00481f0d): FAILURE -- no free roster slot. ----
        const cfg_building &cb = v.cfg_buildings[equivalent];

        // 0x00481e10-0x00481e79: a FINE/pixel-space position, masked against `general`'s bw_mask/
        // bh_mask (NOT the tile-space width_mask/height_mask pair).
        const int32_t masked_x =
            static_cast<int32_t>(v.geom->bw_mask) & (static_cast<int32_t>(cb.width) * 16 + corner_col * 32);
        const int32_t masked_y =
            static_cast<int32_t>(v.geom->bh_mask) & (static_cast<int32_t>(cb.height) * 16 + corner_row * 32);

        if (*v.sim_active != 0) {
            // 0x00481e7c-0x00481ed1: the original offscreen_snd_volume+snd_play pair, fused into
            // ONE position-carrying record (the LIFT-NOTIFY offscreen conversion): the hosted sink re-runs that
            // exact pair synchronously at emit.
            c.snd_play_at(cb.sound_explo, fine_to_tile(masked_x), fine_to_tile(masked_y));
        }

        // 0x00481ed6-0x00481f0d: param_4 is the RAW game_clock value here (NOT
        // `game_clock - tick_budget` -- see the header banner's note that this call site does NOT
        // share the "elapsed" derivation sim_bldg_state_destroyed.cpp's own fx_anim_spawn calls use).
        c.fx_anim_spawn(static_cast<uint32_t>(masked_x), static_cast<uint32_t>(masked_y),
                        static_cast<uint32_t>(frame_at(cb.anim, 7)), *v.game_clock, /*param_5=*/1u);
    }

    // ---- shared tail (0x00481f12-0x00481f72): reached from EITHER arm. ----
    c.unit_unlink_tile(player, unit_index);
    c.fow_remove_sight(player, u.x, u.y, v.cfg_units[u.unit_proto_id].sight); // position UNCHANGED --
                                                                              // this function never
                                                                              // writes unit.x/y.
    c.unit_teardown(player, unit_index);
}

} // namespace detail

// ---- the public wrappers --------------------------------------------------------------------------

void unit_state_deploy_approach() {
    sim_state st = state();
    detail::unit_state_deploy_approach(st.read, st.own, live_unit_state_deploy_approach_calls());
}

void unit_state_deploy_to_building() {
    sim_state st = state();
    detail::unit_state_deploy_to_building(st.read, st.own, live_unit_state_deploy_to_building_calls());
}


} // namespace mh::sim
