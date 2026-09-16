//
// tact/tact_active_unit_count_hud_draw.cpp -- see tact_active_unit_count_hud_draw.h. Translated from
// the DISASSEMBLY, not from Ghidra's .c (whose float-return artifacts around the color value are
// decompiler noise -- see the header banner).
//
#include "tact/tact_active_unit_count_hud_draw.h"

#include "addr/mh_calls.gen.h" // frontier callees (Law 4)
#include "state/host_api.h"
#include "state/host_events.h"

namespace mh::tact {

const active_unit_count_hud_draw_calls &live_active_unit_count_hud_draw_calls() {
    static const active_unit_count_hud_draw_calls c = {
        mh::state::evt::inv_tact_active_count_hud,
        mh::state::evt::inv_tact_vis_margin,
    };
    return c;
}

namespace detail {

void active_unit_count_hud_draw(const tact_view &v, tact_store &own,
                                const active_unit_count_hud_draw_calls &c) {
    // @0x00434dda-0x00434dee: unconditional reset -- this function is the PRODUCER of the pair, not
    // a gated consumer (see header banner step 0).
    own.active_unit_count()        = 0;
    own.active_unit_count_cached() = 0;

    // @0x00434dee-0x00434e5d: LOOP, i = TACT_UNIT_FIRST_SLOT..TACT_UNIT_LAST_SLOT inclusive.
    for (int32_t i = TACT_UNIT_FIRST_SLOT; i <= TACT_UNIT_LAST_SLOT; ++i) {
        const tact_unit &u = v.units[i];
        if (u.owner == 0 && u.type != 0 && u.type < 0x80) {
            own.active_unit_count() += 1;
            if ((u.status & 1) == 1) {
                own.active_unit_count_cached() += 1;
            }
        }
    }

    // @0x00434e5d-0x00434f19: the whole draw tail -- one icon blit and two formatted number lines
    // -- as ONE scope emit carrying the two counts. Unconditional, exactly as the original's draws
    // are: the readout is emitted even when both counts are zero (T2 pins this).
    //
    // WHAT LEFT WITH IT, and this is the lift working rather than detail being lost: the anchor
    // literals (0x8a/0x90/0x9a, clip 0x16x0x18), the ": %2d" format, the packed colour
    // (0xb4,0,0xfa) and the sel_panel_icon_gfx_at(0) RAW POINTER no longer cross the boundary --
    // mh.dll's sink holds them (seams/host_event_sink.cpp draw_active_count_hud), and a real host
    // draws its own HUD from the two numbers. R4: values cross, geometry does not.
    c.hud_readout_changed(own.active_unit_count(), own.active_unit_count_cached());

    // @0x00434f19: unconditional, argumentless. AFTER the draws, and the record order is the
    // contract that keeps it there.
    c.vis_map_clear_right_margin();
}

} // namespace detail

void active_unit_count_hud_draw() {
    tact_state st = state();
    detail::active_unit_count_hud_draw(st.read, st.own, live_active_unit_count_hud_draw_calls());
}


} // namespace mh::tact
