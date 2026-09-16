//
// tact/tact_unit_destroy.cpp -- see tact_unit_destroy.h. Translated from the DISASSEMBLY, not from
// Ghidra's C.
//
#include "tact/tact_unit_destroy.h"

#include "addr/mh_calls.gen.h" // frontier callees (Law 4)
#include "state/host_api.h"
#include "state/host_events.h"
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "tact/tact_unit_vision.h"
#include "state/rebind_targets.gen.h"

namespace mh::tact {

const unit_destroy_calls &live_unit_destroy_calls() {
    static const unit_destroy_calls c = {
        MH_LIBMH_BIND(llm_tact_unit_vision_remove),
        MH_LIBMH_BIND(llm_tact_selection_panel_refresh),
        MH_LIBMH_BIND(llm_tact_active_unit_count_hud_draw),
        mh::state::evt::inv_tact_player_rows,
    };
    return c;
}

namespace detail {

void unit_destroy(tact_store &own, const unit_destroy_calls &c, uint32_t unit_idx) {
    c.unit_vision_remove((int32_t)unit_idx); // @0x00431da8

    // @0x00431db0-0x00431dbe: reads units[unit_idx].type -- never used again. Not reproduced.

    tact_unit    &u   = own.unit_at((int32_t)unit_idx);
    const int32_t col = u.pos_col; // @0x00431dc1
    const int32_t row = u.pos_row; // @0x00431dd2
    u.status          = 0;         // @0x00431de3

    // @0x00431df1-0x00432006: clear this unit's `.building` stamp from up to 9 candidate tiles.
    auto clear_if_match = [&](int32_t c, int32_t r) -> bool {
        auto &field = own.planes().tile_object_at(c, r).building;
        if (field == unit_idx) {
            field = 0;
            return true;
        }
        return false;
    };

    if (clear_if_match(col, row)) { // @0x00431df1-0x00431e22: EARLY EXIT on match
        // every other candidate is skipped
    } else {
        clear_if_match(col, row);                                 // @0x00431e27-0x00431e58: REDUNDANT re-check of the same tile
                                                                  // block A just found not-matching (memory unchanged in between,
                                                                  // so this can never itself match) -- preserved literally
        clear_if_match(col + 1, row);                             // @0x00431e58-0x00431e8b
        clear_if_match(col, row + 1);                             // @0x00431e8b-0x00431ebc: fixed 2026-08-26 -- the EAX
                                                                  // operand here is `col` with NO increment (contrast the
                                                                  // INC EAX at 0x00431e5b/0x00431ebf for the true +1-col
                                                                  // candidates); the +8 in the 0xd1ec8a immediate is a
                                                                  // ROW-stride step (row-stride is 8 bytes, col-stride is
                                                                  // 0x800), so this candidate is (col, row+1), not
                                                                  // (col+1, row+1). Was collapsed into the next candidate
                                                                  // below, silently dropping this tile from the search.
        clear_if_match(col + 1, row + 1);                         // @0x00431ebc-0x00431eef
        if (col > 0 && row > 0) clear_if_match(col - 1, row - 1); // @0x00431eef-0x00431f30
        if (col > 0) {
            clear_if_match(col - 1, row);     // @0x00431f30-0x00431f69
            clear_if_match(col - 1, row + 1); // @0x00431f69-0x00431f9c
        }
        if (row > 0) {
            clear_if_match(col, row - 1);     // @0x00431f9c-0x00431fd3
            clear_if_match(col + 1, row - 1); // @0x00431fd3-0x00432006
        }
    }

    // @0x00432006-0x00432023
    u.type = 0;
    --own.unit_active_count();
    own.planes().passable_at(col, row) = mh::state::PASSABLE_DEFAULT;

    // @0x0043202a-0x00432039
    c.selection_panel_refresh();
    c.active_unit_count_hud_draw();
    c.ui_draw_player_row_list(-1);
}

} // namespace detail

void unit_destroy(uint32_t unit_idx) {
    tact_state st = state();
    detail::unit_destroy(st.own, live_unit_destroy_calls(), unit_idx);
}

} // namespace mh::tact


namespace mh::tact {


} // namespace mh::tact
