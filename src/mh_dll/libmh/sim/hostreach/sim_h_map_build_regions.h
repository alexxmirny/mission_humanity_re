#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// The callees both functions in this TU reach, indirected for offline testability like every other
// sibling `_calls` struct in this subsystem (translator-brief rule 3b).
//
// THREE are this subsystem's own TRANSLATED siblings -- their bindings resolve to the siblings' PUBLIC
// wrappers (behavior-identical to calling them directly; the indirection only buys the stub seam
// net_selftest.exe simtest needs, per sim_map_apply_area.h's own "CALL ROUTING" banner).
// FIVE are Law-4 FRONTIER originals, bound as `&mh::call::<name>` (conductor correction, see
// above); the struct indirection is still what lets net_selftest.exe substitute a test double
// rather than faulting on an unmapped game VA -- that property comes from the struct, not from the
// binder macro.
struct map_build_regions_calls {
    // -- Law-4 FRONTIER originals (`mh::call::`) -- CONDUCTOR CORRECTION 2026-09-10: the batch
    // context listed these five as owned rows to bind through MH_LIBMH_BIND; none is a
    // migration-ledger row in any domain, so they stay original and route through mh::call::.
    // See the .cpp's binding block for why the macro could never have existed for them. --
    void (*init_region_route_step_deltas)();    // llm_map_init_region_route_step_deltas @0x00423299
    void (*compute_obstacle_proximity_flags)(); // llm_map_compute_obstacle_proximity_flags @0x0042207b
    void (*seed_regions_multires)();            // llm_map_seed_regions_multires @0x00422c54
    void (*compute_region_merge_threshold)();   // llm_map_compute_region_merge_threshold @0x0042308b
    // llm_map_region_flood_fill @0x00422450, __mh_watcall_ebx_volatile(EAX=x, EDX=y, EBX=block),
    // returns the newly-flood-filled region's cell count.
    int32_t (*region_flood_fill)(uint32_t x, uint32_t y, llm_map_region *block);

    // -- translated siblings (public wrappers, see the header banner's CALL ROUTING correction) --
    // llm_map_merge_small_regions @0x004230f7 -- sim_map_region_helpers.h public wrapper.
    void (*merge_small_regions)();
    // map::block_8::GetNextBlock @0x004222cf -- sim/libtrans/sim_lt_map_region_pool.h public wrapper
    // (the real symbol is `get_next_block`, not `map_block_8_GetNextBlock`).
    llm_map_region *(*get_next_block)();
    // llm_map_region_pick_smaller @0x004229c3 -- sim_map_region_helpers.h public wrapper.
    llm_map_region *(*region_pick_smaller)(llm_map_region *block, int32_t x, int32_t y);
};

const map_build_regions_calls &live_map_build_regions_calls();

namespace detail {

// llm_map_build_regions @0x00423335. See the header banner (SENTINEL, TWO DIFFERENT LEFTOVER
// FALLBACKS, WIDTH_M/HEIGHT_M second writer).
void build_regions(const sim_view &v, sim_store &own, const map_build_regions_calls &c);

// llm_map_assign_remaining_tiles_to_regions @0x00422a42. See the header banner (SENTINEL, THE FIXPOINT
// COUNTER).
void assign_remaining_tiles_to_regions(const sim_view &v, sim_store &own, const map_build_regions_calls &c);

} // namespace detail

// Live wrappers: the logic applied to state() and live_map_build_regions_calls(). Both take no
// parameters and return void per the committed prototypes (the .asm headers).
void build_regions();
void assign_remaining_tiles_to_regions();

} // namespace mh::sim
