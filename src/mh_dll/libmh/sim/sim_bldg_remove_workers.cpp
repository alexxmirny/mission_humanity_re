//
// sim/sim_bldg_remove_workers.cpp -- see sim_bldg_remove_workers.h. Translated from the DISASSEMBLY
// (tmp/decomp/llm_strat_bldg_remove_workers_004917c5.asm).
//
#include "sim/sim_bldg_remove_workers.h"

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const remove_workers_calls &live_remove_workers_calls() {
    static const remove_workers_calls gc = {
        MH_LIBMH_BIND(llm_strat_refresh_building),
        MH_LIBMH_BIND(llm_strat_bldg_uses_workers),
        MH_LIBMH_BIND(llm_strat_bldg_clear_staffed_flag),
    };
    return gc;
}

namespace {
// llm_strat_bldg_state members this function reads, transcribed from the disassembly's CMP
// immediates (0x0049188d/0x004918ac/0x004918cb/0x004918eb) -- same values sim_bldg_add_workers.cpp's
// own BLDG_STATE_* block declares for the same four states (anonymous-namespace, different TU, so
// redeclared here rather than shared, per that file's own precedent note).
inline constexpr uint16_t BLDG_STATE_CONSTRUCTION = 0x64;
inline constexpr uint16_t BLDG_STATE_CHARGE_STEP  = 0x6a;
inline constexpr uint16_t BLDG_STATE_UPGRADING    = 0x82;
inline constexpr uint16_t BLDG_STATE_DISMANTLING  = 0x6b;
} // namespace

namespace detail {

uint32_t remove_workers(const sim_view &v, sim_store &own, const remove_workers_calls &gc,
                        uint16_t player, uint32_t building_id, uint32_t count) {
    // ---- the clamp (0x004917e4-0x00491820): cap `count` at `current_workers` -- read fresh, not
    // cached (translator brief rule 16). Signed compare against the zero-extended (always
    // non-negative) current_workers, matching the asm's MOVZX+CMP+JGE exactly. ----
    if ((int32_t)(uint32_t)building_of(v, player, (int32_t)building_id).current_workers < (int32_t)count) {
        count = (uint32_t)building_of(v, player, (int32_t)building_id).current_workers;
    }

    // ---- the subtract (0x00491820-0x00491836): a NATIVE 16-BIT truncating SUBTRACT -- `SUB word ptr
    // [current_workers], AX` only touches the low 16 bits of the (already-clamped) `count`, matching
    // the Ghidra draft's own `*puVar1 = *puVar1 - (short)count;` and the same int16_t-cast idiom
    // sim_bldg_add_workers.cpp's ADD side uses. ----
    {
        building &mb       = own.building_at(player, (int32_t)building_id);
        mb.current_workers = (uint16_t)(mb.current_workers - (int16_t)count);
    }

    // 0x0049183d-0x00491844: unconditional refresh, regardless of the clamp outcome.
    gc.refresh_building(player, (int32_t)building_id);

    // ---- the staffed-flag clear (0x00491849-0x00491905): `current_workers` is RE-READ fresh here
    // (refresh_building is an external call that could in principle write it, same discipline
    // sim_bldg_add_workers.cpp's set_staffed_flag gate uses) -- only when that fresh read is exactly
    // 0 does the five-way OR below run at all. ----
    if (building_of(v, player, (int32_t)building_id).current_workers == 0) {
        const int32_t  uses_workers = gc.uses_workers((uint32_t)player, (int32_t)building_id);
        const uint16_t state        = building_of(v, player, (int32_t)building_id).state;
        if (uses_workers != 0 || state == BLDG_STATE_CONSTRUCTION || state == BLDG_STATE_CHARGE_STEP ||
            state == BLDG_STATE_UPGRADING || state == BLDG_STATE_DISMANTLING) {
            gc.clear_staffed_flag(player, (int32_t)building_id);
        }
    }

    // The return value is the clamped `count` computed above -- neither the refresh nor the
    // staffed-flag clear can change it (0x00491905-0x0049190b just moves it into the return slot).
    return count;
}

} // namespace detail

// ---- the public surface --------------------------------------------------------------------------

uint32_t remove_workers(uint16_t player, uint32_t building_id, uint32_t count) {
    sim_state st = state();
    return detail::remove_workers(st.read, st.own, live_remove_workers_calls(), player, building_id,
                                  count);
}


} // namespace mh::sim


namespace mh::sim {
} // namespace mh::sim
