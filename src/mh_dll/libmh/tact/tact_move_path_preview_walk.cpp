//
// tact/tact_move_path_preview_walk.cpp -- see tact_move_path_preview_walk.h. Translated from the
// DISASSEMBLY, not from Ghidra's C.
//
#include "tact/tact_move_path_preview_walk.h"


namespace mh::tact {

namespace detail {

void move_path_preview_walk(mh::state::mode_planes &planes, int32_t start_col, int32_t start_row,
                            int32_t path_slot_id, int32_t *out_col, int32_t *out_row) {
    int32_t cur_col = start_col;
    int32_t cur_row = start_row;

    // @0x0042eb34-0x0042eb42: clear the starting tile's overlay before tracing anything.
    mh::state::tile_overlay(planes, cur_col, cur_row) = 0;

    int32_t entry_idx = 0;
    for (;;) {
        // @0x0042eb49-0x0042eb67: owner is the LITERAL 0 (see header banner), never the caller's.
        const mh::game::mh_llm_strat_path_waypoint &entry =
            planes.path_waypoint_at(0, path_slot_id, entry_idx);
        const int32_t heading = entry.heading;
        if (heading == 0) {
            *out_col = cur_col;
            *out_row = cur_row;
            return;
        }
        const int32_t run_length = entry.run_length;
        ++entry_idx;

        for (int32_t remaining = run_length; remaining > 0; --remaining) {
            // @0x0042ebc5-0x0042ec03: stamp the CURRENT tile -- high nibble := (heading/3)+1,
            // low nibble preserved.
            {
                uint8_t &ov = mh::state::tile_overlay(planes, cur_col, cur_row);
                ov          = (uint8_t)(((heading / 3) << 4) + 0x10) | (uint8_t)(ov & 0xf);
            }

            // @0x0042ec09-0x0042ecc8: the direction cascade. Values outside this set are a no-op --
            // the original's own fallthrough, not an omission.
            switch (heading) {
                case 1: cur_row += 1; break;
                case 4:
                    cur_col -= 1;
                    cur_row += 1;
                    break;
                case 7: cur_col -= 1; break;
                case 0xa:
                    cur_col -= 1;
                    cur_row -= 1;
                    break;
                case 0xd: cur_row -= 1; break;
                case 0x10:
                    cur_col += 1;
                    cur_row -= 1;
                    break;
                case 0x13: cur_col += 1; break;
                case 0x16:
                    cur_col += 1;
                    cur_row += 1;
                    break;
                default: break;
            }

            // @0x0042ecc8-0x0042ecdd: stop at the first tile whose flags[1] bit 0x80 is clear.
            if ((planes.tile_object_at(cur_col, cur_row).flags[1] & 0x80) == 0) {
                *out_col = cur_col;
                *out_row = cur_row;
                return;
            }

            // @0x0042ecf1-0x0042ed2d: OR (heading/3)+1 (unshifted) into the NEW tile's overlay.
            {
                uint8_t &ov = mh::state::tile_overlay(planes, cur_col, cur_row);
                ov          = (uint8_t)(ov | ((heading / 3) + 1));
            }
        }
    }
}

} // namespace detail

void move_path_preview_walk(int32_t start_col, int32_t start_row, int32_t path_slot_id,
                            int32_t *out_col, int32_t *out_row) {
    tact_state st = state();
    detail::move_path_preview_walk(st.own.planes(), start_col, start_row, path_slot_id, out_col,
                                   out_row);
}

} // namespace mh::tact


namespace mh::tact {


} // namespace mh::tact
