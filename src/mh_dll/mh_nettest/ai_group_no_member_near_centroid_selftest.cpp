//
// ai_group_no_member_near_centroid_selftest.cpp -- `aitest` cases for
// llm_strat_ai_group_no_member_near_centroid @0x004d66df (RI-AI batch C / AI1C, closed at T3).
//
// WHY THIS FILE EXISTS. The row was closed on reimpl-verify plus a shadow site (shadow_ai_c6.ini)
// that ARMED and was reached ZERO times over two soaks (20000 + 45000 steps, all-AI, up to 5 of 8
// players eliminated): it is the step counterpart of task_code 0xd (ARM_SCATTER_RANDOM), a narrow
// task-machine arm neither scenario selected. AI1's done_when requires a T3 to carry its written
// reason PLUS a compensating test; this is that test, written at AI1 close (2026-08-30). The
// scenario gap is unchanged and a zero-call site is still not evidence.
//
// WHAT IT PINS: the STRICT UNSIGNED `dist < 100` threshold (JNC @0x004d6759 -- 100 exactly is NOT
// near); the EARLY RETURN on the first near member, i.e. the walk stops rather than scoring every
// member; the empty-group answer being 1 (the shared-epilogue exit, not a fallthrough accident);
// the member walk following unit::ai_group_next; and the argument ORDER handed to
// toroidal_dist_sq -- (centroid_x, centroid_y, unit.x, unit.y), which is the pair a swapped
// translation would render indistinguishable on a symmetric fixture and is therefore checked
// against ASYMMETRIC coordinates.
//
#include "ai_test_support.h"

#include "ai/ai_group_task_predicates.h"

namespace mh::ai::test {
namespace {

using namespace mh::ai;

struct dist_call {
    int32_t x1, y1, x2, y2;
};

// The two outward calls. group_compute_centroid publishes a fixed anchor; toroidal_dist_sq answers
// from a queue of scripted distances (NOT a real metric) so a case can put an exact 100 next to a
// 99 without hunting for coordinates that produce them -- and records every call, which is how the
// EARLY RETURN is observed at all.
struct recorder {
    std::vector<dist_call> dists;
    std::vector<uint32_t>  answers;
    size_t                 cursor     = 0;
    int32_t                centroid_x = 0, centroid_y = 0;
    int                    centroid_calls = 0;

