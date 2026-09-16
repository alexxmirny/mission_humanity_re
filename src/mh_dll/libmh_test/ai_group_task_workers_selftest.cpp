//
// ai_group_task_workers_selftest.cpp -- `aitest` cases for the two zero-call group-task workers
// (RI-AI batch C / AI1C, both closed at T3):
//   llm_strat_ai_group_rally_formup_worker   @0x004d5dfc, 179 B (task_code 0x02, rally/form-up)
//   llm_strat_ai_group_scatter_random_worker @0x004d5f4c, 163 B (task_code 0x0d, scatter-random)
//
// WHY THIS FILE EXISTS. Both rows were closed on reimpl-verify plus a shadow site (shadow_ai_c9.ini)
// that ARMED and was reached ZERO times: neither task_code is dispatched from the default 8-way AI
// start. AI1's done_when requires a T3 to carry its written reason PLUS a compensating test; this is
// that test, written at AI1 close (2026-08-30). The scenario gap is unchanged, and a zero-call
// shadow site is still not read as clean.
//
// WHAT IT PINS. For rally: that the centroid lands in active_param_a/b IN THAT ORDER (a = x, b = y),
// which is the exact pairing the exported .c's mislabeled stack locals got wrong and the header
// had to derive from the assembly; that the spiral cursor advances ONE PER MEMBER from 0; and that
// each destination is (centroid + spiral offset) MASKED by map_width_mask/map_height_mask, so the
// wrap is an AND and not a clamp or a modulo. For scatter: that a member with an order already
// pending is skipped WITHOUT drawing a random point (the draw is the RNG side effect, so a skipped
// member that still drew would desync a lockstep peer), that the walk CONTINUES past it, that the
// destination handed to unit_flag_and_move is exactly what random_point_near produced, and that --
// unlike its rally sibling -- it writes NO group state at all.
//
#include "ai_test_support.h"

#include "ai/ai_group_task_workers.h"

namespace mh::ai::test {
namespace {

using namespace mh::ai;

struct move_call {
    uint32_t player;
    int32_t  unit_index;
    uint32_t x, y;
};
struct point_call {
    int32_t x, y, radius;
};

struct recorder {
    std::vector<move_call>  moves;
    std::vector<point_call> points;
    std::vector<uint32_t>   pending;  // unit ids whose order is already pending
    std::vector<int32_t>    point_xy; // scripted (x, y) pairs random_point_near hands back
    size_t                  cursor     = 0;
    int32_t                 centroid_x = 0, centroid_y = 0;
    int                     centroid_calls = 0;

