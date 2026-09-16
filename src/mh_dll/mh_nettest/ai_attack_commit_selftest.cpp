//
// ai_attack_commit_selftest.cpp -- offline oracle for llm_strat_ai_commit_attack_order @0x004d55ef
// and llm_strat_ai_commit_attack_order_alt @0x004d5708 (RI-AI batch D / AI1D). Both bodies stamp the
// attacker's engagement-commit word, estimate weapon damage against the target and cache it on the
// attacker, accrue that cached estimate onto the target's own incoming-damage tally (building or
// unit, depending on the target ref's kind bit), dispatch exactly one outward order-enqueue call, and
// then bump the attacker's player-level `ai_attack_orders_issued` counter through a tail the two
// functions share via a real `JMP` in the original image. This file pins: the OR-not-assign engage
// stamp, that the stamp happens BEFORE the damage estimate (not after), which register gates the
// building/unit split (a2/target_ref's bit 0x40, not the attacker's own ref), which argument is
// passed RAW (target_ref into the damage estimate) versus MASKED to its owner nibble (every argument
// of every enqueue call), the int16_t truncation boundaries on the cached estimate, and -- the
// specific hazard of a near-identical pair -- that `commit_attack_order` calls
// `order_attack_target_enqueue` while `commit_attack_order_alt` calls
// `order_attack_target_alt_enqueue`, while BOTH functions' building branches call the exact same
// `order_attack_building_reposition_alt_enqueue`.
//
#include "ai_test_support.h"

#include "ai/ai_attack_commit.h"

