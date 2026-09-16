#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// Every external callee this closure reaches, indirected for offline testability -- same reason as
// sim_order_enqueue.h's `calls` (a direct mh::call:: in a detail:: function is untestable by
// net_selftest.exe simtest / the offline fixture). Both are REAL calls in production: they are
// themselves in-scope sim/AI migration functions (batch E), not presentation-cut-set members, and the
// shadow site for bldg_placement_check_and_preview already claims _G_LLM_STRAT_FX_ANIMS as an
// extra_regions region for exactly this reason -- see mh_shadow.gen.h.
//
// NAMED `placement_preview_calls`, NOT the bare `calls` sim_order_enqueue.h uses -- both live at
// `mh::sim` namespace scope and reimpl_probe.cpp includes every sim TU's header, so a second `calls`/
// `live_calls()` here would be an ODR redefinition (caught at build time, fixed 2026-08-12).
struct placement_preview_calls {
    // llm_fx_anim_seq_cancel @0x004539cc. Cancels any in-flight instance of the given anim-seq id
    // before a fresh preview overlay is drawn.
    void (*fx_anim_seq_cancel)(int32_t anim_seq_start_frame);

    // llm_strat_fx_anim_spawn @0x00464855. x/y are FINE coords (tile<<5); the return value is never
    // used by either call site in this closure, matching the original (the CALL result is discarded).
    uint32_t (*fx_anim_spawn)(uint32_t x, uint32_t y, uint32_t kind, double game_clock,
                              uint32_t param_5);
};

const placement_preview_calls &live_placement_preview_calls();

namespace detail {

// llm_bldg_placement_check_and_preview @0x00453a6d. See the header banner above for the full
// derivation; the .cpp carries the per-branch address citation.
int32_t bldg_placement_check_and_preview(const sim_view &v, const placement_preview_calls &gc, int32_t origin_x,
                                         int32_t origin_y, int32_t building_index);

// llm_bldg_calc_placement_corner_from_center @0x0048d054. `unit_index` indexes v.cfg_units DIRECTLY
// (it is a cfg unit-proto id, not a live roster index -- the asm never touches a roster, only
// Unit[]/Building[] cfg tables) -- despite the parameter's name, which is the original's own and is
// kept for fidelity to the committed prototype. `out_col`/`out_row` are plain pointers here (the
// public wrapper below carries the same `uint32_t*` pointee to match the committed call/shadow shape
// exactly, TACT1-P C6, 2026-09-04 -- same convention as sim_unit_facing24_delta.h's out-params).
void bldg_calc_placement_corner_from_center(const sim_view &v, uint16_t unit_index, int32_t center_x,
                                            int32_t center_y, uint32_t *out_col, uint32_t *out_row);

} // namespace detail

// ---- the public surface, in original-behaviour terms -----------------------------------------
// Parameter types match the committed prototypes in addr/mh_calls.gen.h exactly (the drift gate
// enforces this on export/shadow installation).

int32_t bldg_placement_check_and_preview(int32_t origin_x, int32_t origin_y, int32_t building_index);
void    bldg_calc_placement_corner_from_center(uint16_t unit_index, int32_t center_x, int32_t center_y,
                                               uint32_t *out_col, uint32_t *out_row);

namespace detail {
} // namespace detail

} // namespace mh::sim
