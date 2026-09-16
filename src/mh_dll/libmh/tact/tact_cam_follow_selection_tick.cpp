//
// tact/tact_cam_follow_selection_tick.cpp -- see tact_cam_follow_selection_tick.h. Translated from
// the DISASSEMBLY (tmp/decomp_tact/llm_tact_cam_follow_selection_tick_0042e957.asm), not from
// Ghidra's .c.
//
#include "tact/tact_cam_follow_selection_tick.h"

#include <cstdlib>

#include "addr/mh_calls.gen.h" // frontier callee (Law 4): llm_tact_vis_map_fill_default
#include "state/host_api.h"
#include "state/host_events.h"

namespace mh::tact {

const cam_follow_selection_tick_calls &live_cam_follow_selection_tick_calls() {
    static const cam_follow_selection_tick_calls c = {
        mh::state::evt::inv_tact_vis_map,
    };
    return c;
}

namespace detail {

void cam_follow_selection_tick(const tact_view &v, tact_store &own,
                               const cam_follow_selection_tick_calls &c) {
    // @0x0042e96f-0x0042e988: the residual deltas, computed ONCE against the camera position at
    // entry. The four blocks below either leave a delta at this stale value (nudge case) or force
    // it to 0 (edge case); nothing here is re-derived from a fresh camera read.
    int32_t delta_col = own.map_cam_col() - *v.cam_follow_target_col;
    int32_t delta_row = own.map_cam_row() - *v.cam_follow_target_row;

    // @0x0042e98b-0x0042e9af: camera more than 1 tile past the target on the col axis -> nudge 2
    // tiles back, unless already at/near the col-0 edge (<=1), in which case treat as caught up.
    if (delta_col > 1) {
        if (own.map_cam_col() > 1) {
            own.map_cam_col() -= 2;
            c.vis_map_fill_default();
        } else {
            delta_col = 0;
        }
    }

    // @0x0042e9af-0x0042e9d3: same shape for the row axis.
    if (delta_row > 1) {
        if (own.map_cam_row() > 1) {
            own.map_cam_row() -= 2;
            c.vis_map_fill_default();
        } else {
            delta_row = 0;
        }
    }

    // @0x0042e9d3-0x0042ea17: camera more than 1 tile short of the target on the col axis -> nudge
    // 2 tiles forward, unless already within 1 tile of the right map edge. `*v.win_w / 32` is
    // plain truncating division -- see this header's own note on the shift idiom it replaces.
    if (delta_col < -1) {
        const int32_t visible_cols = *v.win_w / 32;
        const int32_t right_edge   = *v.map_width - visible_cols - 1;
        if (right_edge > own.map_cam_col()) {
            own.map_cam_col() += 2;
            c.vis_map_fill_default();
        } else {
            delta_col = 0;
        }
    }

    // @0x0042ea17-0x0042ea5a: same shape for the row axis against the bottom map edge.
    // `own.window_height() / 24` is a literal IDIV in the .asm; C++ `/` on `int` matches directly.
    if (delta_row < -1) {
        const int32_t visible_rows = own.window_height() / 24;
        const int32_t bottom_edge  = *v.map_height - visible_rows - 1;
        if (bottom_edge > own.map_cam_row()) {
            own.map_cam_row() += 2;
            c.vis_map_fill_default();
        } else {
            delta_row = 0;
        }
    }

    // @0x0042ea5a-0x0042ea80: final gate over the LOCAL residuals left by the four blocks above
    // (never a fresh read of the camera/target globals). CDQ/XOR/SUB in the .asm is the
    // sign-agnostic abs() idiom; std::abs reproduces it exactly for these int32_t locals.
    if (std::abs(delta_col) < 2 && std::abs(delta_row) < 2) {
        own.cam_follow_selection() = 0;
    }
}

} // namespace detail

void cam_follow_selection_tick() {
    tact_state st = state();
    detail::cam_follow_selection_tick(st.read, st.own, live_cam_follow_selection_tick_calls());
}


} // namespace mh::tact
