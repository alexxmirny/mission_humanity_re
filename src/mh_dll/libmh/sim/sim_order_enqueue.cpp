//
// sim/sim_order_enqueue.cpp -- see sim_order_enqueue.h. Translated from the DISASSEMBLY
// (tmp/decomp/llm_strat_*_enqueue_*.asm), not from Ghidra's C draft -- the draft mis-shapes the
// tile-coordinate bias-correction as a multi-line bit-twiddle where the disassembly is a plain
// truncating divide by 32 (SAR/SBB idiom; confirmed value-for-value against C's `/` on a signed int).
//
#include "sim/sim_order_enqueue.h"

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const calls &live_calls() {
    static const calls gc = {
        MH_LIBMH_BIND(llm_strat_order_scratch_reset),
        MH_LIBMH_BIND(llm_strat_order_scratch_set_field),
        MH_LIBMH_BIND(llm_strat_order_enqueue),
        MH_LIBMH_BIND(llm_strat_unit_get_coords),
        MH_LIBMH_BIND(llm_strat_bldg_get_coords),
        MH_LIBMH_BIND(llm_strat_unit_select_weapon),
        MH_LIBMH_BIND(llm_strat_target_class),
        MH_LIBMH_BIND(llm_strat_unit_in_weapon_range),
        MH_LIBMH_BIND(llm_unit_state_is_boarding),
        MH_LIBMH_BIND(llm_strat_bldg_footprint_random_offset),
        MH_LIBMH_BIND(llm_strat_storage_type_accepts_unit),
        MH_LIBMH_BIND(llm_strat_storage_get_approach_tile),
        MH_LIBMH_BIND(llm_strat_unit_notify_status),
        MH_LIBMH_BIND(llm_strat_unit_order_move_enqueue),
        MH_LIBMH_BIND(llm_strat_order_collect_available_projects_enqueue),
        MH_LIBMH_BIND(llm_strat_unit_order_move_auto),
        MH_LIBMH_BIND(llm_strat_order_dispatch),
    };
    return gc;
}

namespace {
// The tile<-fine conversion every attack/reposition handler performs: `SAR EDX,0x1f / SHL EDX,0x5 /
// SBB EAX,EDX / SAR EAX,0x5` on the fine coordinate. Value-for-value this is C's truncating `/ 32`
// on a signed int (verified against the asm's bias-correction for the negative case: x=-1 -> the asm
// gives 0, which is truncation, not floor(-1/32) = -1) -- so the plain operator reproduces it exactly
// and is what every call site below uses instead of re-deriving the shift.
inline int32_t fine_to_tile(int32_t fine) { return fine / 32; }
} // namespace

