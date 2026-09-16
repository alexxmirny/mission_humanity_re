//
// sim/sim_unit_target_tracking.h -- per-tick target tracking/engagement for a unit's PRIMARY and
// SECONDARY (target2) targets (RI-SIM / SIM1A).
//
// Three functions, one translation unit because the driver directly calls the target2 tracker:
//   llm_strat_unit_target_tick               @0x0047e065 (0x17c bytes) -- void(void), globals-driven
//   llm_strat_unit_update_target_tracking     @0x00448ee1 (0x181 bytes) -- int(player, unit_idx)
//   llm_strat_unit_update_target2_tracking    @0x00449062 (0x22d bytes) -- int(player, unit_idx)
//
// llm_strat_unit_target_tick is the per-tick DRIVER for the target2 engagement: it verifies target2
// is still alive (building or unit, per the target2_ref bit layout), re-tracks it via
// llm_strat_unit_update_target2_tracking (a LOCAL call -- both are defined in this TU, so no
// mh::call:: indirection for that one edge), then either fires or releases the target. It reads NO
// parameters -- everything comes from three globals the per-unit tick driver sets before calling it:
// _G_LLM_STRAT_CUR_UNIT / _G_LLM_STRAT_CUR_INDEX / _G_LLM_STRAT_CUR_PLAYER.
//
// update_target_tracking is the PRIMARY-target sibling of update_target2_tracking: same shape
// (fetch the target's fine coords, and for a guided weapon (Weapon.homing==2) run the lead/predict
// pair), different field pair (target_fine_x/y, target_ref/target_index vs target2_fine_x/y,
// target2_ref/target2_index). It is NOT called by target_tick in this batch -- it is reached
// elsewhere (e.g. llm_strat_unit_update_rotation, outside this batch), via mh::call:: there. Unlike
// update_target2_tracking, it does NOT re-check the target's energy -- it always runs and always
// returns 1.
//
// update_target2_tracking DOES re-check its own target's energy>0 before acting (release + clear
// target2_ref/target2_index and return 0 if not), and target_tick ALSO checks target2's energy
// before calling update_target2_tracking -- the two checks are genuinely redundant across the two
// call sites (target_tick's own alive-check gates whether it calls update_target2_tracking at all;
// update_target2_tracking then re-derives the same energy fact independently). Preserve the
// redundancy exactly, per the translator brief -- do not dedupe it into one check.
//
// ---- DECLARED NEED: sim_view has no members for the three "current tick" globals ------------------
// llm_strat_unit_target_tick reads _G_LLM_STRAT_CUR_UNIT (map_object_unit* @0x00e162e0),
// _G_LLM_STRAT_CUR_INDEX (ushort @0x00e58142), and _G_LLM_STRAT_CUR_PLAYER (game_t_Player_s, a
// 2-byte type per the MOVZX word access, @0x00e58144). NONE of the three are in mh_regions.gen.h
// today (checked: no RID_STRAT_CUR_* exists), so this file references view members that do not yet
// exist -- per the translator brief's "stop and declare it, do not work around it with an offset"
// (same move sim_unit_passive_engage.cpp made for the missing sim_store accessor). This TU will not
// compile until the conductor adds, to sim_state.h's sim_view and sim_state.cpp's state():
//   sim_view:
//     const uint16_t *cur_player; // _G_LLM_STRAT_CUR_PLAYER @0x00e58144 (game_t_Player_s, 2 bytes)
//     const uint16_t *cur_index;  // _G_LLM_STRAT_CUR_INDEX  @0x00e58142 (ushort)
//   sim_state.cpp, in state():
//     v.cur_player = ptr<const uint16_t>(RID_STRAT_CUR_PLAYER);
//     v.cur_index  = ptr<const uint16_t>(RID_STRAT_CUR_INDEX);
// backed by two NEW region-registry entries (RID_STRAT_CUR_PLAYER, RID_STRAT_CUR_INDEX) in
// mh_regions.gen.h, which this TU may not edit.
//
// CORRECTED (reimpl-verify 2026-08-10): llm_strat_unit_target_tick's own "current unit" is bound via
// `*v.cur_unit` (sim_view::cur_unit, landed alongside sim_store::cur_unit() by the rotation/soldiers
// translations in this same slice), matching those two siblings' choice for the identical triad --
// NOT via unit_of(v, *v.cur_player, *v.cur_index) as an earlier draft of this file assumed. The
// original's every field access on the current unit goes through _G_LLM_STRAT_CUR_UNIT directly
// (0x0047e07d et al.); CUR_PLAYER/CUR_INDEX are read only to pass to callees, never to address the
// current unit's own record. cur_player/cur_index remain real sim_view members (the callees below
// are keyed by them, and own.unit_at()/unit_of() still need player+index for OTHER units, e.g. the
// target2 record), just not for binding "u" itself.
//
#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// ---- the outward calls -----------------------------------------------------------------------
//
// Indirected for the same reason as every other module here (sim_unit_passive_engage.h,
// sim_order_enqueue.h, ...): a direct mh::call:: inside a detail:: body reaches into the live game
// image, which makes the body untestable by net_selftest.exe simtest. All seven are ORIGINAL
// functions outside this batch -- nothing here is a sibling reimplementation needing a stub/record
// variant, so live_unit_target_tracking_calls() is the only binder.
struct unit_target_tracking_calls {
    // llm_strat_unit_get_coords -- writes *out_x/*out_y (fine_coord).
    void (*unit_get_coords)(uint16_t player, int32_t unit_index, int32_t *out_x, int32_t *out_y);
    // llm_strat_weapon_pixel_distance_ratio -- x87/float10-precision return.
    double (*weapon_pixel_distance_ratio)(int32_t weapon_id, int32_t x1, int32_t y1, int32_t x2,
                                          int32_t y2);
    // llm_strat_unit_predict_coords_after_delay -- unused_ebx/unused_ecx are genuine Watcom
    // register-reuse artifacts the callee ignores (see the .cpp derivation); writes *out_x/*out_y.
    void (*unit_predict_coords_after_delay)(uint32_t player, int32_t unit_idx, uint32_t unused_ebx,
                                            uint32_t unused_ecx, double time_delta, uint32_t *out_x,
                                            uint32_t *out_y);
    // llm_strat_target_release_ref -- mode 3 in every call site this TU makes.
    void (*target_release_ref)(uint32_t player_idx, int32_t unit_idx, uint32_t mode);
    // llm_strat_target_class -- owner_and_kind_flag is the RAW (unmasked) packed ref.
    int32_t (*target_class)(uint32_t owner_and_kind_flag, int32_t roster_slot);
    // llm_strat_unit_in_weapon_range.
    uint32_t (*unit_in_weapon_range)(int32_t player, int32_t unit_idx, int32_t tile_x, int32_t tile_y,
                                     int32_t target_class);
    // llm_strat_unit_fire_at_target2_if_aimed -- no params, no return.
    void (*unit_fire_at_target2_if_aimed)();
};

