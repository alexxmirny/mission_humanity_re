//
// ai/ai_queue_reconcile.cpp -- see ai_queue_reconcile.h. Translated from the DISASSEMBLY
// (tmp/decomp_ai/llm_strat_ai_queue_reconcile_bldg_change_004e2d91.asm), not from Ghidra's C.
//
// EVERY ABSOLUTE RE-DERIVED against addr/mh_structs.gen.h, not transcribed. The body computes
// `player * 0x288fc` five separate times (SHL 2 / ADD / SHL 7 / SUB / SHL 2 / SHL 6 / ADD = 166140)
// and reaches player_data through it at
//   0xe935ec = player_data + 0x2572c = ai_bldg_queue_count
//   0xe935f0 = player_data + 0x25730 = ai_bldg_queue[0].status   (+ 0x12 per slot, the entry stride)
//   0xe935fe =                        ai_bldg_queue[0].building_index (+0xe inside the entry)
// with the base 0xe6dec0 fixed independently as ai_enabled's absolute minus its committed offset.
// The tail reaches the ROSTER instead: `player * 27300 + index * 273 + 0xc3d2a2` is
// buildings[player][index].building_id (base 0xc3d2a0, row stride 27300, record stride 0x111 = 273,
// building_id at +0x2), and 0xc3d363 / 0xc3d364 are that record's `x` / `y` at +0xc3 / +0xc4. So no
// byte offset and no literal VA appears below (Law 1).
//
// TWO INDEX SPACES, AND THIS FUNCTION KEEPS THEM STRAIGHT. `building_index` is a ROSTER slot
// throughout -- it indexes buildings[player][] in the tail and it is compared against the queue
// entry's `building_index` field, which the struct documents as a roster index. The cfg TYPE index
// enters only as the record's own `building_id`, and it is the only thing handed to
// llm_strat_bldg_total_resource_cost (IMUL 0x842, the cfg Building stride) and to
// llm_strat_bldg_queue_construction (whose committed second parameter is `building_type`). That is
// worth stating because the layer-0 sibling llm_strat_ai_bldg_register_visible_building genuinely
// DOES conflate the two -- see its module -- and this one does not.
//
// THE THREE PHASES, site by site:
//
//   0x004e2da2  llm_strat_ai_queue_flush_unit_train_entries_2(player) -- unconditional, first thing
//               after the register moves. Note the call is made with EDX still holding
//               building_index, but the committed prototype takes only EAX, and the callee's own
//               body reads only ESI (= EAX). One argument.
//   0x004e2dab  PHASE 1: scan slots [0, count). Match on the KIND NIBBLE being 3 (repair) or 4
//               (upgrade) AND the entry's building_index equalling ours; on a hit, OR 0xc0 into the
//               status and JMP straight to phase 2 (0x004e2e1f). Note this is a BREAK into phase 2,
//               not a return -- the stamp does not skip anything.
//   0x004e2e1f  PHASE 2: scan again. Match the WHOLE status byte against 0x81 and the same
//               building_index; on a hit, ASSIGN status = 0x01 and RETURN (0x004e2e40-43),
//               skipping the tail entirely.
//   0x004e2e63  PHASE 3: cost = total_resource_cost(buildings[player][building_index].building_id);
//               if it is non-zero, queue_construction(player, that same building_id,
//               record.x, record.y). A zero cost queues nothing.
//
// BOTH SCANS RE-READ THE COUNT EVERY ITERATION (`CMP EDX,[..+0xe935ec] / JC` at 0x004e2e17 and
// 0x004e2e5b) and both comparisons are UNSIGNED. Nothing in this body writes the count, so the
// re-read cannot matter here -- it is kept because it is what the original does. The tail's
// llm_strat_bldg_queue_construction DOES bump the count, but it runs after both loops.
//
// THE WRITE SET IS player_data AND NOTHING ELSE, and every outward call was audited individually
// rather than taken from the matrix:
//   * llm_strat_ai_queue_flush_unit_train_entries_2 @0x004e2c98 -- a 20-byte head that jumps into a
//     shared tail at 0x004e2c67. Its three stores are `OR byte ptr [..+0xe935f0],0xc0` (a queue
//     status), `DEC dword ptr [..+0xe963a8]` and `DEC dword ptr [..+0xe96538]` -- player_data
//     +0x284e8 and +0x28678, the two training-queued counters. It reads the cfg Unit table. No
//     other store, no outward call.
//   * llm_strat_bldg_total_resource_cost @0x004e2d52 -- PURE. Its whole 0x3f-byte body reads the cfg
//     Building table and accumulates into EBX; it writes no memory at all.
//   * llm_strat_bldg_queue_construction @0x004e2550 -- appends one entry to
//     player_data[player].ai_bldg_queue and increments the count. Nothing else, and it calls
//     nothing. It places no building and issues no order, so it is NOT an escape: ai_state.h
//     already binds it REAL for batch A layer 3 on exactly this reasoning.
// So all three run for real in the shadow arm and the restore of player_data undoes all of it.
//
#include "ai/ai_queue_reconcile.h"


