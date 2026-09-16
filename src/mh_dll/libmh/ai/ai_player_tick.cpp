//
// ai/ai_player_tick.cpp -- see ai_player_tick.h. Translated from the DISASSEMBLY
// (tmp/decomp/llm_strat_ai_player_tick_004e8b9d.asm), not from Ghidra's C: the draft renders the x87
// square-root chain as a call through an UNINITIALISED float10, which is a decompiler artifact (see
// the comment on x87_sqrt_and_trunc below), and its plate for the tail store claims a loop over every
// OTHER player that the assembly does not contain -- see the comment at the bottom of
// detail::player_tick.
//
#include "ai/ai_player_tick.h"
#include "fp/x87_shapes.h" // CRT-X87: the hoisted x87 blocks (the asm moved, it did not change)

namespace mh::ai {
namespace detail {

int32_t x87_sqrt_and_trunc(uint32_t radius_sq) {
    return ::mh::fp::x87_sqrt_and_trunc(radius_sq);
}

void player_tick(const ai_view &v, const ai_store &own, const ai_calls &gc, int32_t player) {
    // ---- the alive gate (0x004e8bb2-0x004e8bc1) ----------------------------------------------
    if ((v.strat_players[player].status_flags & PLAYER_STATUS_ALIVE) == 0) return;

    player_data &pd = own.players[player]; // player_data IS the AI's own store -- see ai_state.h

    // ---- the invasion-force short-circuit (0x004e8bdf-0x004e8bf6) ---------------------------------
    //
    // player_data::ai_invasion_force's own field comment already documents this branch: 0 = full AI
    // base (falls through to everything below), non-zero = invasion-force-only AI, which skips
    // straight to the two invasion calls and returns.
    if (pd.ai_invasion_force != 0) {
        gc.invasion_spawn_reinforcements(player);
        gc.invasion_launch_attack_group((uint32_t)player);
        return;
    }

    // ---- the one-shot "spawn my opening unit next to home" edge (0x004e8bfb-0x004e8c76) ----------
    if (pd.ai_established == 0) {
        // race = 2 (alien) or 1 (human) -- the literal register values GetStartingUnit is called
        // with at 0x004e8c0d/0x004e8c14, selected by is_alien_race.
        const int32_t race    = (pd.is_alien_race != 0) ? 2 : 1;
        const int32_t unit_id = gc.GetStartingUnit((uint32_t)race);

        // unit_create is called with (x=home_x, y=home_y+1) -- the RAW home tile with y bumped by
        // one (0x004e8c41-0x004e8c4e) -- but the immediately following unit_order_move_with_bump
        // call reloads BOTH home_x and home_y RAW, with no +1 (0x004e8c56-0x004e8c5c). Not a
        // decompiler artifact: two separate loads of the same fields, two different values.
        const int32_t home_x = pd.ai_home_tile_x;
        const int32_t home_y = pd.ai_home_tile_y;

        // Register trace at 0x004e8c1e-0x004e8c67: EAX=home_x, EDX=home_y+1, EBX=unit(16-bit),
        // ECX=player(16-bit), stack=2 (is_ship) for unit_create; then EAX=player(16-bit, reloaded),
        // EDX=the returned unit index, EBX=home_x(RAW, reloaded), ECX=home_y(RAW, reloaded) for
        // unit_order_move_with_bump.
        const uint32_t created_unit =
            gc.unit_create((uint32_t)home_x, (uint32_t)(home_y + 1), (uint16_t)unit_id,
                           (uint16_t)player, 2);
        // (uint16_t)player, not (uint32_t): the original stores ONE 16-bit-truncated copy
        // (MOVZX EAX,SI @0x004e8c22 -> [EBP-0x18]) and reloads that same slot for BOTH calls
        // (0x004e8c4e, 0x004e8c64). Narrowed here for literal fidelity after the reimpl-verify pass
        // -- the difference is observably inert either way (ai_unit_move_bump.cpp masks its `player`
        // parameter to 16 bits at all four of its uses), so this is symmetry with the unit_create
        // call above, not a correctness fix.
        gc.unit_order_move_with_bump((uint32_t)(uint16_t)player, (int32_t)created_unit,
                                     (uint32_t)home_x, (uint32_t)home_y);

        pd.ai_established = 1; // 0x004e8c6c
    }

    // ---- the expansion-gate radius, or the flat fallback (0x004e8c76-0x004e8cde) ------------------
    gc.resource_spend_rate_update(player);

    if (pd.ai_score_cat_1 != 0) {
        const int32_t  home_x    = pd.ai_home_tile_x; // RAW, no +1 -- matches
        const int32_t  home_y    = pd.ai_home_tile_y; // ai_expand_gate_value's own field comment
        const uint32_t radius_sq = gc.bldg_max_defense_radius_sq(player, home_x, home_y);
        pd.ai_expand_gate_value  = x87_sqrt_and_trunc(radius_sq); // 0x004e8caf-0x004e8ccc
    } else {
        pd.ai_expand_gate_value = 5; // 0x004e8cd4
    }

    // ---- build-category scoring, unconditional (0x004e8cde) ---------------------------------------
    gc.score_build_categories(player);

    // ---- refresh this player's cached assessment of every OTHER alive player (0x004e8ce5-0x004e8d32)
    //
    // Loop bound is `EBX < 8`, SIGNED (CMP/JL) -- MAX_PLAYERS. The out-pointer is
    // &pd.ai_opponent_assessments[j] (0x004e8d0c-0x004e8d23: ECX = &pd.ai_established, i.e.
    // &pd + 0x10, then LEA EDX,[ECX+0x286c8] = &pd + 0x286d8 = &pd.ai_opponent_assessments[0], plus
    // j*0x3c) -- the TICKING player's own array, indexed by the assessed player j, not j's array.
    for (int32_t j = 0; j < MAX_PLAYERS; ++j) {
        if ((v.strat_players[j].status_flags & PLAYER_STATUS_ALIVE) == 0) continue;
        gc.update_opponent_relations((uint32_t)j, &pd.ai_opponent_assessments[j]);
    }

    gc.recompute_shortage_state((uint32_t)player);

    // ---- the three established+scored-gated planner phases (0x004e8d32-0x004e8ddf) ----------------
    //
    // Each of the first two also gates on one bit of player_data::ai_phase_flags -- see that field's
    // own comment: 0x1 = construction planner (+ repair/upgrade scan), 0x2 = plan_unit_training,
    // 0x4 = the unit-group task machine (used further below). The translator's declared_need for the
    // first two names was taken: AI_PHASE_CONSTRUCTION / AI_PHASE_UNIT_TRAINING now sit beside
    // AI_PHASE_UNIT_GROUPS in ai_state.h, so all three bits of the mask are named in one place.
    if ((pd.ai_phase_flags & AI_PHASE_UNIT_TRAINING) != 0 && pd.ai_established != 0 && pd.ai_score_cat_1 != 0) {
        gc.plan_unit_training(player); // 0x004e8d6a
    }
    if ((pd.ai_phase_flags & AI_PHASE_CONSTRUCTION) != 0 && pd.ai_established != 0 && pd.ai_score_cat_1 != 0) {
        gc.plan_construction((uint32_t)player); // 0x004e8da4
        gc.scan_bldg_repair_upgrade(player);    // 0x004e8dab
    }
    // The third gate drops the phase-flags test entirely -- established+scored is the whole
    // condition (0x004e8dc6-0x004e8dd8).
    if (pd.ai_established != 0 && pd.ai_score_cat_1 != 0) {
        gc.order_collect_available_projects_thunk(player); // 0x004e8dda
    }

    // ---- the AI build queue, unconditional (0x004e8ddf) --------------------------------------------
    gc.bldg_queue_process((uint32_t)player);

    // ---- the unit-group task machine (0x004e8dfc-0x004e8e8a) ---------------------------------------
    if ((pd.ai_phase_flags & AI_PHASE_UNIT_GROUPS) != 0 && pd.ai_established != 0) {
        gc.group_home_guard_replenish(player);
        gc.group_form_patrol(player);
        gc.group_form_surplus_from_pool4(player);
        gc.group_form_standby_from_pool3(player);
        gc.group_expansion_form_or_repurpose((uint32_t)player);
        gc.army_milestone_advance_or_attack((uint32_t)player);

        // The per-group reap/redistribute sweep, starting at AI_SEED_GROUP_COUNT (5) -- the seed
        // groups are never touched here. Loop bound compare is `EBX < ai_group_count`, UNSIGNED
        // (CMP/JC @0x004e8e84-0x004e8e8a). member_count==0 (word compared to 0 via JBE -- equality
        // to zero is sign-independent, so a plain `!= 0` here matches the original for every bit
        // pattern) removes the group; a non-empty group is redistributed instead.
        //
        // THE REDISTRIBUTE ARM INCREMENTS THE INDEX TWICE. The original has ONE increment specific
        // to that arm (INC EBX @0x004e8e61) AND falls through into the SAME shared tail increment
        // (INC EBX @0x004e8e6d) every path reaches -- the remove arm reaches only the shared one.
        // So a group that gets redistributed this tick has its immediate successor skipped entirely.
        // Confirmed real from the listing (both arms' jumps land on 0x004e8e6d / fall into it), not a
        // decompiler artifact -- do not "tidy" this into a uniform ++g.
        uint32_t g = (uint32_t)AI_SEED_GROUP_COUNT;
        while (g < (uint32_t)pd.ai_group_count) {
            if (pd.ai_groups[g].member_count != 0) {
                gc.group_redistribute_units((uint32_t)player, (int32_t)g); // 0x004e8e5c
                ++g;                                                       // 0x004e8e61 (redistribute's own)
            } else {
                gc.group_remove(player, g); // 0x004e8e68
                // no increment of its own -- falls straight into the shared one below
            }
            ++g; // 0x004e8e6d -- the SHARED tail every path reaches
        }
    }

    // ---- the tail: THIS PLAYER'S OWN slot in ITS OWN relation array (0x004e8e8c-0x004e8ecb) --------
    //
    // NOT what the original plate claims ("refresh this player's slot in every OTHER player's
    // relation array"): there is no loop here at all. The assembly computes a single address,
    // &pd.ai_player_relation[player] (EAX folds to player*0x28900 = player*0x288fc + player*4, i.e.
    // the array's own base offset plus index `player` within it), and stores once. Value is -1 while
    // _G_LLM_STRAT_AI_FOREIGN_BLDG_CHANGE_FLAG is non-zero, +1 otherwise.
    pd.ai_player_relation[player] = (*v.foreign_bldg_change_flag != 0) ? -1 : 1;
}

} // namespace detail

void player_tick(int32_t player) {
    const ai_state st = state();
    detail::player_tick(st.read, st.own, live_calls(), player);
}

} // namespace mh::ai
