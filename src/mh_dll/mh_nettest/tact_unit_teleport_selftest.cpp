//
// tact_unit_teleport_selftest.cpp -- offline oracle for
//   llm_tact_unit_teleport @0x00432df0 (libmh/tact/tact_unit_teleport.cpp)
//
// WHY OFFLINE, NOT RIG: the function's own body calls llm_tact_unit_destroy (-> the roster-refresh
// cascade -> a real framebuffer blit) and llm_tact_fx_spawn (-> real DirectSound playback) on its
// invalid-destination paths. Both are TACT-CUT2 SHARED callees tools/data/tact_shared_callees.json
// classifies `effectful`, ungated (the open, non-autonomous TACT-CUT2 item). All eight outward
// calls are mocked via the file's own `unit_teleport_calls` table.
//
#include "tact/tact_unit_teleport.h"

#include "tact_test_support.h"

namespace mh::tact::test {

namespace {
using namespace mh::tact;

constexpr int32_t UNIT_IDX = 5;

struct call_log {
    std::vector<uint32_t>                                                                  destroy_calls;
    std::vector<std::tuple<int32_t, uint8_t, int32_t, int32_t, int32_t, int32_t, uint8_t>> fx_calls;
    std::vector<int32_t>                                                                   vision_add_calls;
    std::vector<int32_t>                                                                   vision_remove_calls;
    std::vector<std::tuple<int32_t, int32_t>>                                              camera_calls;
    std::vector<std::tuple<int32_t, int32_t>>                                              cmd_advance_calls;
    std::vector<int32_t>                                                                   facing_calls;
    int32_t                                                                                rand_value = 0;
    int32_t                                                                                out_dx     = 0;
    int32_t                                                                                out_dy     = 0;
    void                                                                                   reset() { *this = call_log{}; }
};
call_log g_log;

const unit_teleport_calls &rec_calls() {
    static const unit_teleport_calls c = {
        [](uint32_t unit_idx) { g_log.destroy_calls.push_back(unit_idx); },
        [](int32_t fx_type, uint8_t owner, int32_t x, int32_t y, int32_t x2, int32_t y2,
           uint8_t altitude) -> int32_t {
            g_log.fx_calls.push_back({fx_type, owner, x, y, x2, y2, altitude});
            return 0;
        },
        [](int32_t unit_idx) { g_log.vision_add_calls.push_back(unit_idx); },
        [](int32_t unit_idx) { g_log.vision_remove_calls.push_back(unit_idx); },
        [](int32_t target_col, int32_t target_row) {
            g_log.camera_calls.push_back({target_col, target_row});
        },
        []() -> int32_t { return g_log.rand_value; },
        [](int32_t unit_idx, int32_t cmd_slot_index) {
            g_log.cmd_advance_calls.push_back({unit_idx, cmd_slot_index});
        },
        [](int32_t facing_dir, int32_t *out_dx, int32_t *out_dy) {
            g_log.facing_calls.push_back(facing_dir);
            *out_dx = g_log.out_dx;
            *out_dy = g_log.out_dy;
        },
    };
    return c;
}

} // namespace

void run_unit_teleport_tests() {
    // T1: field_28-flagged zone refuses entirely, 0x00432e3a/0x00432e41 -- no call fires at all.
    {
        tact_fixture fx;
        g_log.reset();
        fx.teleport_zones[3].field_28 = 1;
        fx.units[UNIT_IDX].pos_col    = 10;
        fx.units[UNIT_IDX].pos_row    = 10;
        tact_store own                = fx.store();
        auto       planes             = own.planes();
        detail::unit_teleport(own, planes, rec_calls(), 3, UNIT_IDX);
        ck_eq((uint32_t)(g_log.destroy_calls.size() + g_log.fx_calls.size() +
                         g_log.vision_add_calls.size() + g_log.vision_remove_calls.size()),
              0u, "T1: field_28-flagged zone -- no call fires at all, 0x00432e41");
    }

    // T2: NO_ENEMY zone refuses owner==1 specifically -- the polarity is the OPPOSITE of the field
    // name's first-glance reading (0x00432e5b-0x00432e66).
    {
        tact_fixture fx;
        g_log.reset();
        fx.teleport_zones[3].no_enemy = 1;
        fx.units[UNIT_IDX].owner      = 1;
        tact_store own                = fx.store();
        auto       planes             = own.planes();
        detail::unit_teleport(own, planes, rec_calls(), 3, UNIT_IDX);
        ck_eq((uint32_t)(g_log.vision_remove_calls.size() + g_log.destroy_calls.size()), 0u,
              "T2: NO_ENEMY zone refuses owner==1, 0x00432e62 JZ");
    }

    // T3: NO_ENEMY zone does NOT refuse owner==0 -- confirms T2 is not simply "always refuse".
    // mode==0, slot 0 destination clear -> the move completes.
    {
        tact_fixture fx;
        g_log.reset();
        fx.teleport_zones[3].no_enemy    = 1;
        fx.teleport_zones[3].dest_col[0] = 40;
        fx.teleport_zones[3].dest_row[0] = 41;
        fx.units[UNIT_IDX].owner         = 0;
        fx.units[UNIT_IDX].pos_col       = 10;
        fx.units[UNIT_IDX].pos_row       = 10;
        tact_store own                   = fx.store();
        auto       planes                = own.planes();
        detail::unit_teleport(own, planes, rec_calls(), 3, UNIT_IDX);
        ck_eq((uint32_t)g_log.vision_remove_calls.size(), 1u,
              "T3: NO_ENEMY zone does NOT refuse owner==0 -- the move proceeds");
    }

    // T4: mid-move cancel (progress != 0), 0x00432e7b-0x00432ec3: facing_to_delta is called with
    // facing_dir, and the two passable writes use DELIBERATELY distinct (dx,dy) so a swapped
    // add/subtract or a swapped col/row would fail.
    {
        tact_fixture fx;
        g_log.reset();
        g_log.out_dx = 3;
        g_log.out_dy = -2;
        // dest resolves to the (0xff,0xff) sentinel so the function takes the destroy+fx fallback
        // AFTER the cancel block, which touches neither passable nor tile_objects -- isolating the
        // cancel block's own writes rather than having the subsequent real move's own
        // passable[cur_col][cur_row]=DEFAULT write (0x00433151) silently overwrite them back.
        fx.teleport_zones[3].dest_col[0] = 0xff;
        fx.teleport_zones[3].dest_row[0] = 0xff;
        fx.units[UNIT_IDX].facing_dir    = 7;
        fx.units[UNIT_IDX].progress      = 5;
        fx.units[UNIT_IDX].pos_col       = 10;
        fx.units[UNIT_IDX].pos_row       = 10;
        tact_store own                   = fx.store();
        auto       planes                = own.planes();
        detail::unit_teleport(own, planes, rec_calls(), 3, UNIT_IDX);
        ck_eq((uint32_t)g_log.facing_calls.size(), 1u, "T4: facing_to_delta called once, 0x00432e8f");
        if (g_log.facing_calls.size() == 1) {
            ck_eq((uint32_t)g_log.facing_calls[0], 7u, "T4: facing_dir forwarded unchanged");
        }
        ck_eq((uint32_t)planes.passable_at(13, 8), mh::state::PASSABLE_DEFAULT,
              "T4: passable[cur_col+dx][cur_row+dy] freed, 0x00432ea5");
        ck_eq((uint32_t)planes.passable_at(10, 10), mh::state::PASSABLE_BLOCKED,
              "T4: passable[cur_col][cur_row] re-occupied, 0x00432eb5");
        ck_eq((uint32_t)fx.units[UNIT_IDX].progress, 0u, "T4: progress reset to 0, 0x00432ec3");
    }

    // T5: DIRECT, mode==0, resolved (0xff,0xff) -- destroy + death-fx fallback, 0x00433032.
    // Deliberately non-symmetric pixel math: cur_col != cur_row so px/py cannot be swapped unnoticed.
    {
        tact_fixture fx;
        g_log.reset();
        fx.teleport_zones[3].dest_col[0] = 0xff;
        fx.teleport_zones[3].dest_row[0] = 0xff;
        fx.teleport_zones[3].death       = 9;
        fx.units[UNIT_IDX].pos_col       = 4;
        fx.units[UNIT_IDX].pos_row       = 7;
        tact_store own                   = fx.store();
        auto       planes                = own.planes();
        detail::unit_teleport(own, planes, rec_calls(), 3, UNIT_IDX);
        ck_eq((uint32_t)g_log.destroy_calls.size(), 1u, "T5: unit_destroy fires once, 0x00433035");
        if (g_log.destroy_calls.size() == 1)
            ck_eq(g_log.destroy_calls[0], (uint32_t)UNIT_IDX, "T5: destroys the right unit");
        ck_eq((uint32_t)g_log.fx_calls.size(), 1u, "T5: fx_spawn fires once, 0x0043306b");
        if (g_log.fx_calls.size() == 1) {
            const auto &call = g_log.fx_calls[0];
            ck_eq((uint32_t)std::get<0>(call), 9u, "T5: fx_type == zone.death, 0x00433064");
            ck_eq((uint32_t)std::get<1>(call), 0u, "T5: owner == 0, 0x0043305e");
            ck_eq((uint32_t)std::get<2>(call), 4u * 32u + 16u, "T5: x == cur_col*32+16, 0x0043304a");
            ck_eq((uint32_t)std::get<3>(call), 7u * 24u + 12u, "T5: y == cur_row*24+12, 0x00433040");
            ck_eq((uint32_t)std::get<4>(call), 4u * 32u + 16u, "T5: x2 == x, 0x0043304d");
            ck_eq((uint32_t)std::get<5>(call), 7u * 24u + 12u, "T5: y2 == y, 0x00433043");
            ck_eq((uint32_t)std::get<6>(call), 0x28u, "T5: altitude == 0x28, 0x0043303a");
        }
        ck_eq((uint32_t)g_log.vision_remove_calls.size(), 0u,
              "T5: no move attempted after the destroy fallback");
    }

    // T6: DIRECT, destination tile occupied -- the 2x2 cascade steps to (dest_col+1, dest_row)
    // when only that corner is free, 0x0043308d-0x004330a4/0x004330f1.
    {
        tact_fixture fx;
        g_log.reset();
        fx.teleport_zones[3].dest_col[0]       = 50;
        fx.teleport_zones[3].dest_row[0]       = 60;
        fx.units[UNIT_IDX].pos_col             = 10;
        fx.units[UNIT_IDX].pos_row             = 10;
        tact_store own                         = fx.store();
        auto       planes                      = own.planes();
        planes.tile_object_at(50, 60).building = 1; // occupied
        planes.tile_object_at(51, 60).building = 0; // free
        detail::unit_teleport(own, planes, rec_calls(), 3, UNIT_IDX);
        ck_eq((uint32_t)fx.units[UNIT_IDX].pos_col, 51u, "T6: lands one column over, 0x004330de");
        ck_eq((uint32_t)fx.units[UNIT_IDX].pos_row, 60u, "T6: row unchanged, CASE_INCX");
    }

    // T7: DIRECT, all four corners of the 2x2 block occupied -- give up entirely, 0x004330d5. No
    // vision/tile/passable/pos writes and no cmd_advance/camera calls.
    {
        tact_fixture fx;
        g_log.reset();
        fx.teleport_zones[3].dest_col[0]       = 70;
        fx.teleport_zones[3].dest_row[0]       = 70;
        fx.units[UNIT_IDX].pos_col             = 10;
        fx.units[UNIT_IDX].pos_row             = 10;
        tact_store own                         = fx.store();
        auto       planes                      = own.planes();
        planes.tile_object_at(70, 70).building = 1;
        planes.tile_object_at(71, 70).building = 1;
        planes.tile_object_at(70, 71).building = 1;
        planes.tile_object_at(71, 71).building = 1;
        detail::unit_teleport(own, planes, rec_calls(), 3, UNIT_IDX);
        ck_eq((uint32_t)g_log.vision_remove_calls.size(), 0u,
              "T7: all four corners occupied -- no move attempted, 0x004330d5");
        ck_eq((uint32_t)fx.units[UNIT_IDX].pos_col, 10u, "T7: position unchanged");
    }

    // T8: DIRECT success -- verifies every write of the final-placement block, 0x004330f7-0x004331d9.
    // owner==0 so the camera-centre call also fires.
    {
        tact_fixture fx;
        g_log.reset();
        fx.teleport_zones[3].dest_col[0] = 30;
        fx.teleport_zones[3].dest_row[0] = 35;
        fx.units[UNIT_IDX].pos_col       = 12;
        fx.units[UNIT_IDX].pos_row       = 15;
        fx.units[UNIT_IDX].owner         = 0;
        fx.units[UNIT_IDX].cmd_index     = 2;
        tact_store own                   = fx.store();
        auto       planes                = own.planes();
        detail::unit_teleport(own, planes, rec_calls(), 3, UNIT_IDX);
        ck_eq((uint32_t)planes.tile_object_at(12, 15).building, 0u,
              "T8: old tile's building cleared, 0x00433125");
        ck_eq((uint32_t)planes.passable_at(12, 15), mh::state::PASSABLE_DEFAULT,
              "T8: old tile freed, 0x00433151");
        ck_eq((uint32_t)planes.passable_at(30, 35), mh::state::PASSABLE_BLOCKED,
              "T8: new tile occupied, 0x00433161");
        ck_eq((uint32_t)planes.tile_object_at(30, 35).building, (uint32_t)UNIT_IDX,
              "T8: new tile's building == unit_id, 0x00433179");
        ck_eq((uint32_t)fx.units[UNIT_IDX].pos_col, 30u, "T8: pos_col updated, 0x0043318a");
        ck_eq((uint32_t)fx.units[UNIT_IDX].pos_row, 35u, "T8: pos_row updated, 0x0043319a");
        ck_eq((uint32_t)g_log.vision_remove_calls.size(), 1u, "T8: vision_remove fires, 0x004330fa");
        ck_eq((uint32_t)g_log.vision_add_calls.size(), 1u, "T8: vision_add fires, 0x004331a3");
        ck_eq((uint32_t)g_log.cmd_advance_calls.size(), 1u, "T8: unit_cmd_advance fires, 0x004331b9");
        if (g_log.cmd_advance_calls.size() == 1) {
            ck_eq((uint32_t)std::get<1>(g_log.cmd_advance_calls[0]), 2u,
                  "T8: cmd_advance's slot arg == unit.cmd_index, 0x004331af");
        }
        ck_eq((uint32_t)g_log.camera_calls.size(), 1u, "T8: owner==0 -- camera_center fires, 0x004331d4");
        if (g_log.camera_calls.size() == 1) {
            ck_eq((uint32_t)std::get<0>(g_log.camera_calls[0]), 30u, "T8: camera target_col");
            ck_eq((uint32_t)std::get<1>(g_log.camera_calls[0]), 35u, "T8: camera target_row");
        }
    }

    // T9: DIRECT, owner!=0 -- camera_center_on_tile is NOT called, 0x004331cc JNZ.
    {
        tact_fixture fx;
        g_log.reset();
        fx.teleport_zones[3].dest_col[0] = 30;
        fx.teleport_zones[3].dest_row[0] = 35;
        fx.units[UNIT_IDX].owner         = 1;
        tact_store own                   = fx.store();
        auto       planes                = own.planes();
        detail::unit_teleport(own, planes, rec_calls(), 3, UNIT_IDX);
        ck_eq((uint32_t)g_log.camera_calls.size(), 0u, "T9: owner!=0 -- no camera call, 0x004331cc");
    }

    // T10: DIRECT, mode==2 (SEQUENCE) advances field_03 to the next NON-EMPTY slot, 0x00432ee8-
    // 0x00432f3c. slot 0 and slot 1 both populated -> field_03 starts at 0, advances to 1, uses
    // dest_col[1]/dest_row[1] as the destination this call.
    {
        tact_fixture fx;
        g_log.reset();
        fx.teleport_zones[3].mode        = 2;
        fx.teleport_zones[3].field_03    = 0;
        fx.teleport_zones[3].dest_col[0] = 20;
        fx.teleport_zones[3].dest_row[0] = 20;
        fx.teleport_zones[3].dest_col[1] = 21;
        fx.teleport_zones[3].dest_row[1] = 22;
        fx.units[UNIT_IDX].pos_col       = 5;
        fx.units[UNIT_IDX].pos_row       = 5;
        tact_store own                   = fx.store();
        auto       planes                = own.planes();
        detail::unit_teleport(own, planes, rec_calls(), 3, UNIT_IDX);
        ck_eq((uint32_t)fx.teleport_zones[3].field_03, 1u,
              "T10: field_03 advances to 1, 0x00432f36");
        ck_eq((uint32_t)fx.units[UNIT_IDX].pos_col, 21u, "T10: used dest_col[1], not dest_col[0]");
        ck_eq((uint32_t)fx.units[UNIT_IDX].pos_row, 22u, "T10: used dest_row[1]");
    }

    // T11: mode==2 (SEQUENCE) resets field_03 to 0 when the next slot is EMPTY (both zero),
    // 0x00432f28 -- confirms T10 is not simply "always advance".
    {
        tact_fixture fx;
        g_log.reset();
        fx.teleport_zones[3].mode        = 2;
        fx.teleport_zones[3].field_03    = 0;
        fx.teleport_zones[3].dest_col[0] = 20;
        fx.teleport_zones[3].dest_row[0] = 20;
        // dest_col[1]/dest_row[1] left at the fixture's zero default -- slot 1 is empty.
        fx.units[UNIT_IDX].pos_col = 5;
        fx.units[UNIT_IDX].pos_row = 5;
        tact_store own             = fx.store();
        auto       planes          = own.planes();
        detail::unit_teleport(own, planes, rec_calls(), 3, UNIT_IDX);
        ck_eq((uint32_t)fx.teleport_zones[3].field_03, 0u,
              "T11: empty next slot resets field_03 to 0, 0x00432f28");
        ck_eq((uint32_t)fx.units[UNIT_IDX].pos_col, 20u,
              "T11: used dest_col[0] (the reset slot), not dest_col[1]");
    }

    // T12: mode==1 (RANDOM) burns exactly `llm_rand()/4096` steps through non-empty slots, wrapping
    // at 8. rand_value chosen so rand/4096 == 2: starting slot 0 (populated), advances to slot 1
    // (populated) consuming budget 1, then slot 2 (populated) consuming budget 2, stops there.
    {
        tact_fixture fx;
        g_log.reset();
        g_log.rand_value                 = 2 * 4096 + 100; // /4096 == 2 (truncating)
        fx.teleport_zones[3].mode        = 1;
        fx.teleport_zones[3].dest_col[0] = 1;
        fx.teleport_zones[3].dest_row[0] = 1;
        fx.teleport_zones[3].dest_col[1] = 2;
        fx.teleport_zones[3].dest_row[1] = 2;
        fx.teleport_zones[3].dest_col[2] = 33;
        fx.teleport_zones[3].dest_row[2] = 34;
        fx.units[UNIT_IDX].pos_col       = 5;
        fx.units[UNIT_IDX].pos_row       = 5;
        tact_store own                   = fx.store();
        auto       planes                = own.planes();
        detail::unit_teleport(own, planes, rec_calls(), 3, UNIT_IDX);
        ck_eq((uint32_t)fx.units[UNIT_IDX].pos_col, 33u,
              "T12: RANDOM scan lands on slot 2 for rand/4096==2, 0x00432fa3");
        ck_eq((uint32_t)fx.units[UNIT_IDX].pos_row, 34u, "T12: slot 2's row");
    }

    // T12b: mode==1 (RANDOM) post-loop validity check resets slot ONLY when BOTH dest_col AND
    // dest_row are zero -- NOT when either one alone is zero, 0x00432fc1-0x00432fe9. A slot with
    // col==0 (a legitimate map-edge column) and a nonzero row is a REAL destination, not empty.
    // Caught by adversarial review (reimpl-verify, 2026-08-26): the first draft used the inverted
    // `!(col!=0 && row!=0)` (reset if EITHER is zero) instead of `col==0 && row==0` (reset only if
    // BOTH are zero) -- the two agree whenever neither field is zero, which is why T12 alone did not
    // catch it.
    {
        tact_fixture fx;
        g_log.reset();
        g_log.rand_value                 = 3 * 4096 + 100; // /4096 == 3 (truncating)
        fx.teleport_zones[3].mode        = 1;
        fx.teleport_zones[3].dest_col[0] = 10;
        fx.teleport_zones[3].dest_row[0] = 10;
        fx.teleport_zones[3].dest_col[1] = 11;
        fx.teleport_zones[3].dest_row[1] = 11;
        fx.teleport_zones[3].dest_col[2] = 12;
        fx.teleport_zones[3].dest_row[2] = 12;
        fx.teleport_zones[3].dest_col[3] = 0; // column 0 -- a real map-edge coordinate, not "empty"
        fx.teleport_zones[3].dest_row[3] = 20;
        fx.units[UNIT_IDX].pos_col       = 5;
        fx.units[UNIT_IDX].pos_row       = 5;
        tact_store own                   = fx.store();
        auto       planes                = own.planes();
        detail::unit_teleport(own, planes, rec_calls(), 3, UNIT_IDX);
        ck_eq((uint32_t)fx.units[UNIT_IDX].pos_col, 0u,
              "T12b: slot 3 kept (col==0 alone is not the empty sentinel), 0x00432feb");
        ck_eq((uint32_t)fx.units[UNIT_IDX].pos_row, 20u, "T12b: slot 3's row used, not reset to slot 0");
    }

    // T13: LINKED (association[0] != 0), mode==0 -- looks up the zone whose `.id` matches
    // association[0] and moves to ITS start_col/start_row, then stamps that zone's field_28,
    // 0x004332a3-0x00433416.
    {
        tact_fixture fx;
        g_log.reset();
        fx.teleport_zones[3].association[0] = 77;
        fx.teleport_zones[9].id             = 77;
        fx.teleport_zones[9].start_col      = 88;
        fx.teleport_zones[9].start_row      = 44;
        fx.units[UNIT_IDX].pos_col          = 5;
        fx.units[UNIT_IDX].pos_row          = 5;
        tact_store own                      = fx.store();
        auto       planes                   = own.planes();
        detail::unit_teleport(own, planes, rec_calls(), 3, UNIT_IDX);
        ck_eq((uint32_t)fx.units[UNIT_IDX].pos_col, 88u, "T13: moved to the linked zone's start_col");
        ck_eq((uint32_t)fx.units[UNIT_IDX].pos_row, 44u, "T13: moved to the linked zone's start_row");
        ck_eq((uint32_t)fx.teleport_zones[9].field_28, 1u,
              "T13: destination zone's field_28 stamped 1, 0x0043340f");
        ck_eq((uint32_t)fx.teleport_zones[3].field_28, 0u,
              "T13: the SOURCE zone's field_28 is untouched (only the destination's is stamped)");
    }

    // T14: LINKED, no zone in 1..0x3f has a matching `.id` -- destroy + death-fx fallback,
    // 0x0043341d, same shape as T5's direct-path fallback.
    {
        tact_fixture fx;
        g_log.reset();
        fx.teleport_zones[3].association[0] = 99; // no zone anywhere has id==99
        fx.teleport_zones[3].death          = 3;
        fx.units[UNIT_IDX].pos_col          = 1;
        fx.units[UNIT_IDX].pos_row          = 2;
        tact_store own                      = fx.store();
        auto       planes                   = own.planes();
        detail::unit_teleport(own, planes, rec_calls(), 3, UNIT_IDX);
        ck_eq((uint32_t)g_log.destroy_calls.size(), 1u, "T14: no match -- unit_destroy fires");
        ck_eq((uint32_t)g_log.fx_calls.size(), 1u, "T14: no match -- fx_spawn fires");
        if (g_log.fx_calls.size() == 1)
            ck_eq((uint32_t)std::get<0>(g_log.fx_calls[0]), 3u, "T14: fx_type == SOURCE zone.death");
    }

    // T15: LINKED, the matched zone's field_28 is already flagged -- silent refusal, 0x004332e5 JA.
    // No destroy/fx fallback either: this is a THIRD outcome distinct from T13 and T14.
    {
        tact_fixture fx;
        g_log.reset();
        fx.teleport_zones[3].association[0] = 55;
        fx.teleport_zones[7].id             = 55;
        fx.teleport_zones[7].field_28       = 1; // already consumed
        tact_store own                      = fx.store();
        auto       planes                   = own.planes();
        detail::unit_teleport(own, planes, rec_calls(), 3, UNIT_IDX);
        ck_eq((uint32_t)(g_log.destroy_calls.size() + g_log.fx_calls.size() +
                         g_log.vision_remove_calls.size()),
              0u, "T15: matched zone already field_28-flagged -- silent refusal, no calls at all");
    }
}

} // namespace mh::tact::test
