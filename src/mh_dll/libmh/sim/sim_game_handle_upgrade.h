//
// sim/sim_game_handle_upgrade.h -- game_HandleUpgrade, the researched-upgrade stat-delta applier
// (RI-SIM / SIM1F, `review_required` in the ledger).
//
// One original function, game_HandleUpgrade @0x0044062a (0x68b bytes). Dispatches on
// Upgrades[upgrade_id].type: type==1 applies a UNIT stat delta (step_speed/turn_speed percent cut,
// armor_prob flat cut) to every Unit[] proto the upgrade's objects[16] table references for `player`;
// type==2 applies a WEAPON stat delta (range_min/missing percent cut, range_max/power/fire_range
// percent gain) to every Weapon[] slot it references. type==0 or type>2 does NOTHING -- see the
// .cpp banner: despite cfg_enum_E_UPGRADE_TYPE's UNIT member being 0, the assembly's own first branch
// (`CMP type,1; JC <return>`) returns immediately on type==0, so only type==1 actually runs the
// "unit" arm -- a real discrepancy from how the batch context doc's prose ("type<2 is the unit-stat
// arm") reads in isolation. When `player == PlayerSide` (the LOCAL player), both arms also build and
// print a "<prefix>: <category>: <name>[, <delta clause>]..." summary via
// utils_w_str_copy/utils_concat into G_TEXT_TMP and game_ui_PrintTextMessage -- on the "fires for
// real under shadow" list per the batch context doc (double-firing under a shadow rig run is
// expected, not a bug).
//
// FIRST WRITER to cfg_units/cfg_weapons in the whole 307-function sim migration set (see
// sim_state.h's `unit_mut()`/`weapon_mut()` comments) -- both cfg TYPE tables are otherwise read-only
// for every other sim function.
//
#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// Indirected the same way every other TU here does: a direct mh::call:: inside a detail:: body
// reaches into the live game image, making the body untestable by net_selftest.exe simtest. Both
// members mirror the ORIGINAL functions' own committed prototypes (addr/mh_calls.gen.h).
struct upgrade_calls {
    void *(*utils_w_str_copy)(void *src, void *dst);  // utils_w_str_copy @0x004d02d2, default cc
    void *(*utils_concat)(void *dst, void *src);      // utils_concat @0x004d02ea, __watcall
    uint32_t (*game_ui_PrintTextMessage)(void *text); // game_ui_PrintTextMessage @0x00496508, __watcall
};

const upgrade_calls &live_upgrade_calls();

namespace detail {

// game_HandleUpgrade @0x0044062a.
void apply_upgrade(const sim_view &v, sim_store &own, const upgrade_calls &c, uint32_t player,
                   int32_t upgrade_id);

} // namespace detail

// Live wrapper: the logic applied to state() and live_upgrade_calls(). Matches the original's
// committed __watcall(EAX=player, EDX=upgrade_id) shape (see the .asm's own param banner).
void handle_upgrade(uint32_t player, int32_t upgrade_id);

namespace detail {
} // namespace detail

} // namespace mh::sim
