//
// tact/tact_teleport_cmdqueue_jump.cpp -- see tact_teleport_cmdqueue_jump.h. Translated from the
// DISASSEMBLY, not from Ghidra's C.
//
#include "tact/tact_teleport_cmdqueue_jump.h"

#include "addr/mh_calls.gen.h"  // frontier callee (Law 4): llm_tact_unit_teleport
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::tact {

const teleport_cmdqueue_jump_calls &live_teleport_cmdqueue_jump_calls() {
    static const teleport_cmdqueue_jump_calls c = {
        MH_LIBMH_BIND(llm_tact_unit_teleport),
    };
    return c;
}

namespace detail {

int32_t teleport_cmdqueue_jump(tact_store &own, const mh::state::mode_planes &planes,
                               const teleport_cmdqueue_jump_calls &c, int32_t unit_id, int32_t dest_x,
                               int32_t dest_y) {
    // @0x004334f9-0x0043351c: the scratch zone is written UNCONDITIONALLY, before the passability
    // check below.
    teleport_zone &z = own.teleport_zone_at(TACT_TELEPORT_SCRATCH_SLOT);
    z.id             = TACT_TELEPORT_SCRATCH_SLOT;
    z.field_28       = 0;
    z.mode           = 0;
    z.no_enemy       = 0;
    z.association[0] = 0;
    z.dest_col[0]    = static_cast<uint16_t>(dest_x);
    z.dest_row[0]    = static_cast<uint16_t>(dest_y);

    // @0x0043352e-0x00433544: refuse when the destination tile has a building on it.
    if (planes.tile_object_at(dest_x, dest_y).building != 0) return 0;

    // @0x00433546-0x00433553: otherwise perform the real teleport and report success.
    c.unit_teleport(TACT_TELEPORT_SCRATCH_SLOT, unit_id);
    return 1;
}

} // namespace detail

int32_t teleport_cmdqueue_jump(int32_t unit_id, int32_t dest_x, int32_t dest_y) {
    tact_state st = state();
    return detail::teleport_cmdqueue_jump(st.own, st.own.planes(), live_teleport_cmdqueue_jump_calls(),
                                          unit_id, dest_x, dest_y);
}

} // namespace mh::tact


namespace mh::tact {


} // namespace mh::tact
