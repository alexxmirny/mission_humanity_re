#include "sim/sim_landing_spot.h"

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"        // ai_say / trace_budget -- the shared trace sink, not AI state
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

namespace {
// TU-local literal operands, cross-checked against the established sibling copies -- see
// sim_landing_spot.h's banner for the derivation and why these live here rather than at header
// scope (a same-TU redefinition collision with sim_combat_credit_planet_conquest_kills.h's
// STRAT_PLAYER_STATUS_ALIVE / sim_game_get_starting_unit.h's RACE_ALIEN once both end up included
// alongside this header in seams/reimpl_probe.cpp).
constexpr uint32_t STRAT_PLAYER_STATUS_SLOT_ENABLED = 0x1u; // bit0 -- "slot claimed"/enabled.
                                                            // Cross-checked against
                                                            // seams/harness.cpp's PS_ENABLED.
constexpr uint32_t STRAT_PLAYER_STATUS_ALIVE = 0x2u;        // bit1 -- has presence. Cross-checked against
                                                            // sim_step.cpp's own STRAT_PLAYER_STATUS_ALIVE
                                                            // (same value).
constexpr uint32_t RACE_ALIEN = 2u;                         // Cross-checked against sim_game_get_starting_unit.h's own
                                                            // RACE_ALIEN (same value).
} // namespace

const landing_spot_calls &live_landing_spot_calls() {
    static const landing_spot_calls c = {
        MH_LIBMH_BIND(llm_rand_below),
        MH_LIBMH_BIND(llm_strat_set_landing_site),
        MH_LIBMH_BIND(llm_strat_count_landing_spots),
        MH_LIBMH_BIND(llm_strat_spawn_invasion_force),
    };
    return c;
}

