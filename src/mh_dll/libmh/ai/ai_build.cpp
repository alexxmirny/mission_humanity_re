//
// ai/ai_build.cpp -- see ai_build.h.
//
// Written as 21 explicit statements rather than a table loop. The original is 21 unrolled blocks
// with no shared control flow, the destinations are 21 differently-typed named fields (a uint32, an
// int32[4] element, four fields of the NEXT player's record), and a reviewer's job here is to check
// the (category, human type, alien type, destination) tuple line by line against the disassembly.
// A table would need a member-pointer scheme that makes that comparison harder, not easier.
//
#include "ai/ai_build.h"


namespace mh::ai {
namespace detail {

namespace {

// `CMP dword ptr [player_data + 0x104c0],0 / JZ human`. Re-read per lookup, exactly as the original
// does -- it reloads the field before each of the 21 blocks rather than hoisting it.
uint32_t race_pick(const ai_view &v, int32_t player, uint32_t human, uint32_t alien) {
    return v.players[player].is_alien_race == 0 ? human : alien;
}

} // namespace

void init_build_candidate_priorities(const ai_view &v, const ai_store &own, const ai_calls &gc,
                                     int32_t player) {
    player_data &me = own.players[player];
    // THE +1 ALIASING. See the header: this is the original's behaviour and seven readers depend on
    // it. For player 7 it addresses memory past the end of the array, deliberately.
    player_data &next = own.players[player + 1];

    me.ai_mother_building_type      = gc.bldg_find_by_ai_build_and_type(player, 1, race_pick(v, player, 0x1a, 6));
    me.ai_build_candidate_primary   = (int32_t)gc.bldg_find_by_ai_build_and_type(player, 2, race_pick(v, player, 0x18, 4));
    me.ai_build_candidate_secondary = (int32_t)gc.bldg_find_by_ai_build_and_type(player, 3, race_pick(v, player, 0x17, 3));

    me.ai_mine_alt_candidates[0] = (int32_t)gc.bldg_find_by_ai_build_and_type(player, 0x40, race_pick(v, player, 0x19, 5));
    me.ai_mine_alt_candidates[1] = (int32_t)gc.bldg_find_by_ai_build_and_type(player, 0x41, race_pick(v, player, 0x19, 5));
    me.ai_mine_alt_candidates[2] = (int32_t)gc.bldg_find_by_ai_build_and_type(player, 0x42, race_pick(v, player, 0x19, 5));
    me.ai_mine_alt_candidates[3] = (int32_t)gc.bldg_find_by_ai_build_and_type(player, 0x43, race_pick(v, player, 0x19, 5));

    next.ai_housing_candidate_heli    = (int32_t)gc.bldg_find_by_ai_build_and_type(player, 0x22, race_pick(v, player, 0x1e, 10));
    next.ai_housing_candidate_plane   = (int32_t)gc.bldg_find_by_ai_build_and_type(player, 0x23, race_pick(v, player, 0x1d, 9));
    next.ai_housing_candidate_vehicle = (int32_t)gc.bldg_find_by_ai_build_and_type(player, 0x21, race_pick(v, player, 0x1b, 7));
    next.ai_housing_candidate_soldier = (int32_t)gc.bldg_find_by_ai_build_and_type(player, 0x20, race_pick(v, player, 0x1c, 8));

    me.ai_turret_candidate         = (int32_t)gc.bldg_find_by_ai_build_and_type(player, 5, race_pick(v, player, 0x23, 0xf));
    me.ai_build_candidate_shortage = (int32_t)gc.bldg_find_by_ai_build_and_type(player, 4, race_pick(v, player, 0x24, 0x10));

    // 0x31 and 0x30 share the same race-selected type constant; only the category differs.
    me.ai_build_candidate_cat_0x31 = (int32_t)gc.bldg_find_by_ai_build_and_type(player, 0x31, race_pick(v, player, 0x1f, 0xb));
    me.ai_build_candidate_cat_0x30 = (int32_t)gc.bldg_find_by_ai_build_and_type(player, 0x30, race_pick(v, player, 0x1f, 0xb));

    me.ai_mine_candidate_tier1 = (int32_t)gc.bldg_find_by_ai_build_and_type(player, 0x50, race_pick(v, player, 0x16, 2));
    me.ai_mine_candidate_tier2 = (int32_t)gc.bldg_find_by_ai_build_and_type(player, 0x51, race_pick(v, player, 0x16, 2));

    me.ai_resource_shortage_candidates[0] = (int32_t)gc.bldg_find_by_ai_build_and_type(player, 0x10, race_pick(v, player, 0x15, 1));
    me.ai_resource_shortage_candidates[1] = (int32_t)gc.bldg_find_by_ai_build_and_type(player, 0x11, race_pick(v, player, 0x15, 1));
    me.ai_resource_shortage_candidates[2] = (int32_t)gc.bldg_find_by_ai_build_and_type(player, 0x12, race_pick(v, player, 0x15, 1));
    me.ai_resource_shortage_candidates[3] = (int32_t)gc.bldg_find_by_ai_build_and_type(player, 0x13, race_pick(v, player, 0x15, 1));
}

} // namespace detail

void init_build_candidate_priorities(int32_t player) {
    const ai_state st = state();
    detail::init_build_candidate_priorities(st.read, st.own, live_calls(), player);
}


} // namespace mh::ai