namespace detail {

int32_t order_recruit_unit_enqueue(const calls &gc, uint32_t unit_id, uint32_t player_id) {
    gc.order_scratch_reset();
    gc.order_scratch_set_field(1, (int32_t)unit_id);
    gc.order_enqueue(0, (uint16_t)player_id, 0xf3, 0xf3);
    return 1; // the original never tests its own enqueue's result
}

int32_t order_queue_construction_enqueue(const calls &gc, int32_t arg4, int32_t arg5, int32_t arg0,
                                         uint16_t player) {
    gc.order_scratch_reset();
    gc.order_scratch_set_field(4, arg4);
    gc.order_scratch_set_field(5, arg5);
    gc.order_scratch_set_field(0, arg0);
    gc.order_enqueue(0, player, 0xf2, 0xf2);
    return 1;
}

void bldg_order_production_add_enqueue(const sim_view &v, const calls &gc, uint16_t player,
                                       int32_t bldg_idx, int32_t unit_type) {
    // 0x0046db8a: online_state != 0. NO scratch_reset -- see the header note; the 0x6d handler reads
    // only args[0]/[1].
    if (building_of(v, player, bldg_idx).online_state != 0) {
        gc.order_scratch_set_field(0, unit_type);
        gc.order_scratch_set_field(1, 1);
        gc.order_enqueue((uint16_t)bldg_idx, (uint16_t)((uint32_t)player | ORDER_KIND_BUILDING), 0x6d,
                         0x6d);
    }
}

void bldg_order_assign_workers_enqueue(const calls &gc, uint16_t player, uint16_t bldg_idx,
                                       int32_t worker_count) {
    gc.order_scratch_set_field(1, worker_count);
    gc.order_enqueue(bldg_idx, (uint16_t)((uint32_t)player | ORDER_KIND_BUILDING), 0x7e, 0x7e);
}

void bldg_order_unassign_workers_enqueue(const calls &gc, uint16_t player, uint16_t bldg_idx,
                                         int32_t worker_count) {
    gc.order_scratch_set_field(1, worker_count);
    gc.order_enqueue(bldg_idx, (uint16_t)((uint32_t)player | ORDER_KIND_BUILDING), 0x7f, 0x7f);
}

void unit_order_exit_storage_enqueue(const sim_view &v, sim_store &own, const calls &gc,
                                     uint16_t player, uint32_t unit_idx, int32_t storage_idx,
                                     uint32_t /*param_4 -- unused, see header*/,
                                     uint32_t /*param_5 -- overwritten before use, see header*/) {
    const unit_storage &st = storage_of(v, player, storage_idx);
    // uint32_t: storage_get_approach_tile's committed out-params are uint32_t * (TACT1-P C6,
    // 2026-09-04); st.exit_tile_{x,y} are plain int32_t fields, implicit-converted in on seed.
    uint32_t exit_fine_x = st.exit_tile_x;
    uint32_t exit_fine_y = st.exit_tile_y;

    const building &host = building_of(v, player, st.b_index);
    const unit     &u    = unit_of(v, player, (int32_t)unit_idx);

    if (host.online_state != 0 &&
        gc.storage_type_accepts_unit(host.building_id, u.unit_proto_id) != 0) {
        gc.order_scratch_reset();
        gc.order_scratch_set_field(2, storage_idx);

        if (v.cfg_units[u.unit_proto_id].move_op_arg == 10) {
            // The unit can teleport straight to the storage's own exit tile.
            gc.order_scratch_set_field(0, exit_fine_x);
            gc.order_scratch_set_field(1, exit_fine_y);
            gc.order_enqueue((uint16_t)unit_idx, (uint16_t)(player | ORDER_KIND_UNIT), 0x24, 0x38);
        } else {
            // Needs a real approach tile computed against the passable grid.
            gc.storage_get_approach_tile((uint16_t)player, (uint16_t)unit_idx, &exit_fine_x, &exit_fine_y,
                                         (uint32_t)storage_idx);
            gc.order_scratch_set_field(0, exit_fine_x);
            gc.order_scratch_set_field(1, exit_fine_y);
            gc.order_enqueue((uint16_t)unit_idx, (uint16_t)(player | ORDER_KIND_UNIT), 0x29, 0xb);
        }
        gc.unit_notify_status(player, (int32_t)unit_idx, 0);
        return;
    }

    // The storage no longer wants this unit -- fall back to a plain move order to the storage's exit
    // tile, stamped with this player's next order-sequence id (INC, skip 0).
    gc.unit_order_move_enqueue((uint16_t)player, (int32_t)unit_idx, (uint32_t)exit_fine_x,
                               (uint32_t)exit_fine_y, own.order_seq_id_by_player(player));
    uint8_t &seq = own.order_seq_id_by_player(player);
    ++seq;
    if (seq == 0) ++seq;
}

void unit_order_attack_target_enqueue(const sim_view &v, const calls &gc, uint16_t player,
                                      int32_t unit_idx, uint16_t target_player, int32_t target_idx,
                                      uint32_t weapon_id, uint16_t tag) {
    int32_t fine_x = 0, fine_y = 0;
    gc.unit_get_coords(target_player, target_idx, &fine_x, &fine_y);

    if (weapon_id == 0xffffffffu) {
        const uint32_t target_mask =
            (unit_of(v, target_player, target_idx).elevation == 0) ? 1u : 2u;
        weapon_id = gc.unit_select_weapon((uint16_t)player, unit_idx, target_mask);
    }

    gc.order_scratch_reset();
    const int32_t tile_x = fine_to_tile(fine_x);
    const int32_t tile_y = fine_to_tile(fine_y);
    gc.order_scratch_set_field(scratch_field::TARGET_TILE_X, tile_x);
    gc.order_scratch_set_field(scratch_field::TARGET_TILE_Y, tile_y);
    gc.order_scratch_set_field(scratch_field::TARGET_FINE_X, fine_x);
    gc.order_scratch_set_field(scratch_field::TARGET_FINE_Y, fine_y);
    gc.order_scratch_set_field(scratch_field::TARGET_OWNER,
                               (int32_t)((uint32_t)target_player | ORDER_KIND_UNIT));
    gc.order_scratch_set_field(scratch_field::TARGET_INDEX, target_idx);
    gc.order_scratch_set_field(scratch_field::TARGET_WEAPON, (int32_t)weapon_id);

    const int32_t target_class_flags =
        gc.target_class((uint32_t)target_player | ORDER_KIND_UNIT, target_idx);
    const uint32_t in_range =
        gc.unit_in_weapon_range(player, unit_idx, tile_x, tile_y, target_class_flags);

    const unit     &attacker = unit_of(v, player, unit_idx);
    const cfg_unit &proto    = v.cfg_units[attacker.unit_proto_id];

    // Three-way branch: cannot (or should not) close to melee range -> the mover's own move_op_arg
    // as the order code with `tag` as param0; a helicopter/cargo-heli variant -> a FIXED order (0x1a/
    // 0x2e) regardless of `tag` -- see the header note on why this is the one branch the target and
    // target_alt bodies do NOT differ on; anything else -> the direct-attack order (1/`tag`).
    if (in_range == 0 || gc.unit_state_is_boarding((int32_t)(uint32_t)attacker.state) != 0 ||
        proto.move_op_arg != 10 || proto.type == UNIT_TYPE_A_PLANE || proto.type == UNIT_TYPE_H_PLANE) {
        gc.order_enqueue((uint16_t)unit_idx, (uint16_t)((uint32_t)player | ORDER_KIND_UNIT),
                         (int16_t)tag, (uint16_t)proto.move_op_arg);
        gc.unit_notify_status(player, unit_idx, 0);
    } else if (proto.type == UNIT_TYPE_A_HELI || proto.type == UNIT_TYPE_H_HELI ||
               proto.type == UNIT_TYPE_A_HELI_CARGO || proto.type == UNIT_TYPE_H_HELI_CARGO) {
        gc.order_enqueue((uint16_t)unit_idx, (uint16_t)((uint32_t)player | ORDER_KIND_UNIT), 0x1a,
                         0x2e);
    } else {
        gc.order_enqueue((uint16_t)unit_idx, (uint16_t)((uint32_t)player | ORDER_KIND_UNIT), 1,
                         (uint16_t)tag);
    }
}

void unit_order_attack_unit_enqueue(const sim_view &v, const calls &gc, uint16_t player,
                                    int32_t unit_idx, uint16_t target_player,
                                    int32_t target_unit_idx, uint32_t weapon_id) {
    int32_t fine_x = 0, fine_y = 0;
    gc.unit_get_coords(target_player, target_unit_idx, &fine_x, &fine_y);

    if (weapon_id == 0xffffffffu) {
        const uint32_t target_mask =
            (unit_of(v, target_player, target_unit_idx).elevation == 0) ? 1u : 2u;
        weapon_id = gc.unit_select_weapon(player, unit_idx, target_mask);
    }

    gc.order_scratch_reset();
    gc.order_scratch_set_field(scratch_field::AU_FINE_X, fine_x);
    gc.order_scratch_set_field(scratch_field::AU_FINE_Y, fine_y);
    gc.order_scratch_set_field(scratch_field::AU_TARGET_OWNER,
                               (int32_t)((uint32_t)target_player | ORDER_KIND_UNIT));
    gc.order_scratch_set_field(scratch_field::AU_TARGET_INDEX, target_unit_idx);
    gc.order_scratch_set_field(scratch_field::AU_WEAPON, (int32_t)weapon_id);

    gc.order_enqueue((uint16_t)unit_idx, (uint16_t)((uint32_t)player | ORDER_KIND_UNIT), 0x1e, 0x1e);
}

void unit_order_attack_building_enqueue(const sim_view & /*v*/, const calls &gc, uint16_t player,
                                        int32_t unit_idx, uint16_t target_player,
                                        int32_t target_bldg_idx, uint32_t weapon_id) {
    int32_t fine_x = 0, fine_y = 0;
    gc.bldg_get_coords(target_player, target_bldg_idx, &fine_x, &fine_y);

    // Buildings have no elevation -- the auto-select mask is always 1 (ground), unlike the unit
    // target's elevation-gated 1/2.
    if (weapon_id == 0xffffffffu) weapon_id = gc.unit_select_weapon(player, unit_idx, 1u);

    gc.order_scratch_reset();
    // Jitters the aim point onto the building's footprint rather than one fixed corner. fine_x/fine_y
    // are int32_t (matching bldg_get_coords' committed out-params above); this row's committed
    // out-params are uint32_t * (TACT1-P C6, 2026-09-04) -- same 32-bit quantity, cast at this site.
    gc.bldg_footprint_random_offset(player, (uint32_t)unit_idx, target_player, target_bldg_idx,
                                    reinterpret_cast<uint32_t *>(&fine_x),
                                    reinterpret_cast<uint32_t *>(&fine_y));
    gc.order_scratch_set_field(scratch_field::AU_FINE_X, fine_x);
    gc.order_scratch_set_field(scratch_field::AU_FINE_Y, fine_y);
    gc.order_scratch_set_field(scratch_field::AU_TARGET_OWNER,
                               (int32_t)((uint32_t)target_player | ORDER_KIND_BUILDING));
    gc.order_scratch_set_field(scratch_field::AU_TARGET_INDEX, target_bldg_idx);
    gc.order_scratch_set_field(scratch_field::AU_WEAPON, (int32_t)weapon_id);

    gc.order_enqueue((uint16_t)unit_idx, (uint16_t)((uint32_t)player | ORDER_KIND_UNIT), 0x1d, 0x1d);
}

void unit_order_attack_building_reposition_alt_enqueue(const sim_view &v, const calls &gc,
                                                       uint16_t player, int32_t unit_idx,
                                                       uint16_t target_player,
                                                       int32_t  target_bldg_idx,
                                                       uint32_t weapon_id) {
    int32_t fine_x = 0, fine_y = 0;
    gc.bldg_get_coords(target_player, target_bldg_idx, &fine_x, &fine_y);

    if (weapon_id == 0xffffffffu) weapon_id = gc.unit_select_weapon(player, unit_idx, 1u);

    gc.order_scratch_reset();
    int32_t tile_x = fine_to_tile(fine_x);
    int32_t tile_y = fine_to_tile(fine_y);
    gc.order_scratch_set_field(scratch_field::TARGET_TILE_X, tile_x);
    gc.order_scratch_set_field(scratch_field::TARGET_TILE_Y, tile_y);
    gc.order_scratch_set_field(scratch_field::TARGET_FINE_X, fine_x);
    gc.order_scratch_set_field(scratch_field::TARGET_FINE_Y, fine_y);
    gc.order_scratch_set_field(scratch_field::TARGET_OWNER,
                               (int32_t)((uint32_t)target_player | ORDER_KIND_BUILDING));
    gc.order_scratch_set_field(scratch_field::TARGET_INDEX, target_bldg_idx);
    gc.order_scratch_set_field(scratch_field::TARGET_WEAPON, (int32_t)weapon_id);

    const int32_t target_class_flags =
        gc.target_class((uint32_t)target_player | ORDER_KIND_BUILDING, target_bldg_idx);
    const uint32_t in_range =
        gc.unit_in_weapon_range(player, unit_idx, tile_x, tile_y, target_class_flags);

    const unit     &attacker = unit_of(v, player, unit_idx);
    const cfg_unit &proto    = v.cfg_units[attacker.unit_proto_id];

    // NO plane/heli special case here (unlike the attack_target pair) -- just two arms.
    if (in_range == 0 || gc.unit_state_is_boarding((int32_t)(uint32_t)attacker.state) != 0 ||
        proto.move_op_arg != 10) {
        gc.order_enqueue((uint16_t)unit_idx, (uint16_t)((uint32_t)player | ORDER_KIND_UNIT), 0x1c,
                         (uint16_t)proto.move_op_arg);
        gc.unit_notify_status(player, unit_idx, 0);
        return;
    }

    // In range: re-jitter the footprint aim point a SECOND time and overwrite only the
    // tile/fine scratch fields with the fresh point -- target owner/index/weapon from the first
    // pass are left as they were, faithfully (not "cleaned up" into a single random-offset call).
    // Same int32_t-local/uint32_t*-out-param cast as the first call above (TACT1-P C6, 2026-09-04).
    gc.bldg_footprint_random_offset(player, (uint32_t)unit_idx, target_player, target_bldg_idx,
                                    reinterpret_cast<uint32_t *>(&fine_x),
                                    reinterpret_cast<uint32_t *>(&fine_y));
    tile_x = fine_to_tile(fine_x);
    tile_y = fine_to_tile(fine_y);
    gc.order_scratch_set_field(scratch_field::TARGET_TILE_X, tile_x);
    gc.order_scratch_set_field(scratch_field::TARGET_TILE_Y, tile_y);
    gc.order_scratch_set_field(scratch_field::TARGET_FINE_X, fine_x);
    gc.order_scratch_set_field(scratch_field::TARGET_FINE_Y, fine_y);
    gc.order_enqueue((uint16_t)unit_idx, (uint16_t)((uint32_t)player | ORDER_KIND_UNIT), 1, 0x1c);
}

void unit_order_auto_launch_from_storage_enqueue(const sim_view &v, const calls &gc, uint16_t player,
                                                 int32_t unit_idx, uint32_t x, uint32_t y) {
    const unit &u = unit_of(v, player, unit_idx);

    if (u.state == UNIT_STATE_PARKED) {
        const unit_storage &st   = storage_of(v, player, u.home_storage_slot);
        const building     &host = building_of(v, player, st.b_index);

        if (host.built_flags == BUILT_FLAGS_OPERATIONAL) {
            gc.order_scratch_reset();
            if (x == 0xffffffffu) {
                // No explicit tile given: fall back to the storage's own exit tile, torus-masked.
                gc.order_scratch_set_field(
                    0, (int32_t)(((uint32_t)st.exit_tile_x + 2u) & map_width_mask(v)));
                gc.order_scratch_set_field(
                    1, (int32_t)(((uint32_t)st.exit_tile_y + 2u) & map_height_mask(v)));
            } else {
                gc.order_scratch_set_field(0, (int32_t)(x & map_width_mask(v)));
                gc.order_scratch_set_field(1, (int32_t)(y & map_height_mask(v)));
            }
            gc.order_enqueue((uint16_t)unit_idx,
                             (uint16_t)((uint32_t)player | ORDER_KIND_UNIT),
                             (int16_t)v.cfg_units[u.unit_proto_id].default_op_code, 0x20);
        }
        return;
    }

    if (u.state == UNIT_STATE_EXIT_WAIT) {
        gc.order_scratch_reset();
        gc.order_enqueue((uint16_t)unit_idx, (uint16_t)((uint32_t)player | ORDER_KIND_UNIT), 0x1f,
                         0x23);
    }
}

void bldg_order_activate_enqueue(const calls &gc, uint16_t player, int32_t building_index) {
    gc.order_enqueue((uint16_t)building_index, (uint16_t)((uint32_t)player | ORDER_KIND_BUILDING), 0x80,
                     0x80);
}

void bldg_order_upgrade_enqueue(const calls &gc, uint16_t player, int32_t building_index) {
    gc.order_enqueue((uint16_t)building_index, (uint16_t)((uint32_t)player | ORDER_KIND_BUILDING), 0x82,
                     0x82);
}

void bldg_order_restart_construction_enqueue(const calls &gc, uint16_t player, int32_t building_index) {
    gc.order_enqueue((uint16_t)building_index, (uint16_t)((uint32_t)player | ORDER_KIND_BUILDING), 0x6b,
                     0x6b);
}

void bldg_order_repair_cycle_start_enqueue(const sim_view &v, const calls &gc, uint16_t player,
                                           int32_t building_index) {
    const building     &b  = building_of(v, player, building_index);
    const cfg_building &cb = v.cfg_buildings[b.building_id];
    if (b.energy < cb.energy && cb.type != BUILDING_TYPE_H_MAIN_BASE && cb.type != BUILDING_TYPE_A_MAIN_BASE) {
        gc.order_enqueue((uint16_t)building_index, (uint16_t)((uint32_t)player | ORDER_KIND_BUILDING),
                         0x6a, 0x6a);
    }
}

void order_grant_resource_raw(const calls &gc, uint16_t player, uint32_t res_type, uint32_t amount) {
    gc.order_scratch_reset();
    gc.order_scratch_set_field(3, (int32_t)res_type);
    gc.order_scratch_set_field(2, (int32_t)amount);
    gc.order_enqueue(0, player, 0xed, 0xed);
}

void order_population_delta_enqueue(const calls &gc, uint16_t player, int32_t population_delta) {
    gc.order_scratch_reset();
    gc.order_scratch_set_field(1, population_delta);
    gc.order_enqueue(0, player, 0xec, 0xec);
}

int32_t unit_order_state_is_settled(const sim_view &v, int32_t player, int32_t unit_idx) {
    const unit &u = unit_of(v, (uint32_t)player, unit_idx);
    if ((u.order == UNIT_STATE_STOP_TO_DEFAULT && u.state == UNIT_STATE_STOP_TO_DEFAULT) ||
        (u.order == UNIT_STATE_PATROL_SWAP && u.state == UNIT_STATE_MOVE_PATH) ||
        (u.order == UNIT_STATE_PATROL_SWAP && u.state == UNIT_STATE_IDLE_SCATTER) ||
        (u.order == UNIT_STATE_IDLE_SCATTER && u.state == UNIT_STATE_HOVER_ENGAGE)) {
        return 1;
    }
    return 0;
}

void order_collect_available_projects_thunk(const calls &gc, int32_t player) {
    gc.order_collect_available_projects_enqueue((uint16_t)player);
}

// -----------------------------------------------------------------------------------

void order_collect_available_projects_enqueue(const calls &gc, uint16_t player) {
    gc.order_enqueue(0, player, 0xee, 0xee);
}

void unit_issue_default_order(const sim_view &v, const calls &gc, uint32_t player,
                              int32_t unit_index) {
    const unit     &u     = unit_of(v, player, unit_index);
    const cfg_unit &proto = v.cfg_units[u.unit_proto_id];
    gc.order_scratch_reset();
    gc.order_enqueue((uint16_t)unit_index, (uint16_t)((uint32_t)player | ORDER_KIND_UNIT),
                     (int16_t)proto.default_op_code, proto.default_op_code);
}

void unit_order_move_enqueue(const sim_view &v, const calls &gc, uint16_t player, int32_t unit_idx,
                             int32_t x, int32_t y, int32_t move_flag) {
    const unit     &u     = unit_of(v, player, unit_idx);
    const cfg_unit &proto = v.cfg_units[u.unit_proto_id];
    gc.order_scratch_reset();
    gc.order_scratch_set_field(0, x);
    gc.order_scratch_set_field(1, y);
    gc.order_scratch_set_field(0xc, move_flag);
    gc.order_enqueue((uint16_t)unit_idx, (uint16_t)((uint32_t)player | ORDER_KIND_UNIT),
                     proto.move_op_code, proto.move_op_arg);
    gc.unit_notify_status(player, unit_idx, 0);
}

void unit_order_move_auto(const sim_view &v, const calls &gc, uint16_t player, int32_t unit_idx,
                          int32_t x, int32_t y) {
    const unit     &u     = unit_of(v, player, unit_idx);
    const cfg_unit &proto = v.cfg_units[u.unit_proto_id];
    gc.order_scratch_reset();
    gc.order_scratch_set_field(0, x);
    gc.order_scratch_set_field(1, y);
    gc.order_enqueue((uint16_t)unit_idx, (uint16_t)((uint32_t)player | ORDER_KIND_UNIT),
                     proto.move_op_code, proto.move_op_arg);
    gc.unit_notify_status(player, unit_idx, 0);
}

void unit_order_exit_storage_auto(const sim_view &v, const calls &gc, uint16_t player,
                                  uint32_t unit_idx, int32_t storage_idx,
                                  uint32_t /*param_4 -- unused, see header*/,
                                  uint32_t /*param_5 -- overwritten before use, see header*/) {
    const unit_storage &st = storage_of(v, player, storage_idx);
    // uint32_t: storage_get_approach_tile's committed out-params are uint32_t * (TACT1-P C6,
    // 2026-09-04); st.exit_tile_{x,y} are plain int32_t fields, implicit-converted in on seed.
    uint32_t exit_fine_x = st.exit_tile_x;
    uint32_t exit_fine_y = st.exit_tile_y;

    const building &host = building_of(v, player, st.b_index);
    const unit     &u    = unit_of(v, player, (int32_t)unit_idx);

    if (host.online_state != 0 &&
        gc.storage_type_accepts_unit(host.building_id, u.unit_proto_id) != 0) {
        gc.order_scratch_reset();
        gc.order_scratch_set_field(2, (int32_t)storage_idx);

        if (v.cfg_units[u.unit_proto_id].move_op_arg == 10) {
            gc.order_scratch_set_field(0, exit_fine_x);
            gc.order_scratch_set_field(1, exit_fine_y);
            gc.order_enqueue((uint16_t)unit_idx, (uint16_t)((uint32_t)player | ORDER_KIND_UNIT), 0x24,
                             0x38);
        } else {
            gc.storage_get_approach_tile(player, (uint16_t)unit_idx, &exit_fine_x, &exit_fine_y,
                                         storage_idx);
            gc.order_scratch_set_field(0, exit_fine_x);
            gc.order_scratch_set_field(1, exit_fine_y);
            gc.order_enqueue((uint16_t)unit_idx, (uint16_t)((uint32_t)player | ORDER_KIND_UNIT), 0x29,
                             0xb);
        }
        gc.unit_notify_status(player, (int32_t)unit_idx, 0);
        return;
    }

    // The storage no longer wants this unit -- fall back to a plain AI-tick move order. Unlike the
    // _enqueue twin, this path does NOT stamp an order-sequence id: the original calls
    // llm_strat_unit_order_move_auto, not _move_enqueue, and move_auto's own committed signature
    // carries no move_flag parameter for a sequence id to occupy.
    gc.unit_order_move_auto(player, (int32_t)unit_idx, (uint32_t)exit_fine_x, (uint32_t)exit_fine_y);
}

void order_ctrlgrp_select_member(const calls &gc, uint32_t param_1, uint16_t param_2, int32_t a2) {
    gc.order_scratch_reset();
    gc.order_scratch_set_field(0, a2);
    gc.order_dispatch(param_2, param_1 & 0xffffu, 0x34, 0x34);
}

void order_ctrlgrp_flash_member(const calls &gc, uint16_t param_1, uint16_t param_2, int32_t a2) {
    gc.order_scratch_reset();
    gc.order_scratch_set_field(0, a2);
    gc.order_dispatch(param_2, (uint32_t)param_1 & 0xffffu, 0x35, 0x35);
}

void unit_set_order_param(sim_store &own, int32_t player, int32_t unit_index, int16_t param) {
    own.unit_at((uint32_t)player, unit_index).order = (uint16_t)param;
}

// ---- SIM1B -------------------------------------------------------------------------

int32_t bldg_instant_construct_find_slot_enqueue(const sim_view &v, const calls &gc, int32_t arg4,
                                                 int32_t arg5, int32_t building_type_id, int32_t arg1,
                                                 uint16_t player) {
    int32_t sub_slot = 0;
    switch (v.cfg_buildings[building_type_id].type) {
        case BUILDING_TYPE_A_PRODUCTION:
        case BUILDING_TYPE_H_PRODUCTION:
            for (int32_t i = 1; i < v.caps.productions; ++i) {
                if (v.productions[player * v.caps.productions + i].b_index == 0) {
                    sub_slot = i;
                    break;
                }
            }
            break;
        case BUILDING_TYPE_A_MINE:
        case BUILDING_TYPE_H_MINE:
            for (int32_t i = 1; i < v.caps.mines; ++i) {
                if (v.mines[player * v.caps.mines + i].b_index == 0) {
                    sub_slot = i;
                    break;
                }
            }
            break;
        case BUILDING_TYPE_A_PLANT:
        case BUILDING_TYPE_A_COLONY:
        case BUILDING_TYPE_A_MOTHER:
        case BUILDING_TYPE_A_MAIN_BASE:
        case BUILDING_TYPE_A_RELAY:
        case BUILDING_TYPE_A_SILOS:
        case BUILDING_TYPE_A_CIVIL:
        case BUILDING_TYPE_H_PLANT:
        case BUILDING_TYPE_H_COLONY:
        case BUILDING_TYPE_H_MOTHER:
        case BUILDING_TYPE_H_MAIN_BASE:
        case BUILDING_TYPE_H_RELAY:
        case BUILDING_TYPE_H_SILOS:
        case BUILDING_TYPE_H_CIVIL:
            sub_slot = 1;
            break;
        case BUILDING_TYPE_A_TURRET:
        case BUILDING_TYPE_H_TURRET:
            for (int32_t i = 1; i < v.caps.turrets; ++i) {
                if (v.turrets[player * v.caps.turrets + i].b_index == 0) {
                    sub_slot = i;
                    break;
                }
            }
            break;
        case BUILDING_TYPE_A_BARRAKS:
        case BUILDING_TYPE_A_GARAGE:
        case BUILDING_TYPE_A_AIRFIELD:
        case BUILDING_TYPE_A_HELIPAD:
        case BUILDING_TYPE_A_PORT:
        case BUILDING_TYPE_A_SHUTTLE:
        case BUILDING_TYPE_H_BARRACKS:
        case BUILDING_TYPE_H_GARAGE:
        case BUILDING_TYPE_H_AIRFIELD:
        case BUILDING_TYPE_H_HELIPAD:
        case BUILDING_TYPE_H_PORT:
        case BUILDING_TYPE_H_SHUTTLE:
            // Original literal bound (0x10 = 16), NOT STORAGE_PER_PLAYER (25) -- reproduced as-is.
            for (int32_t i = 1; i < 16; ++i) {
                if (v.storage[player * v.caps.storage + i].b_index == 0) {
                    sub_slot = i;
                    break;
                }
            }
            break;
        case BUILDING_TYPE_A_LAB:
        case BUILDING_TYPE_H_LAB:
            for (int32_t i = 1; i < v.caps.labs; ++i) {
                if (v.labs[player * v.caps.labs + i].b_index == 0) {
                    sub_slot = i;
                    break;
                }
            }
            break;
        default:
            break;
    }

    if (sub_slot == 0) return 0;

    // Original literal bound (0x5b = 91), NOT BUILDINGS_PER_PLAYER (100) -- reproduced as-is.
    for (int32_t bldg_slot = 1; bldg_slot < 0x5b; ++bldg_slot) {
        if (building_of(v, player, bldg_slot).building_id == 0) {
            gc.order_scratch_reset();
            gc.order_scratch_set_field(4, arg4);
            gc.order_scratch_set_field(5, arg5);
            gc.order_scratch_set_field(1, arg1);
            gc.order_scratch_set_field(0, building_type_id);
            gc.order_enqueue((uint16_t)bldg_slot, (uint16_t)((uint32_t)player | ORDER_KIND_BUILDING),
                             0xea, 0xea);
            return bldg_slot;
        }
    }
    return 0;
}

} // namespace detail

