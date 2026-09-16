//
// ai_group_remove_selftest.cpp -- `aitest` cases for llm_strat_ai_group_remove @0x004d4f16 (RI-AI
// batch D / AI1D). Pins: the whole-struct swap-compaction of the freed slot with the LAST group
// (skipped entirely when the removed group already IS the last one, leaving the freed slot's stale
// data abandoned rather than cleared); the member-list restamp that follows a real compaction; the
// unconditional ai_group_count decrement; and the two AI_SEED_GROUP_COUNT-bounded sweeps over the
// OTHER groups -- sweep 1 drains any goal==7 group whose link_target_group equals the ORIGINALLY
// REMOVED index, sweep 2 relinks any goal==7 group whose link_target_group equals the just-vacated
// slot (the post-decrement ai_group_count). Per the header's PROOF PATH section this function has no
// shadow site -- arming one would fire the one real callee, route_unit_to_home_storage, twice per
// drained member (once for real, once for comparison) -- so this offline suite, over explicit state
// and a recording `ai_calls`, is the only proof this translation gets.
//
#include "ai_test_support.h"

#include "ai/ai_group_remove.h"

namespace mh::ai::test {
namespace {

using namespace mh::ai;

struct route_call {
    uint32_t player;
    int32_t  unit_id;
};

// Records every route_unit_to_home_storage call and, when armed via `drain_group`, emulates the
// real callee's effect on the group it drains: decrementing member_count and, if a replacement
// head_unit was queued, advancing head_unit -- exactly what unit_order_move/exit_storage_enqueue's
// group_member_unlink chain does in the live game and what this offline suite has no other way to
// exercise (see ai_group_remove.h's PROOF PATH section). Without this, the drain while-loop
// (0x004d4fec-0x004d5023, which has no iteration bound in the original) would spin forever.
struct recorder {
    std::vector<route_call> routes;
    unit_group             *drain_group = nullptr;
    std::vector<uint16_t>   next_heads; // head_unit to install after each call, in order
    size_t                  cursor = 0;

