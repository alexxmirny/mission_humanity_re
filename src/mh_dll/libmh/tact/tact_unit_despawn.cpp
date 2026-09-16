//
// tact/tact_unit_despawn.cpp -- see tact_unit_despawn.h. Translated from the DISASSEMBLY, not from
// Ghidra's C.
//
#include "tact/tact_unit_despawn.h"

#include "addr/mh_calls.gen.h" // frontier callees (Law 4)
#include "state/host_api.h"
#include "state/host_events.h"
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "tact/tact_unit_vision.h"
#include "state/rebind_targets.gen.h"

namespace mh::tact {

const unit_despawn_calls &live_unit_despawn_calls() {
    static const unit_despawn_calls c = {
        MH_LIBMH_BIND(llm_tact_selection_panel_refresh),
        MH_LIBMH_BIND(llm_tact_active_unit_count_hud_draw),
        mh::state::evt::inv_tact_player_rows,
        MH_LIBMH_BIND(llm_tact_unit_vision_remove),
    };
    return c;
}

namespace detail {

void unit_despawn(tact_store &own, const unit_despawn_calls &c, int32_t unit_idx) {
    tact_unit &u = own.unit_at(unit_idx);

    // @0x00432060-0x00432093: cache the tile anchor before status/type are overwritten below --
    // matches the original's own read-everything-first shape (locals at EBP-0x20/-0x1c/-0x18).
    const int32_t pos_col = u.pos_col;
    const int32_t pos_row = u.pos_row;

    // @0x0043209d: clear the selection/active-animation status bits.
    u.status = 0;
    // @0x004320ab: NOT an overwrite -- an 8-bit ADD that wraps mod 256, leaving the prior type
    // recoverable in the high range (see header banner point 2).
    u.type = (uint8_t)(u.type + 0x80);

    // @0x004320b2-0x004320c6: three UI refresh calls; none reads anything from `u`.
    // llm_tact_ui_draw_player_row_list's argument is always the literal -1, never unit_idx.
    c.selection_panel_refresh();
    c.active_unit_count_hud_draw();
    c.ui_draw_player_row_list(-1);

    // @0x004320c6-0x004320d4: `MOV word ptr [EAX + 0xd1ec82],0x0` -- tile base 0xd1ec80 + 0x2,
    // which is the 16-bit `building` field (mh_map_tile_object_data: flags[2]@0, building@2,
    // unit[2]@4), NOT the unit[0..1] occupancy pair at +4. The first shipped version cleared
    // unit[0]/unit[1] here; that offset error survived its own offline oracle (the oracle asserted
    // the same wrong field) and an adversarial review, and was caught by llm_tact_unit_spawn's
    // translator reading the twin write in ITS disassembly (spawn stores the occupant stamp through
    // the same +0x2 word).
    own.planes().tile_object_at(pos_col, pos_row).building = 0;

    // @0x004320dd-0x004320e6: restore default passability at the same tile.
    own.planes().passable_at(pos_col, pos_row) = mh::state::PASSABLE_DEFAULT;

    // @0x004320ed: one fewer active unit.
    --own.unit_active_count();

    // @0x004320f3-0x004320fb: drop this unit from the FOV/vision-cone bookkeeping.
    c.unit_vision_remove(unit_idx);
}

} // namespace detail

void unit_despawn(int32_t unit_idx) {
    tact_state st = state();
    detail::unit_despawn(st.own, live_unit_despawn_calls(), unit_idx);
}

} // namespace mh::tact


namespace mh::tact {


} // namespace mh::tact
