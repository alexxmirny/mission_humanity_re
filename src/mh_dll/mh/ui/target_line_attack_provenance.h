// Read-only, process-local provenance for explicit group-attack admissions.
#pragma once
#include <cstdint>

struct libmh_order_observation;
namespace mh::game {
struct mh_llm_strat_order;
}
namespace mh::order_witness {
struct canonical_order;
}

namespace mh::ui::selected_targets::detail {

struct attack_assignment {
    uint64_t token              = 0;
    uint32_t actor_incarnation  = 0;
    uint32_t target_incarnation = 0;
    uint32_t entity             = 0;
    bool     secondary          = false;
};

// Allocate once, before installing admission/acceptance observers.
bool reserve_attack_provenance();

// Required on every world replacement, replay replacement and roster reseed.
void reset_attack_provenance();

// Only the actual group-attack invocation brackets these calls. Membership is
// established by successful admissions inside the scope, never by the selection.
void begin_attack_group(unsigned target_slot, unsigned target_player,
                        uint16_t selector);
void end_attack_group();

// Native-only POST-success adapter; reads the still-current live container.
void admit_attack_order(unsigned container, unsigned slot, bool unknown_origin);
// Owned, canonical command value. Transfers are not fresh origins.
void admit_attack_order(const libmh_order_observation &observation);
// POST-success metadata amendment. Never admits a new origin or changes game state.
void amend_attack_metadata(const mh::order_witness::canonical_order &before,
                           const mh::order_witness::canonical_order &after);
// Both shapes use the same exact-key cache; native QUEUE headers are already decoded.
attack_assignment attack_assignment_for(const mh::game::mh_llm_strat_order &record);
attack_assignment attack_assignment_for(const libmh_order_observation &observation);

} // namespace mh::ui::selected_targets::detail