// ---- the public surface --------------------------------------------------------------------------

int32_t order_recruit_unit_enqueue(uint32_t unit_id, uint32_t player_id) {
    return detail::order_recruit_unit_enqueue(live_calls(), unit_id, player_id);
}

uint32_t order_queue_construction_enqueue(uint32_t param_1, uint32_t param_2, uint32_t a2,
                                          uint16_t param_4) {
    return (uint32_t)detail::order_queue_construction_enqueue(
        live_calls(), (int32_t)param_1, (int32_t)param_2, (int32_t)a2, param_4);
}

void bldg_order_production_add_enqueue(uint16_t player, int32_t bldg_idx, int32_t unit_type) {
    const sim_view v = state().read;
    detail::bldg_order_production_add_enqueue(v, live_calls(), player, bldg_idx, unit_type);
}

void bldg_order_assign_workers_enqueue(uint16_t player, uint16_t bldg_idx, uint32_t worker_count) {
    detail::bldg_order_assign_workers_enqueue(live_calls(), player, bldg_idx, (int32_t)worker_count);
}

void bldg_order_unassign_workers_enqueue(uint16_t param_1, uint16_t param_2, uint32_t a2) {
    detail::bldg_order_unassign_workers_enqueue(live_calls(), param_1, param_2, (int32_t)a2);
}