    void clear() {
        routes.clear();
        drain_group = nullptr;
        next_heads.clear();
        cursor = 0;
    }
};
recorder g_rec;

void st_route_unit_to_home_storage(uint32_t player, int32_t unit_id) {
    g_rec.routes.push_back({player, unit_id});
    if (g_rec.drain_group != nullptr) {
        --g_rec.drain_group->member_count;
        if (g_rec.cursor < g_rec.next_heads.size()) {
            g_rec.drain_group->head_unit = g_rec.next_heads[g_rec.cursor++];
        }
    }
}

// Only the one member this body reaches is bound; everything else stays null so an unstubbed call
// crashes loudly rather than returning zero (house style -- see ai_group_member_list_selftest.cpp's
// identically-empty table and ai_selftest.cpp's intel_stub::calls()).
const ai_calls &calls() {
    static const ai_calls c = [] {
        ai_calls t{};
        t.route_unit_to_home_storage = &st_route_unit_to_home_storage;
        return t;
    }();
    return c;
}

} // namespace

void run_group_remove_tests() {
    printf("-- ai_group_remove (llm_strat_ai_group_remove) --\n");

    const int32_t P = 3; // not 0/1: separates a player-row bug from a group-index bug

    // ---- T1: group_index == last_index -- the whole copy+restamp step is SKIPPED, and the freed
    // slot's data is ABANDONED, not cleared (0x004d4f4d CMP / 0x004d4f50 JZ). -----------------------
    {
        fixture f;
        g_rec.clear();
        player_data &pd              = f.players[P];
        pd.ai_group_count            = 6; // last_index = 5, same as the group_index passed below
        pd.ai_groups[5].serial_id    = 777;
        pd.ai_groups[5].head_unit    = 55;
        pd.ai_groups[5].member_count = 2;
        pd.ai_groups[5].goal         = 10; // not 7: the (empty, post-decrement) sweep range can't
                                           // reach this slot either way, but keep it inert
        // Deliberately WRONG so a "restamp anyway" bug is visible: if the skip really skips, this
        // poisoned value must survive untouched.
        f.u(P, 55).ai_group_index = 999;

        detail::group_remove(f.view(), f.store(), calls(), P, 5u);

        ck(pd.ai_group_count == 5,
           "T1: ai_group_count still decrements on the already-last path, 0x004d4fc5");
        ck(pd.ai_groups[5].serial_id == 777 && pd.ai_groups[5].head_unit == 55 &&
               pd.ai_groups[5].member_count == 2,
           "T1: the freed slot's data is ABANDONED in place, not cleared, when it was already last, "
           "0x004d4f50");
        ck(f.u(P, 55).ai_group_index == 999,
           "T1: the restamp walk is skipped entirely on the already-last path, 0x004d4f7d");
        ck(g_rec.routes.empty(), "T1: nothing in the (empty) sweep range can fire a call");
    }

    // ---- T2: ONE member -- a whole-struct copy (fields the restamp never touches prove it's a FULL
    // 0xa66-byte struct assignment, not a partial one) plus the single member's restamp. -------------
    {
        fixture f;
        g_rec.clear();
        player_data &pd                     = f.players[P];
        pd.ai_group_count                   = 7; // last_index = 6
        pd.ai_groups[6].serial_id           = 555;
        pd.ai_groups[6].member_count        = 4;
        pd.ai_groups[6].reinforce_pending   = 9;
        pd.ai_groups[6].active_member_count = 4;
        pd.ai_groups[6].head_unit           = 201;
        pd.ai_groups[6].tail_unit           = 201;
        pd.ai_groups[6].goal                = 10;   // harmless once moved into the (out-of-sweep-range) slot 5
        f.u(P, 201).ai_group_next           = 0;    // single member, terminates immediately
        f.u(P, 201).ai_group_index          = 4321; // stale; must become 5, the destination slot

        detail::group_remove(f.view(), f.store(), calls(), P, 5u);

        ck(pd.ai_group_count == 6, "T2: count decrements once, 0x004d4fc5");
        ck(pd.ai_groups[5].serial_id == 555 && pd.ai_groups[5].member_count == 4 &&
               pd.ai_groups[5].reinforce_pending == 9 && pd.ai_groups[5].active_member_count == 4 &&
               pd.ai_groups[5].tail_unit == 201,
           "T2: the LAST group's whole 0xa66-byte record lands on the freed slot, "
           "0x004d4f66-0x004d4f7b (member_count/reinforce_pending/active_member_count/tail_unit are "
           "never individually touched by the restamp, so their survival proves a FULL copy)");
        ck(f.u(P, 201).ai_group_index == 5,
           "T2: the single member's ai_group_index is restamped to the destination slot, 0x004d4f98");
        ck(pd.ai_groups[6].serial_id == 555 && pd.ai_groups[6].head_unit == 201,
           "T2: the copy SOURCE slot keeps its old contents -- nothing clears it");
    }

    // ---- T3: several members walked in full, a decoy unit outside the chain left untouched, and a
    // SECOND group elsewhere in the sweep range left byte-identical -- a translation that walked the
    // whole roster instead of the one moved group, or that scanned an unrelated group, fails here. ---
    {
        fixture f;
        g_rec.clear();
        player_data &pd              = f.players[P];
        pd.ai_group_count            = 8; // last_index = 7, post-decrement count = 7
        pd.ai_groups[7].head_unit    = 301;
        pd.ai_groups[7].member_count = 3;
        pd.ai_groups[7].serial_id    = 707;
        pd.ai_groups[7].goal         = 10; // harmless once moved to slot 5 (post-decrement range is
                                           // [5,7), so slot 5 IS scanned by the sweeps -- goal 10
                                           // keeps it out of both)

        f.u(P, 301).ai_group_next  = 302;
        f.u(P, 302).ai_group_next  = 303;
        f.u(P, 303).ai_group_next  = 0; // terminator
        f.u(P, 301).ai_group_index = 111;
        f.u(P, 302).ai_group_index = 222;
        f.u(P, 303).ai_group_index = 333;
        // A decoy unit NOT reachable from the chain: if the walk kept going past the terminator, or
        // walked some other structure entirely, this would move.
        f.u(P, 304).ai_group_next  = 0;
        f.u(P, 304).ai_group_index = 444;

        // The bystander group: goal == 7 (a sweep CANDIDATE on paper) but its link matches neither
        // the removed index (5) nor the post-decrement count (7), so it must be left byte-identical.
        pd.ai_groups[6].goal              = 7;
        pd.ai_groups[6].link_target_group = 999;
        pd.ai_groups[6].serial_id         = 606;
        pd.ai_groups[6].member_count      = 9;
        pd.ai_groups[6].reinforce_pending = 17;

        detail::group_remove(f.view(), f.store(), calls(), P, 5u);

        ck(pd.ai_groups[5].head_unit == 301 && pd.ai_groups[5].member_count == 3 &&
               pd.ai_groups[5].serial_id == 707,
           "T3: the 3-member group's record is copied onto the freed slot, 0x004d4f66-0x004d4f7b");
        ck(f.u(P, 301).ai_group_index == 5 && f.u(P, 302).ai_group_index == 5 &&
               f.u(P, 303).ai_group_index == 5,
           "T3: every member in the chain is visited and restamped to slot 5, exactly once, "
           "0x004d4f98-0x004d4fa6");
        ck(f.u(P, 304).ai_group_index == 444,
           "T3: a unit outside the chain is never touched -- the walk stops at the 0 terminator, "
           "0x004d4fa6/0x004d4fa8");
        ck(pd.ai_groups[6].goal == 7 && pd.ai_groups[6].link_target_group == 999 &&
               pd.ai_groups[6].serial_id == 606 && pd.ai_groups[6].member_count == 9 &&
               pd.ai_groups[6].reinforce_pending == 17,
           "T3: a second group elsewhere in the sweep range is left byte-identical");
        ck(g_rec.routes.empty(), "T3: no goal-7 group actually matches a sweep target, so nothing is routed");
    }

    // ---- T4: the two AI_SEED_GROUP_COUNT-bounded sweeps, both sides of the goal==7 and link
    // boundaries, and the seed groups' immunity -- group_index (6) and the post-decrement count (11)
    // are picked DISTINCT on purpose: sweep 1 compares link_target_group against the former, sweep 2
    // against the latter (0x004d4fe7 vs 0x004d5078), and a translation that used the wrong one for
    // either sweep is only caught here because the two numbers differ. --------------------------------
    {
        fixture f;
        g_rec.clear();
        player_data &pd   = f.players[P];
        pd.ai_group_count = 12; // last_index = 11, post-decrement count = 11; sweep range [5,11)

        pd.ai_groups[11].serial_id = 2020; // the group that moves into slot 6
        pd.ai_groups[11].goal      = 3;    // harmless once moved

        // index 4: a SEED group (< AI_SEED_GROUP_COUNT). Matches the drain condition on paper --
        // neither sweep may ever reach it.
        pd.ai_groups[4].goal              = 7;
        pd.ai_groups[4].link_target_group = 6;
        pd.ai_groups[4].member_count      = 5;
        pd.ai_groups[4].reinforce_pending = 88;
        pd.ai_groups[4].head_unit         = 601;

        // index 5: the real DRAIN match (goal 7, link == group_index 6).
        pd.ai_groups[5].goal              = 7;
        pd.ai_groups[5].link_target_group = 6;
        pd.ai_groups[5].member_count      = 3;
        pd.ai_groups[5].reinforce_pending = 55;
        pd.ai_groups[5].head_unit         = 401;

        // index 7 / 8: goal near-misses on BOTH sides of the exact 7, link still matching 6.
        pd.ai_groups[7].goal              = 6;
        pd.ai_groups[7].link_target_group = 6;
        pd.ai_groups[7].member_count      = 1;
        pd.ai_groups[7].reinforce_pending = 77;
        pd.ai_groups[8].goal              = 8;
        pd.ai_groups[8].link_target_group = 6;
        pd.ai_groups[8].member_count      = 1;
        pd.ai_groups[8].reinforce_pending = 66;

        // index 9: the real RELINK match (goal 7, link == the post-decrement count, 11).
        pd.ai_groups[9].goal              = 7;
        pd.ai_groups[9].link_target_group = 11;

        // index 10: link near-miss, one below the post-decrement count.
        pd.ai_groups[10].goal              = 7;
        pd.ai_groups[10].link_target_group = 10;

        // The drain mock: 3 members, head_unit advances after each call exactly the way the real
        // callee's unlink chain would move it -- so if the translation cached head_unit BEFORE the
        // loop instead of re-reading it fresh every iteration (0x004d5015), calls 2 and 3 below
        // would carry the stale 401 instead of 402/403.
        g_rec.drain_group = &pd.ai_groups[5];
        g_rec.next_heads  = {402, 403};

        detail::group_remove(f.view(), f.store(), calls(), P, 6u);

        g_rec.drain_group = nullptr;

        ck(pd.ai_group_count == 11, "T4: count decrements once regardless of the sweeps, 0x004d4fc5");
        ck(pd.ai_groups[6].serial_id == 2020, "T4: the compaction copy still runs before either sweep");
        ck(pd.ai_groups[11].serial_id == 2020,
           "T4: the abandoned source slot keeps its old data -- nothing clears it");

        ck(g_rec.routes.size() == 3, "T4: exactly the 3-member drain group is routed, nothing else");
        ck(g_rec.routes[0].player == (uint32_t)P && g_rec.routes[0].unit_id == 401,
           "T4: drain call 1 uses the group's head_unit as it stood at entry, 0x004d501e");
        ck(g_rec.routes[1].unit_id == 402 && g_rec.routes[2].unit_id == 403,
           "T4: head_unit is RE-READ fresh every iteration, not cached before the loop, 0x004d5015");
        ck(pd.ai_groups[5].member_count == 0 && pd.ai_groups[5].reinforce_pending == 0,
           "T4: the drained group's roster empties and reinforce_pending clears, 0x004d5025");

        ck(pd.ai_groups[7].member_count == 1 && pd.ai_groups[7].reinforce_pending == 77,
           "T4: goal 6 (one below 7) is not a match -- the group is left untouched, 0x004d4fd6");
        ck(pd.ai_groups[8].member_count == 1 && pd.ai_groups[8].reinforce_pending == 66,
           "T4: goal 8 (one above 7) is not a match either -- exact equality, not a range, 0x004d4fd6");

        ck(pd.ai_groups[9].link_target_group == 6,
           "T4: the real relink match rewrites link_target_group to the vacated slot, 0x004d5083");
        ck(pd.ai_groups[10].link_target_group == 10,
           "T4: link == count-1 (10) is one below the real relink target (11) and is left alone, "
           "0x004d5078");

        ck(pd.ai_groups[4].member_count == 5 && pd.ai_groups[4].reinforce_pending == 88 &&
               pd.ai_groups[4].link_target_group == 6,
           "T4: the seed group at index 4 (< AI_SEED_GROUP_COUNT) is never scanned by either sweep");
        bool seed_routed = false;
        for (const auto &c : g_rec.routes)
            if (c.unit_id == 601) seed_routed = true;
        ck(!seed_routed, "T4: the seed group's head_unit (601) is never routed");
    }

    // ---- T5: a matching group whose roster is ALREADY empty -- the drain while-loop runs zero
    // iterations (member_count == 0 up front), but reinforce_pending is still cleared: the clear sits
    // OUTSIDE the while and INSIDE the if, so zero iterations still reach it, 0x004d5013-0x004d5025. -
    {
        fixture f;
        g_rec.clear();
        player_data &pd                   = f.players[P];
        pd.ai_group_count                 = 7; // last_index = 6
        pd.ai_groups[6].goal              = 3; // harmless once copied onto the freed slot (group_index 3 below)
        pd.ai_groups[5].goal              = 7;
        pd.ai_groups[5].link_target_group = 3; // matches the removed index passed below
        pd.ai_groups[5].member_count      = 0;
        pd.ai_groups[5].reinforce_pending = 42;

        detail::group_remove(f.view(), f.store(), calls(), P, 3u);

        ck(g_rec.routes.empty(),
           "T5: member_count already 0 means the drain while-loop runs zero times, 0x004d5013");
        ck(pd.ai_groups[5].reinforce_pending == 0,
           "T5: reinforce_pending is cleared even with zero drain iterations, 0x004d5025");
    }

    // ---- T6: a group that fails BOTH the drain and relink gates outright (goal != 7) -- neither
    // sweep so much as reads its link_target_group, let alone writes it. The probe sits at index 6,
    // DISTINCT from both the destination (group_index 5) and the source (last_index 7) below, so the
    // compaction copy cannot be the thing that leaves its fields looking untouched. -------------------
    {
        fixture f;
        g_rec.clear();
        player_data &pd                   = f.players[P];
        pd.ai_group_count                 = 8;  // last_index = 7, post-decrement count = 7; sweep range [5,7)
        pd.ai_groups[7].goal              = 10; // the compaction source; harmless once moved onto slot 5
        pd.ai_groups[6].goal              = 0;  // not 7
        pd.ai_groups[6].link_target_group = 5;  // would match group_index below if goal were checked loosely
        pd.ai_groups[6].member_count      = 4;
        pd.ai_groups[6].reinforce_pending = 13;

        detail::group_remove(f.view(), f.store(), calls(), P, 5u);

        ck(pd.ai_groups[6].member_count == 4 && pd.ai_groups[6].reinforce_pending == 13 &&
               pd.ai_groups[6].link_target_group == 5,
           "T6: goal != 7 fails the shared gate for BOTH sweeps -- the group is untouched, 0x004d4fd6");
        ck(g_rec.routes.empty(), "T6: no call fires when the goal gate itself fails");
    }

    // ---- T7: the 0x20-group cap boundary -- ai_group_count maxes at 0x20 (ai_group_membership.h's
    // pool-full check), so 0x1f is the highest last_index this function is ever handed in practice. -
    {
        fixture f;
        g_rec.clear();
        player_data &pd              = f.players[P];
        pd.ai_group_count            = 0x20; // the cap; last_index = 0x1f
        pd.ai_groups[0x1f].serial_id = 3131;
        pd.ai_groups[0x1f].goal      = 2; // harmless once moved

        detail::group_remove(f.view(), f.store(), calls(), P, 5u);

        ck(pd.ai_group_count == 0x1f, "T7: decrements from the 0x20 cap to 0x1f, 0x004d4fc5");
        ck(pd.ai_groups[5].serial_id == 3131,
           "T7: the compaction copy still works at the top of the domain, 0x1f -> 5");
        ck(g_rec.routes.empty(), "T7: nothing in the wide sweep range matches goal 7, so no calls fire");
    }
}

} // namespace mh::ai::test
