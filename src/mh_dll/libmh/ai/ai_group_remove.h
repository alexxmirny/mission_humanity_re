#pragma once
#include <cstdint>

#include "ai/ai_state.h"

namespace mh::ai {

namespace detail {

void group_remove(const ai_view &v, const ai_store &own, const ai_calls &gc, int32_t player,
                  uint32_t group_index);

} // namespace detail

// Public wrapper. Signature matches the committed prototype in addr/mh_calls.gen.h exactly.
void group_remove(int32_t player, uint32_t group_index);

} // namespace mh::ai