    void clear() {
        dists.clear();
        answers.clear();
        cursor         = 0;
        centroid_x     = 0;
        centroid_y     = 0;
        centroid_calls = 0;
    }
};
recorder g_rec;

void st_group_compute_centroid(int32_t player, int32_t group_index, uint32_t *out_x, uint32_t *out_y) {
    (void)player;
    (void)group_index;
    ++g_rec.centroid_calls;
    *out_x = (uint32_t)g_rec.centroid_x;
    *out_y = (uint32_t)g_rec.centroid_y;
}

uint32_t st_toroidal_dist_sq(int32_t x1, int32_t y1, int32_t x2, int32_t y2) {
    g_rec.dists.push_back({x1, y1, x2, y2});
    return g_rec.cursor < g_rec.answers.size() ? g_rec.answers[g_rec.cursor++] : 0xffffffffu;
}

const ai_calls &calls() {
    static const ai_calls c = [] {
        ai_calls t{};
        t.group_compute_centroid = &st_group_compute_centroid;
        t.toroidal_dist_sq       = &st_toroidal_dist_sq;
        return t;
    }();
    return c;
}

// Link `ids` into group G's member chain in order, giving each unit a distinct tile so the
// argument-order assertion below has something asymmetric to bite on.
void seed_chain(fixture &f, int p, int g, std::initializer_list<int> ids) {
    unit_group &grp  = f.players[p].ai_groups[g];
    int         prev = 0;
    for (int id : ids) {
        f.u(p, id).x = (uint8_t)(10 + id); // distinct per unit, and different from y
        f.u(p, id).y = (uint8_t)(40 + id);
        if (prev == 0)
            grp.head_unit = (uint16_t)id;
        else
            f.u(p, prev).ai_group_next = (uint16_t)id;
        prev = id;
    }
    if (prev != 0) f.u(p, prev).ai_group_next = 0;
    grp.member_count = (int16_t)ids.size();
}

} // namespace

void run_group_no_member_near_centroid_tests() {
    printf("-- ai_group_task_predicates (llm_strat_ai_group_no_member_near_centroid) --\n");

    const int32_t P = 2;
    const int32_t G = 3; // not group 0: separates a group-index bug from a player-row bug

    // ---- N1: an EMPTY group answers 1, and asks for no distance at all. The centroid IS still
    // computed -- the call is unconditional, before the chain is read (0x004d66fd). ------------
    {
        fixture f;
        g_rec.clear();
        f.players[P].ai_groups[G].head_unit = 0;

        const int32_t r = detail::group_no_member_near_centroid(f.view(), f.store(), calls(), P, G);

        ck(r == 1, "N1: the empty chain falls through to the shared epilogue's 1");
        ck(g_rec.centroid_calls == 1, "N1: the centroid is computed unconditionally");
        ck(g_rec.dists.empty(), "N1: no member, no distance query");
    }

    // ---- N2: a near member returns 0 IMMEDIATELY -- the second member is never queried. ------
    {
        fixture f;
        g_rec.clear();
        g_rec.answers = {99, 0}; // the first member is near; the second would be nearer still
        seed_chain(f, P, G, {4, 7});

        const int32_t r = detail::group_no_member_near_centroid(f.view(), f.store(), calls(), P, G);

        ck(r == 0, "N2: dist 99 < 100 -- a near member is present");
        ck(g_rec.dists.size() == 1, "N2: the walk STOPS at the first near member, 0x004d6759");
    }

    // ---- N3: the threshold is STRICT -- exactly 100 is NOT near. A `<=` translation returns 0
    // here. -------------------------------------------------------------------------------------
    {
        fixture f;
        g_rec.clear();
        g_rec.answers = {100, 101};
        seed_chain(f, P, G, {4, 7});

        const int32_t r = detail::group_no_member_near_centroid(f.view(), f.store(), calls(), P, G);

        ck(r == 1, "N3: 100 is not < 100 -- neither member counts as near");
        ck(g_rec.dists.size() == 2, "N3: both members are queried when none is near");
    }

    // ---- N4: the near member is found through the CHAIN, not by roster order -- unit 7 is the
    // head and unit 4 the second, so a walk that iterated the roster ascending would query 4
    // first and (with these answers) return 1. --------------------------------------------------
    {
        fixture f;
        g_rec.clear();
        g_rec.answers = {500, 1}; // second-queried member is the near one
        seed_chain(f, P, G, {7, 4});

        const int32_t r = detail::group_no_member_near_centroid(f.view(), f.store(), calls(), P, G);

        ck(r == 0, "N4: the second member in the CHAIN is near");
        ck(g_rec.dists.size() == 2 && g_rec.dists[0].x2 == 10 + 7 && g_rec.dists[1].x2 == 10 + 4,
           "N4: members are visited head -> ai_group_next, 0x004d6762");
    }

    // ---- N5: the argument order is (centroid_x, centroid_y, unit.x, unit.y). The centroid and
    // the unit tile are deliberately four DIFFERENT numbers, so any transposition of the pairs or
    // of x and y within a pair is visible. -----------------------------------------------------
    {
        fixture f;
        g_rec.clear();
        g_rec.centroid_x = 21;
        g_rec.centroid_y = 34;
        g_rec.answers    = {777};
        seed_chain(f, P, G, {5}); // unit 5 sits at (15, 45)

        (void)detail::group_no_member_near_centroid(f.view(), f.store(), calls(), P, G);

        ck(g_rec.dists.size() == 1 && g_rec.dists[0].x1 == 21 && g_rec.dists[0].y1 == 34 &&
               g_rec.dists[0].x2 == 15 && g_rec.dists[0].y2 == 45,
           "N5: toroidal_dist_sq(centroid_x, centroid_y, unit.x, unit.y), 0x004d6740-0x004d6754");
    }

    // ---- N6: the comparison is UNSIGNED. A distance with the top bit set is HUGE, not negative:
    // a signed `< 100` would call 0x80000000 near and answer 0. --------------------------------
    {
        fixture f;
        g_rec.clear();
        g_rec.answers = {0x80000000u};
        seed_chain(f, P, G, {6});

        const int32_t r = detail::group_no_member_near_centroid(f.view(), f.store(), calls(), P, G);

        ck(r == 1, "N6: 0x80000000 is far under the unsigned compare (JNC), not near");
    }
}

} // namespace mh::ai::test
