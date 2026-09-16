//
// tact_unit_destroy_selftest.cpp -- offline oracle for
//   llm_tact_unit_destroy @0x00431d8d (libmh/tact/tact_unit_destroy.h)
//
// WHY OFFLINE, NOT RIG: see tact_unit_destroy.h's banner -- 29 functions/55 regions reachable (the
// presentation cascade via ui_draw_player_row_list), structurally expensive to rig-arm. REVIEW
// REQUIRED (writes shared `passable`/`tile_objects`) -- see tools/data/tact_review_ledger.json for
// this function's disposition. All four outward calls mocked via the calls struct.
//
#include "tact/tact_unit_destroy.h"

#include "tact_test_support.h"

namespace mh::tact::test {

namespace {
using namespace mh::tact;

struct destroy_recorder {
    std::vector<int32_t> vision_remove_units;
    int32_t              selection_panel_refresh_calls    = 0;
    int32_t              active_unit_count_hud_draw_calls = 0;
    std::vector<int32_t> ui_draw_player_row_list_args;
    void                 reset() { *this = destroy_recorder{}; }
};
destroy_recorder g_rec;

unit_destroy_calls rec_calls() {
    return {
        [](int32_t unit_idx) { g_rec.vision_remove_units.push_back(unit_idx); },
        []() { g_rec.selection_panel_refresh_calls++; },
        []() { g_rec.active_unit_count_hud_draw_calls++; },
        [](int32_t selected_row) { g_rec.ui_draw_player_row_list_args.push_back(selected_row); },
    };
}

constexpr uint32_t UNIT_IDX = 12;

} // namespace

void run_unit_destroy_tests() {
    // T1: the (col,row) tile itself matches -- EARLY EXIT. A decoy at (col+1,row) also carries this
    // unit's id but must survive untouched, proving the early exit really skips the rest of the
    // search rather than merely happening to stop. 0x00431df1-0x00431e22.
    {
        tact_fixture fx;
        g_rec.reset();
        tact_unit &u                                = fx.units[UNIT_IDX];
        u.pos_col                                   = 10;
        u.pos_row                                   = 10;
        u.status                                    = 0x55;
        u.type                                      = 3;
        fx.planes().tile_object_at(10, 10).building = UNIT_IDX; // the match
        fx.planes().tile_object_at(11, 10).building = UNIT_IDX; // decoy -- must survive
        fx.planes().passable_at(10, 10)             = mh::state::PASSABLE_BLOCKED;
        fx.unit_active_count                        = 5;

        tact_store own = fx.store();
        detail::unit_destroy(own, rec_calls(), UNIT_IDX);

        ck_eq((uint32_t)fx.planes().tile_object_at(10, 10).building, 0u, "T1: matched tile cleared, 0x00431e17");
        ck_eq((uint32_t)fx.planes().tile_object_at(11, 10).building, UNIT_IDX,
              "T1: decoy at (col+1,row) NOT touched -- early exit, 0x00431e22");
        ck_eq((uint32_t)own.unit_at(UNIT_IDX).status, 0u, "T1: status = 0, 0x00431de3");
        ck_eq((uint32_t)own.unit_at(UNIT_IDX).type, 0u, "T1: type = 0 (slot emptied), 0x0043200d");
        ck_eq((uint32_t)own.unit_active_count(), 4u, "T1: active count decremented, 0x00432017");
        ck_eq((uint32_t)fx.planes().passable_at(10, 10), (uint32_t)mh::state::PASSABLE_DEFAULT,
              "T1: passable_at(col,row) = DEFAULT, 0x00432023");
        ck_eq((uint32_t)g_rec.vision_remove_units.size(), 1u, "T1: vision_remove called once, 0x00431da8");
        if (g_rec.vision_remove_units.size() == 1)
            ck_eq((uint32_t)g_rec.vision_remove_units[0], UNIT_IDX, "T1: ...with this unit's own id");
        ck_eq((uint32_t)g_rec.selection_panel_refresh_calls, 1u, "T1: selection_panel_refresh, 0x0043202a");
        ck_eq((uint32_t)g_rec.active_unit_count_hud_draw_calls, 1u, "T1: active_unit_count_hud_draw, 0x0043202f");
        ck_eq((uint32_t)g_rec.ui_draw_player_row_list_args.size(), 1u, "T1: ui_draw_player_row_list called once");
        if (g_rec.ui_draw_player_row_list_args.size() == 1)
            ck_eq((uint32_t)g_rec.ui_draw_player_row_list_args[0], (uint32_t)-1,
                  "T1: ...with selected_row = -1, 0x00432039");
    }

    // T2: (col,row) does NOT match -- falls into the else branch. THREE separate candidates
    // ((col+1,row), (col,row+1), and (col-1,row-1)) all carry this unit's id and ALL get cleared:
    // every check past the redundant re-check is unconditional, not another early exit. Also pins
    // the (col,row+1) vs (col+1,row+1) distinction directly: (col,row+1) matches and is cleared,
    // while (col+1,row+1) carries a DIFFERENT id and must survive untouched -- catching the
    // 2026-08-26 bug where those two candidates were collapsed into one. 0x00431e58-0x00431f30.
    {
        tact_fixture fx;
        g_rec.reset();
        tact_unit &u                              = fx.units[UNIT_IDX];
        u.pos_col                                 = 5;
        u.pos_row                                 = 5;
        fx.planes().tile_object_at(5, 5).building = 0;        // no match at (col,row) itself
        fx.planes().tile_object_at(6, 5).building = UNIT_IDX; // (col+1,row) -- match #1
        fx.planes().tile_object_at(5, 6).building = UNIT_IDX; // (col,row+1) -- match #2
        fx.planes().tile_object_at(4, 4).building = UNIT_IDX; // (col-1,row-1) -- match #3
        fx.planes().tile_object_at(6, 6).building = 99;       // (col+1,row+1) -- different id, untouched
        fx.unit_active_count                      = 1;

        tact_store own = fx.store();
        detail::unit_destroy(own, rec_calls(), UNIT_IDX);

        ck_eq((uint32_t)fx.planes().tile_object_at(6, 5).building, 0u, "T2: (col+1,row) cleared, 0x00431e58");
        ck_eq((uint32_t)fx.planes().tile_object_at(5, 6).building, 0u, "T2: (col,row+1) cleared, 0x00431e8b");
        ck_eq((uint32_t)fx.planes().tile_object_at(4, 4).building, 0u, "T2: (col-1,row-1) ALSO cleared, 0x00431eef");
        ck_eq((uint32_t)fx.planes().tile_object_at(6, 6).building, 99u,
              "T2: (col+1,row+1), different id, untouched, 0x00431ebc");
    }

    // T3: col==0 AND row==0 -- both guarded candidate groups ((col-1,*) and (*,row-1)) are skipped
    // entirely rather than computing a negative tile index. The function must still complete
    // normally (teardown fields, all three outward calls) with only the two unconditional
    // candidates ((col,row) itself and the redundant re-check) considered. 0x00431eef/0x00431f30/
    // 0x00431f9c (all three guards false).
    {
        tact_fixture fx;
        g_rec.reset();
        tact_unit &u                              = fx.units[UNIT_IDX];
        u.pos_col                                 = 0;
        u.pos_row                                 = 0;
        fx.planes().tile_object_at(0, 0).building = 0; // no match -- exercises the else branch
        fx.unit_active_count                      = 1;

        tact_store own = fx.store();
        detail::unit_destroy(own, rec_calls(), UNIT_IDX);

        ck_eq((uint32_t)own.unit_at(UNIT_IDX).type, 0u, "T3: completes normally at the (0,0) boundary");
        ck_eq((uint32_t)own.unit_active_count(), 0u, "T3: active count decremented");
        ck_eq((uint32_t)fx.planes().passable_at(0, 0), (uint32_t)mh::state::PASSABLE_DEFAULT,
              "T3: passable_at(0,0) = DEFAULT despite the (0,0) boundary");
        ck_eq((uint32_t)g_rec.selection_panel_refresh_calls, 1u, "T3: refresh calls still fire at the boundary");
    }
}

} // namespace mh::tact::test
