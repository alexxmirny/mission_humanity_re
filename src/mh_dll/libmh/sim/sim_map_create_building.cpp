//
// sim/sim_map_create_building.cpp -- see sim_map_create_building.h. Translated from the DISASSEMBLY
// (tmp/decomp/map_CreateBuilding_004622cb.asm) -- every field/offset the Ghidra .c draft names was
// independently re-derived from the raw IMUL/ADD/MOV address arithmetic and cross-checked against
// mh_structs.gen.h's static_asserts one at a time (mh_map_object_building, mh_cfg_final_struct_Building,
// mh_map_object_production/_mine/_turret/_lab, mh_map_object_unit_storage, mh_llm_strat_player_profile).
// It agrees with the .c draft on every field name and value; it is not a rubber stamp -- three real
// gaps turned up in the process, all flagged below and in the header banner: the turret case's
// undocumented pad-field writes, the concurrent apply_area_to_map sibling's unverified signature, and
// a contradiction between the assembly and sim_state.h's own foreign_bldg_event_pending() comment.
//
#include "sim/sim_map_create_building.h"

#include "addr/mh_calls.gen.h" // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"       // ai_say / trace_budget -- the shared trace sink, not AI state

// The four ALREADY-TRANSLATED siblings, called directly (see the header banner).
#include "sim/sim_bldg_staffed_flag.h"  // set_staffed_flag
#include "sim/sim_bldg_worker_assign.h" // assign_workers
#include "sim/sim_fog_of_war.h"         // sight_add_circle
#include "sim/sim_refresh_building.h"   // refresh_building
// DECLARED NEED (see the header banner): this is a CONCURRENT sibling translation in this same batch
// slice; its public wrapper's real signature is unverified. Included AS IF `apply_area_to_map(int32_t,
// int32_t, const uint8_t *)` is its committed shape.
#include "sim/sim_map_apply_area.h" // apply_area_to_map

// BUILDING_TYPE_A_*/H_* -- pinned in sim_order_enqueue.h (SIM1C), reused rather than duplicated, same
// convention sim_bldg_construct_finalize.h already established for this exact switch shape.
#include "sim/sim_order_enqueue.h"
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"
#include "crt/crt_select.h" // LIB-CRT: MH_CRT() picks the vendored CRT in the standalone build

