//
// sim/hostreach/sim_h_diplomacy_restore_relations.cpp -- see sim_h_diplomacy_restore_relations.h.
// Translated from the DISASSEMBLY (tmp/decomp_sim/llm_diplomacy_restore_relations_00465c6e.asm).
//
#include "sim/hostreach/sim_h_diplomacy_restore_relations.h"

namespace mh::sim {

namespace {
// player_profile.status_flags bit0 (E_STRAT_PLAYER_STATUS bit0 -- "slot enabled"; no committed Ghidra
// enum, per sim_lt_diplomacy.h's declared_needs note). libmh/sim/ keeps its own file-local copy per TU,
// same posture as every sibling that tests this bit (sim_land_players_on_planet.cpp,
// sim_lt_diplomacy.cpp, sim_landing_spot.cpp, sim_game_update_progress.cpp, ...).
constexpr uint32_t SLOT_ENABLED = 0x1u;
} // namespace

namespace detail {

// ---- llm_diplomacy_restore_relations @0x00465c6e ----------------------------------------------------
void diplomacy_restore_relations(const sim_view &v, sim_store &own,
                                 const diplomacy_set_relation_calls &c_set_relation) {
    // 0x00465c8d-0x00465ced: outer loop over player i, 0..MAX_PLAYERS-1 inclusive.
    for (int32_t i = 0; i < MAX_PLAYERS; ++i) {
        // 0x00465ca1-0x00465ceb: inner loop over player j, 0..MAX_PLAYERS-1 inclusive.
        for (int32_t j = 0; j < MAX_PLAYERS; ++j) {
            // 0x00465cae-0x00465ccc: both i and j must be enabled slots in the PROFILE roster (v,
            // NOT own.player_relation_at's table -- see the header's two-table hazard note) before
            // the relation is read/re-applied at all. j's flag is only tested once i's already held
            // (JZ on i skips straight past j's TEST), reproduced as two sequential early-continues.
            if ((v.profiles[i].status_flags & SLOT_ENABLED) == 0) continue;
            if ((v.profiles[j].status_flags & SLOT_ENABLED) == 0) continue;

            // 0x00465cd0-0x00465ce4: Players[i].relation[j] (own.player_relation_at -- a DIFFERENT
            // table/stride than v.profiles above) re-applied through the threaded sibling.
            const uint8_t relation =
                own.player_relation_at(static_cast<uint32_t>(i), static_cast<uint32_t>(j));
            detail::diplomacy_set_relation(v, own, c_set_relation, i, j, relation);
        }
    }
}

} // namespace detail

// ---- the public wrapper ------------------------------------------------------------------------------

void diplomacy_restore_relations() {
    sim_state st = state();
    detail::diplomacy_restore_relations(st.read, st.own);
}

} // namespace mh::sim
