#pragma once
#include <cstdint>

#include "sim/sim_state.h"
#include "sim/hostreach/sim_h_map_build_regions.h"     // map_build_regions_calls, live_map_build_regions_calls(), detail::assign_remaining_tiles_to_regions -- wave-1 sibling
#include "sim/hostreach/sim_h_map_region_flood_fill.h" // detail::region_flood_fill -- INTRA-WAVE sibling; see the ASSUMED SIGNATURE note above

namespace mh::sim {

// The one callee try_seed_region_at reaches that isn't the intra-wave flood_fill edge (which is
// assumed to carry no `_calls` struct of its own -- see the header banner's ASSUMED SIGNATURE note).
struct map_region_seed_calls {
    // map_block_8_GetNextBlock @0x004222cf -- translated sibling, sim/libtrans/sim_lt_map_region_pool.h
    // public wrapper.
    llm_map_region *(*get_next_block)();
};

const map_region_seed_calls &live_map_region_seed_calls();

namespace detail {

// llm_map_try_seed_region_at @0x004228e9, `__mh_watcall_ebx_volatile(EAX=x, EDX=y, EBX=max_d)`. See
// the header banner (THE SCAN GATE, THE BYTE TRUNCATION, the "DOES NOT INSTALL THE SEED CELL" note).
// The intra-wave flood_fill call has NO `_calls` struct to thread (confirmed against the landed
// sibling), so nothing is threaded here.
void try_seed_region_at(const sim_view &v, sim_store &own, const map_region_seed_calls &c, int32_t x,
                        int32_t y, int32_t max_d);

// llm_map_seed_regions_multires @0x00422c54. See the header banner (THE MULTI-RESOLUTION SEQUENCE, THE
// FOUR-SCALAR RESET). `c_build` is the wave-1 sibling's calls struct, threaded per rule 3c for the
// trailing fixpoint-sweep call.
void seed_regions_multires(const sim_view &v, sim_store &own, const map_region_seed_calls &c,
                           const map_build_regions_calls &c_build = live_map_build_regions_calls());

} // namespace detail

// Live wrappers: the logic applied to state() and live_map_region_seed_calls(). Both signatures match
// the committed prototypes verbatim (the .asm headers) -- try_seed_region_at forwards its three
// parameters POSITIONALLY into the generated `__mh_watcall_ebx_volatile` thunk, which owns the
// register contract; this wrapper does not second-guess it.
void try_seed_region_at(int32_t x, int32_t y, int32_t max_d);
void seed_regions_multires();

} // namespace mh::sim
