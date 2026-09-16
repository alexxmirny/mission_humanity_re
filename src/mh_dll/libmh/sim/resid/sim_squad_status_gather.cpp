//
// sim/resid/sim_squad_status_gather.cpp -- see sim_squad_status_gather.h. Translated from the
// DISASSEMBLY (tmp/decomp_sim_resid/llm_strat_bldg_gather_nearby_squad_status_0044d468.asm,
// tmp/decomp_sim_resid/llm_strat_try_enter_tactical_mission_0044d34f.asm); the exported .c drafts are
// a cross-check, not the source of truth.
//
#include "sim/resid/sim_squad_status_gather.h"

#include "addr/mh_calls.gen.h"  // typed callables for the effectful/frontier originals we still call OUT to
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"
#include "fp/x87.h" // CRT-X87: the shared x87 truncation helpers

namespace mh::sim {

const squad_status_gather_calls &live_squad_status_gather_calls() {
    static const squad_status_gather_calls c = {
        MH_LIBMH_BIND(llm_strat_tile_dist_wrapped),
        MH_LIBMH_BIND(map_SavePlanetToDisk),
        MH_LIBMH_BIND(llm_tact_mission_start),
    };
    return c;
}

namespace {

// utils_math_trunc @0x004d0596 (ST0-in/ST0-out, x87-register-only -- not stack-passable, hence not a
// mh::call:: stub). Both call sites in this file (unit energy%, building energy%) are ORDINARY calls,
// not compiler-inlined -- reproduced as this TU's own copy of the sim_bldg_economy.cpp /
// sim_bldg_completion_dispatch.cpp precedent's exact instruction sequence (each TU keeps its own
// copy; this is not a new shared helper). FISTP width confirmed 32-bit at both call sites in this
// function (opcode `db5de4`, ModRM reg field 3 -> DB /3 == FISTP m32int).
int32_t trunc_to_int32(double val) {
    return ::mh::fp::trunc_i32(val);
}

// energy% = trunc(raw_energy * scale / max_energy), floored at 1 -- the SAME idiom at BOTH call
// sites (unit @0x0044d688-d6bf, building @0x0044d791-d7c8), differing only in which three values feed
// it. Kept as one local helper within this TU (not a cross-TU shared helper, per rule 4) since both
// uses are in the same file.
int32_t energy_status_percent(double raw_energy, double scale, double max_energy) {
    int32_t pct = trunc_to_int32(raw_energy * scale / max_energy);
    if (pct <= 0) pct = 1; // @0x0044d6b2-d6be / @0x0044d7bb-d7c7
    return pct;
}

} // namespace

namespace detail {

// ---- llm_strat_bldg_gather_nearby_squad_status @0x0044d468 ------------------------------------------
int32_t bldg_gather_nearby_squad_status(const sim_view &v, sim_store &own,
                                        const squad_status_gather_calls &c, int32_t scan_player,
                                        int32_t bldg_owner, int32_t bldg_idx) {
    // 0x0044d49c-d4ec: zero all 64 SQUAD_STATUS slots' four fields, one addressed store at a time,
    // in the original's own field order (energy_pct, unit_proto_id, unit_slot_index, is_commando).
    for (int32_t i = 0; i < SQUAD_STATUS_CAPACITY; ++i) {
        squad_status_slot &slot = own.squad_status_at(i);
        slot.energy_pct         = 0;
        slot.unit_proto_id      = 0;
        slot.unit_slot_index    = 0;
        slot.is_commando        = 0;
    }

    const building     &target_bldg = v.buildings[bldg_owner * v.caps.buildings + bldg_idx];
    const cfg_building &target_cfg  = v.cfg_buildings[target_bldg.building_id];

    // 0x0044d4ee-d58d: the building's own tile position, offset by HALF its cfg footprint span.
    // FIELD/AXIS PAIRING IS SWAPPED FROM THE SIBLING CONVENTION -- see the header's derivation note
    // and the translator's uncertainties entry: `height` (cfg offset 0x9) pairs with `.x`/width_mask
    // here, `width` (cfg offset 0xa) pairs with `.y`/height_mask, the opposite of
    // sim_bldg_footprint_random_offset.cpp's width->X/height->Y. Transcribed exactly as read.
    const int32_t x1 = static_cast<int32_t>(map_width_mask(v)) &
                       (static_cast<int32_t>(target_cfg.height) / 2 + target_bldg.x);
    const int32_t y1 = static_cast<int32_t>(map_height_mask(v)) &
                       (static_cast<int32_t>(target_cfg.width) / 2 + target_bldg.y);

    int32_t count = 0; // squad_status_count, [EBP-0x34]

    // 0x0044d59e-d76f: scan control group 0 (LITERAL index 0 -- not scan_player-selected) for
    // soldier-carrying units within SQUAD_SCAN_RADIUS_TILES of (x1,y1), while count stays under
    // SQUAD_STATUS_CAPACITY.
    const ctrl_group &scan_group = v.ctrl_groups[0];
    for (int32_t i = 0; i < scan_group.count && count < SQUAD_STATUS_CAPACITY; ++i) {
        const int32_t   unit_slot = scan_group.unit_ids[i];
        const unit     &u         = v.units[scan_player * v.caps.units + unit_slot];
        const cfg_unit &proto     = v.cfg_units[u.unit_proto_id];

        if (proto.soldier_count <= 0) continue; // 0x0044d5ea-d5f1: not a soldier-carrying unit type

        const int32_t dist = c.tile_dist_wrapped(x1, y1, u.x, u.y); // 0x0044d627
        if (dist >= SQUAD_SCAN_RADIUS_TILES) continue;              // 0x0044d62f

        if (count + proto.soldier_count > SQUAD_STATUS_CAPACITY) continue; // 0x0044d662-d665: skip the
                                                                           // WHOLE unit, not a partial fill

        const int32_t energy_pct =
            energy_status_percent(u.energy, *v.unit_energy_status_percent_scale, proto.energy);
        const int32_t is_commando = (proto.soldier_type == SOLDIER_TYPE_COMMANDO) ? 1 : 0;

        // 0x0044d6c6-d76a: append `proto.soldier_count` IDENTICAL slots, one per soldier the unit
        // carries -- the original's own k-loop writes the same four values every iteration; not a
        // translation artifact (see uncertainties).
        for (int32_t k = 0; k < proto.soldier_count; ++k) {
            squad_status_slot &slot = own.squad_status_at(count);
            slot.energy_pct         = energy_pct;      // 0x0044d6e4
            slot.unit_proto_id      = u.unit_proto_id; // 0x0044d707
            slot.unit_slot_index    = unit_slot;       // 0x0044d716
            slot.is_commando        = is_commando;     // 0x0044d748 / 0x0044d75a
            ++count;
        }
    }

    // 0x0044d774-d80f: the target building's own energy%, then the five SQUAD_BB_* scalars and the
    // final count, in the original's own store order.
    const int32_t target_energy_pct =
        energy_status_percent(target_bldg.energy, *v.bldg_energy_status_percent_scale, target_cfg.energy);
    own.squad_bb_target_energy_pct()   = target_energy_pct;       // 0x0044d7c8
    own.squad_bb_target_building_id()  = target_bldg.building_id; // 0x0044d7e7
    own.squad_bb_target_building_idx() = bldg_idx;                // 0x0044d7ef
    own.squad_bb_scan_player()         = scan_player;             // 0x0044d7f7
    own.squad_bb_target_owner()        = bldg_owner;              // 0x0044d7ff
    own.squad_status_count()           = count;                   // 0x0044d807

    return count;
}

// ---- llm_strat_try_enter_tactical_mission @0x0044d34f ------------------------------------------------
void try_enter_tactical_mission(const sim_view &v, sim_store &own, const squad_status_gather_calls &c,
                                uint32_t player, uint32_t target_owner, uint32_t target_bldg_idx) {
    // 0x0044d377: same-TU sibling, plain detail:: call (G21) -- never mh::call::.
    const int32_t count = bldg_gather_nearby_squad_status(
        v, own, c, static_cast<int32_t>(player), static_cast<int32_t>(target_owner),
        static_cast<int32_t>(target_bldg_idx));

    if (count == 0) return; // 0x0044d37f-d383: no squad nearby, entry refused

    // 0x0044d38a-d38f: RETURN VALUE DISCARDED -- the original's own defect (see header banner); this
    // is the ONLY backup of the strategic planet before the tactical mission wipes its working state.
    c.save_planet_to_disk(static_cast<uint32_t>(*v.planet_index), 1);

    own.game_mode() = 6; // 0x0044d394: GAME_MODE 6 = tactical mission (mh_launch_export.h confirms)

    c.mission_start(); // 0x0044d39b
}

} // namespace detail

// ---- the public wrappers -----------------------------------------------------------------------------

int32_t bldg_gather_nearby_squad_status(int32_t scan_player, int32_t bldg_owner, int32_t bldg_idx) {
    sim_state st = state();
    return detail::bldg_gather_nearby_squad_status(st.read, st.own, live_squad_status_gather_calls(),
                                                   scan_player, bldg_owner, bldg_idx);
}

void try_enter_tactical_mission(uint32_t player, uint32_t target_owner, uint32_t target_bldg_idx) {
    sim_state st = state();
    detail::try_enter_tactical_mission(st.read, st.own, live_squad_status_gather_calls(), player,
                                       target_owner, target_bldg_idx);
}

} // namespace mh::sim
