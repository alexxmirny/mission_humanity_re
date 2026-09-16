//
// sim/sim_bldg_clear_flag_bit0_notify.cpp -- see sim_bldg_clear_flag_bit0_notify.h. Translated from
// the DISASSEMBLY (tmp/decomp/llm_strat_bldg_clear_flag_bit0_notify_0049663d.asm), not from the
// Ghidra .c draft.
//
#include "sim/sim_bldg_clear_flag_bit0_notify.h"

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const clear_flag_bit0_notify_calls &live_clear_flag_bit0_notify_calls() {
    static const clear_flag_bit0_notify_calls c = {
        MH_LIBMH_BIND(llm_strat_bldg_notify_ui),
    };
    return c;
}

namespace detail {

void clear_flag_bit0_notify(sim_store &own, const clear_flag_bit0_notify_calls &c, uint16_t player,
                            int32_t building_index) {
    // 0x0049666d-0x00496689: two independent IMUL-pair address computations of the SAME slot
    // (buildings[player][building_index]) -- Watcom's usual re-derive-rather-than-cache idiom, not
    // two different targets. Reproduced as a single read-modify-write through one reference.
    building &b = own.building_at((uint32_t)player, building_index);

    // 0x00496673: AND DL,0xfe -- clear bit0 of built_flags. Per docs/structs.md and this slice's
    // power-network functions (see the header banner), bit0 is "connected/reached"; this is the
    // graph's "mark unreached" primitive, not an unverified bit.
    b.built_flags = (uint8_t)(b.built_flags & 0xfeu);

    // 0x0049668f-0x00496696: unconditional notify, EDX(building_index) loaded before EAX(player) but
    // both are read before the CALL so evaluation order is not observable here (same non-issue
    // sim_bldg_refresh_all_buildings.cpp's own comment notes for its analogous call site).
    c.notify_ui(player, (uint32_t)building_index);
}

} // namespace detail

// ---- the public surface --------------------------------------------------------------------------

void clear_flag_bit0_notify(uint16_t player, int32_t building_index) {
    sim_state st = state();
    detail::clear_flag_bit0_notify(st.own, live_clear_flag_bit0_notify_calls(), player, building_index);
}


} // namespace mh::sim


namespace mh::sim {
} // namespace mh::sim
