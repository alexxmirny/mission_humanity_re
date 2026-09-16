//
// ai/ai_queue_type_query.cpp -- see ai_queue_type_query.h. Translated from the DISASSEMBLY
// (tmp/decomp_ai/llm_strat_ai_bldg_type_queue_has_pending_004d3a27.asm and
//  tmp/decomp_ai/llm_strat_ai_bldg_type_already_queued_004d3a93.asm), not from Ghidra's C.
//
// EVERY ABSOLUTE RE-DERIVED against addr/mh_structs.gen.h rather than transcribed:
//   0xe935ec = player_data + 0x2572c = ai_bldg_queue_count   (player stride 166140, built by
//              SHL 2 / ADD / SHL 7 / SUB / SHL 2 / SHL 6 / ADD at 0x004d3a6f-0x004d3a83)
//   0xe935f0 = player_data + 0x25730 = ai_bldg_queue[0].status        (entry stride 0x12, IMUL)
//   0xe935f1 =                       = ai_bldg_queue[0].tick_or_unit_id
//   0xd9ec88 = Building   + 0x8      = Building[id].type              (IMUL 0x842 = the stride)
// so no byte offset and no literal VA appears below (Law 1).
//
#include "ai/ai_queue_type_query.h"


namespace mh::ai {
namespace detail {

namespace {

// The status low nibble that both predicates accept. The field comment on
// llm_strat_ai_bldg_queue_entry::status calls nibble 1 "construction", dispatched by
// llm_strat_ai_bldg_queue_process_entry.
inline constexpr uint8_t QUEUE_KIND_CONSTRUCTION = 1;

} // namespace

int32_t bldg_type_queue_has_pending(const ai_view &v, int32_t player_idx, uint32_t bldg_type) {
    const player_data &pd = v.players[player_idx];
    // UNSIGNED bound: CMP EBX,dword ptr [.. + 0xe935ec] / JC @0x004d3a85-0x004d3a8b.
    const uint32_t count = (uint32_t)pd.ai_bldg_queue_count;
    for (uint32_t i = 0; i < count; ++i) {
        const auto &qe = pd.ai_bldg_queue[i];
        // MOVZX EDX,byte ptr [.. + 0xe935f1] / IMUL 0x842 / MOVZX EDX,byte ptr [.. + 0xd9ec88] /
        // CMP EDX,ESI @0x004d3a41-0x004d3a55. The cfg record is reached through the queue entry's
        // building id and it is the record's `type` FIELD that is compared -- the class, not the id.
        if ((uint32_t)v.cfg_buildings[qe.tick_or_unit_id].type != bldg_type) continue;
        // MOV AL,byte ptr [.. + 0xe935f0] / AND AL,0xf / CMP AL,0x1 @0x004d3a59-0x004d3a63.
        if ((uint8_t)(qe.status & 0xf) != QUEUE_KIND_CONSTRUCTION) continue;
        return 1; // MOV EAX,0x1 @0x004d3a65
    }
    return 0; // XOR EAX,EAX @0x004d3a8d
}

int32_t bldg_type_already_queued(const ai_view &v, int32_t player, uint32_t building_type) {
    const player_data &pd = v.players[player];
    // Same unsigned bound, same entry stride: CMP EBX,.. / JC @0x004d3ae4-0x004d3aea.
    const uint32_t count = (uint32_t)pd.ai_bldg_queue_count;
    for (uint32_t i = 0; i < count; ++i) {
        const auto &qe = pd.ai_bldg_queue[i];
        // MOVZX EDX,byte ptr [.. + 0xe935f1] / CMP EDX,ESI @0x004d3aad-0x004d3ab6 -- the queue
        // entry's own building id, with NO cfg hop. This is the whole difference from the sibling.
        if ((uint32_t)qe.tick_or_unit_id != building_type) continue;
        if ((uint8_t)(qe.status & 0xf) != QUEUE_KIND_CONSTRUCTION) continue;
        return 1;
    }
    return 0;
}

} // namespace detail

int32_t bldg_type_queue_has_pending(int32_t player_idx, uint32_t bldg_type) {
    const ai_state st = state();
    return detail::bldg_type_queue_has_pending(st.read, player_idx, bldg_type);
}

int32_t bldg_type_already_queued(int32_t player, uint32_t building_type) {
    const ai_state st = state();
    return detail::bldg_type_already_queued(st.read, player, building_type);
}

// ---- the differential-oracle arms ---------------------------------------------------------------
//
// Nothing to stub -- neither body makes an outward call. Nothing to restore either: the state matrix
// measures ZERO write cells for both, direct and transitive, so the verdict is entirely the return.
//
// WHAT A VACUOUS GREEN LOOKS LIKE HERE, and it is the reason for the `hits` counters. Both
// predicates return 0 by falling off the end of the walk, and so would a translation that read the
// wrong queue, the wrong field or an empty count. A run in which `hits` stays 0 has certified only
// that "nothing matched" agrees with "nothing matched". Read `hits` before the divergence count.
// `scanned` is the companion: hits == 0 with scanned == 0 means the queue was EMPTY (a scenario
// problem), hits == 0 with scanned > 0 means the queue was full of non-matches (a real, if narrow,
// comparison of the reject path).

} // namespace mh::ai
