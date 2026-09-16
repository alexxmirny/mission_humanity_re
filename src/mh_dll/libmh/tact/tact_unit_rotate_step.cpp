//
// tact/tact_unit_rotate_step.cpp -- see tact_unit_rotate_step.h. Translated from the DISASSEMBLY,
// not from Ghidra's C.
//
#include "tact/tact_unit_rotate_step.h"

#include "addr/mh_calls.gen.h"  // frontier callees (Law 4): llm_tact_unit_vision_add/_remove
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "tact/tact_unit_vision.h"

namespace mh::tact {
namespace detail {

void unit_rotate_step(tact_store &own, int32_t unit_idx, int32_t target_dir) {
    tact_unit &u = own.unit_at(unit_idx);

    // @0x00430e15-0x00430e1c: a turn already mid-step is not re-stepped.
    if (u.progress != 0) return;

    int32_t type = u.type; // @0x00430e22-0x00430e30 -- used by the rotate-speed add below

    MH_LIBMH_BIND(llm_tact_unit_vision_remove)(unit_idx); // @0x00430e33-0x00430e3b

    // @0x00430e3b-0x00430e70: delta and its two range folds, preserved literally.
    int32_t delta = target_dir - u.facing_dir;
    if (delta > 12) delta = 12 - delta;
    if (delta < -11) delta = -delta - 12;

    if (delta < 0) {
        // @0x00430e79-0x00430ea6
        if (u.facing_dir == 1)
            u.facing_dir = 0x18;
        else
            --u.facing_dir;
    } else {
        // @0x00430ea8-0x00430ed5
        if (u.facing_dir == 0x18)
            u.facing_dir = 1;
        else
            ++u.facing_dir;
    }

    MH_LIBMH_BIND(llm_tact_unit_vision_add)(unit_idx); // @0x00430ed5-0x00430edd

    // @0x00430edd-0x00430ef4: accumulate, not overwrite.
    u.move_state_timer += own.character_type_at(type).rotate;
}

} // namespace detail

void unit_rotate_step(int32_t unit_idx, int32_t target_dir) {
    tact_state st = state();
    detail::unit_rotate_step(st.own, unit_idx, target_dir);
}

} // namespace mh::tact


namespace mh::tact {


} // namespace mh::tact