void unit_order_exit_storage_enqueue(uint32_t player, uint32_t unit_idx, int32_t storage_idx,
                                     uint32_t param_4, uint32_t param_5) {
    sim_state st = state();
    detail::unit_order_exit_storage_enqueue(st.read, st.own, live_calls(), (uint16_t)player,
                                            unit_idx, storage_idx, param_4, param_5);
}

void unit_order_attack_target_enqueue(uint32_t param_1, int32_t param_2, uint32_t a2, int32_t param_4,
                                      uint32_t param_5) {
    const sim_view v = state().read;
    detail::unit_order_attack_target_enqueue(v, live_calls(), (uint16_t)param_1, param_2,
                                             (uint16_t)a2, param_4, param_5, 0x1a);
}

void unit_order_attack_target_alt_enqueue(uint32_t param_1, int32_t param_2, uint32_t a2,
                                          int32_t param_4, uint32_t param_5) {
    const sim_view v = state().read;
    detail::unit_order_attack_target_enqueue(v, live_calls(), (uint16_t)param_1, param_2,
                                             (uint16_t)a2, param_4, param_5, 0x1b);
}

void unit_order_attack_unit_enqueue(uint32_t player, int32_t unit_idx, uint32_t target_player,
                                    int32_t target_unit_idx, uint32_t weapon_id) {
    const sim_view v = state().read;
    detail::unit_order_attack_unit_enqueue(v, live_calls(), (uint16_t)player, unit_idx,
                                           (uint16_t)target_player, target_unit_idx, weapon_id);
}