namespace mh::ai {
namespace detail {

reconcile_report queue_reconcile_bldg_change(const ai_view &v, const ai_store &own,
                                             const ai_calls &gc, uint32_t player,
                                             uint32_t building_index) {
    reconcile_report rep{};

    // 0x004e2da2. Unconditional and first -- it runs even when nothing else in this body does.
    gc.queue_flush_unit_train_entries_2((int32_t)player);

    const player_data &pd = v.players[player];
    player_data       &wp = own.players[player];

    rep.scanned = pd.ai_bldg_queue_count;

    // ---- PHASE 1: settle a pending repair or upgrade for this building ----------------------------
    for (uint32_t slot = 0; slot < (uint32_t)pd.ai_bldg_queue_count; ++slot) {
        const auto   &qe   = pd.ai_bldg_queue[slot];
        const uint8_t kind = qe.status & QUEUE_KIND_MASK;
        // The two nibble tests are written as separate blocks in the original (0x004e2db9 for
        // repair, 0x004e2df3 for upgrade, each re-deriving the entry address from scratch) and
        // converge on ONE stamp at 0x004e2dc6. They are the same condition, so they are one here.
        if ((kind != QUEUE_KIND_REPAIR && kind != QUEUE_KIND_UPGRADE) ||
            (uint32_t)qe.building_index != building_index)
            continue;
        // 0x004e2dc6: OR, not assign -- the kind nibble and any 0x20 survive.
        wp.ai_bldg_queue[slot].status |= QUEUE_STATUS_SETTLED;
        rep.stamped = true;
        rep.slot    = (int32_t)slot;
        break; // JMP 0x004e2e1f -- into phase 2, NOT out of the function
    }

    // ---- PHASE 2: is a committed construction for it already in the plan? -------------------------
    for (uint32_t slot = 0; slot < (uint32_t)pd.ai_bldg_queue_count; ++slot) {
        const auto &qe = pd.ai_bldg_queue[slot];
        // WHOLE-BYTE compare here, unlike phase 1's nibble mask. An entry that also carried 0x20 or
        // 0x40 would not match.
        if (qe.status != QUEUE_STATUS_COMMITTED_CONSTRUCTION ||
            (uint32_t)qe.building_index != building_index)
            continue;
        // 0x004e2e39: a plain byte STORE of 1. Any other bit the entry carried is gone with it.
        wp.ai_bldg_queue[slot].status = QUEUE_STATUS_LIVE_CONSTRUCTION;
        rep.outcome                   = reconcile_outcome::revived;
        rep.slot                      = (int32_t)slot;
        return rep; // 0x004e2e40 -- the tail is skipped entirely
    }

    // ---- PHASE 3: put it back in the build plan ---------------------------------------------------
    const building &b = building_of(v, player, (int32_t)building_index);
    // MOVZX EAX,word ptr [..+0xc3d2a2] @0x004e2e89 -- the cfg TYPE id, read off the roster record.
    const int32_t building_type = (int32_t)(uint32_t)(uint16_t)b.building_id;
    rep.cost                    = gc.bldg_total_resource_cost(building_type);
    if (rep.cost == 0) {
        rep.outcome = reconcile_outcome::cost_zero;
        return rep;
    }
    // 0x004e2e99-0x004e2eb0. x and y are MOVZX'd from BYTE fields into EBX and ECX, and the callee's
    // committed prototype narrows them again to BX (int16) and CX (uint16); building_id is re-read
    // from the same record rather than reused from EAX, which is the same value either way.
    gc.bldg_queue_construction((int32_t)player, building_type, (int16_t)(uint16_t)(uint8_t)b.x,
                               (uint16_t)(uint8_t)b.y);
    rep.outcome = reconcile_outcome::requeued;
    return rep;
}

} // namespace detail

void queue_reconcile_bldg_change(uint32_t player, uint32_t building_index) {
    const ai_state st = state();
    (void)detail::queue_reconcile_bldg_change(st.read, st.own, live_calls(), player, building_index);
}

// ---- the differential-oracle arm ----------------------------------------------------------------
//
// Nothing is stubbed. All three callees write only player_data (or nothing at all) -- the audit is
// in the file header -- and player_data is the site's one declared region, so the restore between the
// arms undoes everything. Stubbing an in-region writer manufactures the divergence it looks like it
// prevents.
//
// WHAT A VACUOUS GREEN LOOKS LIKE HERE. The caller fires on a soft building removal, so the call
// itself is meaningful; what can be empty is the queue. If `scan_max == 0` then BOTH scans walked
// zero entries on every call and the only things exercised were the flush and the tail. And
// `stamped == 0` with `revived == 0` means neither queue-editing branch -- five of the body's seven
// distinguishing behaviours -- ever ran. Read those before the divergence count.

} // namespace mh::ai
