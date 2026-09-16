//
// tact/tact_unit_move_advance.cpp -- see tact_unit_move_advance.h. Translated from the
// DISASSEMBLY, not from Ghidra's C.
//
#include "tact/tact_unit_move_advance.h"

#include "addr/mh_calls.gen.h"  // frontier callees (Law 4): vision_add/_remove, set_anim_state
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "tact/tact_unit_set_anim_state.h"
#include "tact/tact_unit_vision.h"

namespace mh::tact {
namespace detail {

void unit_move_advance(tact_store &own, int32_t unit_idx, int32_t delta_col, int32_t delta_row) {
    tact_unit &u = own.unit_at(unit_idx);

    // The two dead params (see the header banner) are never read; col/row come straight from the
    // unit record.
    int32_t col = u.pos_col;
    int32_t row = u.pos_row;

    // @0x00430fcd-0x00430ffe: only on a freshly-started step.
    if (u.progress == 0) {
        own.planes().passable_at(col, row)                         = mh::state::PASSABLE_DEFAULT;
        own.planes().passable_at(col + delta_col, row + delta_row) = mh::state::PASSABLE_BLOCKED;
    }

    // @0x00430ffe-0x00431019: still animating this step -- stop.
    ++u.progress;
    if (u.progress <= 0x1f) return;

    // @0x0043101f-0x00431033: arrival.
    MH_LIBMH_BIND(llm_tact_unit_vision_remove)(unit_idx);

    // @0x00431038-0x004310b3: consume one RLE run-length tick of the current path waypoint.
    mh::state::path_waypoint &wp = own.planes().path_waypoint_at(0, u.move_path_slot, u.move_path_step);
    --wp.run_length;
    if (wp.run_length == 0) ++u.move_path_step;

    // @0x004310b3-0x004310c1: release the OLD tile's occupancy stamp.
    own.planes().tile_object_at(col, row).building = 0;

    // @0x004310ca-0x004310e7: claim the NEW tile.
    int32_t new_col                                        = col + delta_col;
    int32_t new_row                                        = row + delta_row;
    own.planes().tile_object_at(new_col, new_row).building = (uint16_t)unit_idx;

    // @0x004310ee-0x00431119: commit position (byte-truncated, matching the original's byte store)
    // and reset progress.
    u.pos_col  = (uint8_t)new_col;
    u.pos_row  = (uint8_t)new_row;
    u.progress = 0;

    // @0x0043111c-0x00431155: 2-state walk-cycle toggle.
    uint8_t anim = u.anim_state;
    if (anim == 0)
        MH_LIBMH_BIND(llm_tact_unit_set_anim_state)(unit_idx, 1);
    else if (anim == 1)
        MH_LIBMH_BIND(llm_tact_unit_set_anim_state)(unit_idx, 0);

    // @0x00431155-0x0043115d
    MH_LIBMH_BIND(llm_tact_unit_vision_add)(unit_idx);
}

} // namespace detail

void unit_move_advance(int32_t unit_idx, int32_t delta_col, int32_t delta_row) {
    tact_state st = state();
    detail::unit_move_advance(st.own, unit_idx, delta_col, delta_row);
}

// ---- the rebind ABI shim -------------------------------------------------------------------------
//
// FIVE arguments, not three: the original's two dead middle ones occupy EDX/EBX and the generated
// dispatcher marshals them, so the five-argument shape has to exist.
// The binder pins every target against the COMMITTED export prototype, and compares types
// EXACTLY (rebind_verify.gen.cpp's per-row static_assert). Where the public wrapper above spells
// that shape differently, the committed shape still has to exist somewhere -- that is this shim,
// and all it does is forward. It sat beside the differential oracle until F2D retired it and was
// never part of it; gen_libmh_rebind routes the row here through libmh_rebind_targets.json.
namespace rebind_arm {

void unit_move_advance(int32_t unit_idx, uint32_t /*arg_edx_unused*/, uint32_t /*arg_ebx_unused*/,
                       int32_t delta_col, int32_t delta_row) {
    mh::tact::unit_move_advance(unit_idx, delta_col, delta_row);
}

} // namespace rebind_arm

} // namespace mh::tact
