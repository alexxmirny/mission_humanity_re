//
// ai/ai_players_tick.cpp -- see ai_players_tick.h. Translated from the DISASSEMBLY
// (tmp/decomp_ai/llm_strat_ai_players_tick_004db905.asm), not from Ghidra's C draft, per
// translator-brief rule 1.
//
#include "ai/ai_players_tick.h"

namespace mh::ai {
namespace detail {

// Outward calls go through the shared module-wide `ai_calls` (ai_state.h), not a per-unit struct --
// see the header banner's 2026-08-28 correction. `gc.player_tick` / `gc.ai_unit_group_tick` /
// `gc.active_unit_tick` / `gc.passive_engage_tick` are this function's four dispatch edges;
// `gc.recompute_map_influence` / `gc.turret_threat_rescan` are the two loop-1 trigger edges.
void players_tick(const ai_view &v, const ai_store &own, const ai_calls &gc, double dt) {
    // ---- step 0: re-stamp the torus wrap masks from the current map extents (0x004db914-0x004db925)
    // NOT AI-owned (see the header banner / ai_store::map_width_mask's own comment) -- this function
    // is simply one of three writers, re-deriving `extent - 1` every AI tick like the others do.
    *own.map_width_mask  = static_cast<uint32_t>(*v.map_width - 1);
    *own.map_height_mask = static_cast<uint32_t>(*v.map_height - 1);

    // ---- loop 1 (0x004db92e-0x004db98d): map-influence / turret-rescan one-shot triggers --------
    // Loop bound RE-READ every iteration (0x004db985) -- never hoisted into a local, matching the
    // original and translator-brief's "do not cache the loop bound" rule.
    for (int32_t p = 0; p < *v.active_player_count; ++p) {
        player_data &pd = own.players[p]; // player_data IS the AI's own store -- see ai_state.h

        // 0x004db944-0x004db950: `TEST dword [ai_map_changed_pending], ai_enabled` -- a BITWISE AND
        // of the two player_data dwords, not a logical `&&`. `ai_enabled` is RELOADED at this site
        // and again at the second test below (0x004db96f) -- not cached across the two tests either.
        if ((pd.ai_map_changed_pending & pd.ai_enabled) != 0) {
            gc.recompute_map_influence(p); // 0x004db954
        }
        // 0x004db96f-0x004db97b: the same bitwise-AND shape against ai_turret_rescan_pending.
        if ((pd.ai_turret_rescan_pending & pd.ai_enabled) != 0) {
            gc.turret_threat_rescan(static_cast<uint32_t>(p)); // 0x004db97f
        }
    }

    // ---- loop 2 (0x004db991-0x004db9b7): ai_clock += dt, x87-shaped -------------------------------
    // `FLD float [ai_clock] / FADD double [dt] / FSTP float [ai_clock]`: dt is a genuine DOUBLE
    // addend, so the accumulate happens in a double-wide intermediate and is rounded to float in ONE
    // step on the store -- reproduced as `(float)((double)ai_clock + dt)`, NOT
    // `ai_clock + (float)dt` (which would round dt to float first and is a different answer). See
    // the header banner's FP section.
    for (int32_t p = 0; p < *v.active_player_count; ++p) {
        player_data &pd = own.players[p];
        pd.ai_clock     = static_cast<float>(static_cast<double>(pd.ai_clock) + dt); // 0x004db9a5-0x004db9af
    }

    // ---- loop 3 (0x004db9c3-0x004dba25): STRATEGY catch-up on ai_clock_s --------------------------
    // Per player, an INNER `while` (0x004dba22 jumps back to the top), not an `if`. The test is
    // UNORDERED-AWARE (see header banner): `!(s >= clock)` reproduces JNC's "unordered counts as
    // continue" behaviour, which a plain `s < clock` would not.
    for (int32_t p = 0; p < *v.active_player_count; ++p) {
        player_data &pd = own.players[p];
        while (!(pd.ai_clock_s >= pd.ai_clock)) { // 0x004db9d9-0x004db9e8
            if (pd.ai_enabled != 0) {
                gc.player_tick(p); // 0x004db9f5
            }
            pd.ai_clock_s = pd.ai_clock_s + *v.ai_strategy_period; // 0x004dba0e-0x004dba1b
        }
    }

    // ---- loop 4 (0x004dba31-0x004dba93): TACTIC catch-up on ai_clock_t, identical shape -----------
    for (int32_t p = 0; p < *v.active_player_count; ++p) {
        player_data &pd = own.players[p];
        while (!(pd.ai_clock_t >= pd.ai_clock)) { // 0x004dba47-0x004dba56
            if (pd.ai_enabled != 0) {
                gc.ai_unit_group_tick(p); // 0x004dba63
            }
            pd.ai_clock_t = pd.ai_clock_t + *v.ai_tactic_period; // 0x004dba7c-0x004dba89
        }
    }

    // ---- loop 5 (0x004dba9f-0x004dbb08): MOVE catch-up on ai_clock_m -------------------------------
    // The call is CHOSEN by ai_enabled rather than gated by it -- one of the two ALWAYS runs while
    // the catch-up condition holds; there is no "do nothing" arm. Both arms fall into the same
    // advance-then-retest tail (the original's label layout -- LAB_004dba9f falls into the advance at
    // LAB_004dbaa6, the enabled arm jumps straight there -- collapses to this without any
    // behavioural difference).
    for (int32_t p = 0; p < *v.active_player_count; ++p) {
        player_data &pd = own.players[p];
        while (!(pd.ai_clock_m >= pd.ai_clock)) { // 0x004dbae4-0x004dbaf3
            if (pd.ai_enabled != 0) {
                gc.active_unit_tick(p); // 0x004dbb00
            } else {
                gc.passive_engage_tick(p); // 0x004dbaa1 -- drives a NON-AI (human) player's
                                           // passive engagement too; see the header banner.
            }
            pd.ai_clock_m = pd.ai_clock_m + *v.ai_move_period; // 0x004dbaba-0x004dbac7
        }
    }
    // PRESERVE-BUG: none of the three catch-up loops above bounds its iteration count. If the
    // corresponding period is <= 0 the original spins forever, and so does this. See the header
    // banner's PRESERVE-BUG section.
}

} // namespace detail

void players_tick(double dt) {
    const ai_state st = state();
    detail::players_tick(st.read, st.own, live_calls(), dt);
}

} // namespace mh::ai
