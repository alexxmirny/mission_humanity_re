#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// ---- the outward calls -------------------------------------------------------------------------
//
// Indirected for offline testability (net_selftest.exe simtest), same reasoning as every other sim
// `calls` table. Every member is an ORIGINAL game function (or, for the two qsort comparators, a raw
// game CODE ADDRESS handed to the game's own qsort as DATA) -- none is reimplemented here. The two
// comparator addresses are `void *`, not function pointers, matching ai_state.h's
// `scan_target_sort_cmp` precedent for the identical "address handed to qsort, never called by name"
// shape.
struct pathfind_route_leg_group_and_sort_calls {
    // struct_array_malloc_impl @0x004d013d -- (count, struct_size) -> heap buffer or nullptr.
    void *(*struct_array_malloc_impl)(uint32_t count, uint32_t struct_size);
    // utils_free @0x004d0244.
    void (*utils_free)(void *ptr);
    // The game's own qsort @0x004de8a6 -- see the header banner; never std::sort.
    void (*qsort)(void *base, uint32_t num, uint32_t width, void *compare);
    // llm_strat_slot_dist_to_ref @0x004cbd63 (frontier) -- distance from leg `slot_index` to the
    // ref point stashed by this function's own step 2.
    int32_t (*slot_dist_to_ref)(int32_t slot_index);
    // llm_strat_claim_free_slots_within_dist @0x004cbc28 (frontier) -- stamps `claim_value` onto
    // every still-unassigned (wave_rank == -1) leg within `max_dist` of `origin_index`.
    void (*claim_free_slots_within_dist)(int32_t record_count, int32_t max_dist, int32_t claim_value,
                                         int32_t origin_index);
    // llm_strat_pathfind_route_leg_reconcile @0x00420616 -- this function's own sibling/hand-off,
    // called as the ORIGINAL (see the header banner's shadow-isolation note).
    int32_t (*pathfind_route_leg_reconcile)(int32_t step_count);
    // mh::addr::group_move_scratch_cmp_dist_004cbe0c, as DATA (`int(int*,int*){return *a-*b;}`),
    // never called directly here -- only ever handed to `qsort` above as its comparator.
    void *cmp_dist;
    // mh::addr::group_move_scratch_cmp_wave_rank_004cbe42, as DATA (compares .wave_rank, offset 8 of
    // llm_strat_group_scratch_member) -- same "handed to qsort, never called directly" posture.
    void *cmp_wave_rank;
};

const pathfind_route_leg_group_and_sort_calls &live_pathfind_route_leg_group_and_sort_calls();

// The logic over an EXPLICIT state, so `net_selftest.exe simtest` can drive it over heap buffers with
// no game and no rig.
namespace detail {

// llm_strat_pathfind_route_leg_group_and_sort @0x004cba1e. See the header banner for the full shape;
// the .cpp carries the per-instruction derivation.
int32_t pathfind_route_leg_group_and_sort(const sim_view &v, sim_store &own,
                                          const pathfind_route_leg_group_and_sort_calls &c,
                                          uint32_t leg_count, int32_t ref_x, int32_t ref_y);

// The two qsort comparators, translated (LIB-CRT 2026-09-08). Both were raw VA literals passed to
// qsort as DATA -- FUN_004cbe0c and FUN_004cbe42, real Ghidra function boundaries that had been
// deliberately left untranslated on the reasoning that the original sort and the original comparator
// must stay paired. That reasoning still holds and is why these are selected by MH_CRT_CMP
// (crt/crt_select.h) rather than used unconditionally: the hosted build passes the VAs to the game's
// qsort, the standalone build passes these to crt/crt_qsort.h's transcription of it, and the two
// halves never cross.
//
// NEITHER RETURNS -1/0/+1 -- both return a wrapping 32-bit DIFFERENCE, and crt_qsort.h only ever
// tests the sign, so that is faithful and not a latent bug.
int32_t group_move_scratch_cmp_dist(void *a, void *b);      // @0x004cbe0c  *(int32*)a - *(int32*)b
int32_t group_move_scratch_cmp_wave_rank(void *a, void *b); // @0x004cbe42  the .wave_rank field

} // namespace detail

// Public wrapper. Signature matches the committed call/export signature
// (sig_llm_strat_pathfind_route_leg_group_and_sort) exactly.
int32_t pathfind_route_leg_group_and_sort(uint32_t leg_count, int32_t ref_x, int32_t ref_y);

namespace detail {
} // namespace detail

} // namespace mh::sim
