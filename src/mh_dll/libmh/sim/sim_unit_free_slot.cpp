#include "sim/sim_unit_free_slot.h"

#include "ai/ai_state.h" // ai_say / trace_budget -- the shared trace sink, not AI state

namespace mh::sim {

namespace {

// `unit_above` is `uint8_t[2]` (`map_t_unit_full_id`) in the generated header -- the byte<->word pair
// every sim/ TU touching this field defines locally (per-TU duplication is the established convention;
// see e.g. sim_unit_put_on_map.cpp's original definition). Read half matches
// sim_unit_state_move_walker.cpp's `unit_full_id_word`; write half matches
// sim_unit_put_on_map.cpp's `store_unit_full_id_word`.
inline uint16_t unit_full_id_word(const uint8_t (&packed)[2]) {
    return static_cast<uint16_t>(packed[0] | (packed[1] << 8));
}
inline void store_unit_full_id_word(uint8_t (&packed)[2], uint16_t value) {
    packed[0] = static_cast<uint8_t>(value & 0xffu);
    packed[1] = static_cast<uint8_t>((value >> 8) & 0xffu);
}

// The two header-row (`units[player][0]`) DECs at 0x00487b84/0x00487b95 are bare `DEC word ptr` (not
// `SUB ..., <imm>`), i.e. literal value 1 -- kept as their own function-local constants rather than
// folded into sim_order_enqueue.h's `UNIT_STATE_STOP_TO_DEFAULT`, per every sibling TU's own established
// caution that the numeric coincidence (both are 1) is not proof of shared enum semantics. Two separate
// constants because these are two DIFFERENT header-row counters (`unit_above`, `order`).
inline constexpr uint16_t kUnitZeroAboveHeaderDecrement = 1; // 0x00487b84
inline constexpr uint16_t kUnitZeroOrderHeaderDecrement = 1; // 0x00487b95

} // namespace

namespace detail {

void unit_free_slot(sim_store &own, uint32_t player, int32_t slot) {
    // `player` is truncated to its low 16 bits at all four uses in the original (`MOVZX EAX, word ptr
    // [...]`), hoisted once here matching every sibling TU's single-truncation precedent. `slot` is used
    // FULL 32-bit (no truncation) in the per-unit-record term, and is never bounds-checked -- reproduced
    // literally.
    const uint32_t player16 = player & 0xffffu;

    unit &u = own.unit_at(player16, slot);

    // 0x00487b55: units[player][slot].unit_proto_id = 0 -- marks the slot free.
    u.unit_proto_id = 0;

    // 0x00487b71: units[player][slot].unit_above = {0,0} -- clears this slot's own soldier-chain head.
    store_unit_full_id_word(u.unit_above, 0);

    unit &header = own.unit_at(player16, 0);

    // 0x00487b84: units[player][0].unit_above -= 1 -- the header row's roster live-count (see
    // sim_bldg_unmap_footprint.h's own derivation of this same field doubling as a WORD count).
    store_unit_full_id_word(
        header.unit_above,
        static_cast<uint16_t>(unit_full_id_word(header.unit_above) - kUnitZeroAboveHeaderDecrement));

    // 0x00487b95: units[player][0].order -= 1 -- the header row's in-production/live-order counter (the
    // decrement side of the same idiom sim_unit_apply_production_completion.cpp's own header-row
    // decrement documents for the completion path).
    header.order = static_cast<uint16_t>(header.order - kUnitZeroOrderHeaderDecrement);
}

} // namespace detail

// ---- the public surface --------------------------------------------------------------------------

void unit_free_slot(uint32_t player, int32_t slot) {
    sim_state st = state();
    detail::unit_free_slot(st.own, player, slot);
}


} // namespace mh::sim
