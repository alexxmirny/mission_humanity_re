//
// sim/libtrans/sim_lt_map_setup_dimensions.cpp -- see sim_lt_map_setup_dimensions.h. Translated
// from the DISASSEMBLY (tmp/decomp_lib_trans/llm_map_setup_dimensions_004989c2.asm), not from
// Ghidra's C draft beside it (the draft reads `width`/`height` as bare globals and renders the
// bw_mask/bh_mask lines as if they reused the just-computed big_width/big_height, eliding the .asm's
// re-read; it also renders the pathfinder byte as a plain `(char)width - 1`, eliding the 8-bit read
// the DL-register form performs -- both load-bearing per the header banner).
//
#include "sim/libtrans/sim_lt_map_setup_dimensions.h"

#include "addr/mh_calls.gen.h" // mh::state::evt::view_minimap_zoom_for_size -- the tail-call target
#include "ai/ai_state.h"       // ai_say / trace_budget -- the shared trace sink, not AI state
#include "state/host_api.h"
#include "state/host_events.h"

namespace mh::sim {

const lt_map_setup_dimensions_calls &live_lt_map_setup_dimensions_calls() {
    static const lt_map_setup_dimensions_calls c = {
        mh::state::evt::view_minimap_zoom_for_size,
    };
    return c;
}

namespace detail {

void map_setup_dimensions(const sim_view &v, sim_store &own, const lt_map_setup_dimensions_calls &c) {
    // 0x004989da-0x004989e0: general.width_mask = width - 1. First of three separate reads of
    // `width` in this body (translator-brief rule 16 -- sim_view is a live read, never cached).
    own.width_mask_mut() = static_cast<uint32_t>(*v.map_width - 1);

    // 0x004989e5-0x004989eb: general.height_mask = height - 1. First of two separate reads of
    // `height`.
    own.height_mask_mut() = static_cast<uint32_t>(*v.map_height - 1);

    // 0x004989f0-0x004989f8: general.big_width = width << 5 (tiles*32 = pixels). SECOND read of
    // `width`, independent of the one already used for width_mask above.
    own.big_width_mut() = static_cast<uint32_t>(*v.map_width) << 5;

    // 0x004989fd-0x00498a05: general.big_height = height << 5. SECOND read of `height`.
    own.big_height_mut() = static_cast<uint32_t>(*v.map_height) << 5;

    // 0x00498a0a-0x00498a10: general.bw_mask = general.big_width - 1 -- the .asm RE-READS big_width
    // back out of memory (MOV EAX,[0x00e153a0]) rather than reusing the register that just computed
    // it two lines above. Calling big_width_mut() again reproduces that: it is a fresh dereference of
    // the same live field, not a cached local (translator-brief W2/16 -- never hoist, never cache).
    own.bw_mask_mut() = own.big_width_mut() - 1u;

    // 0x00498a15-0x00498a1b: general.bh_mask = general.big_height - 1, same re-read discipline.
    own.bh_mask_mut() = own.big_height_mut() - 1u;

    // 0x00498a20-0x00498a2d: general.pathfinder_params->width_mask -- an 8-bit read of width's low
    // byte (MOV DL, byte ptr [width]) followed by an 8-bit DEC, not a 32-bit `width - 1` truncated
    // afterward (they agree on every value; the instruction is the spec, translator-brief rule 7).
    // THIRD and last read of `width`. The pointer is dereferenced unconditionally, matching the
    // original's lack of a null check (translator-brief rule 11 -- preserve what the original does
    // not guard; no guard is added here).
    const uint8_t width_byte                = static_cast<uint8_t>(*v.map_width);
    own.pathfinder_params_mut()->width_mask = static_cast<uint8_t>(width_byte - 1);

    // 0x00498a30: tail-call, routed through this unit's _calls struct (translator-brief rule 3b) so
    // net_selftest.exe libtranstest can stub it. Classified `state` (verified) -- arming is not
    // gated on it (see the header's TAIL CALL banner).
    c.set_minimap_zoom_for_size();
}

} // namespace detail

// ---- the public wrapper ------------------------------------------------------------------------

void map_setup_dimensions() {
    sim_state st = state();
    detail::map_setup_dimensions(st.read, st.own, live_lt_map_setup_dimensions_calls());
}


} // namespace mh::sim
