#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// ---- llm_strat_load_base_layout_dmp's callees -----------------------------------------------------
struct load_base_layout_dmp_calls {
    // SIMABI-VFS (2026-09-10): the resource arrives as a COPY into a buffer WE own, and the two
    // members below are ours to allocate and release. It used to arrive as the HOST's pointer,
    // which this body then freed through the vendored CRT -- a cross-heap free that happens to work
    // only because mh.dll and the game share one heap, and one of exactly two such sites in libmh.
    // `asset_read(name, null, 0)` is the size query (it loads nothing); the second call fills the
    // buffer and returns the asset's full length again.
    int32_t (*asset_read)(const char *name, void *dst, uint32_t dst_cap);
    void *(*mem_alloc)(uint32_t size);
    void (*mem_free)(void *ptr);
    int32_t (*bldg_queue_construction)(int32_t player, int32_t building_type, int16_t x, uint16_t y);
    uint32_t (*toroidal_dist_sq)(int32_t x1, int32_t y1, int32_t x2, int32_t y2);
    uint32_t (*dmp_enqueue_scripted_order)(uint32_t param_1, uint32_t param_2, uint32_t param_3,
                                           uint16_t param_4);
};

const load_base_layout_dmp_calls &live_load_base_layout_dmp_calls();

namespace detail {

// llm_strat_load_base_layout_dmp @0x004d8839. See the header banner above for the full per-section
// derivation; the .cpp carries the per-branch address citation.
void load_base_layout_dmp(const sim_view &v, sim_store &own, int32_t player, char *dmp_path,
                          const load_base_layout_dmp_calls &c);

} // namespace detail

// Live wrapper: the logic applied to state() (and live_load_base_layout_dmp_calls()). Matches the
// committed prototype (sig_llm_strat_load_base_layout_dmp) exactly.
void load_base_layout_dmp(int32_t player, char *dmp_path);

namespace detail {
} // namespace detail

} // namespace mh::sim
