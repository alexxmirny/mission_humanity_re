//
// lockstep/lt_peer_timing_reset.cpp -- see lt_peer_timing_reset.h. Translated from the DISASSEMBLY
// (tmp/decomp_lib_trans/llm_net_lockstep_peer_timing_reset_0049fef7.asm), NOT from the exported .c
// draft, whose field names follow the stale Ghidra record layout (see the header).
//
#include "lockstep/lt_peer_timing_reset.h"


namespace mh::lockstep {
namespace detail {

// ---- llm_net_lockstep_peer_timing_reset @0x0049fef7 ------------------------------------------------
//
// 8 iterations (`CMP dword ptr [EBP-0x18],0x8` @0x0049ff16 -- MAX_PLAYERS). Per entry i (base
// 0x00e58c40, stride 0xc, corrected record {int32_t order_marker; double horizon}):
//   horizon      = HORIZON_NONE  -- two dword stores, low @0x0049ff2a then high @0x0049ff34,
//                                   0/0xbff00000 = -1.0 bit-for-bit.
//   order_marker = 0             -- @0x0049ff42.
// The original recomputes `i*0xc` twice (0x0049ff26 and again at 0x0049ff3e) and stores in the order
// horizon-low, horizon-high, order_marker; neither the recomputation nor the store order is
// observable here (both fields start and end at the same values regardless), so this is written as
// the natural two-field assignment rather than transcribed instruction-for-instruction. There is
// also a dead `MOV EAX,[EBP-0x18]` immediately before the loop's `INC` (@0x0049ff1e) -- the
// decompiler correctly drops it; noted so a reviewer does not go looking for a missing statement.
void peer_timing_reset(const engine_state &st) {
    for (int32_t i = 0; i < MAX_PLAYERS; ++i) {
        st.peer[i].horizon      = HORIZON_NONE; // 0x0049ff2a / 0x0049ff34
        st.peer[i].order_marker = 0;            // 0x0049ff42
    }
}

} // namespace detail

void peer_timing_reset() {
    detail::peer_timing_reset(state());
}

} // namespace mh::lockstep
