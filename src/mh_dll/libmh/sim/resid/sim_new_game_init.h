//
// sim/resid/sim_new_game_init.h -- llm_strat_new_game_init @0x00455df0, the strategic new-game state
// wipe (RI-SIM sim_resid batch; session-entry-only writer, proof:OFFLINE).
//
// Called with no arguments (void __watcall llm_strat_new_game_init(void)) from `llm_strat_mode_init`
// (boot stage 4) and from the intra-slice sibling `llm_strat_session_state_reset`'s full-reset path
// (reset_flag 0 or 2), which calls it DIRECTLY -- see sim_session_state_reset.cpp's
// `new_game_init(v, own, live_new_game_init_calls())` call, and this batch's rule 2.
//
// ---- BODY, in assembly order (tmp/decomp_sim_resid/llm_strat_new_game_init_00455df0.asm) ----------
//
//   0x00455e08: game::ClearAvailableProjects().
//
//   0x00455e0d-0x00455ffc: PER-PLAYER LOOP, player = 0..7 (MAX_PLAYERS, `CMP [player],0x8; JL`):
//     * 0x00455e24-0x00455e39: llm_strat_player_profile_init(player, /*controller_flags*/0,
//       /*race*/0, /*game_clock*/0.0, /*color_index*/0, name_str, /*side_id*/-1). Register/stack
//       split confirmed against mh_calls.gen.h's committed signature
//       `(player, controller_flags, race, game_clock, color_index, name_str, side_id)`: EAX=player,
//       EDX=0, EBX=0 (the three register args), then the double 0.0 (two zero pushes), then the int
//       0, then name_str, then -1 (three more pushes, right-to-left). `name_str` is the raw address
//       0x0050109d (Ghidra auto-label `s__00501091+0xc`) -- see the DECLARED-NEED note below.
//     * 0x00455e3e-0x00455e97: PER-FIELD loop, row = 0..299 (PROGRESS_ROW_COUNT), THREE byte fields
//       explicitly zeroed one at a time (NOT a whole-record memset -- three separate `MOV byte ptr
//       ...,0x0` stores at +0/+1/+2 of each 3-byte record): `progress[player][row].available = 0`
//       (0x00455e64), `.acquired = 0` (0x00455e7a), `.f3 = 0` (0x00455e90).
//     * 0x00455e99-0x00455f5a: PER-FIELD loop, weapon = 0..31 (WEAPON_COUNT, the full Weapon[32]
//       table), copying FIVE fields from the baseline column [8] to the per-player column
//       [player] -- five separate field-loop bodies, not a record memcpy:
//         range_min[player] = range_min[8]   (int32,  0x00455ec6/0x00455ecc)
//         range_max[player] = range_max[8]   (int32,  0x00455ee8/0x00455eee)
//         missing[player]   = missing[8]     (int32,  0x00455f0a/0x00455f10)
//         power[player]     = power[8]       (double, 0x00455f2c/0x00455f32, x87 FLD/FSTP)
//         fire_range[player]= fire_range[8]  (double, 0x00455f4e/0x00455f54, x87 FLD/FSTP)
//     * 0x00455f5f-0x00455fd9: PER-FIELD loop, unit = 0..99 (UNIT_COUNT, the full Unit[100] table),
//       copying THREE fields from baseline column [8] to column [player]:
//         step_speed[player] = step_speed[8] (double, 0x00455f89/0x00455f8f, x87 FLD/FSTP)
//         turn_speed[player] = turn_speed[8] (double, 0x00455fab/0x00455fb1, x87 FLD/FSTP)
//         armor_prob[player] = armor_prob[8] (int32,  0x00455fcd/0x00455fd3)
//     * 0x00455fdb-0x00455ffc: slot = 0..9 (SHUTTLE_SLOTS_PER_PLAYER), calls
//       llm_strat_prod_shuttle_slot_release(player, slot) for every slot (register order confirmed
//       against mh_calls.gen.h: EAX=player, EDX=slot).
//
//   0x00456001: llm_strat_invasion_alert_reset_all().
//   0x00456006-0x00456012: utils_fill_data(&fog_of_war, sizeof(mh_map_struct_fog_of_war) [0x90000],
//     0) -- register order confirmed against mh_calls.gen.h's `(ptr, size, default_)` =
//     (EAX, EBX, EDX). This is a WHOLE-REGION clear of RID_FOG_OF_WAR (589824 bytes = both
//     `visible_by_count` and `discovered`), not a field-level write -- see the DECLARED-NEED note.
//   0x00456017: _G_LLM_STRAT_SAVE_MISC_DWORD = 0xf (sim_state.h's own `save_misc_dword()` comment
//     already names this exact function as the seeder).
//   0x0045602f: llm_map_set_zoom_scale(1.0, 1.0) (register order confirmed: both stack doubles,
//     pushed right-to-left, high-then-low halves, both literally 1.0 / 0x3ff00000).
//
// ---- DECLARED NEEDS (see the translator's structured report for the authoritative list) -----------
//
// (1) `name_str` @0x0050109d has NO named binding anywhere in mh_addrs.gen.h. Ghidra's auto-label is
//     `s__00501091+0xc` (i.e. offset 0xc into a blob Ghidra calls `s_wynalazek_00501091`). That same
//     base blob is read as TWELVE ASCII bytes (offset 0..0xb) by the sibling `llm_strat_tech_tables_
//     reset` (tmp/decomp_sim_resid/llm_strat_tech_tables_reset_00455b5a.c) AND as a RAW DOUBLE via
//     `FADD double ptr [0x0050109e]` (offset 0xd) by `map_FillDefaults`
//     (tmp/decomp_sim_resid/map_FillDefaults_0045603e.asm:182) -- three different interpretations of
//     overlapping byte ranges of the SAME blob, which is the signature of LINKER-PACKED separate
//     statics (cf. sim_state.h's `ui_widget` lattice comment), not one coherent array or string. This
//     translation therefore does NOT re-derive a string value: it needs a new `sim_view` scalar
//     (proposed `new_game_init_default_side_name`, `const char *`, bound at exactly 0x0050109d,
//     i.e. NOT at the blob's Ghidra-guessed base) so the call site names neither a literal VA nor a
//     byte offset. Conductor: please read the actual bytes at 0x0050109d before naming/typing this
//     one -- it may itself be one further field of a still-larger overlapping record.
// (2) No `sim_store` accessor exposes a raw base pointer/address for the WHOLE `RID_FOG_OF_WAR`
//     region (589824 bytes) -- only the two field-level, differently-indexed accessors
//     `fog_visible_by_count_at(x,y,player)` / `fog_discovered_at(x,y)` exist, and neither can serve
//     as the `dst` argument to a single whole-region `utils_fill_data` call. Proposed:
//     `void *fog_of_war_base()` on `sim_store`, an address-escape accessor in the same family as
//     `text_scratch()` (RID_FOG_OF_WAR is `OWN_READONLY` per mh_regions.gen.h from every OTHER
//     region-flag consumer's point of view, but this closure's own MF_MEASURED-covered write here is
//     exactly why `sim_view` already carries `MF_VIEW` for it -- see mh_addrs.gen.h's `fog_of_war`
//     comment).
//
#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

