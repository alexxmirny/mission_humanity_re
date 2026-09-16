//
// sim/sim_game_update_progress.cpp -- see sim_game_update_progress.h. Translated from the DISASSEMBLY
// (tmp/decomp/game_UpdateProgress_004402b0.asm), cross-checked branch-for-branch against the raw
// bytes; the Ghidra .c draft matches it exactly (see the header banner).
//
#include "sim/sim_game_update_progress.h"

#include "addr/mh_calls.gen.h"                            // typed callables for the original functions we still call OUT to
#include "sim/libtrans/sim_lt_progress_unlock_fixpoint.h" // the rebound propagate_unlocks
#include "ai/ai_state.h"                                  // ai_say / trace_budget -- the shared trace sink, not AI state
#include "addr/mh_rebind.gen.h"                           // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

namespace {

// ---- cfg_enum_E_INVETION_TYPE (rule 17a; see the header's "THE cfg_enum_E_INVETION_TYPE /
// SLOT_ENABLED DOMAINS" section for why this is a fresh, file-local copy rather than a shared include).
// Values cross-checked against sim_game_handle_progress.h's already-verified INVENTION_TYPE_* constants
// (same domain, same mapping).
// (Local copies removed 2026-09-02: the LT1B rebind's include chain now brings
// sim_game_handle_progress.h's identical inline constexpr set into scope, and a same-scope second
// copy is a C2872 ambiguity -- the same dedupe sim_lt_progress_unlock_fixpoint.cpp took.)

// Ghidra's own E_PLANET_STATUS member names (also used, independently, by sim_invasion.h's
// PLANET_STATUS_CONQUERED/_INVASION -- same values, fresh file-local copy per this file's own
// no-shared-header rule; DECLARED NEED per rule 17a: no committed C++ enum exists for this domain).
constexpr int32_t PLANET_STATUS_CONQUERED = 2;
constexpr int32_t PLANET_STATUS_INVASION  = 4;

// llm_strat_player_profile::status_flags bit 0 (E_STRAT_PLAYER_STATUS bit0 -- no committed Ghidra enum
// exists for this field either; see the header banner). Named to match sim_game_speed_recompute.cpp's
// STRAT_PLAYER_STATUS_ALIVE/_HUMAN sibling convention.
constexpr uint32_t STRAT_PLAYER_STATUS_SLOT_ENABLED = 0x1u;

} // namespace

const game_update_progress_calls &live_game_update_progress_calls() {
    static const game_update_progress_calls c = {
        MH_LIBMH_BIND(llm_strat_count_landing_spots),
        MH_LIBMH_BIND(llm_progress_finalize_acquire),
        &mh::sim::progress_propagate_unlocks, // REBOUND 2026-09-02: translated (LT1B), ours binds directly
    };
    return c;
}

namespace detail {

void game_update_progress(const sim_view &v, sim_store &own, const game_update_progress_calls &c,
                          uint16_t plr, uint16_t inv) {
    // 0x004402ea-0x00440303: progress[plr][inv].acquired = true, unconditional, before the dispatch.
    own.progress_at(plr, inv).acquired = true;

    // 0x00440304-0x00440327: dispatch on Progress[inv].type. Values outside 1..6 (the C++ switch's
    // implicit default) fall straight through to the shared propagate_unlocks(plr) tail below, matching
    // the disassembly's own JA-to-default bounds check.
    switch (v.cfg_inventions[inv].type) {
        case INVENTION_TYPE_BUILDING: // caseD_1: no-op
            break;

        case INVENTION_TYPE_UNIT: // caseD_2: no-op
            break;

        case INVENTION_TYPE_PROJECT: // caseD_3, 0x0044032e-0x0044033b
            c.finalize_acquire(plr, inv);
            break;

        case INVENTION_TYPE_PLANET: { // caseD_4, 0x004403db-0x00440478 -- see header's "THE PLANET CASE"
            const int32_t landing_spots = c.count_landing_spots();
            if (landing_spots > 2 &&
                v.planet_status[v.cfg_inventions[inv].index] != PLANET_STATUS_INVASION) {
                own.planet_status_at(v.cfg_inventions[inv].index) = PLANET_STATUS_CONQUERED;
            }
            if (plr == static_cast<uint16_t>(*v.player_side)) {
                for (int32_t i = 0; i < MAX_PLAYERS; ++i) {
                    if (plr != static_cast<uint16_t>(i) &&
                        (v.profiles[i].status_flags & STRAT_PLAYER_STATUS_SLOT_ENABLED) != 0) {
                        own.progress_at(static_cast<uint32_t>(i), inv).acquired = true;
                        c.propagate_unlocks(static_cast<uint16_t>(i));
                        // NOTE: no finalize_acquire(i, inv) here and no trailing finalize_acquire(plr, inv)
                        // after the loop -- both this case's own JMP-to-default landings confirmed by
                        // address (see header banner). Do not "symmetrize" this with the SYSTEM case below.
                    }
                }
            }
            break;
        }

        case INVENTION_TYPE_SYSTEM: // caseD_5, 0x00440352-0x004403bf -- see header's "THE SYSTEM CASE"
            if (plr == static_cast<uint16_t>(*v.player_side)) {
                for (int32_t i = 0; i < MAX_PLAYERS; ++i) {
                    if (plr != static_cast<uint16_t>(i) &&
                        (v.profiles[i].status_flags & STRAT_PLAYER_STATUS_SLOT_ENABLED) != 0) {
                        own.progress_at(static_cast<uint32_t>(i), inv).acquired = true;
                        c.propagate_unlocks(static_cast<uint16_t>(i));
                        c.finalize_acquire(static_cast<uint16_t>(i), inv);
                    }
                }
            }
            // 0x004403bf-0x004403cc: unconditional, whether or not the loop above ran at all.
            c.finalize_acquire(plr, inv);
            break;

        case INVENTION_TYPE_UPGRADE: // caseD_6, 0x00440340-0x0044034d
            c.finalize_acquire(plr, inv);
            break;

        default:
            break;
    }

    // 0x0044047a-0x00440483: the shared tail every case (or no case at all) converges on.
    c.propagate_unlocks(plr);
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

void game_update_progress(uint16_t plr, uint16_t inv) {
    sim_state st = state();
    detail::game_update_progress(st.read, st.own, live_game_update_progress_calls(), plr, inv);
}


} // namespace mh::sim
