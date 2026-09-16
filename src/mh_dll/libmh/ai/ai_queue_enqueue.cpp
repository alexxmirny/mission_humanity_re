//
// ai/ai_queue_enqueue.cpp -- see ai_queue_enqueue.h. Translated from the DISASSEMBLY
// (tmp/decomp_ai/llm_strat_ai_queue_train_unit_004e268c.asm,
//  .../llm_strat_ai_queue_bldg_repair_004e284d.asm,
//  .../llm_strat_ai_queue_bldg_upgrade_004e2a2e.asm), not from Ghidra's C.
//
// EVERY ABSOLUTE RE-DERIVED against addr/mh_structs.gen.h rather than transcribed. All three
// bodies rebuild the player stride 166140 = 0x288fc inline, several times each
// (SHL 2 / ADD / SHL 7 / SUB / SHL 2 / SHL 6 / ADD -- first at 0x004e26a1, 0x004e2864, 0x004e2a45):
//   0xe935ec = player_data + 0x2572c = ai_bldg_queue_count
//   0xe935f0 = player_data + 0x25730 = ai_bldg_queue[0].status              (+0x12 per slot)
//   0xe935f1 =                       = ai_bldg_queue[0].tick_or_unit_id     (entry +1, byte)
//   0xe935f2 =                       = ai_bldg_queue[0].build_tile_x        (entry +2, short)
//   0xe935f4 =                       = ai_bldg_queue[0].resource_reserved[0](entry +4, short)
//   0xe935f6/f8/fa/fc                = ai_bldg_queue[0].resource_reserved[1..4]
//   0xe935fe =                       = ai_bldg_queue[0].building_index      (entry +0xe, int)
//   0xe7e40c = player_data + 0x1054c = ai_start_units_remaining
//   0xe963a8 = player_data + 0x284e8 = ai_train_queued_by_unit_type[0]      (+4 per unit type)
//   0xe96538 = player_data + 0x28678 = ai_train_queued_by_ai_unit[0]        (+4 per ai_unit class)
// the cfg Unit table (base 0xe4a098, stride 0x23f = 575, built by SHL 3 / ADD / SHL 6 / SUB at
// 0x004e27ad-0x004e27b7):
//   0xe4a23f = Unit + 0x1a7 = resource[0].id     (+8 per entry)
//   0xe4a243 = Unit + 0x1ab = resource[0].val
//   0xe4a2cf = Unit + 0x237 = ai_unit
// the cfg Building table (base 0xd9ec80, stride 0x842, IMUL at 0x004e295b / 0x004e2aff):
//   0xd9f376 = Building + 0x6f6 = resource[0].id      / 0xd9f37a = resource[0].val
//   0xd9f3b6 = Building + 0x736 = resource_2[0].id    / 0xd9f3ba = resource_2[0].val
//   0xd9eecc = Building + 0x24c = energy      (double, the FULL charge a repair climbs to)
//   0xd9f3f6 = Building + 0x776 = energy_d    (double, the per-tick increment)
// and the roster (base 0xc3d2a0, row stride 27300 = 100 * 0x111, record stride 0x111 = 273):
//   0xc3d2a2 = buildings[p][i].building_id (word)  /  0xc3d2b9 = .energy (double, +0x19)
// so no byte offset and no literal VA appears below (Law 1).
//
// THE COST WALKS READ `val` AS A WORD. `MOV DX,word ptr [.. + 0xd9f3ba]` @0x004e294b,
// `MOV DX,word ptr [.. + 0xd9f37a]` @0x004e2b3b and `MOV BX,word ptr [.. + 0xe4a243]` @0x004e27ee
// each take the LOW HALF of a 32-bit `val` and add it to a 16-bit field. A cost above 65535 wraps,
// and a cost with bit 15 set accrues as a negative short. Reproduced by the explicit narrowing.
//
// THE ENTRY POINTER IS RE-DERIVED FROM THE COUNT AT EVERY SINGLE STORE (`IMUL EDX,[..+0xe935ec],
// 0x12` appears seven times in queue_bldg_repair alone). Nothing in any of the three bodies writes
// the count before its final increment, so this cannot matter -- it is noted, not modelled, for the
// same reason queue_reconcile notes its re-read count.
//
// WRITE SETS, taken from tmp/state_matrix.json and then checked against the listing:
//   queue_train_unit    player_data only (5 write + 4 rw cells). Reads cfg Unit and the AI.SCR cap.
//   queue_bldg_repair   player_data only (7 write + 3 rw). Reads cfg Building and the roster.
//   queue_bldg_upgrade  player_data DIRECTLY (3 write + 2 rw), PLUS the order container through
//                       llm_strat_order_grant_resource_raw -> llm_strat_order_enqueue, which the
//                       matrix attributes to the callee. Its shadow site therefore declares the
//                       three _G_LLM_STRAT_ORDER_* regions as extras; the other two declare none.
//
#include "ai/ai_queue_enqueue.h"