struct new_game_init_calls {
    void (*clear_available_projects)(); // game_ClearAvailableProjects @0x0041c02e
    void (*player_profile_init)(uint32_t player, uint32_t controller_flags, uint32_t race,
                                double game_clock, uint32_t color_index, char *name_str,
                                int32_t side_id); // llm_strat_player_profile_init @0x00454985
    void (*prod_shuttle_slot_release)(int32_t player,
                                      int32_t slot);                // llm_strat_prod_shuttle_slot_release @0x0046318d
    void (*invasion_alert_reset_all)();                             // llm_strat_invasion_alert_reset_all @0x0049b447
    void *(*fill_data)(void *ptr, uint32_t size, uint8_t default_); // utils_fill_data @0x004d1780
    void (*map_set_zoom_scale)(double zoom_x, double zoom_y);       // llm_map_set_zoom_scale @0x004a7e12
};

const new_game_init_calls &live_new_game_init_calls();

namespace detail {

// llm_strat_new_game_init @0x00455df0. Reads the baseline (column-8) Weapon/Unit stat overrides
// through `v`; rewrites the per-player profile roster, `progress[][]`, the per-player Weapon/Unit
// override columns, `fog_of_war`, and `save_misc_dword` through `own`; reaches the frontier
// originals through `c`. void return, matching the original.
void new_game_init(const sim_view &v, sim_store &own, const new_game_init_calls &c);

} // namespace detail

// Live wrapper: the logic applied to state() and live_new_game_init_calls().
void new_game_init();

} // namespace mh::sim
