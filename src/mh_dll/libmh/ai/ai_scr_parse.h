#pragma once
#include <cstdint>

#include "ai/ai_state.h"

namespace mh::ai {

// The logic over an EXPLICIT state, so net_selftest.exe aitest can drive it over heap buffers with
// no game and no rig. There is no `ai_store` parameter here -- unlike every other translated AI
// function, this one never writes through the AI's own store; every write it makes goes through a
// pointer the keyword table itself supplies, which is why the write side doesn't appear in the
// signature at all.
namespace detail {

void scr_parse(const ai_view &v, const ai_calls &gc, char *filename);

} // namespace detail

void scr_parse(char *filename);

} // namespace mh::ai
