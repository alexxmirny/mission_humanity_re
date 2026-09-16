//
// sim/resid/sim_player_init.cpp -- see sim_player_init.h. Translated from the DISASSEMBLY
// (tmp/decomp_sim_resid/llm_strat_player_profile_init_00454985.asm,
// llm_strat_player_param_defaults_init_00455a84.asm,
// llm_strat_init_human_player_data_004dd91d.asm); the Ghidra .c files beside them are drafts.
//
#include "sim/resid/sim_player_init.h"

#include "addr/mh_calls.gen.h" // typed callables for the effectful/frontier originals we still call OUT to
#include "state/host_api.h"
#include "state/host_events.h"
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const player_init_calls &live_player_init_calls() {
    static const player_init_calls c = {
        mh::state::evt::player_set_color,
        MH_LIBMH_BIND(llm_strat_ai_init_build_candidate_priorities),
    };
    return c;
}

namespace detail {

// ---- llm_strat_player_profile_init @0x00454985 -----------------------------------------------
void player_profile_init(sim_store &own, const player_init_calls &c, int32_t player,
                         uint32_t controller_flags, uint32_t race, double game_clock,
                         uint32_t color_index, char *name_str, int32_t side_id) {
    player_profile &profile = own.profile_at(player);

    // 0x004549bc-0x004549d2: copy name_str -> profile.name (offset 0x714). The original batches two
    // bytes per iteration (Watcom codegen); a plain byte-at-a-time copy here writes the identical
    // bytes to the identical addresses, one read fewer past the terminator, so it cannot diverge.
    // PRESERVE-BUG: UNBOUNDED -- no clamp to the 32-byte name[] array. Because
    // mh_llm_strat_player_profile's field order matches memory order (static_assert'd in
    // mh_structs.gen.h), a name_str longer than the record's trailing fields would corrupt
    // prod_check_clock/side_id/the next player's status_flags here exactly as it would in the
    // original -- reproduced with a raw pointer walk rather than a bounds-checked copy.
    {
        int32_t i = 0;
        for (;;) {
            char b          = name_str[i];
            profile.name[i] = b;
            if (b == '\0') break;
            ++i;
        }
    }

    // 0x004549d4-0x004549f6: strlen(profile.name) via the freshly written copy (REPNE SCASB in the
    // original -- same computed length here).
    int32_t name_len = 0;
    while (profile.name[name_len] != '\0') ++name_len;

    // 0x004549f6-0x00454a40: names longer than 12 chars truncate to the first 9 chars + "...". Matches
    // mh_structs.gen.h's own field comment on `name`. The suffix source is the string at 0x00501039
    // (Ghidra auto-label `s_..._00501039` -- see the header's declared_needs: content inferred from
    // the label's own encoded preview, not independently confirmed by a memory read).
    if (name_len > 12) {
        profile.name[9]    = '\0'; // 0x004549ff
        const char *suffix = "...";
        int32_t     j      = 9;
        for (int32_t k = 0;; ++k, ++j) {
            char b          = suffix[k];
            profile.name[j] = b;
            if (b == '\0') break;
        }
    }

    // 0x00454a41-0x00454a6b: plain field stores, address-derived (see header banner).
    profile.status_flags   = controller_flags;                           // offset 0x0
    profile.race           = race;                                       // offset 0x4
    profile.color_index    = static_cast<int32_t>(color_index);          // offset 0x8
    profile.flag_sprite_id = static_cast<int32_t>(color_index) + 0x453f; // offset 0xc; 0x453f is an
                                                                         // immediate operand, not data

    // 0x00454a86-0x00454a8c
    c.player_set_color(player, color_index);

    // 0x00454a91-0x00454ac5: no x87 involved (see header banner) -- plain assignments reproduce the
    // same bits.
    profile.prod_check_clock   = game_clock; // offset 0x734
    profile.mother_established = 0;          // offset 0x190
    profile.side_id            = side_id;    // offset 0x73c

    // 0x00454acb-0x00454bf5: PRESERVE-BUG -- loop runs i = 1..31, NOT 0..31. Index 0 of every one of
    // these eleven arrays is left untouched by this call (see header banner / mh_structs.gen.h field
    // comments). Reproduced literally, not "fixed".
    for (int32_t i = 1; i < 32; ++i) {
        profile.prod_queue_slot[i]        = 0;
        profile.primary_mother_bldg[i]    = 0;
        profile.primary_mother_unit[i]    = 0;
        profile.units_alive[i]            = 0;
        profile.buildings_alive[i]        = 0;
        profile.units_built_total[i]      = 0;
        profile.buildings_built_total[i]  = 0;
        profile.units_lost_total[i]       = 0;
        profile.buildings_lost_total[i]   = 0;
        profile.units_killed_total[i]     = 0;
        profile.buildings_killed_total[i] = 0;
    }
}

// ---- llm_strat_player_param_defaults_init @0x00455a84 -----------------------------------------
void player_param_defaults_init(sim_store &own) {
    // 0x00455aa3-0x00455afe: for player = 0..7 inclusive (starts at 0 -- no off-by-one in this
    // function), set three parallel double[8] slots. NO x87 in this body -- the original writes each
    // double's raw bit pattern via two 32-bit immediate MOVs; a plain double-literal assignment
    // reproduces the identical bits (10.0 = 0x4024000000000000, -1.0 = 0xbff0000000000000,
    // 1.0 = 0x3ff0000000000000, all read directly off the FMUL-free immediate operands).
    for (int32_t player = 0; player < 8; ++player) {
        own.net_peer_horizon_at(player)                                = 10.0; // 0x00455ab6/ba
        own.net_peer_horizon_pending_at(player)                        = -1.0; // 0x00455ad0/da
        own.game_speed_player_factor_at(static_cast<uint32_t>(player)) = 1.0;  // 0x00455aea/f4
    }
}

// ---- llm_strat_init_human_player_data @0x004dd91d ---------------------------------------------
void init_human_player_data(const sim_view &v, sim_store &own, const player_init_calls &c,
                            uint32_t player_idx, int32_t is_alien_race) {
    // 0x004dd933-0x004dd93c: raise the active-player high-water mark to player_idx+1 if it is
    // currently <= player_idx (UNSIGNED compare -- JC skips the raise).
    if (static_cast<uint32_t>(own.ai_active_player_count()) <= player_idx) {
        own.ai_active_player_count() = static_cast<int32_t>(player_idx) + 1;
    }

    player_data &pd = own.player_at(player_idx);

    // 0x004dd94d-0x004dd993: plain field stores, address-derived (see header banner).
    pd.is_alien_race        = is_alien_race; // offset 0x104c0
    pd.ai_enabled           = 0;             // offset 0x18
    pd.ai_group_count       = 0;             // offset 0x10564
    pd.next_group_serial    = 0;             // offset 0x10560
    pd.ai_target_list_count = 0;             // offset 0x25228
    pd.ai_bldg_queue_count  = 0;             // offset 0x2572c
    pd.ai_clock             = 0.001f;        // offset 0x1003c (bit pattern 0x3a83126f)

    // 0x004dd9a8-0x004dd9e6: ai_clock_{m,t,s} = (float)player_idx * PERIOD * STAGGER, staggering each
    // AI player's think-phase clocks by player_idx/8 of a period. x87 (see header banner): FILD/FST
    // snapshot player_idx as a float local, then each product is independently FMUL'd by its period
    // (float) then the stagger fraction (double) on the 80-bit FPU stack before FSTP truncates to
    // float. sim TUs build /arch:IA32 /fp:precise specifically so this natural float*float*double
    // expression keeps the same extended-precision intermediate the original x87 code does -- see
    // uncertainties[] for the caveat.
    float player_as_float = static_cast<float>(player_idx);
    pd.ai_clock_m         = player_as_float * (*v.ai_move_period) * (*v.ai_clock_stagger_fraction);     // offset 0x10048
    pd.ai_clock_t         = player_as_float * (*v.ai_tactic_period) * (*v.ai_clock_stagger_fraction);   // offset 0x10044
    pd.ai_clock_s         = player_as_float * (*v.ai_strategy_period) * (*v.ai_clock_stagger_fraction); // offset 0x10040

    // 0x004dd9ed-0x004dda03
    pd.ai_attack_orders_issued  = 0; // offset 0x2c
    pd.ai_map_changed_pending   = 0; // offset 0x34
    pd.ai_turret_rescan_pending = 0; // offset 0x38

    // 0x004dda10-0x004dda4b: ai_player_relation[i] = (i == player_idx) ? 1 : -1, for i in [0, 8) --
    // starts at 0, full 8 slots, no off-by-one here (unlike player_profile_init's loop above).
    for (int32_t i = 0; i < 8; ++i) {
        pd.ai_player_relation[i] = (i == static_cast<int32_t>(player_idx)) ? 1 : -1;
    }

    // 0x004dda4d-0x004dda4f
    c.ai_init_build_candidate_priorities(static_cast<int32_t>(player_idx));
}

} // namespace detail

// ---- the public wrappers --------------------------------------------------------------------------

void player_profile_init(int32_t player, uint32_t controller_flags, uint32_t race, double game_clock,
                         uint32_t color_index, char *name_str, int32_t side_id) {
    sim_state st = state();
    detail::player_profile_init(st.own, live_player_init_calls(), player, controller_flags, race,
                                game_clock, color_index, name_str, side_id);
}

void player_param_defaults_init() {
    sim_state st = state();
    detail::player_param_defaults_init(st.own);
}

void init_human_player_data(uint32_t player_idx, int32_t is_alien_race) {
    sim_state st = state();
    detail::init_human_player_data(st.read, st.own, live_player_init_calls(), player_idx,
                                   is_alien_race);
}


} // namespace mh::sim
