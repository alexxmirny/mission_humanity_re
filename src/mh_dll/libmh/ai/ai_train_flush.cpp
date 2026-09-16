//
// ai/ai_train_flush.cpp -- see ai_train_flush.h. Translated from the DISASSEMBLY
// (tmp/decomp_ai/llm_strat_ai_queue_flush_unit_train_entries_2_004e2c98.asm plus the shared tail in
// llm_strat_ai_queue_flush_unit_train_entries_004e2c8e.asm), not from Ghidra's C.
//
// EVERY ABSOLUTE RE-DERIVED against addr/mh_structs.gen.h, with the player stride 166140 = 0x288fc
// built by SHL 2 / ADD / SHL 7 / SUB / SHL 2 / SHL 6 / ADD at 0x004e2c67-0x004e2c7b:
//   0xe935ec = player_data + 0x2572c = ai_bldg_queue_count
//   0xe935f0 = player_data + 0x25730 = ai_bldg_queue[0].status          (+0x12 per slot)
//   0xe935f1 =                         ai_bldg_queue[0].tick_or_unit_id (+0x1 inside the entry)
//   0xe963a8 = player_data + 0x284e8 = ai_train_queued_by_unit_type[0]
//   0xe96538 = player_data + 0x28678 = ai_train_queued_by_ai_unit[0]
//   0xe4a2cf = Unit        + 0x237   = Unit[0].ai_unit                  (IMUL by the 0x23f stride,
//              spelled SHL 3 / ADD / SHL 6 / SUB at 0x004e2c4d-0x004e2c58)
// so no byte offset and no literal VA appears below (Law 1).
//
// TWO UNCHECKED INDEXES, BOTH THE ORIGINAL'S. `tick_or_unit_id` is a BYTE, so it can name 0..255,
// and it is used raw as (a) an index into ai_train_queued_by_unit_type, which is int[100], and
// (b) an index into the cfg Unit table, which is Unit[100]. Neither the original nor this checks.
// The AI never enqueues a unit id above the cfg's Unit count, so the overrun is unreachable in
// practice -- but it is not defended against, and a reimplementation that clamped would diverge on
// exactly the state a corrupted queue produces.
//
// THE COUNT IS RE-READ EVERY ITERATION (CMP EBX,[..+0xe935ec] @0x004e2c7d, inside the loop head),
// and the comparison is UNSIGNED (JC). Nothing here writes the count, so the re-read cannot matter;
// it is kept because it is what the original does.
//
#include "ai/ai_train_flush.h"


namespace mh::ai {
namespace detail {

train_flush_report queue_flush_unit_train_entries_2(const ai_view &v, const ai_store &own,
                                                    int32_t player) {
    train_flush_report rep{};

    const player_data &pd = v.players[player];
    player_data       &wp = own.players[player];

    rep.scanned = pd.ai_bldg_queue_count;

    for (uint32_t slot = 0; slot < (uint32_t)pd.ai_bldg_queue_count; ++slot) {
        const auto &qe = pd.ai_bldg_queue[slot];
        // 0x004e2bf4 (bit 0) then 0x004e2c28 (the whole byte). The first is subsumed by the second
        // -- see the header for why the `status == 1` arm between them is unreachable.
        if (qe.status != 0) continue;

        wp.ai_bldg_queue[slot].status |= TRAIN_FLUSH_STAMP; // 0x004e2c31

        // Both DECs re-read the id byte from the entry (0x004e2c38 and again 0x004e2c46); it cannot
        // have changed between them, but the re-read is what the original does.
        const uint32_t unit_id = (uint32_t)qe.tick_or_unit_id;
        --wp.ai_train_queued_by_unit_type[unit_id]; // 0x004e2c3f
        const uint32_t role = v.cfg_units[unit_id].ai_unit;
        --wp.ai_train_queued_by_ai_unit[role]; // 0x004e2c5f

        ++rep.flushed;
    }
    return rep;
}

} // namespace detail

void queue_flush_unit_train_entries_2(int32_t player) {
    const ai_state st = state();
    (void)detail::queue_flush_unit_train_entries_2(st.read, st.own, player);
}

// ---- the differential-oracle arm ----------------------------------------------------------------
//
// Nothing is stubbed and nothing needs declaring beyond the measured player_data: the body makes no
// outward call at all (its only CALL is the inert Watcom stack probe at 0x004cf46f) and all three
// of its stores are inside player_data.
//
// WHAT A VACUOUS GREEN LOOKS LIKE HERE, and it is the likely case rather than the exotic one. The
// caller is llm_strat_ai_queue_reconcile_bldg_change, which fires on a soft building removal; the
// queue it then walks is usually empty or holds only committed entries, and every such call walks
// to the end having written nothing. `scan_max == 0` means every call saw an empty queue;
// `flushed == 0` means the ONE write path in the function -- the stamp and the two decrements --
// never ran, and the site proved only that an unmatched scan is a no-op. Read both before the
// divergence count.

// ---- the SIBLING ENTRY, 0x004e2c8e -------------------------------------------------------------
//
// See the header. `PUSH 4 / CALL assert_stack_capacity` and then a fall-through into the entry
// above, so there is nothing to translate beyond the delegation -- and the arm's own aggregate is
// separate only so a reader can tell which door a call came through.
void queue_flush_unit_train_entries(int32_t player) {
    const ai_state st = state();
    (void)detail::queue_flush_unit_train_entries_2(st.read, st.own, player);
}


} // namespace mh::ai
