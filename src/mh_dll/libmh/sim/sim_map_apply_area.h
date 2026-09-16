#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// The callees this closure reaches, indirected for offline testability like every other sibling
// `_calls` struct in this subsystem.
//
// TWO are genuinely-original (untranslated) callees -- region_free / get_next_block -- and mirror the
// ORIGINAL functions' committed prototypes (addr/mh_calls.gen.h); their bindings resolve to mh::call::.
//
// THREE are this slice's own TRANSLATED siblings -- region_split / region_pick_smaller /
// merge_small_regions -- whose bindings resolve to the siblings' PUBLIC wrappers (&mh::sim::region_split
// etc., signatures matching sim_map_region_split.h / sim_map_region_helpers.h). Routing them through this
// struct is BEHAVIOR-IDENTICAL in production (the pointer points at the same public wrapper a direct
// mh::sim::X(...) call would reach, which still resolves its own state() and runs its own body) -- it only
// adds the stub seam net_selftest.exe needs, since a direct mh::sim::X(...) inside detail:: resolves
// state() to a real stock VA that is unmapped offline and crashes before any assertion. This is the SAME
// pattern invasion_calls uses to route the translated presence_lost/spawn_enemy_landing siblings.
struct map_apply_area_calls {
    // llm_map_region_free @0x0042239a, __watcall(EAX=region). Reached only from apply_area_to_map's
    // rescan pass, when a split-off remainder's cell_count drops to zero. ORIGINAL/untranslated.
    void (*region_free)(uint32_t region);
    // map::block_8::GetNextBlock @0x004222cf, __watcall, no params. Reached only from region_apply_
    // area's retry-with-allocation pass (see the state-machine banner above). ORIGINAL/untranslated.
    mh::game::mh_llm_map_region *(*get_next_block)(); // typed by the committed prototype (was void*; synced 2026-08-19)
    // llm_map_region_split @0x00423fb8 -- TRANSLATED sibling (sim_map_region_split.h public wrapper).
    // Reached only from apply_area_to_map's rescan pass, once per touched region cell.
    llm_map_region *(*region_split)(llm_map_region *old_region, int32_t seed_col, int32_t seed_row);
    // llm_map_region_pick_smaller @0x004229c3 -- TRANSLATED sibling (sim_map_region_helpers.h public
    // wrapper, typed in and out to match sig_llm_map_region_pick_smaller). Reached only from
    // region_apply_area, chained four times per newly-masked cell (one per orthogonal neighbor).
    llm_map_region *(*region_pick_smaller)(llm_map_region *block, int32_t x, int32_t y);
    // llm_map_merge_small_regions @0x004230f7 -- TRANSLATED sibling (sim_map_region_helpers.h public
    // wrapper). Reached from BOTH functions' tail, to clean up regions now under the merge threshold.
    void (*merge_small_regions)();
};

const map_apply_area_calls &live_map_apply_area_calls();

namespace detail {

// map_ApplyAreaToMap @0x00424406. See the header banner's .prev-reuse derivation.
void apply_area_to_map(const sim_view &v, sim_store &own, const map_apply_area_calls &c, int32_t x_0,
                       int32_t y_0, const uint8_t *area);

// llm_map_region_apply_area @0x004245c3. See the header banner's state-machine derivation.
void region_apply_area(const sim_view &v, sim_store &own, const map_apply_area_calls &c,
                       uint32_t tile_col_origin, int32_t tile_row_origin, const char *area_mask);

} // namespace detail

// Live wrappers: the logic applied to state() and live_map_apply_area_calls(). Parameter types match
// addr/mh_calls.gen.h's own out-call wrappers for these two addresses (`area` committed as uint8_t *,
// TACT1-P C6, 2026-09-04).
void apply_area_to_map(int32_t x_0, int32_t y_0, uint8_t *area);
void region_apply_area(uint32_t tile_col_origin, int32_t tile_row_origin, char *area_mask);

namespace detail {
} // namespace detail

} // namespace mh::sim