void unit_order_attack_building_enqueue(uint32_t param_1, int32_t param_2, uint32_t a2,
                                        int32_t param_4, uint32_t param_5) {
    const sim_view v = state().read;
    detail::unit_order_attack_building_enqueue(v, live_calls(), (uint16_t)param_1, param_2,
                                               (uint16_t)a2, param_4, param_5);
}

void unit_order_attack_building_reposition_alt_enqueue(uint32_t player, int32_t unit_idx,
                                                       uint32_t target_player,
                                                       int32_t  target_bldg_idx,
                                                       uint32_t weapon_idx) {
    const sim_view v = state().read;
    detail::unit_order_attack_building_reposition_alt_enqueue(
        v, live_calls(), (uint16_t)player, unit_idx, (uint16_t)target_player, target_bldg_idx,
        weapon_idx);
}

void unit_order_auto_launch_from_storage_enqueue(uint32_t player, int32_t unit_idx, uint32_t x,
                                                 uint32_t y) {
    const sim_view v = state().read;
    detail::unit_order_auto_launch_from_storage_enqueue(v, live_calls(), (uint16_t)player, unit_idx,
                                                        x, y);
}

void bldg_order_activate_enqueue(uint32_t player_id, int32_t building_index) {
    detail::bldg_order_activate_enqueue(live_calls(), (uint16_t)player_id, building_index);
}

