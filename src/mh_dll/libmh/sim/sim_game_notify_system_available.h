#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// The one external callee this closure reaches (w_sprintf's measured `vss` shape, @0x004d0320),
// indirected for offline testability like every other sim/ TU's `_calls` struct. Signature copied
// verbatim from addr/mh_calls.gen.h's `w_sprintf__vss`.
struct game_notify_system_available_calls {
    int32_t (*w_sprintf__vss)(void *dst, const wchar_t *format, const wchar_t *a0, const wchar_t *a1);
};

const game_notify_system_available_calls &live_game_notify_system_available_calls();

namespace detail {

// llm_game_notify_system_available @0x004401a6. See the header notes above for the full derivation.
// `own` (not const sim_view alone) because the one outward call writes through
// `sim_store::text_scratch()`, the shared presentation-scratch buffer.
void game_notify_system_available(const sim_view &v, sim_store &own,
                                  const game_notify_system_available_calls &c, uint16_t player,
                                  int32_t system_idx);

} // namespace detail

// Live wrapper: the logic applied to state() and live_game_notify_system_available_calls(). Matches
// the committed prototype (sig_llm_game_notify_system_available) exactly.
void game_notify_system_available(uint16_t player, int32_t system_idx);

namespace detail {
} // namespace detail

} // namespace mh::sim