namespace mh::sim {

const create_building_calls &live_create_building_calls() {
    static const create_building_calls c = {
        MH_CRT(utils_fill_data),
        MH_LIBMH_BIND(game_SetEvent),
        // The five already-translated siblings, bound to their real PUBLIC wrappers -- a direct call to
        // the translated sibling (NOT mh::call::), identical to the old `mh::sim::X(...)` form, just now
        // routable through the struct so net_selftest can stub it. See the header's SIBLINGS banner.
        &mh::sim::sight_add_circle,
        &mh::sim::apply_area_to_map,
        &mh::sim::assign_workers,
        &mh::sim::set_staffed_flag,
        &mh::sim::refresh_building,
    };
    return c;
}

namespace {

// ---- little-endian 4-byte pack/unpack over a raw uint8_t[] span --------------------------------
// Two record fields are flattened byte arrays standing in for arrays of 4-byte cfg_t_frame_index
// elements (mh_map_object_building::anim[48] / mh_cfg_final_struct_Building::anim[48], both really
// cfg_t_frame_index[12] per their own comments) -- the SAME pattern sim_state.h's cfg_unit_weapon_id()/
// cfg_upgrade_object_id() already document for other flattened cfg arrays, just not yet given a helper
// for THIS pair. See the DECLARED NEED in the header on promoting these if the pattern recurs.
inline void store_u32_le(uint8_t *dst, uint32_t value) {
    dst[0] = (uint8_t)(value);
    dst[1] = (uint8_t)(value >> 8);
    dst[2] = (uint8_t)(value >> 16);
    dst[3] = (uint8_t)(value >> 24);
}
inline uint32_t load_u32_le(const uint8_t *src) {
    return (uint32_t)src[0] | ((uint32_t)src[1] << 8) | ((uint32_t)src[2] << 16) |
           ((uint32_t)src[3] << 24);
}

} // namespace

namespace detail {

void create_building(const sim_view &v, sim_store &own, const create_building_calls &c, uint16_t player,
                     uint32_t index, uint32_t x_b, int32_t y_b, int32_t building_id, uint32_t param_6,
                     uint32_t sub_id) {
    const uint32_t player32 = player;
    const uint16_t index16  = (uint16_t)index;
    const int32_t  sub_id32 = (int32_t)sub_id;

    const cfg_building &cfg = v.cfg_buildings[building_id];

    // ---- (1) bulk zero-fill, ALWAYS, via the ORIGINAL callee (0x004622d3-0x0046230d) ---------------
    building &b = own.building_at(player32, (int32_t)index);
    c.fill_data(&b, sizeof(building), 0);

    // ---- (2) the record's own scalar fields, ALWAYS (0x00462312-0x004624bb) -----------------------
    b.index          = (int16_t)index16;
    b.building_id    = (uint16_t)building_id;
    b.state          = CREATE_BUILDING_STATE_CONSTRUCTION;
    b.cycle_progress = 0.0; // asm zeroes both dwords -- 0.0 is all-zero bits, bit-identical
    b.online_state   = 0;
    b.built_flags    = 0;
    const double now = *v.game_clock; // re-read many times in the original with no intervening write
                                      // (same "safe reuse" reasoning sim_unit_init_record.cpp applies)
    b.last_tick_time        = now;
    b.energy                = cfg.energy; // straight double copy from cfg, no arithmetic
    b.pending_damage        = 0.0;        // asm zeroes both dwords
    b.shuttle_slot          = 0;
    b.incoming_damage_tally = 0;
    b.sub_id                = (uint8_t)sub_id;
    b.x                     = (uint8_t)x_b;
    b.y                     = (uint8_t)y_b;

    // ---- (3) footprint stamp: tile_objects/passable over the 10x10 cfg mask (0x004624bb-0x00462590)
    const uint32_t wmask = map_width_mask(v);
    const uint32_t hmask = map_height_mask(v);
    for (int32_t i = 0; i < 10; ++i) {
        for (int32_t j = 0; j < 10; ++j) {
            if (cfg.area[i][j] == 0) continue;
            const int32_t tx        = (int32_t)((x_b + (uint32_t)i) & wmask);
            const int32_t ty        = (int32_t)(((uint32_t)y_b + (uint32_t)j) & hmask);
            tile_object  &to        = own.tile_object_at(tx, ty);
            to.building             = index16;
            to.class_owner          = (uint8_t)((uint8_t)player | 0x40u);
            own.passable_at(tx, ty) = 0;
        }
    }

    // ---- (4) sight registration + map-region occupancy (0x00462590-0x004625cd) ---------------------
    // Both siblings are ALREADY TRANSLATED -- routed through `c` (bound to their real public wrappers in
    // live_create_building_calls(), so this is still a direct call to the translated sibling, NOT
    // mh::call::; see the header's SIBLINGS banner). The struct seam only lets net_selftest stub them.
    c.sight_add_circle(player32, (int32_t)x_b, y_b, building_id, cfg.sight);
    // apply_area_to_map's committed public wrapper takes uint8_t* (TACT1-P C6, 2026-09-04; was void*)
    // -- cfg.area[0] is const uint8_t*, so a C-style cast is still needed to drop the const (the
    // callee only reads the mask).
    c.apply_area_to_map((int32_t)x_b, y_b, (uint8_t *)cfg.area[0]);

    // ---- (5) anim/anim_dur (12 entries) then pip triple (4 entries) (0x004625cd-0x00462779) --------
    // b.anim is a flattened uint8_t[48] standing in for cfg_t_frame_index[12] -- see the LE helpers
    // above. b.anim_dur is a plain double[12], no flattening.
    for (int32_t i = 0; i < 12; ++i) {
        store_u32_le(&b.anim[i * 4], 0);
        b.anim_dur[i] = now;
    }
    store_u32_le(&b.anim[0], load_u32_le(&cfg.anim[0])); // re-seed slot 0 from the cfg record

    for (int32_t i = 0; i < 4; ++i) {
        b.pip_frame[i] = 0;
        b.pip_level[i] = 0;
        b.pip_timer[i] = now;
    }
    b.pip_active_count = 0;

    // ---- (6) type-specific sub-record init (0x00462779-0x00462b9d) ---------------------------------
    switch (cfg.type) {
        case BUILDING_TYPE_A_PRODUCTION:
        case BUILDING_TYPE_H_PRODUCTION: {
            production &p0 = own.production_at(player32, 0);
            p0.b_index += 1;
            production &p = own.production_at(player32, sub_id32);
            p.b_index     = (int32_t)index;
            for (int32_t k = 0; k < 100; ++k) p.queued_count[k] = 0;
            p.active_unit_type = 0;
            break;
        }
        case BUILDING_TYPE_A_MINE:
        case BUILDING_TYPE_H_MINE: {
            mine &m0 = own.mine_at(player32, 0);
            m0.b_index += 1;
            own.mine_at(player32, sub_id32).b_index = (int32_t)index;
            break;
        }
        case BUILDING_TYPE_A_TURRET:
        case BUILDING_TYPE_H_TURRET: {
            turret &t0 = own.turret_at(player32, 0);
            t0.b_index += 1;
            turret &t = own.turret_at(player32, sub_id32);
            t.b_index = (int16_t)index16;

            // ---- TURRET BOOT WRITES: seven bytes, ALL NAMED (SIM1-G4 named
            // aim_heading/aim_step_dir/acquire_retry_seed/acquire_retry_counter plus the
            // aim_heading_boot_scratch@+0x6 gap-fix; the SIM-READY prep session of 2026-08-22 closed
            // the last two as cached_sight@+0x1c and resupply_available@+0x25 -- the sibling selftest's
            // pre-existing `+0x1c == cfg.sight` / `+0x25 == 1 (armed)` assertions are what
            // independently corroborated both readings). Offsets/values read directly off the asm
            // (0x004629d6-0x00462a72):
            //   +0x1c (dword) = (uint32_t)Building[building_id].sight (zero-extended byte)
            //   +0xa  (dword) = 1   -> aim_step_dir
            //   +0x2  (dword) = 1   -> aim_heading
            //   +0x6  (dword) = 1   -> aim_heading_boot_scratch (purpose still unconfirmed -- no reader
            //                          found anywhere; SIM1-G4's own struct-field comment has the detail)
            //   +0xe  (dword) = 0x1e (30) -> acquire_retry_seed
            //   +0x12 (dword) = 0x1e (30) -> acquire_retry_counter (cross-validates
            //                          llm_strat_bldg_state_turret_scan's own '(seed % 40) + 30' re-seed)
            t.cached_sight             = (uint32_t)cfg.sight;
            t.aim_step_dir             = 1;
            t.aim_heading              = 1;
            t.aim_heading_boot_scratch = 1;
            t.acquire_retry_seed       = 0x1e;
            t.acquire_retry_counter    = 0x1e;

            // cfg.weapon_id is int32_t (its own field comment: "Read as a DWORD by every AI reader;
            // CreateBuilding copies only the low byte") -- turret.weapon_id is uint8_t, so this is an
            // explicit truncation, not a same-width copy. The zero-check reads back that SAME
            // truncated byte (0x00462a9f/0x00462aa5), but the Weapon[] index in the non-zero branch
            // is a SEPARATE, untruncated re-read of cfg.weapon_id as a full dword (0x00462ad7-
            // 0x00462ade) -- reimpl-verify caught this the first time through (map_CreateBuilding
            // review, SIM1F): indexing by the truncated t.weapon_id instead of the full
            // cfg.weapon_id diverges for any cfg.weapon_id >= 256.
            t.weapon_id = (uint8_t)cfg.weapon_id;
            if (t.weapon_id == 0) {
                t.attack_range = 0;
            } else {
                t.resupply_available = 1; // +0x25
                t.reload_ready_flag  = 1;
                t.attack_range       = v.cfg_weapons[cfg.weapon_id].range_max[player];
            }
            break;
        }
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
        case BUILDING_TYPE_H_SHUTTLE: {
            unit_storage &s0 = own.storage_at(player32, 0);
            s0.b_index += 1;
            unit_storage &s     = own.storage_at(player32, sub_id32);
            s.b_index           = (int32_t)index;
            s.door_mutex_unit   = 0;
            s.door_waiter_count = 0;
            s.docked_count      = 0;
            s.occupancy         = 0;

            // park_x/park_y are plain int32_t fields. They were committed as uint8_t + 3 "reserved"
            // bytes until 2026-08-22, which was a WIDTH DEFECT, not a real layout: the original stores
            // each as ONE 32-bit dword (0x0046290f `MOV dword ptr [EAX+0xc7289c],EDX` and 0x0046293e),
            // and both readers load them back as full dwords. The byte-splatter the .c draft renders
            // was a decompiler artifact of the too-narrow type. Retyped byte -> int in Ghidra (EN v313),
            // so the store_u32_le() workaround this block used to need is gone and a straight
            // assignment is now the faithful reproduction.
            s.park_x = (int32_t)((x_b + (uint32_t)cfg.park_offset_x) & wmask);
            s.park_y = (int32_t)(((uint32_t)y_b + (uint32_t)cfg.park_offset_y) & hmask);

            // exit_tile_x/y are plain int32_t fields (no byte-splitting needed) -- confirmed via a
            // SEPARATE pair of literal cfg offsets 4 bytes further in than park_offset_x/_y, matching
            // shuttle_pad_offset_x/_y exactly (static_assert-confirmed against mh_structs.gen.h).
            s.exit_tile_x = (int32_t)((x_b + (uint32_t)cfg.shuttle_pad_offset_x) & wmask);
            s.exit_tile_y = (int32_t)(((uint32_t)y_b + (uint32_t)cfg.shuttle_pad_offset_y) & hmask);
            break;
        }
        case BUILDING_TYPE_A_LAB:
        case BUILDING_TYPE_H_LAB: {
            lab &l0 = own.lab_at(player32, 0);
            l0.b_index += 1;
            lab &l              = own.lab_at(player32, sub_id32);
            l.b_index           = (int32_t)index;
            l.active_project_id = 0;
            break;
        }
        default:
            break; // every other cfg type: no sub-record to init (matches the switch's own default arm)
    }

    // ---- (7) UNCONDITIONAL shared-tail bookkeeping (0x00462b9d-0x00462c57) -------------------------
    // buildings[player][0] is NOT the new record -- like production[player][0].b_index/mine[player][0]
    // .b_index/turret[player][0].b_index/lab[player][0].b_index/storage[player][0].b_index above, slot 0's own
    // state/energy/index fields are being reused here as a per-player "buildings created" scratch.
    building &b0 = own.building_at(player32, 0);
    b0.state += 1; // the .c draft prints this literal 1 as `IDLE_ACTIVATE` (see uncertainties) -- this
                   // is a raw INC on a repurposed counter field, not a state-machine transition
    b0.energy += 1.0;
    b0.index += 1;

    player_profile &prof = own.profile_at((int32_t)player32);
    prof.buildings_alive[*v.planet_index] += 1;
    prof.buildings_built_total[*v.planet_index] += 1;

    // CONDITIONAL clear -- see the header's DECLARED NEED / this file's uncertainty note: this
    // contradicts sim_state.h's own foreign_bldg_event_pending() accessor comment, which claims an
    // unconditional clear. The condition below is transcribed directly from the CMP/Jcc chain
    // (0x00462c0b-0x00462c4d) and independently corroborated by player_profile's own
    // buildings_built_total field comment ("cumulative; >5 on planet>3 fires an AI event in
    // CreateBuilding").
    if ((int16_t)player != *v.player_side && *v.planet_index > 3 && *v.planet_index != 0x1f &&
        prof.buildings_built_total[*v.planet_index] > 5) {
        own.foreign_bldg_event_pending() = 0;
    }

    c.set_event(CREATE_BUILDING_MAP_OBJECTS_REFRESH);

    b.current_workers = 0;

    // ---- (8) auto-assign construction workers (0x00462c7d-0x00462d22) ------------------------------
    if (v.population[player].human != 0 && v.cfg_buildings[b.building_id].builder_count != 0) {
        int32_t count = (int32_t)param_6;
        if (count == -1) {
            // NOTE: reads Building[] via the ORIGINAL `building_id` PARAMETER here, not
            // `b.building_id` (the record's own truncated-to-uint16 copy) -- see uncertainties. The
            // two are value-identical in every real case; preserved as two distinct reads to match the
            // asm's two distinct address computations exactly.
            count = cfg.builder_count;
        }
        if (count == -2) {
            const int32_t human = v.population[player].human;
            // Signed divide-by-2 reproduced via the ORIGINAL's shift form (not `human / 2`), per the
            // "reproduce the shift, not the arithmetic" rule -- differs from IDIV-style truncation on
            // negative inputs.
            count = (human - (human >> 31)) >> 1;
            if (count > cfg.builder_count) count = cfg.builder_count;
        }
        c.assign_workers(player32, index, count);
    }

    if (v.cfg_buildings[b.building_id].builder_count == 0 || b.current_workers != 0) {
        c.set_staffed_flag(player, (int32_t)index);
    }
    c.refresh_building(player, (int32_t)index);
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

void create_building(uint16_t player, uint32_t index, uint32_t x_b, int32_t y_b, int32_t building_id,
                     uint32_t param_6, uint32_t sub_id) {
    sim_state st = state();
    detail::create_building(st.read, st.own, live_create_building_calls(), player, index, x_b, y_b,
                            building_id, param_6, sub_id);
}


} // namespace mh::sim
