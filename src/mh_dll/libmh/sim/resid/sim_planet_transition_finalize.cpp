//
// sim/resid/sim_planet_transition_finalize.cpp -- see sim_planet_transition_finalize.h. Translated
// from the DISASSEMBLY
// (tmp/decomp_sim_resid/llm_strat_planet_transition_finalize_0044d160.asm), the Ghidra `.c` being a
// draft that silently drops the deploy_starting_squad block (see the header banner).
//
#include "sim/resid/sim_planet_transition_finalize.h"

#include "addr/mh_calls.gen.h" // typed callables for the effectful/frontier originals we call OUT to
#include "state/host_api.h"
#include "state/host_events.h"
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const planet_transition_finalize_calls &live_planet_transition_finalize_calls() {
    static const planet_transition_finalize_calls c = {
        MH_LIBMH_BIND(llm_strat_time_resync_and_tick),
        MH_LIBMH_BIND(llm_strat_invasion_due_check),
        MH_LIBMH_BIND(llm_strat_invasion_alert_clear),
        MH_LIBMH_BIND(llm_strat_prod_deliver_arrivals),
        MH_LIBMH_BIND(llm_strat_deploy_starting_squad),
        MH_LIBMH_BIND(llm_strat_player_presence_lost), // the ORIGINAL -- see the header's routing note
        mh::state::evt::inv_viewport,
    };
    return c;
}

namespace {

// Ghidra enum /Manual/game/E_PLANET_STATUS: UNKNOWN=0 -- same citation
// sim/resid/sim_session_clear_presence_flag.cpp already uses for the identical region.
inline constexpr int32_t PLANET_STATUS_UNKNOWN = 0;

// 0x0044d1ae: the literal planet-index floor the transition-state gate compares against (signed
// JG, so the gated block only runs for planets 7 and above).
inline constexpr int32_t PLANET_INDEX_GATE = 6;

} // namespace

namespace detail {

// ---- llm_strat_planet_transition_finalize @0x0044d160 ----------------------------------------
void planet_transition_finalize(const sim_view &v, sim_store &own,
                                const planet_transition_finalize_calls &c) {
    // 0x0044d178: park the camera pan target off (-1 == 0xffffffff).
    own.cam_pan_target_col() = -1;

    // 0x0044d182-0x0044d196: the four unconditional frontier calls, in original order.
    c.time_resync_and_tick();
    c.invasion_due_check();
    c.invasion_alert_clear(*v.planet_index);
    c.prod_deliver_arrivals();

    // 0x0044d19b: mark the strategic sim as active. DECLARED NEED -- see the header's
    // declared_needs note; sim_active_mut() does not exist in sim_store yet.
    own.sim_active_mut() = 1;

    // 0x0044d1a5-0x0044d1da: the deploy_starting_squad gate. PRESERVE-BUG: the innermost check
    // (planet_transition_state() > 0) re-reads the SAME byte the outer check already pinned to 0,
    // with nothing between the two checks writing it -- so this block is unreachable along any path
    // this translation can prove, and it is transcribed literally anyway per Law 2. See the header
    // banner's PRESERVE-BUG note.
    if (own.planet_transition_state() == 0 && *v.planet_index > PLANET_INDEX_GATE) {
        if (v.planet_status[*v.planet_index] != 0) {
            if (own.planet_transition_state() > 0) {
                // 0x0044d1d5-0x0044d1da: store only the LOW BYTE (AL) of the callee's return into
                // the byte-sized transition-state global -- see the header's WIDTH NOTE.
                const int32_t ret             = c.deploy_starting_squad();
                own.planet_transition_state() = static_cast<uint8_t>(ret);
            }
        }
    }

    // 0x0044d1df-0x0044d229: rejoin point for every path above. If the local side has not yet
    // acquired the current planet's invention AND the planet's status is not UNKNOWN, the local
    // player has just lost presence on this planet -- tell the ORIGINAL
    // llm_strat_player_presence_lost (see the header's routing note), mode 0 (natural loss).
    const int32_t planet = *v.planet_index;
    // MOVZX, not MOVSX: `0x0044d1f2 MOVZX EAX,word ptr [0x00e58354]` ZERO-extends PlayerSide, and
    // sim_view binds it as int16_t, so a plain assignment SIGN-extends instead. Unreachable today --
    // both writers of that global bound it to 0..7 (the reimpl-verify reviewer that raised this
    // showed as much, which is why it is graded PLAUSIBLE and not a divergence) -- but Law 2 says
    // the translation matches the instruction, not the value range that happens to hold.
    const int32_t me  = static_cast<int32_t>(static_cast<uint16_t>(*v.player_side));
    const int32_t inv = v.cfg_planets[planet].invention_index;
    if (progress_of(v, me, inv).acquired != 1 && v.planet_status[planet] != PLANET_STATUS_UNKNOWN) {
        c.player_presence_lost(static_cast<uint32_t>(*v.local_player_slot), 0u);
    }

    // 0x0044d229: unconditional tail.
    c.cam_mark_viewport_dirty();
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

void planet_transition_finalize() {
    sim_state st = state();
    detail::planet_transition_finalize(st.read, st.own, live_planet_transition_finalize_calls());
}

} // namespace mh::sim
