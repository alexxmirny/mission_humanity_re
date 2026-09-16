//
// ai/ai_build_score.cpp -- see ai_build_score.h. Translated from the DISASSEMBLY
// (tmp/decomp_ai/llm_strat_ai_score_build_categories_004e54cc.asm), not from Ghidra's C: the
// decompile's `extraout_ECX*` chain is Ghidra losing track of ECX (the player_data-stride base,
// re-derived by the SAME SHL/ADD/SUB idiom before every store) across each intervening CALL, and it
// mis-decompiles the resulting offsets as reaches through `ai_opponent_assessments`/`ai_build_plan`
// that do not exist in the real field layout. The store targets below come from the field-address
// table in the task brief (verified against addr/mh_structs.gen.h's committed offsets), not from
// that decompile.
//
#include "ai/ai_build_score.h"


namespace mh::ai {
namespace detail {

void score_build_categories(const ai_view &v, const ai_store &own, const ai_calls &gc,
                            int32_t player) {
    player_data &p = own.players[player];

    // The nine llm_strat_ai_bldg_count_by_category calls, in the LISTING'S order
    // (0x004e54dc-0x004e5589), which is not numeric order: 0x23 comes before 0x22. Do not sort it.
    p.ai_score_cat_1    = gc.bldg_count_by_category(player, 1);
    p.ai_score_cat_0x20 = gc.bldg_count_by_category(player, 0x20);
    p.ai_score_cat_0x21 = gc.bldg_count_by_category(player, 0x21);
    p.ai_score_cat_0x23 = gc.bldg_count_by_category(player, 0x23);
    p.ai_score_cat_0x22 = gc.bldg_count_by_category(player, 0x22);
    p.ai_score_cat_0x10 = gc.bldg_count_by_category(player, 0x10);
    p.ai_score_cat_0x11 = gc.bldg_count_by_category(player, 0x11);
    p.ai_score_cat_0x12 = gc.bldg_count_by_category(player, 0x12);
    p.ai_score_cat_0x13 = gc.bldg_count_by_category(player, 0x13);

    // The TURRET pair (0x004e5596-0x004e5601): race_turret_type picks 5 (alien) / 0x19 (human),
    // matching ai_state.h's BLDG_TYPE_A_TURRET/H_TURRET. The original re-tests is_alien_race a
    // second time (0x004e55c8) for the queue-pending call's own type argument rather than reusing a
    // register across the intervening call -- the SAME player, just recomputed (task hazard #4), so
    // one read here is equivalent since nothing writes is_alien_race in between.
    const uint8_t turret_type = race_turret_type(v.players[player].is_alien_race);
    p.ai_score_bldg_type_a    = gc.bldg_count_by_type(player, turret_type) +
                             gc.bldg_type_queue_has_pending(player, turret_type);

    p.ai_score_cat_0x30 = gc.bldg_count_by_category(player, 0x30);

    // The MINE pair (0x004e5615-0x004e567e): race_mine_type picks 2 (alien) / 0x16 (human), matching
    // BLDG_TYPE_A_MINE/H_MINE. Same shape as the turret pair above.
    const uint8_t mine_type = race_mine_type(v.players[player].is_alien_race);
    p.ai_score_bldg_type_b  = gc.bldg_count_by_type(player, mine_type) +
                             gc.bldg_type_queue_has_pending(player, mine_type);

    // The original's tail POP*4/RET is the ordinary epilogue -- it means `return`.
}

// llm_strat_ai_player_score_tier @0x004e5686, translated from the DISASSEMBLY
// (tmp/decomp/llm_strat_ai_player_score_tier_004e5686.asm), NOT from Ghidra's .c. Ghidra's .c folds
// this into one big short-circuit && expression with comma-operator side effects
// (`iVar1 = N, cond`), and a first reading of it is a trap: each `iVar1 = N` assignment fires as soon
// as the PREVIOUS tier's own gate is satisfied, one field BEFORE the field that gates entry to the
// NEXT tier -- so the naive grouping (bundle each `MOV EDX,N` with the comparison syntactically next
// to it in the .c) is off by one field in every tier from 2 up. Read directly off the five `MOV
// EDX,N` immediates and the CMP each is sandwiched between (0x004e56c0/0x004e56d7/0x004e56e5/
// 0x004e56f3/0x004e5713), the real gates are:
//   tier 1 <- c10 && c20                        (0x004e56aa/0x004e56b7, MOV EDX,1 BEFORE testing c21)
//   tier 2 <- tier1 && c21 && c11                (0x004e56c5/0x004e56ce, MOV EDX,2 BEFORE testing c30)
//   tier 3 <- tier2 && c30                       (0x004e56dc, MOV EDX,3 BEFORE testing c12)
//   tier 4 <- tier3 && c12                       (0x004e56ea, MOV EDX,4 BEFORE testing c13)
//   tier 5 <- tier4 && c13 && c23 && c22         (0x004e56f8/0x004e5701/0x004e570a, MOV EDX,5 LAST)
// i.e. c30 gates tier 3 (not tier 2, despite sitting next to c21/c11 in the .c's nesting), and c12
// gates tier 4 alone (not bundled with c13/c23). Getting this wrong makes the function return a LOWER
// tier than the original whenever a later-tier field is set out of order relative to an
// earlier-but-differently-grouped one (e.g. c21&&c11 true, c30 false: the original still returns 2,
// a naive grouping that requires c30 for tier 2 returns 1).
int32_t player_score_tier(const ai_view &v, int32_t player_idx) {
    const player_data &p = v.players[player_idx];

    int32_t tier = 0;
    if (p.ai_score_cat_0x10 != 0 && p.ai_score_cat_0x20 != 0) {
        tier = 1;
        if (p.ai_score_cat_0x21 != 0 && p.ai_score_cat_0x11 != 0) {
            tier = 2;
            if (p.ai_score_cat_0x30 != 0) {
                tier = 3;
                if (p.ai_score_cat_0x12 != 0) {
                    tier = 4;
                    if (p.ai_score_cat_0x13 != 0 && p.ai_score_cat_0x23 != 0 &&
                        p.ai_score_cat_0x22 != 0) {
                        tier = 5;
                    }
                }
            }
        }
    }
    return tier;
}

// llm_strat_ai_bldg_count_by_category @0x004d370a, translated from the DISASSEMBLY
// (tmp/decomp/llm_strat_ai_bldg_count_by_category_004d370a.asm), which matches Ghidra's own .c
// exactly here (checked instruction-by-instruction; no discrepancy). Two independent walks, summed:
//
// (1) the roster (0x004d3742-0x004d379b): `remaining` seeds from buildings[player][0].index and the
// loop index starts at 0 (`XOR ECX,ECX`) and is NOT reset to 1 the way the sibling roster walks in
// this cluster are -- verified against the raw bytes, not assumed from precedent. building_of(v,
// player, 0)'s own building_id is never a real building, so starting at 0 rather than 1 makes no
// observable difference; it is reproduced as written rather than "corrected" to match its siblings.
//
// (2) the AI build queue (0x004d379f-0x004d37e9): a plain indexed `for` over
// [0, ai_bldg_queue_count), no remaining-count sentinel, testing status == 1 (kind 1 = construction)
// before reading the queued building type.
int32_t bldg_count_by_category(const ai_view &v, int32_t player, uint32_t category) {
    int32_t count = 0;

    uint32_t remaining = (uint16_t)building_of(v, (uint32_t)player, 0).index;
    int32_t  idx       = 0;
    while (remaining != 0) {
        const building &b = building_of(v, (uint32_t)player, idx);
        if (b.building_id != 0) {
            if ((v.cfg_buildings[b.building_id].ai_build & 0xfffu) == category) ++count;
            --remaining;
        }
        ++idx;
    }

    const player_data &pd = v.players[player];
    for (int32_t i = 0; i < pd.ai_bldg_queue_count; ++i) {
        const auto &qe = pd.ai_bldg_queue[i];
        if (qe.status == 1 && (v.cfg_buildings[qe.tick_or_unit_id].ai_build & 0xfffu) == category)
            ++count;
    }

    return count;
}

} // namespace detail

void score_build_categories(int32_t player) {
    const ai_state st = state();
    detail::score_build_categories(st.read, st.own, live_calls(), player);
}

int32_t player_score_tier(int32_t player_idx) {
    const ai_state st = state();
    return detail::player_score_tier(st.read, player_idx);
}

int32_t bldg_count_by_category(int32_t player, uint32_t category) {
    const ai_state st = state();
    return detail::bldg_count_by_category(st.read, player, category);
}


} // namespace mh::ai
