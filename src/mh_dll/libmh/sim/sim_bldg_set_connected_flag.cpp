//
// sim/sim_bldg_set_connected_flag.cpp -- see sim_bldg_set_connected_flag.h. Translated from the
// DISASSEMBLY (tmp/decomp/llm_bldg_set_connected_flag_004965d6.asm), not from the Ghidra .c draft.
//
#include "sim/sim_bldg_set_connected_flag.h"

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const bldg_set_connected_flag_calls &live_bldg_set_connected_flag_calls() {
    static const bldg_set_connected_flag_calls c = {
        MH_LIBMH_BIND(llm_strat_bldg_notify_ui),
    };
    return c;
}

namespace detail {

void bldg_set_connected_flag(sim_store &own, const bldg_set_connected_flag_calls &c, uint16_t player,
                             int32_t b_index) {
    // 0x004965ed-0x00496622: two independent MOVZX/IMUL/IMUL/ADD address computations of the SAME slot
    // (buildings[player][b_index]) -- Watcom's usual re-derive-rather-than-cache idiom, not two
    // different targets. Reproduced as a single read-modify-write through one reference.
    building &b = own.building_at((uint32_t)player, b_index);

    // 0x0049660c: OR DL,0x1 -- set bit0 of built_flags. Per docs/structs.md and this slice's
    // power-network functions (see the header banner), bit0 is "connected/reached"; this is the
    // graph's "mark reached" primitive, the mirror of llm_strat_bldg_clear_flag_bit0_notify's
    // "mark unreached".
    b.built_flags = (uint8_t)(b.built_flags | 0x1u);

    // 0x00496628-0x0049662f: unconditional notify, EDX(b_index) loaded before EAX(player) but both
    // are read before the CALL so evaluation order is not observable here (same non-issue
    // sim_bldg_clear_flag_bit0_notify.cpp's own comment notes for its analogous call site).
    c.notify_ui(player, (uint32_t)b_index);
}

} // namespace detail

// ---- the public surface --------------------------------------------------------------------------

void bldg_set_connected_flag(uint16_t player, int32_t b_index) {
    sim_state st = state();
    detail::bldg_set_connected_flag(st.own, live_bldg_set_connected_flag_calls(), player, b_index);
}


} // namespace mh::sim


namespace mh::sim {
} // namespace mh::sim
