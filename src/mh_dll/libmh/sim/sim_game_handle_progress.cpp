//
// sim/sim_game_handle_progress.cpp -- see sim_game_handle_progress.h. Translated from the DISASSEMBLY
// (tmp/decomp/game_HandleProgress_0043ff0c.asm), which the Ghidra .c draft agrees with on every
// branch; the only draft discrepancy is the dropped NOTHING2() call (see the header).
//
#include "sim/sim_game_handle_progress.h"

#include "addr/mh_calls.gen.h"                    // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"                          // ai_say / trace_budget -- the shared trace sink, not AI state
#include "sim/sim_game_notify_system_available.h" // the already-translated SYSTEM-case sibling's public wrapper
#include "state/host_api.h"
#include "state/host_events.h"
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const handle_progress_calls &live_handle_progress_calls() {
    static const handle_progress_calls c = {
        MH_LIBMH_BIND(game_AddToAvailableBuildingsWithCheck),
        mh::state::evt::progress_unit_available,
        MH_LIBMH_BIND(game_AddProjectToAvailableWithCheck),
        MH_LIBMH_BIND(game_AddPlanetToAvailable),
        MH_LIBMH_BIND(game_HandleUpgrade),
        MH_LIBMH_BIND(game_UpdateProgress),
    };
    return c;
}

namespace detail {

void game_handle_progress(const sim_view &v, sim_store &own, const handle_progress_calls &c,
                          uint16_t player, uint16_t inv) {
    // 0x0043ff46-0x0043ff5f: progress[player][inv].available = true. ONE fetch, per the "always
    // through `own`, never re-resolve" hazard note every other sim_store per-index write follows.
    own.progress_at(player, static_cast<int32_t>(inv)).available = true;

    // 0x0043ff65-0x0043ff71: CALL NOTHING2() -- omitted. See header banner: NOTHING2's own committed
    // marshalling (addr/mh_calls.gen.h) already takes zero argument registers, so the EAX/EDX loads
    // immediately before this call site are dead regardless of this function's own body.

    // 0x0043ff72-0x00440040: switch on Progress[inv].type (the CFG Invention table, DISTINCT from the
    // per-player `progress` state just written above -- see the header banner's field-offset note).
    // Every case reads the SAME `Progress[inv].index` before dispatching.
    const uint8_t  type  = v.cfg_inventions[inv].type;
    const uint16_t index = v.cfg_inventions[inv].index;

    switch (type) {
        case INVENTION_TYPE_BUILDING: // caseD_1, 0x0043ffb8
            c.add_to_available_buildings_with_check(player, static_cast<int32_t>(index));
            break;
        case INVENTION_TYPE_UNIT: // caseD_2, 0x0043ff9c
            c.progress_notify_unit_available(player, index);
            break;
        case INVENTION_TYPE_PROJECT: // caseD_3, 0x0043ffd1
            c.add_project_to_available_with_check(player, static_cast<uint32_t>(index));
            break;
        case INVENTION_TYPE_PLANET: // caseD_4, 0x00440029
            c.add_planet_to_available(player, static_cast<int32_t>(index));
            break;
        case INVENTION_TYPE_SYSTEM: // caseD_5, 0x00440010. Already-translated sibling -- reached through
            // its own public wrapper per the batch's explicit instruction, NOT the `c.` calls table (see
            // header banner uncertainties note on the tension with sim_bldg_apply_damage.h's convention).
            mh::sim::game_notify_system_available(player, static_cast<int32_t>(index));
            break;
        case INVENTION_TYPE_UPGRADE: // caseD_6, 0x0043ffea
            c.handle_upgrade(player, static_cast<int32_t>(index));
            c.update_progress(player, inv);
            break;
        default:
            // type==0 (UNDEFINED) or any value >6: the JA-taken bounds-check path in the assembly, which
            // has no case body at all -- ordinary fall-through to return.
            break;
    }
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

void game_handle_progress(uint16_t player, uint16_t inv) {
    sim_state st = state();
    detail::game_handle_progress(st.read, st.own, live_handle_progress_calls(), player, inv);
}


} // namespace mh::sim
