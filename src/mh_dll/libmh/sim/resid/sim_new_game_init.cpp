//
// sim/resid/sim_new_game_init.cpp -- see sim_new_game_init.h. Translated from the DISASSEMBLY
// (tmp/decomp_sim_resid/llm_strat_new_game_init_00455df0.asm), the Ghidra .c being a draft.
//
#include "sim/resid/sim_new_game_init.h"

#include "addr/mh_calls.gen.h" // typed callables for the frontier originals we call OUT to
#include "state/host_api.h"
#include "state/host_events.h"
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"
#include "crt/crt_select.h" // LIB-CRT: MH_CRT() picks the vendored CRT in the standalone build

namespace mh::sim {

const new_game_init_calls &live_new_game_init_calls() {
    static const new_game_init_calls c = {
        MH_LIBMH_BIND(game_ClearAvailableProjects),
        MH_LIBMH_BIND(llm_strat_player_profile_init),
        MH_LIBMH_BIND(llm_strat_prod_shuttle_slot_release),
        MH_LIBMH_BIND(llm_strat_invasion_alert_reset_all),
        MH_CRT(utils_fill_data),
        mh::state::evt::view_zoom_scale,
    };
    return c;
}

namespace {

inline constexpr int32_t MAX_WEAPONS              = 0x20; // Weapon[32], the full table
inline constexpr int32_t MAX_UNITS                = 0x64; // Unit[100], the full table
inline constexpr int32_t SHUTTLE_SLOTS_PER_PLAYER = 0xa;  // llm_strat_prod_shuttle_slot_release loop bound
inline constexpr int32_t BASELINE_STAT_COLUMN     = 8;    // the Weapon/Unit per-type "master" column
                                                          // every per-player column is copied FROM
inline constexpr uint32_t SAVE_MISC_DWORD_NEW_GAME = 0xf; // 0x00456017 literal

} // namespace

namespace detail {

// ---- llm_strat_new_game_init @0x00455df0 -----------------------------------------------------------
void new_game_init(const sim_view &v, sim_store &own, const new_game_init_calls &c) {
    // 0x00455e08.
    c.clear_available_projects();

    // 0x00455e0d-0x00455ffc: per-player loop, player = 0..MAX_PLAYERS-1.
    for (uint32_t player = 0; player < static_cast<uint32_t>(MAX_PLAYERS); ++player) {
        // 0x00455e24-0x00455e39: llm_strat_player_profile_init(player, controller_flags=0, race=0,
        // game_clock=0.0, color_index=0, name_str, side_id=-1). `name_str` is the raw address
        // 0x0050109d (Ghidra auto-label `s__00501091+0xc`) -- see the header's DECLARED-NEED (1).
        // Using v.empty_name_str once the conductor binds it.
        c.player_profile_init(player, 0, 0, 0.0, 0,
                              const_cast<char *>(v.empty_name_str), -1);

        // 0x00455e3e-0x00455e97: PER-FIELD zero, row = 0..PROGRESS_ROW_COUNT-1 -- three separate
        // byte stores (available/acquired/f3), NOT a whole-record memset (see header banner).
        for (int32_t row = 0; row < PROGRESS_ROW_COUNT; ++row) {
            player_progress &p = own.progress_at(player, row);
            p.available        = 0; // 0x00455e64
            p.acquired         = 0; // 0x00455e7a
            p.f3               = 0; // 0x00455e90
        }

        // 0x00455e99-0x00455f5a: PER-FIELD copy of the baseline (column 8) Weapon stats down to this
        // player's own column, weapon = 0..MAX_WEAPONS-1. Five separate field copies, not a record
        // memcpy -- see header banner for the per-field addresses.
        for (int32_t weapon = 0; weapon < MAX_WEAPONS; ++weapon) {
            const cfg_weapon &src  = v.cfg_weapons[weapon];
            cfg_weapon       &dst  = own.weapon_mut(static_cast<uint32_t>(weapon));
            dst.range_min[player]  = src.range_min[BASELINE_STAT_COLUMN];  // 0x00455ec6/0x00455ecc
            dst.range_max[player]  = src.range_max[BASELINE_STAT_COLUMN];  // 0x00455ee8/0x00455eee
            dst.missing[player]    = src.missing[BASELINE_STAT_COLUMN];    // 0x00455f0a/0x00455f10
            dst.power[player]      = src.power[BASELINE_STAT_COLUMN];      // 0x00455f2c/0x00455f32 (x87)
            dst.fire_range[player] = src.fire_range[BASELINE_STAT_COLUMN]; // 0x00455f4e/0x00455f54 (x87)
        }

        // 0x00455f5f-0x00455fd9: PER-FIELD copy of the baseline (column 8) Unit stats, unit =
        // 0..MAX_UNITS-1. Three separate field copies, not a record memcpy.
        for (int32_t unit_id = 0; unit_id < MAX_UNITS; ++unit_id) {
            const cfg_unit &src    = v.cfg_units[unit_id];
            cfg_unit       &dst    = own.unit_mut(static_cast<uint32_t>(unit_id));
            dst.step_speed[player] = src.step_speed[BASELINE_STAT_COLUMN]; // 0x00455f89/0x00455f8f (x87)
            dst.turn_speed[player] = src.turn_speed[BASELINE_STAT_COLUMN]; // 0x00455fab/0x00455fb1 (x87)
            dst.armor_prob[player] = src.armor_prob[BASELINE_STAT_COLUMN]; // 0x00455fcd/0x00455fd3
        }

        // 0x00455fdb-0x00455ffc: release all SHUTTLE_SLOTS_PER_PLAYER shuttle slots for this player.
        for (int32_t slot = 0; slot < SHUTTLE_SLOTS_PER_PLAYER; ++slot) {
            c.prod_shuttle_slot_release(static_cast<int32_t>(player), slot);
        }
    }

    // 0x00456001.
    c.invasion_alert_reset_all();

    // 0x00456006-0x00456012: WHOLE-REGION clear of RID_FOG_OF_WAR (589824 bytes, both
    // `visible_by_count` and `discovered`) -- see the header's DECLARED-NEED (2). Using
    // own.fog_of_war_base() once the conductor binds it.
    c.fill_data(own.fog_of_war_base(), sizeof(mh::game::mh_map_struct_fog_of_war), 0);

    // 0x00456017.
    own.save_misc_dword() = static_cast<int32_t>(SAVE_MISC_DWORD_NEW_GAME);

    // 0x0045602f.
    c.map_set_zoom_scale(1.0, 1.0);
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

void new_game_init() {
    sim_state st = state();
    detail::new_game_init(st.read, st.own, live_new_game_init_calls());
}

} // namespace mh::sim
