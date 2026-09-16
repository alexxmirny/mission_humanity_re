//
// sim/sim_unit_passive_engage.cpp -- see sim_unit_passive_engage.h. Translated from the DISASSEMBLY
// (tmp/decomp/llm_strat_unit_passive_engage_tick_004ee50e.asm), not from Ghidra's C: the draft folds
// the outer proto_id/energy guard and the order-pending/idle dispatch into a single
// `if (cond1 && (side_effect, cond2)) {...} else if (...) {...} LAB_004ee6d4: ...` expression whose
// nesting is easy to misread on a skim -- it IS structurally faithful once the braces are followed
// all the way through (LAB_004ee6d4 really is nested inside the whole `energy > 0.0` guard, matching
// the raw JZ/JNC targets at 0x004ee586/0x004ee598 exactly, which both skip past LAB_004ee6d4 straight
// to the loop increment) -- but every branch below was re-walked against the JZ/JNZ/JNC targets
// rather than trusted from the .c's brace nesting.
//
// ---- DECLARED NEED: sim_store has no accessor for _G_LLM_STRAT_AI_ENGAGE_CANDIDATE_SCRATCH_COUNT --
// This function resets and reads that counter six times (see the header). It is already bound in the
// AI domain (ai/ai_state.h's ai_view::engage_scratch_count / ai_store::engage_scratch_count, region
// RID_STRAT_AI_ENGAGE_CANDIDATE_SCRATCH_COUNT), but sim_store has no member for it. This TU
// references `own.engage_candidate_scratch_count()` as if sim_store already had it, per the
// translator brief's "stop and declare it -- do not work around it with an offset" (see
// ai_scan_visible.cpp for the same move made against a missing view member). Needed central addition:
//   sim_state.h, on sim_store:
//     int32_t &engage_candidate_scratch_count() { return *engage_candidate_scratch_count_; }
//   backed by a private `int32_t *engage_candidate_scratch_count_;` constructor parameter, bound in
//   sim_state.cpp's state() via `ptr<int32_t>(RID_STRAT_AI_ENGAGE_CANDIDATE_SCRATCH_COUNT)` -- the
//   SAME RID ai_state.cpp already binds, so this is not a second independent binding (matches the
//   order-queue precedent sim_state.h's own header comment documents: two modules resolving the same
//   RID through mh::state::ptr is fine; only a binding that does NOT come from the registry is the
//   hazard it warns about).
// Until that lands, this TU will not compile on its own -- by design, so the gap stays visible rather
// than silently patched over with a raw offset.
//
#include "sim/sim_unit_passive_engage.h"

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"        // ai_say / trace_budget -- the shared trace sink, not AI state
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const unit_passive_engage_calls &live_unit_passive_engage_calls() {
    static const unit_passive_engage_calls c = {
        MH_LIBMH_BIND(llm_strat_ai_unit_is_order_pending),
        MH_LIBMH_BIND(llm_strat_unit_is_idle_or_patrolling),
        MH_LIBMH_BIND(llm_strat_ai_scan_targets_for_engage),
        MH_LIBMH_BIND(llm_strat_ai_target_ref_is_alive),
        MH_LIBMH_BIND(llm_strat_ai_engage_select_and_commit),
        MH_LIBMH_BIND(llm_strat_ai_scan_target_list_for_engage_candidates),
        MH_LIBMH_BIND(llm_strat_ai_unit_scan_engage_candidates_in_range),
        MH_LIBMH_BIND(llm_strat_ai_engage_filter_and_commit_target),
        MH_LIBMH_BIND(llm_strat_unit_issue_default_order),
    };
    return c;
}

