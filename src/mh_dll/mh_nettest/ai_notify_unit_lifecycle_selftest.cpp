//
// ai_notify_unit_lifecycle_selftest.cpp -- offline oracle for llm_strat_ai_notify_unit_lifecycle
// @0x004dbb38 (RI-AI batch D / AI1D). OFFLINE ONLY: this function has no shadow arm (see the
// header's PROOF PATH note -- arming one would double-fire llm_strat_ai_group_member_link's own
// writes), so this file is the function's entire proof. It pins: the top ai_enabled gate and the
// player_ 4-bit mask (0x004dbb4e); the jump-table dispatch and its unassigned slot 3
// (0x004dbb74-0x004dbb7d); caseD_1's threat/engage clear + group-link reset + pending-spawn
// decrement + the state==0x1f classify gate + its ALWAYS-runs queue-reconcile tail; caseD_2's
// pure queue-reconcile-only body (mask/kind boundaries, the byte/word truncation, first-match-
// stops, the count bound); caseD_4's unconditional group-link reset + heli-mother early-out +
// invasion-force direct link + its OWN classify chain, whose group assignment for soldier/ground
// DIFFERS from caseD_1's (group 0, not group 2) and which never reaches the queue scan; and that
// `unit_type` is truly dead going in.
//
#include "ai_test_support.h"

#include "ai/ai_notify_unit_lifecycle.h"

namespace mh::ai::test {
namespace {

// ---- the group_member_link / four-classifier recorder --------------------------------------------
//
// A calls set with the five members this body reaches bound and everything else deliberately null,
// so an unstubbed call fails loudly rather than returning zero (ai_test_support.h's own rule,
// mirrored from ai_selftest.cpp's intel_stub).
struct link_call {
    uint32_t player;
    int32_t  group;
    uint32_t unit_id;
};

struct recorder {
    std::vector<link_call> links;
    int                    soldier_calls = 0, ground_calls = 0, plane_calls = 0, heli_calls = 0;
    int32_t                soldier_answer = 0, ground_answer = 0, plane_answer = 0, heli_answer = 0;

