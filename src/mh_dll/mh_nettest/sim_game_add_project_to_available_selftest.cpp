//
// sim_game_add_project_to_available_selftest.cpp -- `simtest` offline oracle for
// game_AddProjectToAvailable / game_AddProjectToAvailableWithCheck
// (sim/sim_game_add_project_to_available.{h,cpp}, RI-SIM / SIM1F, batch F).
//
// AddProjectToAvailable is straight-line address arithmetic into one call (no branch); WithCheck is
// a bare 16-bit-narrowing forwarder with no gate of its own. Both are fully offline-coverable via the
// one-member `_calls` struct's recording stub -- no live-image call, no float, no branch to miss.
//
// EXPECTED VALUES HAND-DERIVED FROM THE DISASSEMBLY
// (tmp/decomp/game_AddProjectToAvailable_0041422e.asm, game_AddProjectToAvailableWithCheck_00440174.asm)
// per the header's own derivation, not the .cpp.
//
#include "sim/sim_game_add_project_to_available.h"

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

struct InsertCall {
    int32_t *arr;
    int32_t  size;
    uint32_t player;
    int32_t  item;
    int32_t  new_item;
};
std::vector<InsertCall> g_inserts;

void stub_insert(int32_t *arr, int32_t size, uint32_t player, int32_t item, int32_t new_item) {
    g_inserts.push_back({arr, size, player, item, new_item});
}

const add_project_to_available_calls g_calls = {stub_insert};

void clear_calls() { g_inserts.clear(); }

} // namespace

void run_add_project_to_available_tests() {
    sim_fixture fx;

    // C1: bucket = base + (player*AVAILABLE_PROJECTS_TYPES_PER_PLAYER + type)*AVAILABLE_PROJECTS_SLOTS_PER_BUCKET,
    // where `type` comes from Projects[p_i].type -- hand-derived directly from the fixture's raw
    // buffer, not by calling own.available_projects_bucket() (that would just test the accessor
    // against itself). size is the bucket's own capacity constant; item is the constant ITEM_UNUSED;
    // new_item is p_i verbatim (widened to int32_t).
    // Mutation note: swapping the player/type multiply order, or passing `p_i` where `type` belongs,
    // would flip the pointer or new_item checks red.
    fx.reset();
    clear_calls();
    fx.cfg_projects[9].type = 1;
    {
        sim_store own = fx.store();
        detail::add_project_to_available(fx.view(), own, g_calls, /*player*/ 2u, /*p_i*/ 9u);
    }
    ck_eq((uint32_t)g_inserts.size(), 1u, "C1: exactly one insert call");
    {
        int32_t *expected = fx.available_projects.data() +
                            (2 * AVAILABLE_PROJECTS_TYPES_PER_PLAYER + 1) * AVAILABLE_PROJECTS_SLOTS_PER_BUCKET;
        ck(g_inserts[0].arr == expected, "C1: bucket ptr = base + (player(2)*2 + type(1))*50");
    }
    ck_eq((uint32_t)g_inserts[0].size, (uint32_t)AVAILABLE_PROJECTS_SLOTS_PER_BUCKET, "C1: size == bucket cap (50)");
    ck_eq(g_inserts[0].player, 2u, "C1: player passed through unchanged");
    ck_eq((uint32_t)g_inserts[0].item, (uint32_t)ITEM_UNUSED, "C1: item == ITEM_UNUSED(0)");
    ck_eq((uint32_t)g_inserts[0].new_item, 9u, "C1: new_item == p_i(9)");

    // C2: type == 0 selects the OTHER bucket of the same player's 2-bucket row (distinct from C1's
    // type==1), proving `type` truly indexes a second dimension rather than being folded away.
    fx.reset();
    clear_calls();
    fx.cfg_projects[9].type = 0;
    {
        sim_store own = fx.store();
        detail::add_project_to_available(fx.view(), own, g_calls, /*player*/ 2u, /*p_i*/ 9u);
    }
    {
        int32_t *expected = fx.available_projects.data() +
                            (2 * AVAILABLE_PROJECTS_TYPES_PER_PLAYER + 0) * AVAILABLE_PROJECTS_SLOTS_PER_BUCKET;
        ck(g_inserts[0].arr == expected, "C2: type(0) selects the OTHER bucket, distinct from C1's type(1)");
    }

    // C3: a different player selects a different row entirely (catches a player-index-ignored bug).
    fx.reset();
    clear_calls();
    fx.cfg_projects[9].type = 1;
    {
        sim_store own = fx.store();
        detail::add_project_to_available(fx.view(), own, g_calls, /*player*/ 5u, /*p_i*/ 9u);
    }
    {
        int32_t *expected = fx.available_projects.data() +
                            (5 * AVAILABLE_PROJECTS_TYPES_PER_PLAYER + 1) * AVAILABLE_PROJECTS_SLOTS_PER_BUCKET;
        ck(g_inserts[0].arr == expected, "C3: player(5) selects a distinct row from C1's player(2)");
    }
}

void run_add_project_to_available_with_check_tests() {
    sim_fixture fx;

    // WithCheck is a bare forwarder: MOVZX's the low 16 bits of `player`, then calls
    // add_project_to_available() unmodified -- no gate, despite the function's name.
    fx.reset();
    clear_calls();
    fx.cfg_projects[7].type = 1;
    {
        sim_store own = fx.store();
        detail::add_project_to_available_with_check(fx.view(), own, g_calls, /*player*/ (uint16_t)3,
                                                    /*p_i*/ 7u);
    }
    ck_eq((uint32_t)g_inserts.size(), 1u, "with_check: forwards unconditionally (exactly one call)");
    {
        int32_t *expected = fx.available_projects.data() +
                            (3 * AVAILABLE_PROJECTS_TYPES_PER_PLAYER + 1) * AVAILABLE_PROJECTS_SLOTS_PER_BUCKET;
        ck(g_inserts[0].arr == expected, "with_check: bucket ptr matches player(3)/type(1) exactly");
    }
    ck_eq(g_inserts[0].player, 3u, "with_check: player widened from uint16_t unchanged");
    ck_eq((uint32_t)g_inserts[0].new_item, 7u, "with_check: new_item == p_i(7)");

    // A distinct (player, type, p_i) triple than the direct-call tests above, to rule out the
    // with_check wrapper silently reusing stale arguments.
    fx.reset();
    clear_calls();
    fx.cfg_projects[42].type = 0;
    {
        sim_store own = fx.store();
        detail::add_project_to_available_with_check(fx.view(), own, g_calls, /*player*/ (uint16_t)6,
                                                    /*p_i*/ 42u);
    }
    {
        int32_t *expected = fx.available_projects.data() +
                            (6 * AVAILABLE_PROJECTS_TYPES_PER_PLAYER + 0) * AVAILABLE_PROJECTS_SLOTS_PER_BUCKET;
        ck(g_inserts[0].arr == expected, "with_check C2: bucket ptr matches player(6)/type(0)");
    }
    ck_eq((uint32_t)g_inserts[0].new_item, 42u, "with_check C2: new_item == p_i(42)");
}

} // namespace mh::sim::test