    void clear() {
        moves.clear();
        points.clear();
        pending.clear();
        point_xy.clear();
        cursor         = 0;
        centroid_x     = 0;
        centroid_y     = 0;
        centroid_calls = 0;
    }
    bool is_pending(uint32_t id) const {
        for (uint32_t p : pending)
            if (p == id) return true;
        return false;
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

void st_unit_flag_and_move(uint32_t player, int32_t unit_index, uint32_t x, uint32_t y) {
    g_rec.moves.push_back({player, unit_index, x, y});
}

uint32_t st_unit_is_order_pending(uint32_t player, uint32_t unit_id) {
    (void)player;
    return g_rec.is_pending(unit_id) ? 1u : 0u;
}

void st_random_point_near(int32_t x, int32_t y, int32_t radius, int32_t *out_x, int32_t *out_y) {
    g_rec.points.push_back({x, y, radius});
    const size_t  i  = g_rec.cursor * 2;
    const int32_t px = i < g_rec.point_xy.size() ? g_rec.point_xy[i] : -1;
    const int32_t py = i + 1 < g_rec.point_xy.size() ? g_rec.point_xy[i + 1] : -1;
    ++g_rec.cursor;
    *out_x = px;
    *out_y = py;
}

const ai_calls &calls() {
    static const ai_calls c = [] {
        ai_calls t{};
        t.group_compute_centroid = &st_group_compute_centroid;
        t.unit_flag_and_move     = &st_unit_flag_and_move;
        t.unit_is_order_pending  = &st_unit_is_order_pending;
        t.random_point_near      = &st_random_point_near;
        return t;
    }();
    return c;
}

void seed_chain(fixture &f, int p, int g, std::initializer_list<int> ids) {
    unit_group &grp  = f.players[p].ai_groups[g];
    int         prev = 0;
    for (int id : ids) {
        if (prev == 0)
            grp.head_unit = (uint16_t)id;
        else
            f.u(p, prev).ai_group_next = (uint16_t)id;
        prev = id;
    }
    if (prev != 0) f.u(p, prev).ai_group_next = 0;
    grp.member_count = (int16_t)ids.size();
}

// The first few spiral cells, given DISTINCT dx and dy per entry (and dx != dy within an entry) so
// a translation that read the cursor twice, or swapped the two components, changes a destination.
void seed_spiral(fixture &f) {
    const int8_t DX[4] = {0, 1, -2, 3};
    const int8_t DY[4] = {0, -3, 4, 1};
    for (int i = 0; i < 4; ++i) {
        f.spiral[i].dx = (char)DX[i];
        f.spiral[i].dy = (char)DY[i];
    }
}

} // namespace

void run_group_task_workers_tests() {
    printf("-- ai_group_task_workers (rally_formup / scatter_random) --\n");

    const int32_t P = 6;
    const int32_t G = 2;

    // ==== llm_strat_ai_group_rally_formup_worker =================================================

    // ---- R1: the centroid is stored as (a = x, b = y). The two values are deliberately different
    // -- a swap is the error this pairing exists to catch. --------------------------------------
    {
        fixture f;
        g_rec.clear();
        seed_spiral(f);
        g_rec.centroid_x = 20;
        g_rec.centroid_y = 33;
        seed_chain(f, P, G, {}); // empty: the anchor write must not depend on having members

        detail::group_rally_formup_worker(f.view(), f.store(), calls(), P, G);

        const unit_group &grp = f.players[P].ai_groups[G];
        ck(grp.active_param_a == 20 && grp.active_param_b == 33,
           "R1: active_param_a = centroid X, active_param_b = centroid Y");
        ck(g_rec.moves.empty(), "R1: an empty chain moves nothing");
        ck(g_rec.centroid_calls == 1, "R1: the centroid is computed once, before the walk");
    }

    // ---- R2: one move per member, and the spiral cursor advances WITH the member -- member k
    // takes spiral cell k, counting from 0. -----------------------------------------------------
    {
        fixture f;
        g_rec.clear();
        seed_spiral(f);
        g_rec.centroid_x = 20;
        g_rec.centroid_y = 33;
        seed_chain(f, P, G, {3, 8, 5});

        detail::group_rally_formup_worker(f.view(), f.store(), calls(), P, G);

        ck(g_rec.moves.size() == 3, "R2: one move per member");
        ck(g_rec.moves[0].unit_index == 3 && g_rec.moves[1].unit_index == 8 &&
               g_rec.moves[2].unit_index == 5,
           "R2: members are visited head -> ai_group_next");
        // cells: (0,0), (1,-3), (-2,4) against centroid (20, 33), masks 127 / 63
        ck(g_rec.moves[0].x == 20u && g_rec.moves[0].y == 33u,
           "R2: cell 0 is the centroid itself");
        ck(g_rec.moves[1].x == 21u && g_rec.moves[1].y == 30u,
           "R2: cell 1 offsets by (dx, dy) = (1, -3) -- the cursor advanced exactly one");
        ck(g_rec.moves[2].x == 18u && g_rec.moves[2].y == 37u, "R2: cell 2 offsets by (-2, 4)");
        ck(g_rec.moves[0].player == (uint32_t)P, "R2: the player is passed through");
    }

    // ---- R3: the wrap is an AND against map_width_mask / map_height_mask, not a clamp. A
    // negative offset off the left edge wraps to the far side; the two masks are DIFFERENT
    // (127 / 63), so an axis swap is visible too. ------------------------------------------------
    {
        fixture f;
        g_rec.clear();
        seed_spiral(f);
        g_rec.centroid_x = 0;
        g_rec.centroid_y = 0;
        seed_chain(f, P, G, {9, 4});

        detail::group_rally_formup_worker(f.view(), f.store(), calls(), P, G);

        ck(g_rec.moves.size() == 2, "R3: both members moved");
        // cell 1 = (1, -3) from (0, 0): x = 1, y = -3 & 63 = 61
        ck(g_rec.moves[1].x == 1u && g_rec.moves[1].y == 61u,
           "R3: -3 & map_height_mask wraps to 61 -- an AND, and the HEIGHT mask on the y axis");
    }

    // ==== llm_strat_ai_group_scatter_random_worker ===============================================

    // ---- S1: a member whose order is already pending is skipped ENTIRELY -- no random draw and
    // no move -- and the walk continues to the next member. -------------------------------------
    {
        fixture f;
        g_rec.clear();
        g_rec.centroid_x = 40;
        g_rec.centroid_y = 12;
        g_rec.pending    = {8};
        g_rec.point_xy   = {70, 22};
        seed_chain(f, P, G, {8, 5});

        detail::group_scatter_random_worker(f.view(), f.store(), calls(), P, G, 6u);

        ck(g_rec.points.size() == 1,
           "S1: the pending member draws NO random point -- the RNG is not advanced for it");
        ck(g_rec.moves.size() == 1 && g_rec.moves[0].unit_index == 5,
           "S1: only the non-pending member moves, and the walk did not stop at the skip");
    }

    // ---- S2: the destination is exactly what random_point_near produced, and the draw is
    // anchored at the centroid with the caller's radius. ----------------------------------------
    {
        fixture f;
        g_rec.clear();
        g_rec.centroid_x = 40;
        g_rec.centroid_y = 12;
        g_rec.point_xy   = {70, 22, 71, 23};
        seed_chain(f, P, G, {5, 9});

        detail::group_scatter_random_worker(f.view(), f.store(), calls(), P, G, 6u);

        ck(g_rec.points.size() == 2 && g_rec.points[0].x == 40 && g_rec.points[0].y == 12 &&
               g_rec.points[0].radius == 6,
           "S2: random_point_near(centroid_x, centroid_y, class_or_radius)");
        ck(g_rec.moves.size() == 2 && g_rec.moves[0].x == 70u && g_rec.moves[0].y == 22u &&
               g_rec.moves[1].x == 71u && g_rec.moves[1].y == 23u,
           "S2: each member is moved to ITS OWN draw, in order");
    }

    // ---- S3: scatter writes NO group state. Its rally sibling stamps active_param_a/b from the
    // same centroid call, so a copy-paste between the two is the likely error. ------------------
    {
        fixture f;
        g_rec.clear();
        g_rec.centroid_x                         = 40;
        g_rec.centroid_y                         = 12;
        g_rec.point_xy                           = {70, 22};
        f.players[P].ai_groups[G].active_param_a = -111; // poison the two the sibling writes
        f.players[P].ai_groups[G].active_param_b = -222;
        seed_chain(f, P, G, {5});

        detail::group_scatter_random_worker(f.view(), f.store(), calls(), P, G, 6u);

        const unit_group &grp = f.players[P].ai_groups[G];
        ck(grp.active_param_a == -111 && grp.active_param_b == -222,
           "S3: the scatter worker leaves the group's anchor alone -- (void)own is the whole story");
    }

    // ---- S4: every member pending is the "nothing" case -- one centroid call, no draw, no move. -
    {
        fixture f;
        g_rec.clear();
        g_rec.pending = {5, 9};
        seed_chain(f, P, G, {5, 9});

        detail::group_scatter_random_worker(f.view(), f.store(), calls(), P, G, 6u);

        ck(g_rec.centroid_calls == 1 && g_rec.points.empty() && g_rec.moves.empty(),
           "S4: the centroid is still computed, but nothing is drawn and nothing moves");
    }
}

} // namespace mh::ai::test