    void clear() {
        links.clear();
        soldier_calls = ground_calls = plane_calls = heli_calls = 0;
        soldier_answer = ground_answer = plane_answer = heli_answer = 0;
    }
};
recorder g_rec;

int32_t st_soldier(uint32_t /*player*/, uint32_t /*unit_id*/) {
    ++g_rec.soldier_calls;
    return g_rec.soldier_answer;
}
int32_t st_ground(uint16_t /*player*/, uint32_t /*unit_id*/) {
    ++g_rec.ground_calls;
    return g_rec.ground_answer;
}
int32_t st_plane(uint32_t /*player*/, uint32_t /*unit_id*/) {
    ++g_rec.plane_calls;
    return g_rec.plane_answer;
}
int32_t st_heli(uint32_t /*player*/, uint32_t /*unit_id*/) {
    ++g_rec.heli_calls;
    return g_rec.heli_answer;
}
void st_link(uint32_t player, int32_t group, uint32_t unit_id) {
    g_rec.links.push_back({player, group, unit_id});
}

const ai_calls &calls() {
    static const ai_calls c = [] {
        ai_calls t{};
        t.unit_is_ai_soldier = &st_soldier;
        t.unit_is_ai_ground  = &st_ground;
        t.unit_is_ai_plane   = &st_plane;
        t.unit_is_ai_heli    = &st_heli;
        t.group_member_link  = &st_link;
        return t;
    }();
    return c;
}

// ---- H1's own stub: mutates the fixture's unit_proto_id as a side effect of the FIRST classify
// call, so the queue-reconcile scan that follows can only see the new value if it genuinely
// re-reads unit_proto_id rather than using one captured before the classify chain ran. The
// canned-answer recorder above cannot express "and it also writes state", so this one case binds
// its own stub, exactly the way ai_selftest.cpp's `drain_step` does for unit_group_tick.
fixture *g_mutate_target = nullptr;
uint16_t g_mutate_to     = 0;

int32_t st_soldier_mutates_proto(uint32_t player, uint32_t unit_id) {
    ++g_rec.soldier_calls;
    if (g_mutate_target) g_mutate_target->u((int)player, (int)unit_id).unit_proto_id = g_mutate_to;
    return 1;
}

} // namespace

void run_notify_unit_lifecycle_tests() {
    printf("-- ai_notify_unit_lifecycle (llm_strat_ai_notify_unit_lifecycle) --\n");

    fixture f;

    // PLAYER != 0 (catches a player-stride bug) and UNIT != PLAYER (catches an index-swap bug).
    const uint16_t PLAYER         = 2;
    const uint32_t UNIT           = 57;
    const uint16_t UNIT_TYPE_JUNK = 0xbeef; // proven unused by the body -- see group G below

    auto call = [&](uint16_t player_, uint16_t unit_type_, uint32_t unit_id_, uint32_t event_) {
        return detail::notify_unit_lifecycle(f.view(), f.store(), calls(), player_, unit_type_,
                                             unit_id_, event_);
    };
    auto no_calls = [&](const char *why) {
        ck(g_rec.links.empty() && g_rec.soldier_calls == 0 && g_rec.ground_calls == 0 &&
               g_rec.plane_calls == 0 && g_rec.heli_calls == 0,
           why);
    };

    // ================================================================================================
    // A. the top ai_enabled gate (0x004dbb67) and the player_ mask (0x004dbb4e)
    // ================================================================================================
    {
        f.reset();
        g_rec.clear();
        f.players[PLAYER].ai_enabled = 0;
        const auto r                 = call(PLAYER, UNIT_TYPE_JUNK, UNIT, 1);
        ck(r.ai_disabled_at_entry, "A1: ai_enabled==0 top gate fires and returns immediately, 0x004dbb67");
        ck(r.player == PLAYER, "A1: player is recorded even on the early return");
        ck(r.event == -1,
           "A1: event stays the -1 sentinel -- the top gate returns before param_4 is ever read "
           "@0x004dbb74");
        ck(!r.out_of_range_event && !r.threat_and_engage_cleared && !r.ai_disabled_recheck &&
               !r.group_links_reset && !r.pending_spawn_decremented && !r.classified &&
               !r.group_linked && r.group_index_used == -1 && !r.heli_mother_early_out &&
               !r.invasion_force_direct_link && !r.queue_reconciled,
           "A1: every other lifecycle_notify_report field stays at its zero-value when the top gate fires");
        no_calls("A1: the top gate is a pure return -- no classifier and no group_member_link fire, "
                 "0x004dbb67");
    }
    {
        // Masked landing row: the parameter is uint16_t, and `(uint32_t)player_ & 0xfu` widens
        // BEFORE masking, so every high bit of the 16-bit value is discarded, not just the ones
        // above bit 3 of a byte. Event 0 (no_op) is used deliberately: it is the one dispatch arm
        // that touches nothing past player_data::ai_enabled, so this case cannot run off the end of
        // `units`/`buildings`, which the fixture provisions only for players 0..7 (unlike `players`
        // itself, which the fixture deliberately over-provisions to 9 rows) -- see this file's
        // report to the conductor for why a deeper masked-player case is unsafe today.
        f.reset();
        g_rec.clear();
        f.players[8].ai_enabled = 1;
        const auto r            = call(0xfff8, UNIT_TYPE_JUNK, UNIT, 0);
        ck(r.player == 8, "A2: AND EAX,0xf masks the full 16-bit player_ to its low 4 bits, 0x004dbb4e");
        ck(!r.ai_disabled_at_entry, "A2: row 8's own ai_enabled -- not row 0xfff8's -- is what gates it");
    }

    // ================================================================================================
    // B. the jump-table dispatch (0x004dbb74-0x004dbb7d): in range 0/valid, unassigned slot 3, and
    //    the unsigned out-of-range catch-all
    // ================================================================================================
    {
        f.reset();
        g_rec.clear();
        f.players[PLAYER].ai_enabled = 1;
        const auto r                 = call(PLAYER, UNIT_TYPE_JUNK, UNIT, 0);
        ck(!r.ai_disabled_at_entry && !r.out_of_range_event && r.event == 0,
           "B1: event 0 (caseD_0) is in range and recorded, 0x004dbb7d/0x004dbcc7");
        ck(!r.threat_and_engage_cleared && !r.ai_disabled_recheck && !r.group_links_reset &&
               !r.pending_spawn_decremented && !r.classified && !r.group_linked &&
               r.group_index_used == -1 && !r.heli_mother_early_out && !r.invasion_force_direct_link &&
               !r.queue_reconciled,
           "B1: caseD_0 touches no other report field");
        no_calls("B1: caseD_0 is a pure return -- nothing is called");
    }
    {
        f.reset();
        g_rec.clear();
        f.players[PLAYER].ai_enabled = 1;
        const auto r                 = call(PLAYER, UNIT_TYPE_JUNK, UNIT, 5);
        ck(r.out_of_range_event, "B2: param_4 > 4 (unsigned) hits the catch-all, 0x004dbb74 JA");
        ck(r.event == -1,
           "B2: event stays -1 -- the range check returns before `rep.event = param_4` runs");
        ck(!r.ai_disabled_at_entry && !r.threat_and_engage_cleared && !r.ai_disabled_recheck &&
               !r.group_links_reset && !r.pending_spawn_decremented && !r.classified &&
               !r.group_linked && r.group_index_used == -1 && !r.heli_mother_early_out &&
               !r.invasion_force_direct_link && !r.queue_reconciled,
           "B2: every other report field stays at its zero-value on the out-of-range arm");
        no_calls("B2: the out-of-range catch-all is a pure return -- nothing is called");
    }
    {
        // Slot 3 shares caseD_0's jump-table TARGET (there is no separate case-3 arm anywhere in
        // the body), but it is NOT the out-of-range check: 3 is not > 4u, so `rep.event` DOES get
        // stamped 3 before falling into the switch and hitting no case. That is the one observable
        // difference between "slot 3" and "slot 5" -- both no-op, but only one sets event.
        f.reset();
        g_rec.clear();
        f.players[PLAYER].ai_enabled = 1;
        const auto r                 = call(PLAYER, UNIT_TYPE_JUNK, UNIT, 3);
        ck(!r.out_of_range_event && r.event == 3,
           "B3: param_4==3 passes the range check and IS recorded, unlike param_4==5 -- 0x004dbb74");
        ck(!r.threat_and_engage_cleared && !r.ai_disabled_recheck && !r.group_links_reset &&
               !r.pending_spawn_decremented && !r.classified && !r.group_linked &&
               r.group_index_used == -1 && !r.heli_mother_early_out && !r.invasion_force_direct_link &&
               !r.queue_reconciled,
           "B3: slot 3 behaves exactly like caseD_0 once dispatched -- no distinct case-3 arm exists");
        no_calls("B3: slot 3 is a pure return, same as caseD_0 -- nothing is called");
    }

    // ================================================================================================
    // C. caseD_1 (event 1), the state != 0x1f arm: reset + unarmed group-0 link, no classify, no
    //    queue scan -- LAB_004dbbfd/LAB_004dbc01
    // ================================================================================================
    {
        f.reset();
        g_rec.clear();
        f.players[PLAYER].ai_enabled                   = 1;
        f.players[PLAYER].ai_start_units_pending_spawn = 0; // boundary: zero, so the DEC must not fire
        unit &u                                        = f.u(PLAYER, UNIT);
        u.state                                        = 0x1e; // one BELOW the 0x1f classify gate
        u.incoming_threat_damage                       = 77;   // nonzero, distinct
        u.engagement_flags                             = 0x01;
        u.order_status_flags                           = 0x80;
        u.ai_group_next                                = 11; // distinct, non-symmetric seed values
        u.ai_group_prev                                = 22;
        u.ai_group_index                               = 33;
        // A queue entry that WOULD match, to prove this arm never reaches the scan at all.
        f.players[PLAYER].ai_bldg_queue_count              = 1;
        f.players[PLAYER].ai_bldg_queue[0].status          = 0x80;
        f.players[PLAYER].ai_bldg_queue[0].tick_or_unit_id = 9;
        u.unit_proto_id                                    = 9;

        const auto r = call(PLAYER, UNIT_TYPE_JUNK, UNIT, 1);

        ck(r.threat_and_engage_cleared, "C1: the merged threat/engage clear runs, 0x004dbb93/0x004dbb9c");
        ck(u.incoming_threat_damage == 0, "C1: incoming_threat_damage is zeroed, 0x004dbb93");
        ck(u.engagement_flags == 0 && u.order_status_flags == 0,
           "C1: engage_status_word_clear zeroes BOTH bytes of the merged word, 0x004dbb9c");
        ck(!r.ai_disabled_recheck,
           "C1: the ai_enabled re-read @0x004dbba5 cannot fail here -- nothing between it and the top "
           "gate can change player_data::ai_enabled (see this file's report: unreachable by "
           "construction, matching the header's own 'provably always true' claim)");
        ck(r.group_links_reset, "C1: the 3-field group-link reset runs, 0x004dbbbc-0x004dbbce");
        ck(u.ai_group_next == 0 && u.ai_group_prev == 0 && u.ai_group_index == 0xffffu,
           "C1: ai_group_next/prev are zeroed and ai_group_index takes the 0xffff sentinel");
        ck(!r.pending_spawn_decremented && f.players[PLAYER].ai_start_units_pending_spawn == 0,
           "C1: a zero pending-spawn counter is left alone, 0x004dbbd7 boundary");
        ck(!r.classified, "C1: state != 0x1f never reaches the classify gate, 0x004dbbf2");
        ck(r.group_linked && r.group_index_used == 0,
           "C1: the unarmed tail links group 0 directly, LAB_004dbbfd/LAB_004dbc01");
        ck(g_rec.links.size() == 1 && g_rec.links[0].player == PLAYER &&
               g_rec.links[0].group == 0 && g_rec.links[0].unit_id == UNIT,
           "C1: group_member_link is called with (player, 0, unit_id)");
        ck(g_rec.soldier_calls == 0 && g_rec.ground_calls == 0 && g_rec.plane_calls == 0 &&
               g_rec.heli_calls == 0,
           "C1: the unarmed tail never touches the four classify predicates");
        ck(!r.queue_reconciled && f.players[PLAYER].ai_bldg_queue[0].status == 0x80,
           "C1: the unarmed tail returns WITHOUT running the queue-reconcile scan -- a matching "
           "entry is left untouched");
    }

    // ================================================================================================
    // D. caseD_1, the state == 0x1f armed classify chain, plus the pending-spawn decrement and the
    //    queue-reconcile scan that ALWAYS follows it (matched or not)
    // ================================================================================================
    auto arm_case1 = [&]() {
        f.reset();
        g_rec.clear();
        f.players[PLAYER].ai_enabled                   = 1;
        f.players[PLAYER].ai_start_units_pending_spawn = 1; // nonzero: the DEC boundary's other side
        f.u(PLAYER, UNIT).state                        = 0x1f;
    };
    {
        // D1: soldier true -> group 2, and the DEC + the queue scan both fire in the same call.
        arm_case1();
        g_rec.soldier_answer                               = 1;
        f.players[PLAYER].ai_bldg_queue_count              = 1;
        f.players[PLAYER].ai_bldg_queue[0].status          = 0x80;
        f.players[PLAYER].ai_bldg_queue[0].tick_or_unit_id = 42;
        f.u(PLAYER, UNIT).unit_proto_id                    = 42;

        const auto r = call(PLAYER, UNIT_TYPE_JUNK, UNIT, 1);
        ck(r.pending_spawn_decremented && f.players[PLAYER].ai_start_units_pending_spawn == 0,
           "D1: a nonzero pending-spawn counter IS decremented, 0x004dbbd7-0x004dbbe0");
        ck(r.classified, "D1: state==0x1f reaches the classify gate, 0x004dbbf2");
        ck(r.group_linked && r.group_index_used == 2,
           "D1: unit_is_ai_soldier true links GROUP 2, 0x004dbc1a");
        ck(g_rec.soldier_calls == 1 && g_rec.ground_calls == 0 && g_rec.plane_calls == 0 &&
               g_rec.heli_calls == 0,
           "D1: soldier true short-circuits the else-if chain -- ground/plane/heli are never called");
        ck(g_rec.links.size() == 1 && g_rec.links[0].group == 2,
           "D1: group_member_link fires once, with group 2");
        ck(r.queue_reconciled && f.players[PLAYER].ai_bldg_queue[0].status == 0xc0,
           "D1: the queue-reconcile scan runs AFTER a successful classify link and marks the match");
    }
    {
        // D2: ground true, soldier false -- shares soldier's group-2 target, and proves ground is
        // only checked once soldier has already answered false.
        arm_case1();
        g_rec.ground_answer = 1;
        const auto r        = call(PLAYER, UNIT_TYPE_JUNK, UNIT, 1);
        ck(r.group_linked && r.group_index_used == 2,
           "D2: unit_is_ai_ground true ALSO links group 2 (shares soldier's LAB_004dbc1a), 0x004dbc27");
        ck(g_rec.soldier_calls == 1 && g_rec.ground_calls == 1 && g_rec.plane_calls == 0 &&
               g_rec.heli_calls == 0,
           "D2: ground is checked, but plane/heli are not reached once ground matches");
    }
    {
        // D3: plane true, soldier/ground false -> group 4.
        arm_case1();
        g_rec.plane_answer = 1;
        const auto r       = call(PLAYER, UNIT_TYPE_JUNK, UNIT, 1);
        ck(r.group_linked && r.group_index_used == 4, "D3: unit_is_ai_plane true links GROUP 4, 0x004dbc3f");
        ck(g_rec.soldier_calls == 1 && g_rec.ground_calls == 1 && g_rec.plane_calls == 1 &&
               g_rec.heli_calls == 0,
           "D3: soldier and ground are both ruled out before plane is checked; heli never runs");
    }
    {
        // D4: heli true, nothing else -> group 3. All four predicates fire, in order.
        arm_case1();
        g_rec.heli_answer = 1;
        const auto r      = call(PLAYER, UNIT_TYPE_JUNK, UNIT, 1);
        ck(r.group_linked && r.group_index_used == 3, "D4: unit_is_ai_heli true links GROUP 3, 0x004dbc55");
        ck(g_rec.soldier_calls == 1 && g_rec.ground_calls == 1 && g_rec.plane_calls == 1 &&
               g_rec.heli_calls == 1,
           "D4: all four classifiers run in soldier/ground/plane/heli order before the heli match");
    }
    {
        // D5: NONE of the four answers true. classify_and_link_case1 makes no call, but the caller
        // runs the queue-reconcile scan EITHER WAY (0x004dbc61 is reached both by falling out of the
        // CALL and by the heli JZ-on-no-match) -- the "unconditional call" hazard the brief warns of.
        arm_case1();
        f.players[PLAYER].ai_bldg_queue_count              = 1;
        f.players[PLAYER].ai_bldg_queue[0].status          = 0x80;
        f.players[PLAYER].ai_bldg_queue[0].tick_or_unit_id = 5;
        f.u(PLAYER, UNIT).unit_proto_id                    = 5;

        const auto r = call(PLAYER, UNIT_TYPE_JUNK, UNIT, 1);
        ck(r.classified && !r.group_linked && r.group_index_used == -1,
           "D5: reaching the classify gate with no match sets classified but links nothing");
        ck(g_rec.links.empty(), "D5: group_member_link is never called when all four predicates answer false");
        ck(g_rec.soldier_calls == 1 && g_rec.ground_calls == 1 && g_rec.plane_calls == 1 &&
               g_rec.heli_calls == 1,
           "D5: every classifier is still tried once");
        ck(r.queue_reconciled && f.players[PLAYER].ai_bldg_queue[0].status == 0xc0,
           "D5: the queue-reconcile scan runs even though the classify chain matched nothing -- "
           "0x004dbc61 is reached from BOTH the successful CALL and the heli JZ");
    }
    {
        // D6: two matching queue entries -- the scan marks the FIRST and stops; the second is
        // untouched even though it would also match.
        arm_case1();
        g_rec.soldier_answer                               = 1;
        f.players[PLAYER].ai_bldg_queue_count              = 2;
        f.players[PLAYER].ai_bldg_queue[0].status          = 0x80;
        f.players[PLAYER].ai_bldg_queue[0].tick_or_unit_id = 7;
        f.players[PLAYER].ai_bldg_queue[1].status          = 0x80;
        f.players[PLAYER].ai_bldg_queue[1].tick_or_unit_id = 7;
        f.u(PLAYER, UNIT).unit_proto_id                    = 7;

        call(PLAYER, UNIT_TYPE_JUNK, UNIT, 1);
        ck(f.players[PLAYER].ai_bldg_queue[0].status == 0xc0 &&
               f.players[PLAYER].ai_bldg_queue[1].status == 0x80,
           "D6: the scan marks only the FIRST matching entry and stops there");
    }
    {
        // D7: the loop bound is ai_bldg_queue_count, not the 64-entry array extent -- a matching
        // entry ONE PAST the count is invisible to the scan.
        arm_case1();
        g_rec.soldier_answer                               = 1;
        f.players[PLAYER].ai_bldg_queue_count              = 1;    // only index 0 is in range
        f.players[PLAYER].ai_bldg_queue[0].status          = 0x40; // does not match (removed, no 0x80)
        f.players[PLAYER].ai_bldg_queue[0].tick_or_unit_id = 3;
        f.players[PLAYER].ai_bldg_queue[1].status          = 0x80; // WOULD match, but index 1 is out
        f.players[PLAYER].ai_bldg_queue[1].tick_or_unit_id = 3;    // of the declared count
        f.u(PLAYER, UNIT).unit_proto_id                    = 3;

        const auto r = call(PLAYER, UNIT_TYPE_JUNK, UNIT, 1);
        ck(!r.queue_reconciled && f.players[PLAYER].ai_bldg_queue[1].status == 0x80,
           "D7: the scan is bounded by ai_bldg_queue_count -- an entry past it is never examined");
    }
    {
        // H1: ORDER proof. The classify predicate's OWN side effect changes unit_proto_id AFTER the
        // classify gate already read `u.state`, and BEFORE the queue-reconcile scan runs. The scan
        // must see the NEW value, because the original recomputes the address and re-reads
        // unit_proto_id every iteration rather than reusing a value captured earlier in the body.
        arm_case1();
        f.u(PLAYER, UNIT).unit_proto_id                    = 111; // does NOT match the queue entry below
        ai_calls order_calls                               = calls();
        order_calls.unit_is_ai_soldier                     = &st_soldier_mutates_proto;
        g_mutate_target                                    = &f;
        g_mutate_to                                        = 42;
        f.players[PLAYER].ai_bldg_queue_count              = 1;
        f.players[PLAYER].ai_bldg_queue[0].status          = 0x80;
        f.players[PLAYER].ai_bldg_queue[0].tick_or_unit_id = 42; // matches the MUTATED value only

        const auto r    = detail::notify_unit_lifecycle(f.view(), f.store(), order_calls, PLAYER,
                                                        UNIT_TYPE_JUNK, UNIT, 1);
        g_mutate_target = nullptr;
        ck(r.queue_reconciled && f.players[PLAYER].ai_bldg_queue[0].status == 0xc0,
           "H1: the queue scan re-reads unit_proto_id AFTER the classify call runs -- it sees the "
           "callee's own write, proving the value is not cached before the callee, 0x004dbc90");
    }

    // ================================================================================================
    // E. caseD_2 (event 2): the build-queue-reconcile scan ONLY -- no roster/group/re-check touch at
    //    all -- plus the mask/kind boundaries and the byte/word truncation on the compare
    // ================================================================================================
    auto arm_case2 = [&]() {
        f.reset();
        g_rec.clear();
        f.players[PLAYER].ai_enabled = 1;
        unit &u                      = f.u(PLAYER, UNIT);
        // Sentinel-seeded roster fields: caseD_2 must leave every one of these untouched.
        u.incoming_threat_damage = -12345;
        u.engagement_flags       = 0x01;
        u.order_status_flags     = 0x80;
        u.ai_group_next          = 111;
        u.ai_group_prev          = 222;
        u.ai_group_index         = 333;
    };
    {
        arm_case2();
        f.players[PLAYER].ai_bldg_queue_count              = 1;
        f.players[PLAYER].ai_bldg_queue[0].status          = 0x80;
        f.players[PLAYER].ai_bldg_queue[0].tick_or_unit_id = 9;
        f.u(PLAYER, UNIT).unit_proto_id                    = 9;

        const unit &u = f.u(PLAYER, UNIT);
        const auto  r = call(PLAYER, UNIT_TYPE_JUNK, UNIT, 2);

        ck(!r.ai_disabled_at_entry && !r.out_of_range_event && r.event == 2,
           "E1: caseD_2 dispatches cleanly, event recorded");
        ck(!r.threat_and_engage_cleared && !r.ai_disabled_recheck && !r.group_links_reset &&
               !r.pending_spawn_decremented && !r.classified && !r.group_linked &&
               r.group_index_used == -1 && !r.heli_mother_early_out && !r.invasion_force_direct_link,
           "E1: caseD_2 touches NONE of the roster/group/re-check report fields");
        ck(u.incoming_threat_damage == -12345 && u.engagement_flags == 0x01 &&
               u.order_status_flags == 0x80 && u.ai_group_next == 111 && u.ai_group_prev == 222 &&
               u.ai_group_index == 333,
           "E1: caseD_2 leaves every roster field exactly as seeded -- no re-read gate, no reset");
        no_calls("E1: caseD_2 never touches a classify predicate or group_member_link");
        ck(r.queue_reconciled && f.players[PLAYER].ai_bldg_queue[0].status == 0xc0,
           "E1: the queue-reconcile scan itself still runs and marks a real match");
    }
    {
        // E2: status 0x81 -- committed bit set, but the low nibble (entry kind) is 1, not 0/train.
        // The mask keeps the low nibble, so this must NOT match despite the tick_or_unit_id match.
        arm_case2();
        f.players[PLAYER].ai_bldg_queue_count              = 1;
        f.players[PLAYER].ai_bldg_queue[0].status          = 0x81;
        f.players[PLAYER].ai_bldg_queue[0].tick_or_unit_id = 9;
        f.u(PLAYER, UNIT).unit_proto_id                    = 9;
        const auto r                                       = call(PLAYER, UNIT_TYPE_JUNK, UNIT, 2);
        ck(!r.queue_reconciled && f.players[PLAYER].ai_bldg_queue[0].status == 0x81,
           "E2: status 0x81 (kind=1, committed) fails the 0x8f/0x80 mask -- kind must be 0/train");
    }
    {
        // E3: status 0x90 -- bit 0x10 is OUTSIDE the 0x8f mask, so it must be ignored and the entry
        // must still match (0x90 & 0x8f == 0x80).
        arm_case2();
        f.players[PLAYER].ai_bldg_queue_count              = 1;
        f.players[PLAYER].ai_bldg_queue[0].status          = 0x90;
        f.players[PLAYER].ai_bldg_queue[0].tick_or_unit_id = 9;
        f.u(PLAYER, UNIT).unit_proto_id                    = 9;
        const auto r                                       = call(PLAYER, UNIT_TYPE_JUNK, UNIT, 2);
        ck(r.queue_reconciled && f.players[PLAYER].ai_bldg_queue[0].status == 0xd0,
           "E3: status 0x90 still matches -- bit 0x10 is outside the 0x8f mask, 0x004dbcdd/0x004dbce0");
    }
    {
        // E4: status 0x40 alone (removed, no committed bit) must not match.
        arm_case2();
        f.players[PLAYER].ai_bldg_queue_count              = 1;
        f.players[PLAYER].ai_bldg_queue[0].status          = 0x40;
        f.players[PLAYER].ai_bldg_queue[0].tick_or_unit_id = 9;
        f.u(PLAYER, UNIT).unit_proto_id                    = 9;
        const auto r                                       = call(PLAYER, UNIT_TYPE_JUNK, UNIT, 2);
        ck(!r.queue_reconciled && f.players[PLAYER].ai_bldg_queue[0].status == 0x40,
           "E4: a 'removed' entry (status 0x40, no 0x80 bit) never matches");
    }
    {
        // E5: the byte/word truncation, from the ABOVE-0xff side. unit_proto_id 256 can never match
        // any 8-bit tick_or_unit_id -- the compare is unsigned int, not modulo-256.
        arm_case2();
        f.players[PLAYER].ai_bldg_queue_count              = 1;
        f.players[PLAYER].ai_bldg_queue[0].status          = 0x80;
        f.players[PLAYER].ai_bldg_queue[0].tick_or_unit_id = 0; // 256 truncated to a byte would be 0
        f.u(PLAYER, UNIT).unit_proto_id                    = 256;
        const auto r                                       = call(PLAYER, UNIT_TYPE_JUNK, UNIT, 2);
        ck(!r.queue_reconciled && f.players[PLAYER].ai_bldg_queue[0].status == 0x80,
           "E5: unit_proto_id 256 never matches tick_or_unit_id 0 -- no byte truncation on the "
           "16-bit side of the compare, 0x004dbd01");
    }
    {
        // E6: the SAME boundary from BELOW-or-at 0xff -- the maximum representable byte value DOES
        // match when unit_proto_id equals it exactly.
        arm_case2();
        f.players[PLAYER].ai_bldg_queue_count              = 1;
        f.players[PLAYER].ai_bldg_queue[0].status          = 0x80;
        f.players[PLAYER].ai_bldg_queue[0].tick_or_unit_id = 0xff;
        f.u(PLAYER, UNIT).unit_proto_id                    = 0xff;
        const auto r                                       = call(PLAYER, UNIT_TYPE_JUNK, UNIT, 2);
        ck(r.queue_reconciled && f.players[PLAYER].ai_bldg_queue[0].status == 0xc0,
           "E6: unit_proto_id 0xff matches tick_or_unit_id 0xff -- the 8-bit ceiling itself DOES match");
    }

    // ================================================================================================
    // F. caseD_4 (event 4, "unit created"): the group-link reset runs BEFORE either early-out, the
    //    heli-mother early-out (both cfg values, both boundary neighbours), the invasion-force
    //    direct link, and caseD_4's OWN classify chain -- whose group assignment for soldier/ground
    //    differs from caseD_1's, and which never reaches the queue scan
    // ================================================================================================
    auto arm_case4 = [&](uint32_t utype) {
        f.reset();
        g_rec.clear();
        f.players[PLAYER].ai_enabled        = 1;
        f.players[PLAYER].ai_invasion_force = 0;
        unit &u                             = f.u(PLAYER, UNIT);
        u.unit_proto_id                     = 5; // < UNIT_TYPE_COUNT, safe cfg_units index
        u.ai_group_next                     = 44;
        u.ai_group_prev                     = 55;
        u.ai_group_index                    = 66;
        f.cfg_units[5].type                 = utype;
    };
    {
        // F1: the heli-mother early-out, "A" variant. group_links_reset ALREADY happened (the reset
        // is unconditional at the top of caseD_4, BEFORE the type is even read) -- and a matching
        // queue entry proves the scan never runs on this arm either.
        arm_case4((uint32_t)UNIT_TYPE_A_HELI_MOTHER);
        f.players[PLAYER].ai_bldg_queue_count              = 1;
        f.players[PLAYER].ai_bldg_queue[0].status          = 0x80;
        f.players[PLAYER].ai_bldg_queue[0].tick_or_unit_id = 5;

        const auto r = call(PLAYER, UNIT_TYPE_JUNK, UNIT, 4);
        ck(r.heli_mother_early_out, "F1: UNIT_TYPE_A_HELI_MOTHER (0x13) takes the early-out, 0x004dbd7e");
        ck(r.group_links_reset, "F1: the group-link reset ran BEFORE the heli-mother check, unconditionally");
        ck(f.u(PLAYER, UNIT).ai_group_next == 0 && f.u(PLAYER, UNIT).ai_group_prev == 0 &&
               f.u(PLAYER, UNIT).ai_group_index == 0xffffu,
           "F1: and it actually zeroed/sentinelled the three link fields, 0x004dbd49-0x004dbd5b");
        ck(!r.classified && !r.group_linked && !r.invasion_force_direct_link && !r.queue_reconciled,
           "F1: the heli-mother early-out returns before classify, invasion-force check, or the "
           "queue scan");
        no_calls("F1: the heli-mother early-out calls none of the four classifiers nor group_member_link");
        ck(f.players[PLAYER].ai_bldg_queue[0].status == 0x80,
           "F1: caseD_4 never runs the queue-reconcile scan, even with a matching entry present");
    }
    {
        // F2: the "H" variant of the same early-out.
        arm_case4((uint32_t)UNIT_TYPE_H_HELI_MOTHER);
        const auto r = call(PLAYER, UNIT_TYPE_JUNK, UNIT, 4);
        ck(r.heli_mother_early_out, "F2: UNIT_TYPE_H_HELI_MOTHER (0x14) ALSO takes the early-out, 0x004dbd8b");
    }
    {
        // F3: boundary neighbours -- 0x12 and 0x15 must NOT take the early-out.
        arm_case4(0x12);
        const auto r1 = call(PLAYER, UNIT_TYPE_JUNK, UNIT, 4);
        ck(!r1.heli_mother_early_out, "F3a: cfg type 0x12 (one below A_HELI_MOTHER) does not early-out");
        arm_case4(0x15);
        const auto r2 = call(PLAYER, UNIT_TYPE_JUNK, UNIT, 4);
        ck(!r2.heli_mother_early_out, "F3b: cfg type 0x15 (one above H_HELI_MOTHER) does not early-out");
    }
    {
        // F4: the invasion-force direct link. Deliberately a non-1 nonzero value (7): the gate is
        // "!= 0", not "== 1".
        arm_case4(0x01); // an ordinary, non-heli-mother type
        f.players[PLAYER].ai_invasion_force                = 7;
        f.players[PLAYER].ai_bldg_queue_count              = 1; // a match that must stay untouched too
        f.players[PLAYER].ai_bldg_queue[0].status          = 0x80;
        f.players[PLAYER].ai_bldg_queue[0].tick_or_unit_id = 5;

        const auto r = call(PLAYER, UNIT_TYPE_JUNK, UNIT, 4);
        ck(!r.heli_mother_early_out && r.invasion_force_direct_link,
           "F4: a nonzero (here 7, not 1) ai_invasion_force takes the direct-link arm, 0x004dbd91");
        ck(r.group_linked && r.group_index_used == 0,
           "F4: the invasion-force arm links group 0 directly, skipping classification entirely");
        ck(g_rec.links.size() == 1 && g_rec.links[0].group == 0,
           "F4: group_member_link fires once with group 0");
        ck(!r.classified, "F4: rep.classified is never set -- the classify chain is never reached");
        ck(g_rec.soldier_calls == 0 && g_rec.ground_calls == 0 && g_rec.plane_calls == 0 &&
               g_rec.heli_calls == 0,
           "F4: the invasion-force path skips unit-type classification entirely, per "
           "player_data::ai_invasion_force's own field comment");
        ck(!r.queue_reconciled && f.players[PLAYER].ai_bldg_queue[0].status == 0x80,
           "F4: caseD_4 never runs the queue scan on the invasion-force arm either");
    }
    {
        // F5: caseD_4's classify chain, soldier true. GROUP ASSIGNMENT DIFFERS FROM caseD_1: here
        // soldier links GROUP 0, not group 2.
        arm_case4(0x01);
        g_rec.soldier_answer = 1;
        const auto r         = call(PLAYER, UNIT_TYPE_JUNK, UNIT, 4);
        ck(r.classified && r.group_linked && r.group_index_used == 0,
           "F5: caseD_4's soldier match links GROUP 0 -- NOT group 2 like caseD_1's D1, 0x004dbda2");
        ck(g_rec.soldier_calls == 1 && g_rec.ground_calls == 0 && g_rec.plane_calls == 0 &&
               g_rec.heli_calls == 0,
           "F5: soldier true short-circuits the OR -- ground is never even called");
    }
    {
        // F6: ground true, soldier false -- also group 0, and proves ground IS checked when soldier
        // answers false (the OR's second operand).
        arm_case4(0x01);
        g_rec.ground_answer = 1;
        const auto r        = call(PLAYER, UNIT_TYPE_JUNK, UNIT, 4);
        ck(r.group_linked && r.group_index_used == 0,
           "F6: caseD_4's ground match ALSO links group 0, 0x004dbdb3");
        ck(g_rec.soldier_calls == 1 && g_rec.ground_calls == 1 && g_rec.plane_calls == 0 &&
               g_rec.heli_calls == 0,
           "F6: ground is evaluated once soldier answers false -- plane/heli are not reached");
    }
    {
        // F7: plane true (soldier/ground false) -> group 4, same as caseD_1.
        arm_case4(0x01);
        g_rec.plane_answer = 1;
        const auto r       = call(PLAYER, UNIT_TYPE_JUNK, UNIT, 4);
        ck(r.group_linked && r.group_index_used == 4, "F7: caseD_4's plane match links group 4, 0x004dbdcf");
        ck(g_rec.soldier_calls == 1 && g_rec.ground_calls == 1 && g_rec.plane_calls == 1 &&
               g_rec.heli_calls == 0,
           "F7: soldier and ground are both ruled out before plane, heli is not reached");
    }
    {
        // F8: heli true -> group 3, same as caseD_1, and all four predicates fire.
        arm_case4(0x01);
        g_rec.heli_answer = 1;
        const auto r      = call(PLAYER, UNIT_TYPE_JUNK, UNIT, 4);
        ck(r.group_linked && r.group_index_used == 3, "F8: caseD_4's heli match links group 3, 0x004dbdec");
        ck(g_rec.soldier_calls == 1 && g_rec.ground_calls == 1 && g_rec.plane_calls == 1 &&
               g_rec.heli_calls == 1,
           "F8: all four classifiers run in order before the heli match");
    }
    {
        // F9: none match -- straight return, no call, no link, and (unlike caseD_1's D5) NEVER
        // reaches the queue scan even though a matching entry is present.
        arm_case4(0x01);
        f.players[PLAYER].ai_bldg_queue_count              = 1;
        f.players[PLAYER].ai_bldg_queue[0].status          = 0x80;
        f.players[PLAYER].ai_bldg_queue[0].tick_or_unit_id = 5;

        const auto r = call(PLAYER, UNIT_TYPE_JUNK, UNIT, 4);
        ck(r.classified && !r.group_linked && r.group_index_used == -1,
           "F9: caseD_4's classify chain reached but nothing matched");
        ck(g_rec.links.empty(), "F9: no group_member_link call when all four predicates answer false");
        ck(!r.queue_reconciled && f.players[PLAYER].ai_bldg_queue[0].status == 0x80,
           "F9: caseD_4's non-match returns directly -- @0x004dbde4 goes to caseD_0, NEVER through "
           "the queue-reconcile tail caseD_1/caseD_2 share");
    }

    // ================================================================================================
    // G. `unit_type` (DX) is truly dead: two runs differing ONLY in unit_type produce an identical
    //    report and an identical call trace.
    // ================================================================================================
    {
        auto run_with_unit_type = [&](uint16_t unit_type_) {
            f.reset();
            g_rec.clear();
            f.players[PLAYER].ai_enabled = 1;
            f.u(PLAYER, UNIT).state      = 0x1f;
            g_rec.ground_answer          = 1; // exercise a real classify link, not just the no-op arm
            return call(PLAYER, unit_type_, UNIT, 1);
        };
        const auto ra        = run_with_unit_type(0x0000);
        const int  soldier_a = g_rec.soldier_calls, ground_a = g_rec.ground_calls;
        const auto rb = run_with_unit_type(0x1234);
        ck(ra.player == rb.player && ra.event == rb.event &&
               ra.ai_disabled_at_entry == rb.ai_disabled_at_entry &&
               ra.out_of_range_event == rb.out_of_range_event &&
               ra.threat_and_engage_cleared == rb.threat_and_engage_cleared &&
               ra.ai_disabled_recheck == rb.ai_disabled_recheck &&
               ra.group_links_reset == rb.group_links_reset &&
               ra.pending_spawn_decremented == rb.pending_spawn_decremented &&
               ra.classified == rb.classified && ra.group_linked == rb.group_linked &&
               ra.group_index_used == rb.group_index_used &&
               ra.heli_mother_early_out == rb.heli_mother_early_out &&
               ra.invasion_force_direct_link == rb.invasion_force_direct_link &&
               ra.queue_reconciled == rb.queue_reconciled,
           "G1: unit_type 0x0000 vs 0x1234 produce a field-for-field identical lifecycle_notify_report -- DX "
           "is genuinely dead, per the header's own claim");
        ck(soldier_a == g_rec.soldier_calls && ground_a == g_rec.ground_calls,
           "G1: and the call trace (which classifier ran, how many times) is identical too");
    }
}

} // namespace mh::ai::test
