//
// tact_unit_despawn_selftest.cpp -- offline oracle for
//   llm_tact_unit_despawn @0x00432048 (libmh/tact/tact_unit_despawn.cpp)
//
// WHY OFFLINE, NOT RIG: see tact_unit_despawn.h's banner -- its own write closure (55 regions) is
// mostly unregistered presentation scratch reached through three non-wall UI-refresh calls, none of
// which is worth the state-registry investment for this 189-byte function. All four outward calls
// are mocked via the file's own `unit_despawn_calls` table.
//
#include "tact/tact_unit_despawn.h"

#include "tact_test_support.h"

namespace mh::tact::test {

namespace {
using namespace mh::tact;

constexpr int32_t UNIT_ID = 5;

struct despawn_recorder {
    int32_t              selection_panel_refresh_calls    = 0;
    int32_t              active_unit_count_hud_draw_calls = 0;
    std::vector<int32_t> ui_draw_player_row_list_args;
    std::vector<int32_t> unit_vision_remove_args;
    void                 reset() { *this = despawn_recorder{}; }
};
despawn_recorder g_rec;

const unit_despawn_calls &rec_calls() {
    static const unit_despawn_calls c = {
        []() { g_rec.selection_panel_refresh_calls++; },
        []() { g_rec.active_unit_count_hud_draw_calls++; },
        [](int32_t selected_row) { g_rec.ui_draw_player_row_list_args.push_back(selected_row); },
        [](int32_t unit_idx) { g_rec.unit_vision_remove_args.push_back(unit_idx); },
    };
    return c;
}

} // namespace

void run_unit_despawn_tests() {
    // T1: the five direct writes + all four outward calls, on a unit with a distinctive tile and a
    // type value that will NOT wrap (well below 0x80's complement), 0x0043209d-0x004320fb.
    {
        tact_fixture fx;
        g_rec.reset();
        constexpr int32_t COL = 17, ROW = 42;
        tact_unit        &u = fx.units[UNIT_ID];
        u.status            = 0xff; // any nonzero, so T1 can prove it was cleared, not merely left
        u.type              = 5;
        u.pos_col           = COL;
        u.pos_row           = ROW;
        // The original's tile write is ONE word store at tile+0x2 = `building` (0x004320d4:
        // MOV word ptr [EAX + 0xd1ec82],0x0). unit[0..1] at +4/+5 are seeded as SENTINELS that
        // must SURVIVE -- the first shipped translation cleared them instead of `building`, and
        // this oracle originally asserted that same wrong field (self-consistent, so it passed).
        fx.planes().tile_object_at(COL, ROW).building = 0x7799;
        fx.planes().tile_object_at(COL, ROW).unit[0]  = 0x77;
        fx.planes().tile_object_at(COL, ROW).unit[1]  = 0x99;
        fx.planes().passable_at(COL, ROW)             = mh::state::PASSABLE_BLOCKED;
        fx.unit_active_count                          = 3;

        tact_store own = fx.store();
        detail::unit_despawn(own, rec_calls(), UNIT_ID);

        ck_eq((uint32_t)own.unit_at(UNIT_ID).status, 0u, "T1: status cleared, 0x0043209d");
        ck_eq((uint32_t)own.unit_at(UNIT_ID).type, 5u + 0x80u, "T1: type += 0x80, 0x004320ab");
        ck_eq((uint32_t)fx.planes().tile_object_at(COL, ROW).building, 0u,
              "T1: tile.building (word at tile+0x2) cleared, 0x004320d4");
        ck_eq((uint32_t)fx.planes().tile_object_at(COL, ROW).unit[0], 0x77u,
              "T1: tile.unit[0] (+4) UNTOUCHED -- the write is at +0x2, not the unit pair");
        ck_eq((uint32_t)fx.planes().tile_object_at(COL, ROW).unit[1], 0x99u,
              "T1: tile.unit[1] (+5) UNTOUCHED -- the write is at +0x2, not the unit pair");
        ck_eq((uint32_t)fx.planes().passable_at(COL, ROW), (uint32_t)mh::state::PASSABLE_DEFAULT,
              "T1: passable reset to PASSABLE_DEFAULT, 0x004320e6");
        ck_eq((uint32_t)fx.unit_active_count, 2u, "T1: active count decremented, 0x004320ed");
        ck_eq((uint32_t)g_rec.selection_panel_refresh_calls, 1u,
              "T1: selection_panel_refresh called exactly once, 0x004320b2");
        ck_eq((uint32_t)g_rec.active_unit_count_hud_draw_calls, 1u,
              "T1: active_unit_count_hud_draw called exactly once, 0x004320b7");
        ck_eq((uint32_t)g_rec.ui_draw_player_row_list_args.size(), 1u,
              "T1: ui_draw_player_row_list called exactly once, 0x004320c1");
        if (g_rec.ui_draw_player_row_list_args.size() == 1) {
            ck_eq((uint32_t)g_rec.ui_draw_player_row_list_args[0], 0xffffffffu,
                  "T1: ui_draw_player_row_list argument is the literal -1, NOT unit_idx, 0x004320bc");
        }
        ck_eq((uint32_t)g_rec.unit_vision_remove_args.size(), 1u,
              "T1: unit_vision_remove called exactly once, 0x004320f3");
        if (g_rec.unit_vision_remove_args.size() == 1) {
            ck_eq((uint32_t)g_rec.unit_vision_remove_args[0], (uint32_t)UNIT_ID,
                  "T1: unit_vision_remove(unit_idx) forwarded unchanged, 0x004320f3");
        }
    }

    // T3: the type ADD wraps mod 256 (0x80 + 0x80 == 0x00), proving an 8-bit add rather than a
    // widening add or a plain overwrite -- 0x004320ab.
    {
        tact_fixture fx;
        g_rec.reset();
        tact_unit &u = fx.units[UNIT_ID];
        u.type       = 0x80;
        u.pos_col = u.pos_row = 0;

        tact_store own = fx.store();
        detail::unit_despawn(own, rec_calls(), UNIT_ID);

        ck_eq((uint32_t)own.unit_at(UNIT_ID).type, 0u, "T3: 0x80 + 0x80 wraps to 0 mod 256, 0x004320ab");
    }

    // T4: a SECOND, distinct (pos_col, pos_row) than T1's, so a translation that hardcoded T1's tile
    // rather than reading u.pos_col/pos_row would still pass T1 alone but fail here.
    {
        tact_fixture fx;
        g_rec.reset();
        constexpr int32_t COL2 = 3, ROW2 = 100;
        tact_unit        &u                             = fx.units[UNIT_ID];
        u.pos_col                                       = COL2;
        u.pos_row                                       = ROW2;
        fx.planes().tile_object_at(COL2, ROW2).building = 0x1111;
        fx.planes().tile_object_at(COL2, ROW2).unit[0]  = 0x11;
        fx.planes().passable_at(COL2, ROW2)             = mh::state::PASSABLE_BLOCKED;

        tact_store own = fx.store();
        detail::unit_despawn(own, rec_calls(), UNIT_ID);

        ck_eq((uint32_t)fx.planes().tile_object_at(COL2, ROW2).building, 0u,
              "T4: distinct-tile building stamp cleared, matching u.pos_col/pos_row, 0x004320c9-0x004320d4");
        ck_eq((uint32_t)fx.planes().tile_object_at(COL2, ROW2).unit[0], 0x11u,
              "T4: unit[0] sentinel untouched at the distinct tile too");
        ck_eq((uint32_t)fx.planes().passable_at(COL2, ROW2), (uint32_t)mh::state::PASSABLE_DEFAULT,
              "T4: distinct-tile passability reset, matching u.pos_col/pos_row, 0x004320dd-0x004320e6");
    }
}

} // namespace mh::tact::test
