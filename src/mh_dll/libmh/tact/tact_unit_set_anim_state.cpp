//
// tact/tact_unit_set_anim_state.cpp -- see tact_unit_set_anim_state.h. Translated from the
// DISASSEMBLY, not from Ghidra's C.
//
#include "tact/tact_unit_set_anim_state.h"


namespace mh::tact {

namespace detail {

void unit_set_anim_state(tact_store &own, int32_t building_id, uint8_t state) {
    tact_unit &u = own.unit_at(building_id);

    // @0x00430f20-0x00430f3e: a CMP whose flags nothing downstream reads (the next instruction is
    // an unconditional store) -- not reproduced.

    // @0x00430f41-0x00430f4e: unconditional.
    u.anim_state = state;
}

} // namespace detail

void unit_set_anim_state(int32_t building_id, uint8_t state) {
    tact_state st = mh::tact::state(); // qualified: `state` above is this function's own parameter
    detail::unit_set_anim_state(st.own, building_id, state);
}

} // namespace mh::tact


namespace mh::tact {


} // namespace mh::tact
