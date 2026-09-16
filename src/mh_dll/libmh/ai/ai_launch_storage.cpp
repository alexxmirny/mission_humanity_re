//
// ai/ai_launch_storage.cpp -- see ai_launch_storage.h. Translated from the DISASSEMBLY
// (tmp/decomp_ai/llm_strat_ai_unit_launch_from_storage_enqueue_004d58e7.asm), not from Ghidra's C.
//
// NO ABSOLUTE APPEARS IN THIS BODY AT ALL: the function touches no state and its single outward edge
// is a tail JMP, which reaches the callee through the generated call layer
// (ai_calls::unit_order_auto_launch_from_storage_enqueue -> 0x0046cf2c). assert_stack_capacity is
// the Watcom stack probe and has no observable effect (zero cells in the state matrix), so it is not
// reproduced -- the same treatment every other translated module gives it.
//
#include "ai/ai_launch_storage.h"


namespace mh::ai {
namespace detail {

uint32_t launch_player_nibble(uint8_t player) {
    // XOR AH,AH / AND AL,0xf / MOVZX EAX,AX @0x004d58f1-0x004d58f5. The zeroing of AH and the MOVZX
    // together clear everything above bit 7 that the caller happened to leave in EAX; the AND leaves
    // only bits 0..3. Taking a uint8_t argument already models the AH/upper-half clear, so `& 0xf`
    // is the whole of it.
    return (uint32_t)(player & 0x0fu);
}

void unit_launch_from_storage_enqueue(const ai_calls &gc, uint8_t player, int32_t unit_id,
                                      uint32_t target_x, uint32_t target_y) {
    // JMP 0x0046cf2c @0x004d58f8 -- a tail call, so EDX/EBX/ECX arrive at the callee exactly as the
    // caller left them and the callee's (void) return is this function's.
    gc.unit_order_auto_launch_from_storage_enqueue(launch_player_nibble(player), unit_id, target_x,
                                                   target_y);
}

} // namespace detail

void unit_launch_from_storage_enqueue(uint8_t player, int32_t unit_id, uint32_t target_x,
                                      uint32_t target_y) {
    detail::unit_launch_from_storage_enqueue(live_calls(), player, unit_id, target_x, target_y);
}


} // namespace mh::ai