namespace mh::ai {
namespace detail {

namespace {

// The cost walk all three share: for the first up-to-seven entries of a cfg resource list, until
// one with a zero id, accrue its `val` into `resource_reserved[its id]`.
//
// THE ACCRUAL INDEX IS UNBOUNDED IN THE ORIGINAL. The store is a bare
// `ADD word ptr [entry + id*2 + 4],DX` (0x004e2952 / 0x004e2b42 / 0x004e27f5) with nothing
// checking `id` against the five-entry field, so an id above 4 writes into the NEXT queue slot.
// C++ cannot reproduce that without undefined behaviour, so the walk skips such an entry and
// counts it into `oob_cost` instead -- the one place in this file where the reimplementation
// deliberately does something different from the original. It is reachable only from cfg data
// (no shipped Building or Unit record has a resource id outside 1..4), and if it ever fires the
// shadow site will diverge and say so rather than corrupting a neighbour quietly.
//
// THE TWO CALLERS' LOOP SHAPES DIFFER AND THE DIFFERENCE IS NOT OBSERVABLE. Upgrade (0x004e2b4b)
// and train (0x004e27fe) test `d < 7` FIRST and the id inside the body; repair (0x004e2968, then
// 0x004e2971) tests the id FIRST and the bound second, so it performs one extra READ at d == 7 --
// one entry past Building.resource_2, landing on `build_time_d`. Whatever that read returns, the
// very next test (`d < 7`, and this is the only SIGNED bound compare in the three bodies -- `JL` at
// 0x004e2974) fails and the loop exits. Same iteration count, same writes, so one walk serves both.
int32_t cost_walk(mh::game::mh_llm_strat_ai_bldg_queue_entry &e,
                  const mh::game::mh_cfg_struct_resource *cost, int32_t *oob_cost) {
    int32_t d = 0;
    for (; d < COST_WALK_MAX; ++d) {
        const uint32_t id = cost[d].id;
        if (id == 0) break;
        if (id >= (uint32_t)RESOURCE_RESERVED_COUNT) {
            ++*oob_cost;
            continue;
        }
        e.resource_reserved[id] = (int16_t)(e.resource_reserved[id] + (int16_t)(uint16_t)cost[d].val);
    }
    return d;
}

} // namespace

// ---- llm_strat_ai_queue_train_unit @0x004e268c ---------------------------------------------------
enqueue_report queue_train_unit(const ai_view &v, const ai_store &own, uint32_t player,
                                uint32_t unit_id) {
    enqueue_report rep{};

    const player_data &pd = v.players[player];
    player_data       &wp = own.players[player];

    // 0x004e26b5, UNSIGNED. `MOV EBX,1` at 0x004e26be is the shared reject arm -- the per-unit cap
    // below jumps to the SAME instruction, so a cap rejection is indistinguishable from a full
    // queue in the return value.
    if ((uint32_t)pd.ai_bldg_queue_count >= (uint32_t)AI_BLDG_QUEUE_CAP) {
        rep.rc = 1;
        return rep;
    }

    // 0x004e26c8: the cap is only consulted once the scripted opening units are exhausted. While
    // ai_start_units_remaining is non-zero the AI may queue the same type without limit.
    if (pd.ai_start_units_remaining == 0) {
        rep.cap_check    = true;
        int32_t matching = 0;
        // 0x004e2709, UNSIGNED, and the count is re-read every iteration.
        for (uint32_t slot = 0; slot < (uint32_t)pd.ai_bldg_queue_count; ++slot) {
            ++rep.cap_scan;
            const auto &qe = pd.ai_bldg_queue[slot];
            // 0x004e26dc: TEST ..,0xf -- the KIND NIBBLE, so a train entry already flagged
            // committed/removed (0x80/0x40) still counts against the cap. Only the nibble is
            // masked; the flag bits are not.
            if ((qe.status & 0x0f) != 0) continue;
            // 0x004e26e5: MOVZX of the byte, compared against the full 32-bit unit_id -- a
            // unit_id above 255 can therefore never match its own queued entries.
            if ((uint32_t)qe.tick_or_unit_id != unit_id) continue;
            ++matching;
        }
        rep.cap_hits = matching;
        // 0x004e2711 / JNC -- UNSIGNED, and >= not >.
        if ((uint32_t)matching >= *v.train_queue_per_unit_cap) {
            rep.rc = 1;
            return rep;
        }
    }

    const int32_t slot = pd.ai_bldg_queue_count;
    auto         &e    = wp.ai_bldg_queue[slot];
    rep.slot           = slot;

    e.status               = QUEUE_STATUS_NEW_TRAIN; // 0x004e2736
    e.tick_or_unit_id      = (uint8_t)unit_id;       // 0x004e2748, a BYTE store: unit_id is truncated
    e.build_tile_x         = 0;                      // 0x004e2756
    e.resource_reserved[0] = 0;                      // 0x004e2767
    for (uint32_t i = 0; i < 4; ++i)                 // 0x004e27a1, UNSIGNED
        e.resource_reserved[1 + i] = 0;              // 0x004e2796

    rep.cost_ids = cost_walk(e, v.cfg_units[unit_id].resource, &rep.oob_cost);

    // 0x004e282e / 0x004e2837. Two tallies, indexed differently: one by the cfg TYPE, one by that
    // type's ai_unit class. Neither index is bounded by the original. These are the counters
    // llm_strat_ai_queue_flush_unit_train_entries_2 decrements again on cancel.
    ++wp.ai_train_queued_by_unit_type[unit_id];
    ++wp.ai_train_queued_by_ai_unit[v.cfg_units[unit_id].ai_unit];
    ++wp.ai_bldg_queue_count; // 0x004e283e
    return rep;               // EBX was zeroed at 0x004e2835
}

// ---- llm_strat_ai_queue_bldg_repair @0x004e284d --------------------------------------------------
enqueue_report queue_bldg_repair(const ai_view &v, const ai_store &own, uint32_t player,
                                 int32_t building_index) {
    enqueue_report rep{};

    const player_data &pd = v.players[player];
    player_data       &wp = own.players[player];

    if ((uint32_t)pd.ai_bldg_queue_count >= (uint32_t)AI_BLDG_QUEUE_CAP) { // 0x004e287f
        rep.rc = 1;
        return rep;
    }

    const int32_t slot = pd.ai_bldg_queue_count;
    auto         &e    = wp.ai_bldg_queue[slot];
    rep.slot           = slot;

    e.status         = QUEUE_STATUS_NEW_REPAIR; // 0x004e2892
    e.building_index = building_index;          // 0x004e28a5

    // 0x004e28d6, MOVZX word off the roster record -- the cfg TYPE, not the roster slot.
    const building &b     = building_of(v, player, building_index);
    const int32_t   btype = (int32_t)(uint32_t)(uint16_t)b.building_id;

    // 0x004e28e5..0x004e2918: FOUR separate immediate stores, [1] through [4]. Unlike train, this
    // leaves resource_reserved[0] and build_tile_x holding whatever the slot held before.
    for (int32_t k = 1; k <= 4; ++k) e.resource_reserved[k] = 0;

    // REPAIR COSTS COME FROM `resource_2`, not `resource` -- 0xd9f3b6 is Building + 0x736. The
    // upgrade sibling twelve bytes away reads the other list. Preserved, not smoothed.
    rep.cost_ids = cost_walk(e, v.cfg_buildings[btype].resource_2, &rep.oob_cost);

    // 0x004e2993: only NOW is the tick counter cleared, after the cost walk -- and the loop below
    // then counts up in it.
    e.tick_or_unit_id = 0;

    // 0x004e29ee-0x004e2a1c. The repair's DURATION, computed by simulating the charge climb:
    // start from the building's CURRENT energy and add the type's per-tick increment until it
    // reaches the type's full energy, counting iterations into the entry's byte tick field.
    //
    // THREE THINGS ARE FAITHFUL RATHER THAN SENSIBLE, and all three are the original's:
    //   * the counter is a BYTE and is INC'd (0x004e29e7), so a repair needing more than 255 ticks
    //     wraps silently;
    //   * nothing bounds the loop -- a type whose energy_d is <= 0 with energy above the building's
    //     current charge spins forever;
    //   * the compare is `FCOMP` + `SAHF` + `JC`, i.e. strictly-less, so an already-full building
    //     produces zero ticks rather than one.
    double acc = b.energy;
    while (acc < v.cfg_buildings[btype].energy) {
        ++e.tick_or_unit_id;
        acc += v.cfg_buildings[btype].energy_d;
        ++rep.ticks;
    }

    ++wp.ai_bldg_queue_count; // 0x004e2a20
    return rep;               // EAX zeroed at 0x004e2a1e
}

// ---- llm_strat_ai_queue_bldg_upgrade @0x004e2a2e -------------------------------------------------
enqueue_report queue_bldg_upgrade(const ai_view &v, const ai_store &own, const ai_calls &gc,
                                  uint32_t player, uint32_t building_index) {
    enqueue_report rep{};

    const player_data &pd = v.players[player];
    player_data       &wp = own.players[player];

    if ((uint32_t)pd.ai_bldg_queue_count >= (uint32_t)AI_BLDG_QUEUE_CAP) { // 0x004e2a60
        rep.rc = 1;
        return rep;
    }

    const int32_t slot = pd.ai_bldg_queue_count;
    auto         &e    = wp.ai_bldg_queue[slot];
    rep.slot           = slot;

    e.status         = QUEUE_STATUS_NEW_UPGRADE; // 0x004e2a73
    e.building_index = (int32_t)building_index;  // 0x004e2a82

    // 0x004e2aa3 / 0x004e2a98: a LOOP of four short stores here, where repair used four immediates.
    for (uint32_t i = 0; i < 4; ++i) e.resource_reserved[1 + i] = 0;

    // 0x004e2ac5: the roster slot is READ BACK OUT OF THE ENTRY just written, rather than reused
    // from the parameter. Same value; kept because it is what the code does and because it means a
    // narrowing of the field (int32) is in the path.
    const int32_t   roster_slot = e.building_index;
    const building &b           = building_of(v, player, roster_slot);
    const int32_t   btype       = (int32_t)(uint32_t)(uint16_t)b.building_id; // 0x004e2af0

    // UPGRADE COSTS COME FROM `resource` -- 0xd9f376 is Building + 0x6f6, the other list from the
    // one queue_bldg_repair reads.
    rep.cost_ids = cost_walk(e, v.cfg_buildings[btype].resource, &rep.oob_cost);

    // 0x004e2b75-0x004e2bcd: four calls, resource ids 1..4 in order, each passing the reserved
    // amount MOVZX'd from the 16-bit field (so an accrued negative arrives as 32768..65535). The
    // callee stages (res_type, amount) into order scratch fields 3 and 2 and enqueues order 0xed --
    // which is why this one writes the ORDER container and its two siblings do not. Note the calls
    // are UNCONDITIONAL: a zero reserve is still sent.
    for (uint32_t k = 1; k <= 4; ++k) {
        gc.order_grant_resource_raw((uint16_t)player, k,
                                    (uint32_t)(uint16_t)e.resource_reserved[k]);
        ++rep.grants;
    }

    ++wp.ai_bldg_queue_count; // 0x004e2bd4
    return rep;               // EAX zeroed at 0x004e2bd2
}

} // namespace detail

int32_t queue_train_unit(int32_t player, uint32_t unit_id) {
    const ai_state st = state();
    return detail::queue_train_unit(st.read, st.own, (uint32_t)player, unit_id).rc;
}

int32_t queue_bldg_repair(int32_t player, int32_t building_index) {
    const ai_state st = state();
    return detail::queue_bldg_repair(st.read, st.own, (uint32_t)player, building_index).rc;
}

int32_t queue_bldg_upgrade(int32_t player, uint32_t building_index) {
    const ai_state st = state();
    return detail::queue_bldg_upgrade(st.read, st.own, live_calls(), (uint32_t)player, building_index).rc;
}

// ---- the differential-oracle arms ----------------------------------------------------------------
//
// WHAT A VACUOUS GREEN LOOKS LIKE HERE, and it is the dominant risk for all three. Every one of
// them returns immediately when the queue is full, writing NOTHING. A run in which the AI's queue
// sits at 64 would log a large, honest call count and compare nothing at all. So each arm reports
// `full=` (guard rejections) alongside `appended=`, and `appended > 0` -- not the call count -- is
// the anti-vacuity condition. Two more, per function:
//   train    `capchk=` counts calls that reached the per-unit cap scan (i.e. the player had no
//            scripted start units left) and `capful=` those it rejected. capchk == 0 means the
//            whole cap branch, half the body, never ran.
//   repair   `tickmax=` is the largest energy-loop iteration count seen. tickmax == 0 means every
//            repaired building was already at full charge and the loop never turned over.
//   upgrade  `costmax=` is the longest cost walk. costmax == 0 means every upgrade type had an
//            empty cost list, so resource_reserved stayed zero and the four grants all sent 0.
//
// NOTHING IS STUBBED IN THE UPGRADE ARM. order_grant_resource_raw writes only the order container,
// which that site declares as an extra region, so the restore between arms undoes it; stubbing it
// would manufacture the divergence it looks like it prevents.

} // namespace mh::ai
