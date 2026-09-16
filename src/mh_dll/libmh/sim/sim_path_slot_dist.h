#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// ---- (1) llm_strat_claim_free_slots_within_dist's outward calls -----------------------------------
struct claim_free_slots_within_dist_calls {
    // llm_strat_dir_step_toroidal_dist @0x004cbc9b -- toroidal tile distance between two group-move
    // scratch slot indices (a_index, b_index).
    int32_t (*dir_step_toroidal_dist)(int32_t a_index, int32_t b_index);
};

const claim_free_slots_within_dist_calls &live_claim_free_slots_within_dist_calls();

// ---- (2) llm_strat_slot_dist_to_ref's outward calls --------------------------------------------------
struct slot_dist_to_ref_calls {
    double (*sqrt_fn)(double x); // llm_sqrt @0x004da9c0
};

const slot_dist_to_ref_calls &live_slot_dist_to_ref_calls();

namespace detail {

// llm_strat_claim_free_slots_within_dist @0x004cbc28. See the header banner for the full derivation.
// Writes `.wave_rank` on every unassigned, in-range slot -- no early exit.
void claim_free_slots_within_dist(sim_store &own, const claim_free_slots_within_dist_calls &c,
                                  int32_t record_count, int32_t max_dist, int32_t claim_value,
                                  int32_t origin_index);

// llm_strat_slot_dist_to_ref @0x004cbd63. See the header banner for the full derivation. Pure query;
// `own` is non-const only because the four dist-ref/half-extent scratch globals have no const
// accessor (see banner).
int32_t slot_dist_to_ref(const sim_view &v, sim_store &own, const slot_dist_to_ref_calls &c,
                         int32_t slot_index);

} // namespace detail

// Live wrappers: the logic applied to state() and the live_*_calls() above. Match the committed
// prototypes (sig_llm_strat_claim_free_slots_within_dist / sig_llm_strat_slot_dist_to_ref) exactly.
void    claim_free_slots_within_dist(int32_t record_count, int32_t max_dist, int32_t claim_value,
                                     int32_t origin_index);
int32_t slot_dist_to_ref(int32_t slot_index);

namespace detail {
} // namespace detail

} // namespace mh::sim
