//
// ai/ai_train_plan.cpp -- see ai_train_plan.h. Translated from the DISASSEMBLY
// (tmp/decomp_ai/llm_strat_ai_plan_unit_training_004e6d03.asm), not from Ghidra's C.
//
// EVERY ABSOLUTE RE-DERIVED against addr/mh_structs.gen.h + addr/mh_regions.gen.h, with the player
// stride 166140 = 0x288fc spelled SHL 2 / ADD / SHL 7 / SUB / SHL 2 / SHL 6 / ADD at
// 0x004e6d1d-0x004e6d2f (and recomputed identically five more times in the body):
//   0xe7e384 = player_data + 0x104c4 = ai_build_plan_cursor
//   0xe7e388 = player_data + 0x104c8 = ai_build_plan_len_and_flag
//   0xe7e40c = player_data + 0x1054c = ai_start_units_remaining
//   0xe93ad4 = player_data + 0x25c14 = ai_train_source_state[0]      (uint8_t, +1 per type)
//   0xe96538 = player_data + 0x28678 = ai_train_queued_by_ai_unit[0] (int32,   +4 per role)
//   0xe16304 = Progress    + 0x67 + 0x20 = Progress[1].type          (IMUL 0x67 stride)
//   0xe16305 = Progress    + 0x67 + 0x21 = Progress[1].index
//   0xc38750 = progress[0][0]                     (0x384 per player, 3 per row)
//   0xe4a2cf = Unit        + 0x237   = Unit[0].ai_unit  (IMUL by the 0x23f stride, spelled
//   0xe4a2d3 = Unit        + 0x23b   = Unit[0].ai_level  SHL 3 / ADD / SHL 6 / SUB @0x004e6ea1)
//   0xe5f634 = G_BUILDING_COUNT_TOTAL   (cfg_building_sec->total, +0x2c50 of the section record)
//   0xe5c9e0 = G_PROGRESS_COUNT_TOTAL   (cfg_progress_sec->total, +0x908)
//   0xe6049c = G_UNIT_COUNT_TOTAL       (cfg_unit_sec->total,     +0xe64)
// so no byte offset and no literal VA appears below (Law 1).
//
// SIGNEDNESS, one line per comparison, because four of the six differ from what reads naturally:
//   0x004e6d43 JA   UNSIGNED  the build-plan gate
//   0x004e6d72 JBE  UNSIGNED  clear-loop bound, INCLUSIVE
//   0x004e6d84 JC   UNSIGNED  role-scratch clear, < 12
//   0x004e6e2c JBE  UNSIGNED  invention scan, INCLUSIVE, from 1
//   0x004e6e71 JNC  UNSIGNED  role pick: strictly-less wins, so ties keep the LOWER role
//   0x004e6ee5 JBE  UNSIGNED  unit scan, INCLUSIVE, from 1
//   0x004e6ebe JGE  SIGNED    level pick: strictly-greater wins, ties keep the LOWER type
//
#include "ai/ai_train_plan.h"