namespace detail {

// llm_strat_claim_landing_spot @0x00454eb2. See the header banner for the full per-address
// derivation. `own` (not const sim_view) because the whole body reads AND writes
// _G_LLM_STRAT_LANDING_SPOTS through the single sim-owned mutable accessor -- there is no read-only
// view member for this region (nothing else in the sim closure reads it).
int32_t claim_landing_spot(const sim_view &v, sim_store &own, const landing_spot_calls &c,
                           int32_t player, int32_t planet) {
    (void)v; // no const-view state read by this function; landing spots are entirely sim-owned here.

    int32_t free_count        = 0;  // 0x00454ecf: count of scanned spots NOT taken (status != -2)
    int32_t paired_free_index = -1; // 0x00454ed6: an already-taken spot whose pair (i^1) is free
    int32_t result            = 1;  // 0x00454edd: return flag, default success
    int32_t i                 = 0;  // 0x00454ee4: scan index; also the loop-exit modulus base below

    // 0x00454eeb-0x00454f00: scan forward while the slot is populated (status != -1, "end of list")
    // and i < 16.
    for (; own.landing_spot_at(i).status != -1 && i < 16; ++i) {
        // 0x00454f05-0x00454f2a
        if (own.landing_spot_at(i).status == -2 && own.landing_spot_at(i ^ 1).status != -2) {
            paired_free_index = i ^ 1;
        }
        // 0x00454f2d-0x00454f3d
        if (own.landing_spot_at(i).status != -2) {
            ++free_count;
        }
    }

    int32_t chosen;
    if (paired_free_index >= 0) {
        // 0x00454f3f-0x00454f4b: prefer the paired slot of an already-claimed spot.
        chosen = paired_free_index;
    } else if (free_count == 0) {
        // 0x00454f4d-0x00454f51, 0x00454f93-0x00454f9a: every scanned spot is taken. Still commits
        // spot 0 below and still calls set_landing_site -- see the header banner's NOTE. Only the
        // return value signals failure.
        chosen = 0;
        result = 0;
    } else {
        // 0x00454f53-0x00454f8f: random start, then linear-probe forward (mod i) up to 20 tries for
        // a free slot. The retry-decrement and the index-advance are mutually independent (neither
        // depends on the other's new value), so this loop's execution order matches the assembly's
        // observable effect even though the two updates are written in the opposite order from the
        // Ghidra .c draft's for-loop reading -- see the header banner.
        chosen          = c.rand_below(i);
        int32_t retries = 0x14;
        while (own.landing_spot_at(chosen).status == -2 && retries != 0) {
            --retries;
            chosen = (chosen + 1) % i;
        }
    }

    // 0x00454fa1-0x00454fcc: commit the claim and notify the original site-setter.
    own.landing_spot_at(chosen).status = -2;
    c.set_landing_site(static_cast<uint32_t>(player), static_cast<uint32_t>(planet),
                       static_cast<uint32_t>(own.landing_spot_at(chosen).x),
                       static_cast<uint32_t>(own.landing_spot_at(chosen).y),
                       static_cast<uint32_t>(chosen));
    return result;
}

// llm_strat_spawn_enemy_landing @0x004998ae. See the header banner for the full per-address
// derivation. Calls claim_landing_spot() directly (in-TU C++ call, not through `c`/mh::call::) per
// the batch context.
int32_t spawn_enemy_landing(const sim_view &v, sim_store &own, const landing_spot_calls &c) {
    const int32_t planet = *v.planet_index; // 0x004998c6-0x004998cb: G_PLANET_INDEX, saved locally
    int32_t       player = 0;               // 0x004998ce

    // 0x004998d5-0x004998e3: advance past PlayerSide and any disabled slot.
    //
    // MOVZX on PlayerSide (0x004998e5): the .asm zero-extends the 16-bit global before comparing
    // against the 32-bit loop index -- widen through uint16_t first, matching this codebase's
    // established PlayerSide convention (see the header banner), NOT the sign-extension a plain
    // int16_t->int32_t cast would perform.
    while (player < MAX_PLAYERS &&
           (static_cast<int32_t>(static_cast<uint16_t>(*v.player_side)) == player ||
            (v.profiles[player].status_flags & STRAT_PLAYER_STATUS_SLOT_ENABLED) == 0)) {
        ++player;
    }

    // 0x00499907-0x00499917: FAITHFULLY-REPRODUCED OUT-OF-BOUNDS READ when player==MAX_PLAYERS (the
    // loop exhausted all 8 slots) -- see the header banner. v.profiles is an unchecked pointer over
    // the live region, so indexing past MAX_PLAYERS here reproduces the original's exact behavior
    // (real contiguous process memory, not a bounds violation this translation introduces).
    if ((v.profiles[player].status_flags & STRAT_PLAYER_STATUS_ALIVE) == 0) {
        return 0;
    }
    // 0x00499923-0x00499935: the eligibility loop exhausted without finding a slot.
    if (player == MAX_PLAYERS) {
        return 0;
    }

    // 0x00499935-0x00499954: claim a spot, or fall back to a camera-relative position if the map has
    // no free spots left, or the claim itself failed.
    const int32_t spot_count = c.count_landing_spots();
    if (spot_count == 0 || claim_landing_spot(v, own, c, player, planet) == 0) {
        // 0x00499954-0x0049999e: width_mask & (CAM_COL - 15) / height_mask & (CAM_ROW - 15), written
        // through the mutable profile accessor (own), not the const view.
        own.profile_at(player).landing_x[planet] =
            static_cast<int32_t>(map_width_mask(v) & static_cast<uint32_t>(*v.cam_col - 0xf));
        own.profile_at(player).landing_y[planet] =
            static_cast<int32_t>(map_height_mask(v) & static_cast<uint32_t>(*v.cam_row - 0xf));
    }

    // 0x004999a4-0x00499a14: spawn the invasion force at whatever landing_x/landing_y now holds
    // (either the freshly-claimed site or the camera-relative fallback just written above) -- read
    // back through the const view, matching the original's re-read from memory.
    const int32_t is_alien = (v.profiles[player].race == RACE_ALIEN) ? 1 : 0;
    const int32_t spawned  = c.spawn_invasion_force(
        static_cast<uint32_t>(player), is_alien, v.profiles[player].landing_x[planet],
        v.profiles[player].landing_y[planet], planet * 2 + 0x14);

    return spawned > 0 ? 1 : 0;
}

} // namespace detail

// ---- the public wrappers ----------------------------------------------------------------------------

int32_t claim_landing_spot(uint32_t player, uint32_t planet) {
    sim_state st = state();
    return detail::claim_landing_spot(st.read, st.own, live_landing_spot_calls(), (int32_t)player,
                                      (int32_t)planet);
}

int32_t spawn_enemy_landing() {
    sim_state st = state();
    return detail::spawn_enemy_landing(st.read, st.own, live_landing_spot_calls());
}


} // namespace mh::sim
