//
// sim/sim_planet_distance.cpp -- see sim_planet_distance.h. Translated from the DISASSEMBLY
// (tmp/decomp/llm_strat_planet_distance_00493fb2.asm) -- the Ghidra .c draft's parameter order
// happens to already read x1=EAX/y1=EDX/x2=EBX(unaff_EBX)/y2=ECX correctly (verified against the
// .asm's own param banner per the batch context's caveat), and its save/force/restore/sqrt control
// flow matches the assembly exactly; only the out-pointer identification (see the header's push-order
// derivation) was independently re-traced rather than trusted from the draft's local variable names,
// since Ghidra's stack-slot numbering does not correspond 1:1 to the raw frame offsets.
//
#include "sim/sim_planet_distance.h"

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"        // ai_say / trace_budget -- the shared trace sink, not AI state
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"
#include "crt/crt_select.h" // LIB-CRT: MH_CRT() picks the vendored CRT in the standalone build

namespace mh::sim {

const planet_distance_calls &live_planet_distance_calls() {
    static const planet_distance_calls c = {
        MH_LIBMH_BIND(llm_strat_tile_delta_wrapped),
        MH_CRT(llm_sqrt),
    };
    return c;
}

namespace detail {

double planet_distance(const sim_view &v, sim_store &own, const planet_distance_calls &c, int32_t x1,
                       int32_t y1, int32_t x2, int32_t y2) {
    (void)v; // no sim_view member is read; only map_width/map_height (via sim_store) are touched.

    int32_t      &w       = own.map_width_mut();
    int32_t      &h       = own.map_height_mut();
    const int32_t saved_w = w;
    const int32_t saved_h = h;

    // 0x00493fe3-0x00493ff0: force both to 100000 so the wrapped-delta helper below does NOT
    // toroidally wrap -- planets sit on a non-wrapping starfield, not the tile map.
    w = 100000;
    h = 100000;

    // 0x00493ff7-0x0049400b: out_dx == &dx, out_dy == &dy (see the header's push-order derivation).
    int32_t dx = 0;
    int32_t dy = 0;
    c.tile_delta_wrapped(x1, y1, x2, y2, &dx, &dy);

    // 0x00494010-0x0049401b: restore the saved originals BEFORE computing the distance -- matches the
    // assembly's own instruction order (restore, then sqrt), not the reverse.
    w = saved_w;
    h = saved_h;

    // 0x00494020-0x0049403c: dx*dx + dy*dy, widened to double, sqrt.
    return c.sqrt_fn((double)(dx * dx + dy * dy));
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

double planet_distance(int32_t x1, int32_t y1, int32_t x2, int32_t y2) {
    sim_state st = state();
    return detail::planet_distance(st.read, st.own, live_planet_distance_calls(),
                                   static_cast<int32_t>(x1), static_cast<int32_t>(y1),
                                   static_cast<int32_t>(x2), static_cast<int32_t>(y2));
}


} // namespace mh::sim
