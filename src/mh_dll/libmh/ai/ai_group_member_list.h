#pragma once
#include <cstdint>

#include "ai/ai_state.h"

namespace mh::ai {
namespace detail {

// llm_strat_ai_group_member_link @0x004d4a2e. See the header banner for the full per-instruction
// derivation. `v` and `gc` are unused (no reads through the view, no callees) but taken to match
// this domain's `(v, own, gc, ...)` calling shape.
void group_member_link(const ai_view &v, const ai_store &own, const ai_calls &gc, uint32_t player,
                       int32_t ai_group_index, uint32_t unit_id);

// llm_strat_ai_group_member_unlink @0x004d4919. See the header banner for the full per-instruction
// derivation, including the parameter-order recovery from the committed generic names. `v` and `gc`
// are unused for the same reason as above.
void group_member_unlink(const ai_view &v, const ai_store &own, const ai_calls &gc, uint32_t player,
                         int32_t ai_group_index, uint32_t unit_id);

} // namespace detail

// Public wrappers. Signatures match the committed prototypes in addr/mh_calls.gen.h exactly. No
// shadow wrapper -- see the PROOF PATH note above.
void group_member_link(uint32_t player, int32_t ai_group_index, uint32_t unit_id);
void group_member_unlink(uint32_t player, int32_t ai_group_index, uint32_t unit_id);

} // namespace mh::ai
