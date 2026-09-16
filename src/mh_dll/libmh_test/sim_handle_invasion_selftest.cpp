//
// sim_handle_invasion_selftest.cpp -- `simtest` offline oracle for game_HandleInvasion
// (sim/sim_invasion.h/.cpp, RI-SIM / SIM1F). Calls detail::handle_invasion DIRECTLY (not
// through chance_roll's indirection -- that pairing gets its own coverage in
// sim_invasion_chance_roll_selftest.cpp), so every branch here is isolated and driven precisely: the
// eligibility gate on BOTH sides (own-planet unconditional bypass vs. the distant-planet acquired&&
// CONQUERED test), the own-planet success/failure split, the distant-planet clamp's both directions,
// the writes to G_PLANET_STATUS (own.planet_status_at) and _G_LLM_STRAT_INVASION_TIME
// (own.planet_invasion_time_at), and the PlayerSide zero-extension the header flags as a
// reimpl-verify-caught risk (0x004996f6 is a MOVZX, not a MOVSX -- a plain int16_t->int32_t C++ cast
// would sign-extend instead and is WRONG).
//
// EXPECTED VALUES HAND-DERIVED FROM THE DISASSEMBLY (tmp/decomp/game_HandleInvasion_004996c4.asm), not
// the .cpp -- see sim_invasion.h's banner for the address-by-address pseudocode. All eight
// invasion_calls callees are RECORDING STUBS; nothing here touches the live game image.
//
#include "sim/sim_invasion.h"

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// ---- recording stubs (same shape as sim_invasion_chance_roll_selftest.cpp; duplicated locally rather
// than shared, matching this codebase's one-TU-per-selftest-file convention). rand_below and
// w_sprintf__vi are NOT reachable from handle_invasion at all (only chance_roll's scan arm uses them)
// -- kept here only to satisfy invasion_calls's shape, and asserted UNCALLED in every case below.
std::vector<int32_t> g_rand_below_calls;
std::vector<int32_t> g_vi_calls;

struct VssCall {
    void          *dst;
    const wchar_t *fmt, *a0, *a1;
};
std::vector<VssCall> g_vss_calls;
std::vector<void *>  g_print_calls;
int32_t              g_spawn_return = 1;
std::vector<int32_t> g_spawn_calls;
struct RevokeCall {
    uint16_t player, progress_id;
};
std::vector<RevokeCall> g_revoke_calls;
struct PresenceCall {
    uint32_t player, mode;
};
std::vector<PresenceCall> g_presence_calls;
struct AlertCall {
    int32_t planet;
    double  timestamp;
};
std::vector<AlertCall> g_alert_calls;

// Clears only the RECORDING accumulators -- NOT the programmed input g_spawn_return, which each
// case seeds BEFORE run() (run() calls this first). An earlier revision reset g_spawn_return=1
// here, which wiped the "spawn fails" seeding (g_spawn_return=0) so D2 wrongly took the success
// arm. Recordings-only, matching the template selftest's run().
void reset_calls() {
    g_rand_below_calls.clear();
    g_vi_calls.clear();
    g_vss_calls.clear();
    g_print_calls.clear();
    g_spawn_calls.clear();
    g_revoke_calls.clear();
    g_presence_calls.clear();
    g_alert_calls.clear();
}

int32_t stub_rand_below(int32_t upper_bound) {
    g_rand_below_calls.push_back(upper_bound);
    return 0;
}
int32_t stub_w_sprintf_vi(void *, const wchar_t *, int32_t a0) {
    g_vi_calls.push_back(a0);
    return 0;
}
int32_t stub_w_sprintf_vss(void *dst, const wchar_t *format, const wchar_t *a0, const wchar_t *a1) {
    g_vss_calls.push_back({dst, format, a0, a1});
    return 0;
}
uint32_t stub_print_text_message(void *text) {
    g_print_calls.push_back(text);
    return 0;
}
int32_t stub_spawn_enemy_landing() {
    g_spawn_calls.push_back(1);
    return g_spawn_return;
}
void stub_revoke_invention(uint16_t player, uint16_t progress_id) {
    g_revoke_calls.push_back({player, progress_id});
}
uint32_t stub_player_presence_lost(uint32_t player, uint32_t mode) {
    g_presence_calls.push_back({player, mode});
    return 0;
}
void stub_invasion_alert_arm(int32_t planet, double timestamp) {
    g_alert_calls.push_back({planet, timestamp});
}

