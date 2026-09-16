
#pragma once
#include <cstdint>

#include "ai/ai_state.h"

namespace mh::ai {
namespace detail {

// llm_strat_ai_commit_attack_order @0x004d55ef. See the header banner for the full derivation.
void commit_attack_order(const ai_view &v, const ai_store &own, const ai_calls &gc,
                         uint32_t attacker_ref, int32_t attacker_unit_index, uint32_t target_ref,
                         int32_t target_index);

// llm_strat_ai_commit_attack_order_alt @0x004d5708. Identical to the above except for the unit
// branch's enqueue callee (order_attack_target_alt_enqueue instead of
// order_attack_target_enqueue) -- see the header banner's "SHARED TAIL" section for how it reaches
// the primary function's tail.
void commit_attack_order_alt(const ai_view &v, const ai_store &own, const ai_calls &gc,
                             uint32_t attacker_ref, int32_t attacker_unit_index,
                             uint32_t target_ref, int32_t target_index);

} // namespace detail

// Public wrappers. Signatures match the committed prototypes (addr/mh_export.gen.h::
// sig_llm_strat_ai_commit_attack_order[_alt], addr/mh_calls.gen.h) exactly in type, with the
// semantic parameter names this header derives.
void commit_attack_order(uint32_t attacker_ref, int32_t attacker_unit_index, uint32_t target_ref,
                         int32_t target_index);
void commit_attack_order_alt(uint32_t attacker_ref, int32_t attacker_unit_index,
                             uint32_t target_ref, int32_t target_index);

} // namespace mh::ai