void bldg_order_upgrade_enqueue(uint32_t player_id, int32_t building_index) {
    detail::bldg_order_upgrade_enqueue(live_calls(), (uint16_t)player_id, building_index);
}

void bldg_order_repair_cycle_start_enqueue(uint32_t player_id, int32_t building_index) {
    const sim_view v = state().read;
    detail::bldg_order_repair_cycle_start_enqueue(v, live_calls(), (uint16_t)player_id, building_index);
}

void bldg_order_restart_construction_enqueue(uint32_t player_id, int32_t building_index) {
    detail::bldg_order_restart_construction_enqueue(live_calls(), (uint16_t)player_id, building_index);
}

void order_grant_resource_raw(uint16_t player, uint32_t res_type, uint32_t amount) {
    detail::order_grant_resource_raw(live_calls(), player, res_type, amount);
}

void order_population_delta_enqueue(uint32_t player_id, int32_t population_delta) {
    detail::order_population_delta_enqueue(live_calls(), (uint16_t)player_id, population_delta);
}

int32_t unit_order_state_is_settled(int32_t player, int32_t unit_idx) {
    const sim_view v = state().read;
    return detail::unit_order_state_is_settled(v, player, unit_idx);
}

void order_collect_available_projects_thunk(int32_t player) {
    detail::order_collect_available_projects_thunk(live_calls(), player);
}