namespace mh::ai::test {
namespace {

// ---- recorders ------------------------------------------------------------------------------------
//
// One recorder per outward edge batch D wires into `ai_calls` for these two bodies. Every OTHER
// member of the calls set this file builds is left null, so a body that reaches through an unbound
// member (e.g. if a mutation swapped in the wrong callee) crashes loudly instead of returning zero --
// house convention, see ai_selftest.cpp's `intel_stub`.

struct est_call {
    int32_t  attacker_player;
    int32_t  attacker_unit_index;
    uint32_t target_ref;
    int32_t  target_index;
};
struct enqueue_call {
    uint32_t player;
    int32_t  unit_idx;
    uint32_t target_player;
    int32_t  target_index;
    uint32_t weapon_idx;
};

std::vector<est_call>     g_est;
std::vector<enqueue_call> g_target;          // order_attack_target_enqueue -- PRIMARY's unit branch only
std::vector<enqueue_call> g_target_alt;      // order_attack_target_alt_enqueue -- _alt's unit branch only
std::vector<enqueue_call> g_bldg_reposition; // order_attack_building_reposition_alt_enqueue -- BOTH functions' building branch
uint32_t                  g_est_return = 0;

// ---- the ordering probe -----------------------------------------------------------------------
// A test points these at the attacker's own engagement/order-status bytes right before calling into
// the body under test. The estimate-damage stub samples them AT THE MOMENT it is invoked (i.e. mid-
// body, before the body has a chance to overwrite anything else), so the sample proves 0x004d5617 /
// 0x004d5730 (the engagement-commit OR) executed BEFORE 0x004d5645 / 0x004d56a3 / 0x004d575e /
// 0x004d57bf (the estimate call) rather than after -- an order claim the header banner makes but a
// naive "check the final state" assertion cannot see, since the final state looks identical either
// way.
const uint8_t *g_snapshot_engagement_flags      = nullptr;
const uint8_t *g_snapshot_order_status_flags    = nullptr;
uint8_t        g_engagement_flags_at_est_time   = 0xAAu; // poison: "the stub never ran" stays visible
uint8_t        g_order_status_flags_at_est_time = 0xAAu;

uint32_t st_estimate_weapon_damage(int32_t attacker_player, int32_t attacker_unit_index,
                                   uint32_t target_ref, int32_t target_index) {
    if (g_snapshot_engagement_flags != nullptr)
        g_engagement_flags_at_est_time = *g_snapshot_engagement_flags;
    if (g_snapshot_order_status_flags != nullptr)
        g_order_status_flags_at_est_time = *g_snapshot_order_status_flags;
    g_est.push_back({attacker_player, attacker_unit_index, target_ref, target_index});
    return g_est_return;
}
void st_order_attack_target_enqueue(uint32_t player, int32_t unit_idx, uint32_t target_player,
                                    int32_t target_index, uint32_t weapon_idx) {
    g_target.push_back({player, unit_idx, target_player, target_index, weapon_idx});
}
void st_order_attack_target_alt_enqueue(uint32_t player, int32_t unit_idx, uint32_t target_player,
                                        int32_t target_index, uint32_t weapon_idx) {
    g_target_alt.push_back({player, unit_idx, target_player, target_index, weapon_idx});
}
void st_order_attack_building_reposition_alt_enqueue(uint32_t player, int32_t unit_idx,
                                                     uint32_t target_player, int32_t target_bldg_idx,
                                                     uint32_t weapon_idx) {
    g_bldg_reposition.push_back({player, unit_idx, target_player, target_bldg_idx, weapon_idx});
}

void clear() {
    g_est.clear();
    g_target.clear();
    g_target_alt.clear();
    g_bldg_reposition.clear();
    g_est_return                     = 0;
    g_snapshot_engagement_flags      = nullptr;
    g_snapshot_order_status_flags    = nullptr;
    g_engagement_flags_at_est_time   = 0xAAu;
    g_order_status_flags_at_est_time = 0xAAu;
}

// A calls set with exactly the four members these two bodies reach bound, everything else null.
const ai_calls &calls() {
    static const ai_calls c = [] {
        ai_calls t{};
        t.estimate_weapon_damage                       = &st_estimate_weapon_damage;
        t.order_attack_target_enqueue                  = &st_order_attack_target_enqueue;
        t.order_attack_target_alt_enqueue              = &st_order_attack_target_alt_enqueue;
        t.order_attack_building_reposition_alt_enqueue = &st_order_attack_building_reposition_alt_enqueue;
        return t;
    }();
    return c;
}

// Total enqueue calls recorded across ALL THREE possible callees -- used to pin "exactly one outward
// order-enqueue call, never zero, never two" (the header's step 4).
size_t total_enqueue_calls() {
    return g_target.size() + g_target_alt.size() + g_bldg_reposition.size();
}

} // namespace

void run_attack_commit_tests() {
    printf("-- ai_attack_commit (llm_strat_ai_commit_attack_order/_alt) --\n");

    // T1: PRIMARY, UNIT branch. Covers: the OR-semantics commit stamp, that the stamp precedes the
    // damage estimate, the branch gate reading a2 (target_ref) and not the attacker's own ref, the
    // RAW-vs-MASKED argument split, int16_t truncation + accrual math on the UNIT tally (not the
    // building tally), the target_enqueue callee + its five arguments, the trailing literal 4u, and
    // the shared-tail counter landing on the ATTACKER's player.
    {
        fixture f;
        clear();
        const uint32_t ATTACKER_PLAYER = 3, TARGET_PLAYER = 6;
        const int32_t  ATTACKER_IDX = 11, TARGET_IDX = 47;
        // attacker_ref carries an extra 0x40 bit (its OWN building-kind bit) on top of its owner
        // nibble -- 0x004d563d/0x004d5756 test BL (a2/target_ref), never EAX/param_1, so this must
        // NOT select the building branch. Also carries an extra 0x20 bit to prove owner extraction
        // masks it away.
        const uint32_t attacker_ref = 0x60u | ATTACKER_PLAYER;
        // target_ref carries an extra 0x10 bit above its owner nibble, WITHOUT the 0x40 building bit
        // -- unit branch, and the extra bit must survive RAW into the estimate call but be masked
        // away everywhere else.
        const uint32_t target_ref = 0x10u | TARGET_PLAYER;

        unit &au                                              = f.u(ATTACKER_PLAYER, ATTACKER_IDX);
        au.engagement_flags                                   = 0x02u; // a pre-existing unrelated bit -- must SURVIVE the OR
        au.order_status_flags                                 = 0x01u; // ditto
        f.u(TARGET_PLAYER, TARGET_IDX).incoming_threat_damage = 1000;  // baseline to accrue onto
        f.b(TARGET_PLAYER, TARGET_IDX).incoming_damage_tally  = 777;   // must stay untouched (wrong roster)
        g_snapshot_engagement_flags                           = &au.engagement_flags;
        g_snapshot_order_status_flags                         = &au.order_status_flags;
        g_est_return                                          = 42;

        detail::commit_attack_order(f.view(), f.store(), calls(), attacker_ref, ATTACKER_IDX,
                                    target_ref, TARGET_IDX);

        ck(au.engagement_flags == 0x03u,
           "T1: engage_commit_set ORs 0x01 into engagement_flags without clobbering bit 0x02, 0x004d5617");
        ck(au.order_status_flags == 0x81u,
           "T1: engage_commit_set ORs 0x80 into order_status_flags without clobbering bit 0x01, 0x004d5617");
        ck(g_engagement_flags_at_est_time == 0x03u && g_order_status_flags_at_est_time == 0x81u,
           "T1: the commit stamp is already visible when estimate_weapon_damage fires -- OR precedes "
           "CALL, 0x004d5617 before 0x004d5645");

        ck(g_est.size() == 1, "T1: exactly one estimate_weapon_damage call, unit branch, 0x004d56a3");
        ck(g_est[0].attacker_player == (int32_t)ATTACKER_PLAYER,
           "T1: estimate_weapon_damage gets attacker_player MASKED to its owner nibble (AND EAX,0xf, "
           "0x004d5605), not the raw 0x60|player ref");
        ck(g_est[0].attacker_unit_index == ATTACKER_IDX,
           "T1: estimate_weapon_damage gets the attacker's unit index unchanged, 0x004d56a3 reload");
        ck(g_est[0].target_ref == target_ref,
           "T1: estimate_weapon_damage gets target_ref RAW/unmasked (EBX carried from entry, "
           "0x10|player survives), not owner-masked");
        ck(g_est[0].target_index == TARGET_IDX,
           "T1: estimate_weapon_damage gets target_index unchanged (ECX carried from entry, "
           "0x004d5603 MOV EDI,ECX leaves ECX itself untouched through 0x004d56a3)");

        ck(au.committed_weapon_damage_est == 42,
           "T1: the estimate is cached on the attacker as int16_t, 0x004d56a8");
        ck(f.u(TARGET_PLAYER, TARGET_IDX).incoming_threat_damage == 1000 + 42,
           "T1: the cached estimate accrues onto the TARGET UNIT's incoming_threat_damage, 0x004d56c5");
        ck(f.b(TARGET_PLAYER, TARGET_IDX).incoming_damage_tally == 777,
           "T1: the unit branch does NOT touch the building tally -- wrong-roster hazard, 0x004d5683 "
           "not reached");

        ck(total_enqueue_calls() == 1,
           "T1: exactly one outward order-enqueue call fires, never zero, never two -- the JZ at "
           "0x004d5640 selects exactly one of the two arms");
        ck(g_target.size() == 1 && g_target_alt.empty() && g_bldg_reposition.empty(),
           "T1: the unit branch of the PRIMARY function dispatches order_attack_target_enqueue, "
           "0x004d56db -- not the _alt callee and not the building callee");
        ck(g_target[0].player == ATTACKER_PLAYER && g_target[0].unit_idx == ATTACKER_IDX &&
               g_target[0].target_player == TARGET_PLAYER && g_target[0].target_index == TARGET_IDX &&
               g_target[0].weapon_idx == 4u,
           "T1: order_attack_target_enqueue gets (attacker_player MASKED, attacker_unit_index, "
           "target_owner MASKED, target_index, 4u) -- 0x004d568c-0x004d56db");

        ck(f.players[ATTACKER_PLAYER].ai_attack_orders_issued == 1,
           "T1: the shared tail bumps the ATTACKER's ai_attack_orders_issued, 0x004d56fa");
        ck(f.players[TARGET_PLAYER].ai_attack_orders_issued == 0,
           "T1: the shared tail does NOT bump the TARGET's counter -- attacker/target transposition "
           "hazard, 0x004d56e0-0x004d56e3 re-derives ref_owner(attacker_ref) specifically");
    }

    // T2: PRIMARY, BUILDING branch. Covers: the building-tally accrual (not the unit tally), the
    // order_attack_building_reposition_alt_enqueue callee + its arguments, and that the branch really
    // is gated by target_ref's bit 0x40.
    {
        fixture f;
        clear();
        const uint32_t ATTACKER_PLAYER = 2, TARGET_PLAYER = 5;
        const int32_t  ATTACKER_IDX = 8, TARGET_IDX = 19;
        const uint32_t attacker_ref = ATTACKER_PLAYER;
        const uint32_t target_ref   = 0x40u | TARGET_PLAYER; // REF_BLDG_BIT set -- building branch

        f.u(TARGET_PLAYER, TARGET_IDX).incoming_threat_damage = 555; // must stay untouched (wrong roster)
        f.b(TARGET_PLAYER, TARGET_IDX).incoming_damage_tally  = 200;
        g_est_return                                          = 900; // an in-range value; T5 covers the int16_t boundaries

        detail::commit_attack_order(f.view(), f.store(), calls(), attacker_ref, ATTACKER_IDX,
                                    target_ref, TARGET_IDX);

        ck(f.u(ATTACKER_PLAYER, ATTACKER_IDX).engagement_flags == 0x01u &&
               f.u(ATTACKER_PLAYER, ATTACKER_IDX).order_status_flags == 0x80u,
           "T2: engage_commit_set fires on the building branch too -- it is UNCONDITIONAL, 0x004d5617");
        ck(g_est.size() == 1 && g_est[0].target_ref == target_ref,
           "T2: estimate_weapon_damage runs on the building branch with target_ref RAW, 0x004d5645");
        ck(f.u(ATTACKER_PLAYER, ATTACKER_IDX).committed_weapon_damage_est == 900,
           "T2: the estimate is cached on the attacker on the building branch too, 0x004d564a");
        ck(f.b(TARGET_PLAYER, TARGET_IDX).incoming_damage_tally == 200 + 900,
           "T2: the cached estimate accrues onto the TARGET BUILDING's incoming_damage_tally, 0x004d5683");
        ck(f.u(TARGET_PLAYER, TARGET_IDX).incoming_threat_damage == 555,
           "T2: the building branch does NOT touch the unit tally -- wrong-roster hazard, 0x004d56c5 "
           "not reached");

        ck(total_enqueue_calls() == 1,
           "T2: exactly one outward order-enqueue call fires -- the JZ at 0x004d5640 taken");
        ck(g_bldg_reposition.size() == 1 && g_target.empty() && g_target_alt.empty(),
           "T2: the building branch dispatches order_attack_building_reposition_alt_enqueue, 0x004d5699");
        ck(g_bldg_reposition[0].player == ATTACKER_PLAYER && g_bldg_reposition[0].unit_idx == ATTACKER_IDX &&
               g_bldg_reposition[0].target_player == TARGET_PLAYER &&
               g_bldg_reposition[0].target_index == TARGET_IDX && g_bldg_reposition[0].weapon_idx == 4u,
           "T2: order_attack_building_reposition_alt_enqueue gets (attacker_player MASKED, "
           "attacker_unit_index, target_owner MASKED, target_index, 4u) -- 0x004d568c-0x004d5699");

        ck(f.players[ATTACKER_PLAYER].ai_attack_orders_issued == 1,
           "T2: the shared tail bumps the attacker's counter on the building branch too, 0x004d56fa");
    }

    // T3: THE PAIR HAZARD, unit branch. Same inputs as a T1-shaped case but through
    // commit_attack_order_ALT: the ONE place the two functions differ. If this ever calls
    // order_attack_target_enqueue instead of order_attack_target_alt_enqueue -- or fires both, or
    // neither -- the two bodies have been merged or swapped.
    {
        fixture f;
        clear();
        const uint32_t ATTACKER_PLAYER = 4, TARGET_PLAYER = 1;
        const int32_t  ATTACKER_IDX = 30, TARGET_IDX = 5;
        const uint32_t attacker_ref = ATTACKER_PLAYER;
        const uint32_t target_ref   = TARGET_PLAYER; // no REF_BLDG_BIT -- unit branch
        g_est_return                = 17;

        detail::commit_attack_order_alt(f.view(), f.store(), calls(), attacker_ref, ATTACKER_IDX,
                                        target_ref, TARGET_IDX);

        ck(total_enqueue_calls() == 1,
           "T3: exactly one outward order-enqueue call fires (alt, unit) -- the JZ at 0x004d5759 not taken");
        ck(g_target_alt.size() == 1 && g_target.empty() && g_bldg_reposition.empty(),
           "T3: commit_attack_order_alt's unit branch dispatches order_attack_target_ALT_enqueue, "
           "0x004d57f7 -- NOT order_attack_target_enqueue (the primary's callee)");
        ck(g_target_alt[0].player == ATTACKER_PLAYER && g_target_alt[0].unit_idx == ATTACKER_IDX &&
               g_target_alt[0].target_player == TARGET_PLAYER && g_target_alt[0].target_index == TARGET_IDX &&
               g_target_alt[0].weapon_idx == 4u,
           "T3: order_attack_target_alt_enqueue gets the same five-argument shape as the primary's "
           "target_enqueue -- 0x004d57a5-0x004d57f7");
        ck(f.u(ATTACKER_PLAYER, ATTACKER_IDX).committed_weapon_damage_est == 17,
           "T3: _alt still caches the estimate on the attacker, 0x004d5763");
        ck(f.u(TARGET_PLAYER, TARGET_IDX).incoming_threat_damage == 17,
           "T3: _alt still accrues onto the target unit's incoming_threat_damage, 0x004d57e1");
        ck(f.players[ATTACKER_PLAYER].ai_attack_orders_issued == 1,
           "T3: _alt's unit branch still reaches the shared tail counter bump, 0x004d56fa (via the "
           "JMP at 0x004d57fc)");
    }

    // T4: THE PAIR HAZARD, building branch. commit_attack_order_alt's building branch must call the
    // SAME callee as the primary's (order_attack_building_reposition_alt_enqueue) -- the header banner
    // is explicit that this is NOT where the two functions differ, so if a translation invented a
    // separate "alt" building callee here, this is the case that catches it.
    {
        fixture f;
        clear();
        const uint32_t ATTACKER_PLAYER = 7, TARGET_PLAYER = 0;
        const int32_t  ATTACKER_IDX = 2, TARGET_IDX = 90;
        const uint32_t attacker_ref = ATTACKER_PLAYER;
        const uint32_t target_ref   = 0x40u | TARGET_PLAYER; // REF_BLDG_BIT set -- building branch
        g_est_return                = 5;

        detail::commit_attack_order_alt(f.view(), f.store(), calls(), attacker_ref, ATTACKER_IDX,
                                        target_ref, TARGET_IDX);

        ck(total_enqueue_calls() == 1,
           "T4: exactly one outward order-enqueue call fires (alt, building) -- the JZ at 0x004d5759 taken");
        ck(g_bldg_reposition.size() == 1 && g_target.empty() && g_target_alt.empty(),
           "T4: commit_attack_order_alt's building branch dispatches "
           "order_attack_building_reposition_alt_enqueue, 0x004d57b2 -- the SAME callee as the "
           "primary's building branch, not a distinct _alt building callee");
        ck(g_bldg_reposition[0].player == ATTACKER_PLAYER && g_bldg_reposition[0].unit_idx == ATTACKER_IDX &&
               g_bldg_reposition[0].target_player == TARGET_PLAYER &&
               g_bldg_reposition[0].target_index == TARGET_IDX && g_bldg_reposition[0].weapon_idx == 4u,
           "T4: the arguments match the primary's building branch shape exactly, 0x004d57a5-0x004d57b2");
    }

    // T5: THE INT16_T TRUNCATION/SIGN BOUNDARIES on committed_weapon_damage_est, both sides. 0xFFFF
    // truncates to -1, 0x8000 truncates to INT16_MIN, and 0 stays 0 (the vacuous-direction guard: a
    // body that never wrote the field at all would also read back 0 from a zeroed fixture, so this
    // case only tells the OTHER two apart -- it exists to keep the boundary set symmetric).
    {
        struct boundary {
            uint32_t    raw;
            int16_t     expect;
            const char *what;
        };
        const boundary cases[] = {
            {0xFFFFu, (int16_t)-1, "T5: estimate 0xffff truncates to int16_t -1, 0x004d564a store"},
            {0x8000u, (int16_t)-32768, "T5: estimate 0x8000 truncates to INT16_MIN, 0x004d564a store"},
            {0u, (int16_t)0, "T5: estimate 0 truncates to 0, 0x004d564a store"},
        };
        for (const boundary &kase : cases) {
            fixture f;
            clear();
            const uint32_t ATTACKER_PLAYER = 1, TARGET_PLAYER = 2;
            const int32_t  ATTACKER_IDX = 0, TARGET_IDX = 0;
            g_est_return = kase.raw;
            detail::commit_attack_order(f.view(), f.store(), calls(), ATTACKER_PLAYER, ATTACKER_IDX,
                                        TARGET_PLAYER, TARGET_IDX);
            ck(f.u(ATTACKER_PLAYER, ATTACKER_IDX).committed_weapon_damage_est == kase.expect, kase.what);
        }
    }

    // T6: THE SHARED TAIL is genuinely shared code, not four independently-translated copies. Drive
    // all four (function, branch) combinations against the SAME attacker and confirm the counter
    // accumulates once per call regardless of which branch or which function reached it, while a
    // bystander player's counter never moves -- pinning that the tail (0x004d56e0-0x004d5701) is one
    // piece of code all four paths funnel into (three by JMP, one by fallthrough), per the header's
    // "THE SHARED TAIL" section.
    {
        fixture f;
        clear();
        const uint32_t ATTACKER_PLAYER = 6, TARGET_PLAYER = 3, BYSTANDER = 5;
        const int32_t  IDX = 10;
        g_est_return       = 1;

        detail::commit_attack_order(f.view(), f.store(), calls(), ATTACKER_PLAYER, IDX, TARGET_PLAYER, IDX);
        ck(f.players[ATTACKER_PLAYER].ai_attack_orders_issued == 1,
           "T6a: commit_attack_order/unit reaches the shared tail via fallthrough, 0x004d56db->0x004d56e0");

        detail::commit_attack_order(f.view(), f.store(), calls(), ATTACKER_PLAYER, IDX,
                                    0x40u | TARGET_PLAYER, IDX);
        ck(f.players[ATTACKER_PLAYER].ai_attack_orders_issued == 2,
           "T6b: commit_attack_order/building reaches the shared tail via JMP 0x004d569e");

        detail::commit_attack_order_alt(f.view(), f.store(), calls(), ATTACKER_PLAYER, IDX, TARGET_PLAYER,
                                        IDX);
        ck(f.players[ATTACKER_PLAYER].ai_attack_orders_issued == 3,
           "T6c: commit_attack_order_alt/unit reaches the PRIMARY's shared tail via JMP 0x004d57fc");

        detail::commit_attack_order_alt(f.view(), f.store(), calls(), ATTACKER_PLAYER, IDX,
                                        0x40u | TARGET_PLAYER, IDX);
        ck(f.players[ATTACKER_PLAYER].ai_attack_orders_issued == 4,
           "T6d: commit_attack_order_alt/building reaches the PRIMARY's shared tail via JMP 0x004d57b7");

        ck(f.players[BYSTANDER].ai_attack_orders_issued == 0 && f.players[TARGET_PLAYER].ai_attack_orders_issued == 0,
           "T6e: no combination bumps a bystander's or the target's counter -- only the attacker's "
           "(0x004d56e0-0x004d56e3 re-derives ref_owner(attacker_ref) specifically), "
           "across all four (function, branch) paths");
    }
}

} // namespace mh::ai::test
