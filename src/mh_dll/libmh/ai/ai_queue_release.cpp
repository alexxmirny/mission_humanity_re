//
// ai/ai_queue_release.cpp -- see ai_queue_release.h. Translated from the DISASSEMBLY
// (tmp/decomp_aiprep/llm_strat_ai_queue_release_order_004dbf36.asm), not from Ghidra's C: the
// decompile renders the mode-1/2 status stamp as
// `player_data[0].ai_bldg_queue[0].resource_reserved + iVar2 + -4`, a re-materialised pointer into
// the WRONG field. The assembly is `OR byte ptr [EAX + 0xe935f0],0x40` with EAX = player * 0x288fc +
// slot * 0x12, i.e. plainly `queue[slot].status |= 0x40`.
//
// EVERY absolute in that listing was checked against addr/mh_structs.gen.h rather than transcribed:
// player_data's base is 0xe6dec0 (ai_enabled @0xe6ded8 - 0x18) and the stride is 0x288fc, which
// reproduces 0xe7df14..0xe7df20 as resource_spent[1..4], 0xe935ec as ai_bldg_queue_count, and
// 0xe935f0/f1/f6/f8/fa/fc/fe as the queue entry's status / tick_or_unit_id / resource_reserved[1..4]
// / building_index. No byte offset or literal VA appears below (Law 1).
//
// The state matrix agrees on the WRITE set independently: one cell, region `player_data`, 17 reads
// and 6 read-modify-writes -- exactly the four resource_spent subtractions, the tick decrement and
// the status OR.
//
#include "ai/ai_queue_release.h"


namespace mh::ai {
namespace detail {

release_outcome queue_release_order(const ai_view &v, const ai_store &own, int32_t player,
                                    int32_t building_index, int32_t mode) {
    const player_data &pd = v.players[player];

    // The master AI gate. Every AI notification hook opens with it (see the ai_enabled field
    // comment); for a human player this function does nothing at all.
    if (pd.ai_enabled == 0)
        return release_outcome::not_ai;

    // THE DISPATCH IS UNSIGNED. The original is `CMP EBX,1 / JC` then `JBE` then `CMP EBX,2 / JZ`,
    // so mode 0/1/2 take the three paths and EVERYTHING ELSE -- including any negative mode --
    // falls out of the bottom and returns. (The `TEST EBX,EBX / JNZ` at 0x004dbf7d on the mode-0
    // path is dead: JC already proved EBX == 0. Not reproduced.)
    const uint32_t umode = (uint32_t)mode;
    if (umode > 2)
        return release_outcome::bad_mode;

    // The loop bound is a LIVE re-read of the count on every iteration, and the comparison is
    // unsigned (`CMP EDX,[count] / JC`). Nothing in this body writes the count, so the re-read
    // cannot matter here -- it is kept because it is what the original does, not because it is
    // load-bearing.
    for (uint32_t slot = 0; slot < (uint32_t)pd.ai_bldg_queue_count; ++slot) {
        const auto &qe = pd.ai_bldg_queue[slot];

        if (umode == 0) {
            if (qe.status == QUEUE_STATUS_COMMITTED_REPAIR && qe.building_index == building_index) {
                auto &we = own.players[player].ai_bldg_queue[slot];
                // Guarded decrement: the original tests the byte for zero first, so a counter that
                // is already 0 does NOT wrap to 0xff.
                if (we.tick_or_unit_id != 0)
                    --we.tick_or_unit_id;
                // The refund. MOVZX, not MOVSX: `resource_reserved` is int16_t in the struct but the
                // original ZERO-extends each halfword before subtracting, so a reserved value with
                // the high bit set subtracts 0xffff rather than adding 1. The cast is the behaviour.
                // resource_spent is int[4] covering ids 1..4 since the 2026-08-03 reshape, so it is
                // indexed [r - RESOURCE_ID_FIRST]; resource_reserved is still id-indexed.
                for (int32_t r = RESOURCE_ID_FIRST; r <= RESOURCE_ID_LAST; ++r)
                    own.players[player].resource_spent[r - RESOURCE_ID_FIRST] -=
                        (int32_t)(uint32_t)(uint16_t)we.resource_reserved[r];
                return release_outcome::refunded;
            }
            // A committed UPGRADE entry for the same building ENDS the scan with no effect
            // (0x004dc02f jumps straight to the epilogue). It is an early-out, not a continue --
            // a repair entry sitting behind it is never reached.
            if (qe.status == QUEUE_STATUS_COMMITTED_UPGRADE && qe.building_index == building_index)
                return release_outcome::upgrade_blocked;
            continue;
        }

        // Modes 1 and 2 are the same walk with a different status literal, and they converge on the
        // one shared stamp at LAB_004dc075.
        const uint8_t want =
            (umode == 1) ? QUEUE_STATUS_COMMITTED_REPAIR : QUEUE_STATUS_COMMITTED_UPGRADE;
        if (qe.status == want && qe.building_index == building_index) {
            own.players[player].ai_bldg_queue[slot].status |= QUEUE_STATUS_REMOVED;
            return release_outcome::stamped_removed;
        }
    }
    return release_outcome::no_match;
}

} // namespace detail

void queue_release_order(int32_t player, int32_t building_index, int32_t mode) {
    const ai_state st = state();
    (void)detail::queue_release_order(st.read, st.own, player, building_index, mode);
}

// ---- the differential-oracle arm ----------------------------------------------------------------
//
// Nothing to bind and nothing to stub: this function makes NO outward call at all (its only CALL is
// the inert Watcom stack check) and every byte it writes is inside player_data, the site's one
// declared region. So the restore between the two arms undoes all of it.
//
// WHY THIS ARM REPORTS AN OUTCOME HISTOGRAM. The batch-A lesson is that a high
// call count is not coverage. Here the failure mode is concrete and likely: the master gate rejects
// every human player and the queue scan usually finds no match, so a site can log tens of thousands
// of calls of which none reached a single write. `not_ai + bad_mode + no_match == calls` is a
// VACUOUS green, and this histogram is what makes that visible in the run's own log rather than
// something to be assumed either way afterwards.

} // namespace mh::ai