// ---------------------------------------------------------------------------------

void order_collect_available_projects_enqueue(uint16_t player) {
    detail::order_collect_available_projects_enqueue(live_calls(), player);
}

void unit_issue_default_order(uint32_t player, int32_t unit_index) {
    const sim_view v = state().read;
    detail::unit_issue_default_order(v, live_calls(), player, unit_index);
}

void unit_order_move_enqueue(uint16_t player, int32_t unit_idx, uint32_t x, uint32_t y,
                             uint32_t move_flag) {
    const sim_view v = state().read;
    detail::unit_order_move_enqueue(v, live_calls(), player, unit_idx, (int32_t)x, (int32_t)y,
                                    (int32_t)move_flag);
}

void unit_order_move_auto(uint16_t player, int32_t unit_idx, uint32_t x, uint32_t y) {
    const sim_view v = state().read;
    detail::unit_order_move_auto(v, live_calls(), player, unit_idx, (int32_t)x, (int32_t)y);
}

void unit_order_exit_storage_auto(uint32_t param_1, uint32_t param_2, int32_t a2, uint32_t param_4,
                                  uint32_t param_5) {
    const sim_view v = state().read;
    detail::unit_order_exit_storage_auto(v, live_calls(), (uint16_t)param_1, param_2, a2, param_4,
                                         param_5);
}

void order_ctrlgrp_select_member(uint32_t side, uint16_t unit_id, int32_t group_index) {
    detail::order_ctrlgrp_select_member(live_calls(), side, unit_id, group_index);
}

void order_ctrlgrp_flash_member(uint16_t side, uint16_t unit_id, int32_t group_index) {
    detail::order_ctrlgrp_flash_member(live_calls(), side, unit_id, group_index);
}

void unit_set_order_param(int32_t player, int32_t unit_index, int16_t param) {
    sim_state st = state();
    detail::unit_set_order_param(st.own, player, unit_index, param);
}

int32_t bldg_instant_construct_find_slot_enqueue(uint32_t param_1, uint32_t param_2,
                                                 int32_t building_type_id, uint32_t param_4,
                                                 uint16_t player) {
    return detail::bldg_instant_construct_find_slot_enqueue(
        state().read, live_calls(), (int32_t)param_1, (int32_t)param_2, building_type_id,
        (int32_t)param_4, player);
}


} // namespace mh::sim


namespace mh::sim {
} // namespace mh::sim
