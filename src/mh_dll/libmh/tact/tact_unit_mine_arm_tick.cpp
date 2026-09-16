//
// tact/tact_unit_mine_arm_tick.cpp -- see tact_unit_mine_arm_tick.h. Translated from the
// DISASSEMBLY, not from Ghidra's C.
//
#include "tact/tact_unit_mine_arm_tick.h"

#include "addr/mh_calls.gen.h"  // frontier callees: llm_tact_unit_set_anim_state, time_GetCurrentTime,
                                // and the in-manifest cross-TU sibling llm_tact_unit_cmd_advance
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "tact/tact_unit_cmd_advance.h"
#include "tact/tact_unit_set_anim_state.h"
#include "state/promoted_select.h" // LIB-REF-SPLIT: MH_PROMOTED

namespace mh::tact {
namespace detail {

void unit_mine_arm_tick(const tact_view &tv, tact_store &own, int32_t unit_idx) {
    tact_unit &u = own.unit_at(unit_idx);

    // @0x0042f952-0x0042f96a
    const int32_t type = u.type;
    ++u.progress;

    // @0x0042f970-0x0042f9bb
    MH_LIBMH_BIND(llm_tact_unit_set_anim_state)(unit_idx, 4);
    if (u.progress > 0x3f) {
        u.progress = 0;
        MH_LIBMH_BIND(llm_tact_unit_set_anim_state)(unit_idx, 0);
        const int32_t cmd_slot = u.cmd_index; // read BEFORE cmd_advance mutates it
        MH_LIBMH_BIND(llm_tact_unit_cmd_advance)(unit_idx, cmd_slot);
    }

    // @0x0042f9bb-0x0042fa30: unconditional test -- NOT gated on the branch above. progress > 0x1f
    // (unsigned) is what gates the whole blast-marker update (steps below); either way, the
    // move_state_timer accumulate at the very end always runs (both paths land on `default:`).
    if (u.progress > 0x1f) {
        // @0x0042fa30-0x0042fa43: unconditional OVERWRITE every tick this branch runs, not an
        // accumulator -- each arming tick pushes the deadline further out.
        const double now          = MH_PROMOTED_ROW(time_GetCurrentTime)();
        own.mine_blast_time_end() = now + *tv.mine_blast_duration;

        // @0x0042fa43-0x0042fa85: re-latch the marker to this unit's own tile every tick.
        own.blast_marker_col() = u.pos_col;
        own.blast_marker_row() = u.pos_row;

        const uint8_t facing = static_cast<uint8_t>(u.facing_dir - 1);
        if (facing <= 0x17) {
            // @0x0042fa88-0x0042fae5: the RAW 24-entry jump table (read from 0x0042f9d0, not
            // inferred from case labels -- see the header's derivation), 8 near-equal octant bands.
            // EVERY facing value nudges the marker; there is no no-op arm in here.
            switch (facing) {
                case 0x0:
                case 0x1:
                case 0x17: ++own.blast_marker_row(); break;
                case 0x2:
                case 0x3:
                case 0x4:
                    --own.blast_marker_col();
                    ++own.blast_marker_row();
                    break;
                case 0x5:
                case 0x6:
                case 0x7: --own.blast_marker_col(); break;
                case 0x8:
                case 0x9:
                case 0xa:
                    --own.blast_marker_col();
                    --own.blast_marker_row();
                    break;
                case 0xb:
                case 0xc:
                case 0xd: --own.blast_marker_row(); break;
                case 0xe:
                case 0xf:
                case 0x10:
                    ++own.blast_marker_col();
                    --own.blast_marker_row();
                    break;
                case 0x11:
                case 0x12:
                case 0x13: ++own.blast_marker_col(); break;
                case 0x14:
                case 0x15:
                case 0x16:
                    ++own.blast_marker_col();
                    ++own.blast_marker_row();
                    break;
            }
        }
    }

    // @0x0042fae5-0x0042fafc: the unconditional tail both paths converge on.
    u.move_state_timer += own.character_type_at(type).mine_time;
}

} // namespace detail

void unit_mine_arm_tick(int32_t unit_idx) {
    tact_state st = state();
    detail::unit_mine_arm_tick(st.read, st.own, unit_idx);
}

} // namespace mh::tact


namespace mh::tact {


} // namespace mh::tact
