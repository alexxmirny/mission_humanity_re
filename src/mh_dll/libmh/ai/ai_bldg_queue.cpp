//
// ai/ai_bldg_queue.cpp -- see ai_bldg_queue.h. Translated from the DISASSEMBLY
// (tmp/decomp_ai/llm_strat_ai_bldg_queue_process_004e82ab.asm), not from Ghidra's C.
//
// EVERY ABSOLUTE RE-DERIVED against addr/mh_structs.gen.h, with the player stride 166140 = 0x288fc
// rebuilt SIX times in the body (SHL 2 / ADD / SHL 7 / SUB / SHL 2 / SHL 6 / ADD, first at
// 0x004e82f1-0x004e8305):
//   0xe935ec = player_data + 0x2572c = ai_bldg_queue_count
//   0xe935f0 = player_data + 0x25730 = ai_bldg_queue[0].status            (+0x12 per slot)
//   0xe935f6 =                       = ai_bldg_queue[0].resource_reserved[1]  (entry +6, short)
//   0xe7df14 = player_data + 0x10054 = resource_spent[1]                  (+4 per resource id)
//   0xe7df28 = player_data + 0x10068 = ai_mine_yield_by_resource[1]
// so no byte offset and no literal VA appears below (Law 1). The two jump tables were read out of
// the image rather than trusted from the decompile -- see the header.
//
// SIGNEDNESS AND WIDTH, the three that are not what they look like:
//   0x004e83c6 / 0x004e8423  MOVZX  the reserved cost is a SHORT read UNSIGNED into 32 bits...
//   0x004e83d8 / 0x004e8439  JLE    ...and then compared SIGNED against the int spend. So a cost
//                                   with the high bit set reads as 32768..65535, never negative.
//   0x004e8310 / 0x004e833a / 0x004e8371 / 0x004e8520  JC/JNC  every count compare is UNSIGNED.
//
#include "ai/ai_bldg_queue.h"


namespace mh::ai {
namespace detail {

// The compaction copies 18 bytes per slot (REP MOVSD with ECX=4, then MOVSW, 0x004e82d4-0x004e82e9)
// and the struct is 0x12 -- so the struct assignment below IS that copy. If a Ghidra retype ever
// changed the stride this would stop being true silently, hence the assert rather than a comment.
static_assert(sizeof(mh::game::mh_llm_strat_ai_bldg_queue_entry) == 0x12,
              "the queue entry must stay 18 bytes -- phase 1's shift is a REP MOVSD x4 + MOVSW");

bldg_queue_report bldg_queue_process(const ai_view &v, const ai_store &own, const ai_calls &gc,
                                     uint32_t player) {
    bldg_queue_report rep{};

    const player_data &pd = v.players[player];
    player_data       &wp = own.players[player];

    // ---- phase 1: drop every entry flagged removed, closing the gap ----
    for (uint32_t i = 0; i < (uint32_t)pd.ai_bldg_queue_count; ++i) { // 0x004e8355
        // The INNER loop is what re-tests slot i after a removal (the DEC at 0x004e8312 falls
        // through into the loop head at 0x004e8318). Both conditions are re-evaluated against the
        // live count, which this loop itself decrements.
        while (i < (uint32_t)pd.ai_bldg_queue_count &&
               (pd.ai_bldg_queue[i].status & QUEUE_STATUS_REMOVED)) {           // 0x004e833f
            for (uint32_t j = i + 1; j < (uint32_t)pd.ai_bldg_queue_count; ++j) // 0x004e82ee
                wp.ai_bldg_queue[j - 1] = pd.ai_bldg_queue[j];                  // 0x004e82e7
            --wp.ai_bldg_queue_count;                                           // 0x004e8312
            ++rep.compacted;
        }
    }

    // ---- phase 2: dispatch what is left ----
    for (uint32_t i = 0; i < (uint32_t)pd.ai_bldg_queue_count; ++i) { // 0x004e84fe
        const auto &qe = pd.ai_bldg_queue[i];
        ++rep.scanned;
        if (qe.status & QUEUE_STATUS_COMMITTED) { // 0x004e8388
            ++rep.skipped_done;
            continue;
        }

        bool affordable = true;                                                  // EBX = 1 @0x004e8396
        for (int32_t j = 0; j < QUEUE_COST_RESOURCE_COUNT; ++j) {                // 0x004e83e1
            const int32_t cost = (int32_t)(uint16_t)qe.resource_reserved[1 + j]; // MOVZX
            // resource_spent is int[4] over ids 1..4 since the 2026-08-03 reshape, so it indexes
            // [j] where the id-based arrays beside it still index [1 + j].
            if (cost > pd.resource_spent[j]) { // JLE continues
                affordable = false;
                break; // 0x004e83da: the break is what leaves EBX zero
            }
        }
        // AFTER the loop, so it overrides a failure rather than skipping the test.
        if (qe.status & QUEUE_STATUS_AFFORD_WAIVED) { // 0x004e8404
            if (!affordable) ++rep.waived;
            affordable = true;
        }

        if (!affordable) {
            // Ask for the shortfall on every resource we are short of AND have no mine yield for,
            // then stop processing the queue entirely -- see the header on the all-epilogue table.
            for (int32_t j = 0; j < QUEUE_COST_RESOURCE_COUNT; ++j) { // 0x004e8453
                const int32_t cost = (int32_t)(uint16_t)qe.resource_reserved[1 + j];
                if (cost <= pd.resource_spent[j]) continue;             // JLE @0x004e8439
                if (pd.ai_mine_yield_by_resource[1 + j] != 0) continue; // JNZ @0x004e8442
                gc.order_grant_resource_raw((uint16_t)player, (uint32_t)(j + 1), (uint32_t)cost);
                ++rep.granted;
            }
            rep.stopped_short = true;
            return rep; // table @0x004e8283: all five slots are the epilogue
        }

        const uint8_t kind = (uint8_t)(qe.status & QUEUE_KIND_MASK); // 0x004e84b8
        if (kind > QUEUE_KIND_MAX) continue;                         // JA @0x004e84bc
        ++rep.dispatched;
        ++rep.by_kind[kind];
        switch (kind) { // table @0x004e8297
            case 0:
                gc.bldg_queue_handle_recruit_state(player, (int32_t)i); // 0x004e84c9
                break;
            case 1:
                gc.bldg_queue_process_entry(player, (int32_t)i); // 0x004e84d6
                break;
            case 2:
                gc.bldg_queue_handle_state2_empty(); // 0x004e84e3 -- the 11-byte no-op
                break;
            case 3:
            case 4:
                // Slots 3 and 4 of the table BOTH hold 0x004e84f0. Written as a shared arm rather than
                // as `default`, because a nibble of 5..15 is a different outcome (skip, not upgrade).
                gc.bldg_queue_handle_upgrade_or_cancel(player, (int32_t)i); // 0x004e84f0
                break;
            default:
                break;
        }
    }
    return rep;
}

} // namespace detail

void bldg_queue_process(uint32_t player) {
    const ai_state st = state();
    (void)detail::bldg_queue_process(st.read, st.own, live_calls(), player);
}


} // namespace mh::ai
