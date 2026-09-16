//
// tact/tact_unit_death_tick.cpp -- see tact_unit_death_tick.h. Translated from the DISASSEMBLY, not
// from Ghidra's C.
//
#include "tact/tact_unit_death_tick.h"

#include "addr/mh_calls.gen.h"  // frontier callee (Law 4): llm_tact_unit_destroy
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::tact {

const unit_death_tick_calls &live_unit_death_tick_calls() {
    static const unit_death_tick_calls c = {
        MH_LIBMH_BIND(llm_tact_unit_destroy),
    };
    return c;
}

namespace detail {

int32_t unit_death_tick(tact_store &own, const unit_death_tick_calls &c, int32_t unit_idx) {
    tact_unit &u = own.unit_at(unit_idx);

    // @0x0042fb28-0x0042fb36: snapshot the character-class index before `progress` changes below.
    const uint8_t type = u.type;

    // @0x0042fb39: a CMP whose flags nothing downstream reads -- see the header derivation. Not
    // reproduced.

    // @0x0042fb3d-0x0042fb44: byte-wide increment, wraps mod 256 like the original INC.
    ++u.progress;

    // @0x0042fb4a-0x0042fb58: the branch is on the POST-increment value.
    const bool still_dying = u.progress < 0x6a;

    if (still_dying) {
        // @0x0042fb6b-0x0042fb88
        u.move_state_timer += own.character_type_at(type).death_time;
        return 0;
    }

    // @0x0042fb5a-0x0042fb69
    c.unit_destroy(unit_idx);
    return 1;
}

} // namespace detail

int32_t unit_death_tick(int32_t unit_idx) {
    tact_state st = state();
    return detail::unit_death_tick(st.own, live_unit_death_tick_calls(), unit_idx);
}

} // namespace mh::tact


namespace mh::tact {


} // namespace mh::tact