const unit_target_tracking_calls &live_unit_target_tracking_calls();

namespace detail {

// llm_strat_unit_update_target_tracking @0x00448ee1. PRIMARY-target sibling of
// unit_update_target2_tracking below -- see the header comment for the field-pair difference. Always
// returns 1; no aliveness gate on the target.
int32_t unit_update_target_tracking(const sim_view &v, sim_store &own,
                                    const unit_target_tracking_calls &c, uint32_t player,
                                    int32_t unit_idx);

// llm_strat_unit_update_target2_tracking @0x00449062. Re-checks the SECONDARY target's own
// energy>0 first (ORDERED compare -- see the .cpp for the NaN-is-alive idiom, same shape as
// sim_unit_passive_engage.cpp's guard): if not alive, releases the ref (mode 3) and clears BOTH
// target2_ref and target2_index, returns 0. If alive, fetches the target's fine coords and, for a
// guided weapon (Weapon.homing==2), runs the lead/predict pair, then stores target2_fine_x/y and
// returns 1.
int32_t unit_update_target2_tracking(const sim_view &v, sim_store &own,
                                     const unit_target_tracking_calls &c, uint32_t player,
                                     int32_t unit_idx);

// llm_strat_unit_target_tick @0x0047e065. See the header comment for the full driver shape. Calls
// unit_update_target2_tracking directly (LOCAL call, same TU) when target2 is a unit target.
void unit_target_tick(const sim_view &v, sim_store &own, const unit_target_tracking_calls &c);

} // namespace detail

// Live wrappers: the logic applied to state() and live_unit_target_tracking_calls(). Match the
// originals' committed __watcall shapes.
int32_t unit_update_target_tracking(uint32_t player, int32_t unit_idx);
int32_t unit_update_target2_tracking(uint32_t player, int32_t unit_idx);
void    unit_target_tick();

namespace detail {
} // namespace detail

} // namespace mh::sim
