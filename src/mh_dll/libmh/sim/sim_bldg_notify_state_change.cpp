//
// sim/sim_bldg_notify_state_change.cpp -- see sim_bldg_notify_state_change.h. Translated from the
// DISASSEMBLY (tmp/decomp/llm_strat_bldg_notify_state_change_00470c5c.asm), not from the Ghidra .c
// draft.
//
#include "sim/sim_bldg_notify_state_change.h"

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const bldg_notify_state_change_calls &live_bldg_notify_state_change_calls() {
    static const bldg_notify_state_change_calls c = {
        MH_LIBMH_BIND(llm_strat_bldg_notify_ui),
        MH_LIBMH_BIND(game_SetEvent),
    };
    return c;
}

namespace detail {

void notify_state_change(sim_store &own, const bldg_notify_state_change_calls &c, uint16_t player,
                         uint32_t building_id) {
    // 0x00470c79: MOV dword ptr [general+0x24], 0 -- one wide store zeroing BOTH change_flag and
    // change_flag2 (see header HAZARD note).
    own.change_flag()  = 0;
    own.change_flag2() = 0;

    // 0x00470c83-0x00470c8a: unconditional notify. EDX(building_id) loaded before AX(player) but both
    // are read before the CALL, so evaluation order is not observable here (same non-issue
    // sim_bldg_clear_flag_bit0_notify.cpp's own comment notes for its analogous call site).
    c.notify_ui(player, building_id);

    // 0x00470c8f-0x00470c9d: CMP dword ptr [general+0x24], 0; JZ past the event. Re-reads the SAME
    // pair notify_ui may have set (see header HAZARD note) -- equivalent to "the dword is nonzero".
    if (own.change_flag() != 0 || own.change_flag2() != 0) {
        c.set_event(EVENT_BUILD_UNITS_REFRESH);
    }
}

} // namespace detail

// ---- the public surface --------------------------------------------------------------------------

void notify_state_change(uint16_t player, uint32_t building_id) {
    sim_state st = state();
    detail::notify_state_change(st.own, live_bldg_notify_state_change_calls(), player, building_id);
}


} // namespace mh::sim


namespace mh::sim {
} // namespace mh::sim
