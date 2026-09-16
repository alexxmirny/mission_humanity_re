#pragma once
#include <cstdint>

#include "sim/sim_diplomacy_set_relation.h" // sibling _calls struct (rule 3c): detail::diplomacy_set_relation
#include "sim/sim_state.h"

namespace mh::sim {

namespace detail {

// llm_diplomacy_restore_relations @0x00465c6e. Reads profiles[i].status_flags (v, the enabled-slot
// gate) and Players[i].relation[j] (own.player_relation_at, read-only use here -- own is non-const
// only because the accessor's return type is `uint8_t&`), reaches llm_diplomacy_set_relation through
// the threaded sibling calls struct. No outward call of its own.
void diplomacy_restore_relations(
    const sim_view &v, sim_store &own,
    const diplomacy_set_relation_calls &c_set_relation = live_diplomacy_set_relation_calls());

} // namespace detail

// Live wrapper: the logic applied to state(). Matches the original's committed __watcall(void) shape
// (void __watcall llm_diplomacy_restore_relations(void)) -- the promotion seam this batch's conductor
// adapter forwards to.
void diplomacy_restore_relations();

} // namespace mh::sim
