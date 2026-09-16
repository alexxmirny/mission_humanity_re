//
// sim/libtrans/sim_lt_available_remove.cpp -- see sim_lt_available_remove.h. Translated from the
// DISASSEMBLY (tmp/decomp_lib_trans/game_RemoveFromAvailableBuildings_004141e8.asm,
// game_RemoveFromAvailableProjects_0041428e.asm).
//
#include "sim/libtrans/sim_lt_available_remove.h"

#include "addr/mh_calls.gen.h"  // the untranslated original callee (Law 4)
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const lt_available_remove_calls &live_lt_available_remove_calls() {
    static const lt_available_remove_calls c = {
        MH_LIBMH_BIND(game_InsertItemInPlayerArray),
    };
    return c;
}

namespace {
// The Add siblings' own literal (0x0041420d / 0x004142d7: MOV EDX,0x32) -- 50 slots per row/bucket,
// declared per-TU like sim_game_add_to_available_buildings.cpp's copy (rule 17a fallback).
inline constexpr int32_t AVAILABLE_ROW_CAP = 0x32;
// The removal direction's new_item: the found `item` slot is overwritten with 0 (PUSH 0x0 at
// 0x00414205 / 0x004142cf) -- the exact mirror of the Add siblings' ITEM_UNUSED search key.
inline constexpr int32_t REMOVE_WRITES_ZERO = 0;
} // namespace

namespace detail {

void remove_from_available_buildings(sim_store &own, const lt_available_remove_calls &c,
                                     uint32_t player, int32_t b_i) {
    // 0x00414212-0x0041421e: row = AvailableBuildings + player*0xc8 -- the store's escape accessor.
    // 0x00414205/0x00414207-0x00414220: Insert(row, 50, player, ITEM=b_i, NEW=0).
    c.insert_item_in_player_array(own.available_buildings_row(player), AVAILABLE_ROW_CAP, player,
                                  b_i, REMOVE_WRITES_ZERO);
}

void remove_from_available_projects(const sim_view &v, sim_store &own,
                                    const lt_available_remove_calls &c, uint32_t player,
                                    uint32_t p_i) {
    // 0x004142ab-0x004142b2: Projects[p_i].type (the raw [p_i*0xd0 + 0xbe1c6c] read, through the
    // bound view -- cfg_final_struct_Project::type, the Add sibling's documented same-field read).
    const int32_t type = v.cfg_projects[p_i].type;
    // 0x004142bc-0x004142ca: bucket = AvailableProjects + player*0x190 + type*0xc8.
    // 0x004142cf-0x004142df: Insert(bucket, 50, player, ITEM=p_i, NEW=0).
    c.insert_item_in_player_array(own.available_projects_bucket(player, type), AVAILABLE_ROW_CAP,
                                  player, static_cast<int32_t>(p_i), REMOVE_WRITES_ZERO);
}

} // namespace detail

void remove_from_available_buildings(uint32_t player, int32_t b_i) {
    sim_state st = state();
    detail::remove_from_available_buildings(st.own, live_lt_available_remove_calls(), player, b_i);
}

void remove_from_available_projects(uint32_t player, uint32_t p_i) {
    sim_state st = state();
    detail::remove_from_available_projects(st.read, st.own, live_lt_available_remove_calls(), player,
                                           p_i);
}

} // namespace mh::sim
