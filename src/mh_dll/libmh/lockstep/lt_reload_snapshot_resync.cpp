//
// lockstep/lt_reload_snapshot_resync.cpp -- see lt_reload_snapshot_resync.h. Translated from
// tmp/decomp_lib_trans/llm_game_reload_snapshot_resync_clocks_00425bcd.asm, NOT from the .c beside it
// (the .c's plate mislabelled the three emitter calls until the 2026-09-02 correction).
//
#include "lockstep/lt_reload_snapshot_resync.h"

#include "addr/mh_addrs.gen.h"
#include "addr/mh_calls.gen.h"
#include "lockstep/internal_call.h"          // MH_INTERNAL_CALL -- the ack_voice direct edge
#include "orders/issue/issue_group_orders.h" // mh::orders::issue::group_order_ack_voice
#include "addr/mh_export.gen.h"              // mh::exp:: symbols the shadow macro below expands into
#include "lockstep/turn_engine.h"            // MAX_PLAYERS -- one definition, shared with resync.cpp
#include "state/host_api.h"
#include "state/host_events.h"

namespace mh::lockstep {

const reload_calls &live_reload_calls() {
    static const reload_calls cc = {
        mh::state::evt::snd_race_alert,
        // ack_voice is a function WE OWN (mh::orders::issue translated it; issue_promote.cpp's
        // MH_EXPORT_REPLACE claims the entry), so per the direct-call rule this edge binds our
        // production entry -- entry-routed only when promotion has NOT claimed the bytes.
        MH_INTERNAL_CALL(llm_strat_group_order_ack_voice, mh::orders::issue::group_order_ack_voice),
        mh::state::evt::text_race_alert,
    };
    return cc;
}

namespace {

// Per-player record counts, derived from the .asm's own player-stride multipliers
// (context_D.md §4a), not from any struct's field name:
//   units:     IMUL EAX,[p],0x5b04  ->  0x5b04 / sizeof(mh_map_object_unit)     == 0x5b04 / 0xe9  == 100
//   buildings: IMUL EAX,[p],0x6aa4  ->  0x6aa4 / sizeof(mh_map_object_building) == 0x6aa4 / 0x111 == 100
// Same 100 both sides of orders/issue/issue_state.h's UNITS_PER_PLAYER/BUILDINGS_PER_PLAYER, kept as
// this TU's own constants rather than a cross-module include for two unrelated numbers.
// SB-BIND T2 (2026-09-06): these were this TU's own copies of UNITS_PER_PLAYER/BUILDINGS_PER_PLAYER.
// The indexing now reads the DERIVED capacities off reload_state::caps, so what survives here is
// only the pair of asserts -- and they are still worth keeping, because they pin the STOCK strides
// the .asm was read against. Expressed against the one shared source so a stock-value change has a
// single place to be made.
static_assert(mh::state::STOCK_ROSTER_CAPS.units * sizeof(mh::game::mh_map_object_unit) == 0x5b04u,
              "unit player-stride drift vs the .asm's IMUL EAX,[p],0x5b04");
static_assert(mh::state::STOCK_ROSTER_CAPS.buildings *
                      sizeof(mh::game::mh_map_object_building) ==
                  0x6aa4u,
              "building player-stride drift vs the .asm's IMUL EAX,[p],0x6aa4");

} // namespace

namespace detail {

// ---- llm_game_reload_snapshot_resync_clocks @0x00425bcd --------------------------------------------
void reload_snapshot_resync_clocks(const reload_state &st, const reload_calls &calls, double now) {
    // 0x00425be5-0x00425c02: save the three globals the install step below is about to clobber.
    const int32_t saved_snd_enabled = *st.snd_enabled;
    const double  saved_game_clock  = *st.game_clock;
    const int32_t saved_sim_active  = *st.sim_active;

    // 0x00425c05-0x00425c1f: install the temporary clock (sound off, sim halted). The GAME_CLOCK
    // write is a raw 64-bit bit-copy of `now` -- 0x00425c0f/0x00425c17 move it as two dwords, no x87
    // instruction touches it -- so a plain `double` assignment reproduces it exactly (same reasoning
    // as sim/resid/sim_clock_resync.cpp's field assignments).
    *st.snd_enabled = 0;
    *st.game_clock  = now;
    *st.sim_active  = -1; // 0xffffffff

    // 0x00425c29-0x00425c33: the three re-basing calls, IN THIS ORDER, RUN UNCONDITIONALLY. Do not
    // gate, reorder, or drop any of them -- see the header's hazard note. The generated effect-gate
    // layer (mh::effects, installed over each of these three targets' own entries) is what keeps
    // each from firing twice across the shadow window's two arms; this body must not duplicate that
    // job by treating SND_ENABLED as a call-suppression flag of its own.
    calls.race_alert_sound_emit();
    calls.group_order_ack_voice();
    calls.race_alert_text_emit();

    // 0x00425c38-0x00425c53: restore, IN THIS EXACT ORDER -- clock (both dwords), then SND_ENABLED,
    // then SIM_ACTIVE. The order is part of the spec: it is the one place two adjacent stores could
    // be transposed without changing any single variable's OWN final value.
    *st.game_clock  = saved_game_clock;
    *st.snd_enabled = saved_snd_enabled;
    *st.sim_active  = saved_sim_active;

    // 0x00425c58-0x00425cba: reseed slot [0] of every one of the 8 players, UNCONDITIONALLY -- no
    // alive gate, no live-record gate (the one behavioural difference from
    // mh::sim::clock_resync_units_and_buildings; see the header). Each backdate expression re-reads
    // *st.game_clock FRESH, exactly as the .asm's three independent FLD/FADD/FSTP triplets do -- do
    // not hoist a shared local out of this loop (translator-brief rule 16: a "const" read may not be
    // assumed stable across a call or across iterations).
    for (int32_t player = 0; player < MAX_PLAYERS; ++player) {
        st.units[player * st.caps.units].activity_clock =
            *st.game_clock + *st.reload_tick_backdate; // 0x00425c6f-0x00425c82
        st.buildings[player * st.caps.buildings].last_tick_time =
            *st.game_clock + *st.reload_tick_backdate; // 0x00425c88-0x00425c9b
        st.buildings[player * st.caps.buildings].cycle_progress =
            *st.game_clock + *st.reload_cycle_backdate; // 0x00425ca1-0x00425cb4
    }
}

} // namespace detail

void reload_snapshot_resync_clocks(double now) {
    detail::reload_snapshot_resync_clocks(live_reload_state(), live_reload_calls(), now);
}

} // namespace mh::lockstep
