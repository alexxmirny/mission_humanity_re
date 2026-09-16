//
// sim/sim_bldg_add_workers.cpp -- see sim_bldg_add_workers.h. Translated from the DISASSEMBLY
// (tmp/decomp/llm_strat_bldg_add_workers_00491916.asm), which is the authority the header's own
// derivation walk is built from.
//
#include "sim/sim_bldg_add_workers.h"

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const add_workers_calls &live_add_workers_calls() {
    static const add_workers_calls gc = {
        MH_LIBMH_BIND(llm_strat_bldg_uses_workers),
        MH_LIBMH_BIND(llm_strat_refresh_building),
        MH_LIBMH_BIND(llm_strat_bldg_set_staffed_flag),
    };
    return gc;
}

namespace {
// llm_strat_bldg_state members this function reads, transcribed from the disassembly's CMP
// immediates (0x00491958/0x00491977/0x00491996/0x004919b6, re-checked at 0x004919e1/0x004919fe/
// 0x00491a1d/0x00491a68 for the cap-selection block) -- same bare-uint16_t-field reasoning as
// sim_order_dispatch_bldg.cpp's/sim_bldg_finish_order.cpp's own BLDG_STATE_* blocks; not shared with
// them (anonymous-namespace, different TU) so redeclared here with the same values, per
// tmp/decomp/_CONTEXT_sim1b_slice4.md's enum dump.
inline constexpr uint16_t BLDG_STATE_CONSTRUCTION = 0x64;
inline constexpr uint16_t BLDG_STATE_CHARGE_STEP  = 0x6a;
inline constexpr uint16_t BLDG_STATE_DISMANTLING  = 0x6b;
inline constexpr uint16_t BLDG_STATE_UPGRADING    = 0x82;
} // namespace

namespace detail {

int32_t add_workers(const sim_view &v, sim_store &own, const add_workers_calls &gc, uint16_t player,
                    uint32_t building_id, int32_t count) {
    // 0x0049192c-0x0049193c: player/building_id/count are all stashed to the stack once, then
    // player is re-read via a 16-bit MOVZX at every subsequent use (only the low 16 bits ever
    // participate) -- matches sim_bldg_refresh_all_buildings.cpp's/sim_bldg_population_layoff_
    // workers.cpp's own player-narrowing note. `player` already arrives as uint16_t here (the
    // committed prototype), so no extra narrowing step is needed at the call boundary.
    const int32_t uses_workers = gc.uses_workers((uint32_t)player, (int32_t)building_id);

    // ---- the gate (0x00491941-0x004919c9): reject (no-op, return 0) ONLY when uses_workers()==0
    // AND `state` matches none of the four gated states -- see the header's derivation walk. Read
    // `state` fresh (translator brief rule 16: `const` on sim_view is not a promise of stability). ----
    {
        const uint16_t state       = building_of(v, player, building_id).state;
        const bool     state_gated = state == BLDG_STATE_CONSTRUCTION || state == BLDG_STATE_CHARGE_STEP ||
                                 state == BLDG_STATE_UPGRADING || state == BLDG_STATE_DISMANTLING;
        if (uses_workers == 0 && !state_gated) {
            return 0;
        }
    }

    // ---- the cap (0x004919ce-0x00491ad1): a second, independent state check picks the worker-count
    // ceiling. Re-read `state`/`building_id` fresh -- no write has happened between the gate above and
    // here, but every access re-fetches rather than reusing a value read before the external call
    // above, per house style. ----
    const building &b      = building_of(v, player, building_id);
    const uint16_t  state2 = b.state;
    const uint16_t  cfg_id = b.building_id;
    int32_t         cap;
    if (state2 == BLDG_STATE_CONSTRUCTION || state2 == BLDG_STATE_CHARGE_STEP || state2 == BLDG_STATE_DISMANTLING) {
        cap = v.cfg_buildings[cfg_id].builder_count;
    } else if (state2 == BLDG_STATE_UPGRADING) {
        // ONE level of indirection through the UPGRADE TARGET's own cfg record -- NOT the building's
        // own id (0x00491a86-0x00491a9d).
        cap = v.cfg_buildings[v.cfg_buildings[cfg_id].upgrade_index].builder_count;
    } else {
        cap = v.cfg_buildings[cfg_id].worker_count;
    }

    // ---- the clamp (0x00491ae4-0x00491b12): current_workers+count vs cap, SIGNED. `count` CAN go
    // negative here (current_workers already at/above cap) -- preserved, not floored at 0. ----
    const int32_t current_workers_before = (int32_t)(uint32_t)building_of(v, player, building_id).current_workers;
    if (current_workers_before + count > cap) {
        count = cap - current_workers_before;
    }

    // ---- the apply (0x00491b28-0x00491b2b): a NATIVE 16-BIT `ADD word ptr [...],AX` -- truncate
    // `count` to 16 bits before the add (matching the instruction's own width), reading/writing
    // `current_workers` through the mutable store. ----
    building &mb       = own.building_at(player, (int32_t)building_id);
    mb.current_workers = (uint16_t)(mb.current_workers + (int16_t)count);

    // 0x00491b32-0x00491b39: unconditional refresh, regardless of the clamp outcome.
    gc.refresh_building(player, (int32_t)building_id);

    // 0x00491b51-0x00491b67: `current_workers` is re-read fresh here (refresh_building is an
    // external call, so translator brief rule 16 applies again) -- set the staffed flag only if it
    // is now nonzero.
    if (building_of(v, player, building_id).current_workers != 0) {
        gc.set_staffed_flag(player, (int32_t)building_id);
    }

    return count;
}

} // namespace detail

// ---- the public surface --------------------------------------------------------------------------

int32_t add_workers(uint16_t player, uint32_t building_id, int32_t count) {
    sim_state st = state();
    return detail::add_workers(st.read, st.own, live_add_workers_calls(), player, building_id, count);
}


} // namespace mh::sim


namespace mh::sim {
} // namespace mh::sim