namespace detail {

void unit_passive_engage_tick(const sim_view &v, sim_store &own, const unit_passive_engage_calls &c,
                              int32_t player) {
    // ---- ENTRY: same four instructions as llm_strat_ai_active_unit_tick's entry (0x004ee51d-
    // 0x004ee559) -- see that file's derivation of the folded diagonal self-index. Translated
    // independently here, not factored into a shared helper (brief rule 4).
    own.player_at((uint32_t)player).ai_player_relation[player] =
        (*v.foreign_bldg_change_flag == 0) ? 1 : -1;

    // ---- COUNT-DRIVEN roster walk (0x004ee559-0x004ee72d) -- same idiom as
    // target_list_scan_visible_enemies (ai_scan_visible.cpp): `remaining` seeds from the raw 2 bytes
    // at units[player][0]'s offset 0 (unit_above) reassembled little-endian; `unit_id` starts at 1
    // and increments every iteration; `remaining` decrements only when the slot is occupied.
    const unit &u0        = unit_of(v, (uint32_t)player, 0);
    uint32_t    remaining = uint32_t(u0.unit_above[0]) | (uint32_t(u0.unit_above[1]) << 8);
    int32_t     unit_id   = 1;

    while (remaining != 0) {
        const unit &u = unit_of(v, (uint32_t)player, unit_id);
        if (u.unit_proto_id != 0) {
            --remaining;
            // THE WHOLE REST OF THIS ITERATION -- including the shared tail below -- is inside this
            // guard (0x004ee598 JNC 0x004ee724): an unarmed/dead unit does nothing at all, not even
            // the tail. FLDZ/FCOMP/FNSTSW/SAHF/JNC at 0x004ee58d-0x004ee598: JNC (skip to next unit)
            // fires only on the ORDERED energy<=0.0 case -- x87's unordered result (energy is NaN)
            // also sets C0/CF and therefore does NOT skip, so the original processes a NaN-energy
            // unit same as a positive-energy one. `0.0 < u.energy` is IEEE `<`, false on NaN, which
            // would skip -- the opposite of the original (reimpl-verify finding, 2026-08-10). Same
            // `!(<= 0.0)` idiom sim_bldg_alive.cpp's header already documents for this exact
            // FCOMP/JNC shape, and the one llm_strat_unit_attack_target_is_dead's own energy compare
            // already uses (sim_unit_state_predicates.cpp) -- this function's own gate had missed it.
            if (!(u.energy <= 0.0)) {
                if (c.unit_is_order_pending((uint32_t)player, (uint32_t)unit_id) == 0) {
                    // ---- not order-pending: idle/patrolling graduated target-seek ----------------
                    if (c.unit_is_idle_or_patrolling(player, unit_id) != 0) {
                        if (u.passive_engage_target_index != 0) {
                            // (1/6) re-validate the existing passive-engage target; committing here
                            // skips straight to the shared tail.
                            own.engage_candidate_scratch_count() = 0;
                            c.scan_targets_for_engage(player, (int32_t)u.passive_engage_target_index);
                            const int32_t alive = c.target_ref_is_alive(
                                (uint32_t)u.passive_engage_target_ref,
                                (int32_t)u.passive_engage_target_index);
                            if (alive == 0)
                                own.unit_at((uint32_t)player, unit_id).passive_engage_target_index = 0;
                            if (c.engage_select_and_commit(player, unit_id, 1) != 0) goto tail;
                        }
                        // (2/6) fresh scan around the unit itself.
                        own.engage_candidate_scratch_count() = 0;
                        c.scan_targets_for_engage(player, unit_id);
                        if (c.engage_select_and_commit(player, unit_id, 1) == 0) {
                            // (3/6) fall back to the unit's AI-group target list.
                            own.engage_candidate_scratch_count() = 0;
                            c.scan_target_list_for_engage_candidates(player, u.ai_group_index);
                            if (c.engage_select_and_commit(player, unit_id, 1) == 0) {
                                // (4/6) last resort: a fixed 0xe0 range scan; commit result discarded.
                                own.engage_candidate_scratch_count() = 0;
                                c.unit_scan_engage_candidates_in_range(player, unit_id, 0xe0u);
                                c.engage_select_and_commit(player, unit_id, 1);
                            }
                        }
                    }
                    // else: idle_or_patrolling == 0 -- no-op, falls straight through to the tail.
                } else {
                    // ---- order-pending: opportunistic default-order re-issue --------------------
                    if ((u.engagement_flags & 0x1u) != 0 && (u.target_ref & 0x40) != 0) {
                        // (5/6)
                        own.engage_candidate_scratch_count() = 0;
                        c.scan_targets_for_engage(player, unit_id);
                        if (own.engage_candidate_scratch_count() != 0)
                            c.unit_issue_default_order((uint32_t)player, unit_id);
                    }
                    // Falls through to the tail regardless of whether the inner check ran.
                }

            tail:
                // ---- SHARED TAIL (LAB_004ee6d4), reached 3 ways: the goto above, the "not idle"
                // no-op fallthrough, and the end of the order-pending branch fallthrough. -----------
                if (u.target2_ref == 0) {
                    // (6/6)
                    own.engage_candidate_scratch_count() = 0;
                    c.scan_targets_for_engage(player, unit_id);
                    c.scan_target_list_for_engage_candidates(player, u.ai_group_index);
                    c.unit_scan_engage_candidates_in_range(player, unit_id, 0xa0u);
                    c.engage_filter_and_commit_target((uint32_t)player, (uint32_t)unit_id);
                }
            }
        }
        ++unit_id;
    }

    // The ONLY exit path in the function.
    own.player_at((uint32_t)player).ai_target_list_count = 0;
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

void unit_passive_engage_tick(int32_t player) {
    sim_state st = state();
    detail::unit_passive_engage_tick(st.read, st.own, live_unit_passive_engage_calls(), player);
}


} // namespace mh::sim