namespace mh::ai {
namespace detail {

train_plan_report plan_unit_training(const ai_view &v, const ai_store &own, const ai_calls &gc,
                                     int32_t player) {
    train_plan_report rep{};

    const player_data &pd = v.players[player];
    player_data       &wp = own.players[player];

    // The gate. UNSIGNED on both sides: the length field's flag bit is masked off first, and the
    // cursor is compared as a uint (0x004e6d31-0x004e6d43).
    if ((pd.ai_build_plan_len_and_flag & 0x7fffffffu) > (uint32_t)pd.ai_build_plan_cursor)
        return rep;
    rep.ran = true;

    // Pass 0a -- clear the per-unit-type source state, over the BUILDING count, inclusively.
    // See the header: the bound is the original's, and it is not the array's.
    for (uint32_t i = 0; i <= (uint32_t)v.cfg_building_sec->total; ++i)
        wp.ai_train_source_state[i] = 0; // 0x004e6d63

    // Pass 0b -- the two stack scratch buffers. by_role is cleared explicitly by the original
    // (0x004e6d78); by_type is filled by the callee, which does NOT write index 0 -- see the
    // header for why this one is zero-initialised anyway.
    int32_t by_role[AI_UNIT_ROLE_COUNT]     = {};
    int32_t by_type[TRAIN_SOURCE_TABLE_LEN] = {};
    gc.count_unit_build_sources(player, by_type); // 0x004e6d8e

    // Pass 1+2 -- one walk of the cfg invention table, 1..total INCLUSIVE.
    for (uint32_t row = 1; row <= (uint32_t)v.cfg_progress_sec->total; ++row) {
        const cfg_invention &inv = v.cfg_inventions[row];
        if (inv.type != INVENTION_TYPE_UNIT) continue; // 0x004e6da0
        const player_progress &pp = progress_of(v, player, (int32_t)row);
        if (pp.available == 0) continue;                               // 0x004e6dbd
        const uint32_t unit_type = inv.index;                          // MOVZX, u16
        if (pp.f3 != 0) {                                              // 0x004e6ddc
            wp.ai_train_source_state[unit_type] = TRAIN_SOURCE_F3_SET; // 0x004e6dec
            ++rep.marked;
            continue; // JMP 0x004e6e25
        }
        wp.ai_train_source_state[unit_type] = TRAIN_SOURCE_READY; // 0x004e6dfd
        ++rep.marked;
        if (by_type[unit_type] == 0) continue;     // 0x004e6e05
        ++by_role[v.cfg_units[unit_type].ai_unit]; // 0x004e6e1b / 0x004e6e21
    }

    // Pass 3 -- the role with the fewest already queued, among roles with a live source.
    int32_t best_role  = -1;                    // 0x004e6e34
    int32_t best_count = TRAIN_ROLE_COUNT_SEED; // 0x004e6e3b
    for (uint32_t role = 0; role < (uint32_t)AI_UNIT_ROLE_COUNT; ++role) {
        if (by_role[role] == 0) continue; // 0x004e6e49
        ++rep.roles_live;
        const int32_t queued = pd.ai_train_queued_by_ai_unit[role]; // 0x004e6e68
        if ((uint32_t)queued >= (uint32_t)best_count) continue;     // JNC 0x004e6e71
        best_role  = (int32_t)role;
        best_count = queued;
    }
    rep.best_role = best_role;

    // Pass 4 -- within that role, the buildable type with the highest cfg ai_level.
    int32_t best_level = -1; // 0x004e6e7f
    int32_t best_unit  = 0;  // 0x004e6e84
    for (uint32_t t = 1; t <= (uint32_t)v.cfg_unit_sec->total; ++t) {
        if (pd.ai_train_source_state[t] == 0) continue; // 0x004e6e8d
        if (by_type[t] == 0) continue;                  // 0x004e6e97
        const cfg_unit &cu = v.cfg_units[t];
        if ((int32_t)cu.ai_unit != best_role) continue;   // 0x004e6eb3
        if (best_level >= (int32_t)cu.ai_level) continue; // JGE 0x004e6ebe, SIGNED
        best_unit  = (int32_t)t;
        best_level = (int32_t)cu.ai_level;
    }
    rep.best_unit = best_unit;
    if (best_unit == 0) return rep; // 0x004e6eec

    // The tail. queue once, then drain ai_start_units_remaining queueing the SAME type. Both the
    // test and the decrement re-read the counter from player_data every iteration (EBX is rebuilt
    // at 0x004e6f0a-0x004e6f1e), so a callee that moved it would be observed -- kept as written.
    (void)gc.queue_train_unit(player, (uint32_t)best_unit); // 0x004e6ef6, return ignored
    ++rep.queued;
    if (pd.ai_start_units_remaining != 0)                       // 0x004e6efb
        --wp.ai_start_units_remaining;                          // 0x004e6f04
    while (pd.ai_start_units_remaining != 0) {                  // 0x004e6f20
        (void)gc.queue_train_unit(player, (uint32_t)best_unit); // 0x004e6f32
        ++rep.queued;
        --wp.ai_start_units_remaining; // JMP back to 0x004e6f04
    }
    return rep;
}

} // namespace detail

void plan_unit_training(int32_t player) {
    const ai_state st = state();
    (void)detail::plan_unit_training(st.read, st.own, live_calls(), player);
}


} // namespace mh::ai