const invasion_calls g_calls = {
    stub_rand_below,
    stub_w_sprintf_vi,
    stub_w_sprintf_vss,
    stub_print_text_message,
    stub_spawn_enemy_landing,
    stub_revoke_invention,
    stub_player_presence_lost,
    stub_invasion_alert_arm,
};

int32_t run(sim_fixture &fx, int32_t planet, double since_time) {
    reset_calls();
    sim_store own = fx.store();
    return detail::handle_invasion(fx.view(), own, g_calls, planet, since_time);
}

// Asserts NONE of the eight outward calls fired -- the shape every eligibility-fail case must show.
// `ctx` must be a string LITERAL (not a temporary) -- ck() stores the `const char *` pointer only.
void ck_no_calls(const char *ctx) {
    ck(g_rand_below_calls.empty() && g_vi_calls.empty() && g_vss_calls.empty() && g_print_calls.empty() &&
           g_spawn_calls.empty() && g_revoke_calls.empty() && g_presence_calls.empty() &&
           g_alert_calls.empty(),
       ctx);
}

} // namespace

void run_handle_invasion_tests() {
    sim_fixture fx;

    // ==== own-planet arm (planet == *v.planet_index): eligibility is UNCONDITIONALLY true, bypassing
    // the acquired/CONQUERED test entirely -- both cases below leave progress/planet_status at their
    // reset() defaults (acquired=0, status=0/not-CONQUERED) and the invasion still applies. ============
    //
    // D1: spawn succeeds (nonzero) -> print_text_message(INVASION_BEGUN) + revoke_invention +
    // planet_status_at(planet)=INVASION; NO player_presence_lost. Return 1.
    fx.reset();
    fx.planet_index                      = 6;
    fx.player_side                       = 11;
    fx.cfg_planets[6].invention_index    = 77;
    static const wchar_t *TXT_BEGUN      = L"invasion begun (sentinel)";
    fx.text_ptrs[TEXT_ID_INVASION_BEGUN] = TXT_BEGUN;
    g_spawn_return                       = 3; // nonzero -> success
    ck_eq((uint32_t)run(fx, 6, 0.0), 1u, "D1: own-planet, spawn success -> return 1");
    ck((uint32_t)g_spawn_calls.size() == 1, "D1: spawn_enemy_landing called exactly once");
    ck(g_print_calls.size() == 1 && g_print_calls[0] == (void *)TXT_BEGUN,
       "D1: print_text_message(text_ptrs[INVASION_BEGUN])");
    ck(g_revoke_calls.size() == 1 && g_revoke_calls[0].player == 11 && g_revoke_calls[0].progress_id == 77,
       "D1: revoke_invention(player_side=11, invention_index=77)");
    ck_eq((uint32_t)fx.planet_status[6], (uint32_t)PLANET_STATUS_INVASION, "D1: planet_status[6] = INVASION");
    ck((uint32_t)g_presence_calls.size() == 0, "D1: no player_presence_lost on the success arm");
    ck((uint32_t)g_alert_calls.size() == 0, "D1: no invasion_alert_arm on the own-planet arm (that's the distant-planet arm only)");
    ck((uint32_t)g_vss_calls.size() == 0, "D1: no w_sprintf__vss on the own-planet arm");

    // D2: spawn fails (0) -> player_presence_lost(local_player_slot, 0) instead; no print/revoke;
    // planet_status is NOT written. Return 1 regardless (own-planet arm always reports "applied").
    fx.reset();
    fx.planet_index      = 6;
    fx.local_player_slot = 9;
    g_spawn_return       = 0; // failure
    ck_eq((uint32_t)run(fx, 6, 0.0), 1u, "D2: own-planet, spawn fails -> STILL return 1");
    ck(g_presence_calls.size() == 1 && g_presence_calls[0].player == 9 && g_presence_calls[0].mode == 0,
       "D2: player_presence_lost(local_player_slot=9, mode=0)");
    ck((uint32_t)g_print_calls.size() == 0, "D2: no print_text_message on the failure arm");
    ck((uint32_t)g_revoke_calls.size() == 0, "D2: no revoke_invention on the failure arm");
    ck_eq((uint32_t)fx.planet_status[6], 0u, "D2: planet_status[6] left untouched (still 0, no CONQUERED/acquired needed)");

    // ==== distant-planet arm (planet != *v.planet_index): eligibility = acquired==1 && status==
    // CONQUERED, BOTH required. =========================================================================
    //
    // D3a: eligible, and time_delta(=game_clock-since_time) < planet_time[planet] -> clamp uses
    // planet_time[planet] (the LARGER of the two), not time_delta.
    fx.reset();
    fx.planet_index                                  = 20;
    fx.game_clock                                    = 100.0;
    fx.player_side                                   = 2;
    fx.cfg_planets[8].invention_index                = 5;
    fx.cfg_planets[8].name                           = 0xab;
    fx.planet_status[8]                              = PLANET_STATUS_CONQUERED;
    fx.progress[2 * PROGRESS_ROW_COUNT + 5].acquired = 1;
    fx.planet_time[8]                                = 500.0; // LARGER than time_delta below
    static const wchar_t *TXT_ON                     = L"invasion on (sentinel)";
    static const wchar_t *TXT_NAME                   = L"planet name (sentinel)";
    fx.text_ptrs[TEXT_ID_INVASION_ON]                = TXT_ON;
    fx.text_ptrs[0xab]                               = TXT_NAME;
    // since_time=10.0 -> time_delta = 100.0 - 10.0 = 90.0, which is < planet_time[8]=500.0.
    ck_eq((uint32_t)run(fx, 8, 10.0), 1u, "D3a: eligible distant planet -> return 1");
    ck_eq_d(fx.planet_invasion_time[8], 500.0, "D3a: clamp picks planet_time[8]=500.0 (> time_delta=90.0)");
    ck(g_vss_calls.size() == 1 && g_vss_calls[0].dst == (void *)fx.text_scratch.data() &&
           g_vss_calls[0].a0 == (const wchar_t *)TXT_ON && g_vss_calls[0].a1 == (const wchar_t *)TXT_NAME,
       "D3a: w_sprintf__vss(text_scratch(), fmt, text_ptrs[INVASION_ON], text_ptrs[cfg_planets[8].name])");
    ck(g_print_calls.size() == 1 && g_print_calls[0] == (void *)fx.text_scratch.data(),
       "D3a: print_text_message(text_scratch()) -- NOT text_ptrs[INVASION_BEGUN] (that's the own-planet arm's message)");
    ck(g_revoke_calls.size() == 1 && g_revoke_calls[0].player == 2 && g_revoke_calls[0].progress_id == 5,
       "D3a: revoke_invention(player_side=2, invention_index=5)");
    ck_eq((uint32_t)fx.planet_status[8], (uint32_t)PLANET_STATUS_INVASION, "D3a: planet_status[8] = INVASION");
    ck(g_alert_calls.size() == 1 && g_alert_calls[0].planet == 8 && g_alert_calls[0].timestamp == 100.0,
       "D3a: invasion_alert_arm(planet=8, timestamp=*game_clock=100.0)");
    ck((uint32_t)g_spawn_calls.size() == 0, "D3a: spawn_enemy_landing NOT called on the distant-planet arm");
    ck((uint32_t)g_presence_calls.size() == 0, "D3a: player_presence_lost NOT called on the distant-planet arm");

    // D3b: eligible, and time_delta >= planet_time[planet] -> clamp uses time_delta instead.
    fx.reset();
    fx.planet_index                                  = 20;
    fx.game_clock                                    = 1000.0;
    fx.player_side                                   = 4; // reset() defaults this to 7, NOT 0 -- must set the row we seed
    fx.cfg_planets[8].invention_index                = 5;
    fx.planet_status[8]                              = PLANET_STATUS_CONQUERED;
    fx.progress[4 * PROGRESS_ROW_COUNT + 5].acquired = 1;    // eligibility: player_side(4) row, invention 5
    fx.planet_time[8]                                = 50.0; // SMALLER than time_delta below
    // since_time=10.0 -> time_delta = 1000.0 - 10.0 = 990.0, which is >= planet_time[8]=50.0.
    ck_eq((uint32_t)run(fx, 8, 10.0), 1u, "D3b: eligible distant planet -> return 1");
    ck_eq_d(fx.planet_invasion_time[8], 990.0, "D3b: clamp picks time_delta=990.0 (> planet_time[8]=50.0)");

    // D4: NOT eligible -- acquired==0 (default after reset()), status==CONQUERED. The dead-cascade
    // return: 0, with NO outward call of any kind and NO state write.
    fx.reset();
    fx.planet_index     = 20;
    fx.planet_status[8] = PLANET_STATUS_CONQUERED;
    // fx.progress[...].acquired left at its reset() default of 0.
    ck_eq((uint32_t)run(fx, 8, 10.0), 0u, "D4: acquired==0 -> not eligible -> return 0");
    ck_no_calls("D4");
    ck_eq((uint32_t)fx.planet_status[8], (uint32_t)PLANET_STATUS_CONQUERED, "D4: planet_status[8] unchanged (still CONQUERED, not forced to INVASION)");
    ck_eq_d(fx.planet_invasion_time[8], 0.0, "D4: planet_invasion_time[8] unchanged (still 0.0)");

    // D5: NOT eligible -- acquired==1 but status != CONQUERED. Mutation note: this is the discriminator
    // for a translation that drops the "&&" (e.g. ORs the two conditions, or checks only `acquired`).
    fx.reset();
    fx.planet_index                                  = 20;
    fx.cfg_planets[8].invention_index                = 5;
    fx.planet_status[8]                              = 0; // != CONQUERED
    fx.progress[0 * PROGRESS_ROW_COUNT + 5].acquired = 1;
    ck_eq((uint32_t)run(fx, 8, 10.0), 0u, "D5: status != CONQUERED -> not eligible -> return 0");
    ck_no_calls("D5");

    // ==== PlayerSide zero-extension (reimpl-verify 2026-08-16; the header's own flagged risk) =========
    //
    // D6: player_side's raw 16-bit pattern has bit15 set (stored value -32768, i.e. int16_t 0x8000).
    // `static_cast<int32_t>(static_cast<uint16_t>(*v.player_side))` -- the CORRECT widening this
    // translation uses -- yields 32768, not -32768. To probe that without allocating a ~9.8M-entry
    // progress vector (32768 * PROGRESS_ROW_COUNT), invention_index is set to a matching NEGATIVE
    // offset so the CORRECT (zero-extended) address arithmetic lands on a small, in-bounds index this
    // fixture already has (progress[1200]) -- while the WRONG (sign-extended) arithmetic would land on
    // a completely different, far-negative address (player=-32768 -> index ~= -19,659,600), which does
    // NOT hold our seeded acquired=1 (the fixture's progress vector is only 2400 elements; that address
    // is nowhere near it). A translation that sign-extends instead of zero-extending would therefore
    // see eligible=false (or fault) here, not the correct eligible=true this case asserts.
    // Mutation note: replacing `static_cast<uint16_t>(*v.player_side)` with a direct
    // `static_cast<int32_t>(*v.player_side)` (dropping the uint16_t intermediate) flips this red.
    fx.reset();
    fx.planet_index                     = 20;
    fx.player_side                      = (int16_t)0x8000; // -32768 as stored; 32768 when correctly zero-extended
    const int32_t zx_player             = 32768;           // the CORRECT widened value
    const int32_t target_progress_index = 1200;            // well inside the fixture's 2400-entry progress vector
    fx.cfg_planets[8].invention_index =
        target_progress_index - zx_player * PROGRESS_ROW_COUNT; // a large negative "row", by design
    fx.planet_status[8]                                 = PLANET_STATUS_CONQUERED;
    fx.progress[(size_t)target_progress_index].acquired = 1;
    ck_eq((uint32_t)run(fx, 8, 10.0), 1u,
          "D6: bit15-set PlayerSide, correctly zero-extended -> eligible -> return 1");
    ck_eq((uint32_t)fx.planet_status[8], (uint32_t)PLANET_STATUS_INVASION, "D6: planet_status[8] = INVASION (the eligibility gate passed)");
}

} // namespace mh::sim::test
