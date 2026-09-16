//
// tact_teleport_cmdqueue_jump_selftest.cpp -- offline oracle for
//   llm_tact_teleport_cmdqueue_jump @0x004334da (libmh/tact/tact_teleport_cmdqueue_jump.cpp)
//
// WHY OFFLINE, NOT RIG: this function's own measured write closure reaches
// the SAME two TACT-CUT2 SHARED callees its caller llm_tact_unit_cmd_teleport_jump_tick's oracle
// documents -- llm_tact_fx_play_sound (real DirectSound playback) and
// llm_tact_ui_sidebar_roster_refresh (blits into the live UI surface), both ungated (the open,
// non-autonomous TACT-CUT2 item), reached via this function's own call into llm_tact_unit_teleport.
// So the one outward call is mocked via the file's own `teleport_cmdqueue_jump_calls` table --
// proving the scratch-slot write and the passability gate without ever executing the real teleport
// or anything beneath it.
//
#include "tact/tact_teleport_cmdqueue_jump.h"

#include "tact_test_support.h"

namespace mh::tact::test {

namespace {
using namespace mh::tact;

constexpr int32_t UNIT_ID = 7;
constexpr int32_t DEST_X  = 11;
constexpr int32_t DEST_Y  = 22;

struct teleport_recorder {
    std::vector<std::tuple<int32_t, int32_t>> teleport_calls; // (teleport_id, building_id)
    void                                      reset() { *this = teleport_recorder{}; }
};
teleport_recorder g_rec;

const teleport_cmdqueue_jump_calls &rec_calls() {
    static const teleport_cmdqueue_jump_calls c = {
        [](int32_t teleport_id, int32_t building_id) {
            g_rec.teleport_calls.push_back({teleport_id, building_id});
        },
    };
    return c;
}

} // namespace

void run_teleport_cmdqueue_jump_tests() {
    // T1: the scratch zone (TACT_TELEPORT_SCRATCH_SLOT == 65) is written UNCONDITIONALLY, before the
    // passability check -- 0x004334f9-0x0043351c. Checked on the BLOCKED path (building != 0) so a
    // translation that only writes the zone inside the passable branch is caught.
    {
        tact_fixture fx;
        g_rec.reset();
        fx.planes().tile_object_at(DEST_X, DEST_Y).building = 1; // blocked
        tact_store own                                      = fx.store();
        int32_t    result                                   = detail::teleport_cmdqueue_jump(own, fx.planes(), rec_calls(), UNIT_ID,
                                                                                             DEST_X, DEST_Y);
        ck_eq((uint32_t)result, 0u, "T1: building != 0 refuses, returns 0, 0x0043355c");
        ck_eq((uint32_t)g_rec.teleport_calls.size(), 0u,
              "T1: llm_tact_unit_teleport NOT called on the blocked path");
        const teleport_zone &z = own.teleport_zone_at(TACT_TELEPORT_SCRATCH_SLOT);
        ck_eq((uint32_t)z.id, (uint32_t)TACT_TELEPORT_SCRATCH_SLOT,
              "T1: zone.id == 0x41 even on the blocked path, 0x004334f9");
        ck_eq((uint32_t)z.mode, 0u, "T1: zone.mode == 0, 0x00433500");
        ck_eq((uint32_t)z.no_enemy, 0u, "T1: zone.no_enemy == 0, 0x00433507");
        ck_eq((uint32_t)z.field_28, 0u, "T1: zone.field_28 == 0, 0x0043350e");
        ck_eq((uint32_t)z.association[0], 0u, "T1: zone.association[0] == 0, 0x00433515");
        ck_eq((uint32_t)z.dest_col[0], (uint32_t)DEST_X, "T1: zone.dest_col[0] == dest_x, 0x0043351f");
        ck_eq((uint32_t)z.dest_row[0], (uint32_t)DEST_Y, "T1: zone.dest_row[0] == dest_y, 0x00433528");
    }

    // T2: destination tile has NO building -> real teleport fires with (teleport_id=0x41,
    // building_id=unit_id) -- the literal EAX=0x41 argument, NOT the scratch slot's OWN id field
    // re-read, and unit_id forwarded unchanged in EDX -- 0x00433546-0x0043354e.
    {
        tact_fixture fx;
        g_rec.reset();
        fx.planes().tile_object_at(DEST_X, DEST_Y).building = 0; // clear
        tact_store own                                      = fx.store();
        int32_t    result                                   = detail::teleport_cmdqueue_jump(own, fx.planes(), rec_calls(), UNIT_ID,
                                                                                             DEST_X, DEST_Y);
        ck_eq((uint32_t)result, 1u, "T2: building == 0 succeeds, returns 1, 0x00433553");
        ck_eq((uint32_t)g_rec.teleport_calls.size(), 1u, "T2: llm_tact_unit_teleport fires exactly once");
        if (g_rec.teleport_calls.size() == 1) {
            ck_eq((uint32_t)std::get<0>(g_rec.teleport_calls[0]), (uint32_t)TACT_TELEPORT_SCRATCH_SLOT,
                  "T2: teleport_id == literal 0x41, 0x00433549");
            ck_eq((uint32_t)std::get<1>(g_rec.teleport_calls[0]), (uint32_t)UNIT_ID,
                  "T2: building_id == unit_id forwarded unchanged, 0x00433546");
        }
    }

    // T3: a DIFFERENT (dest_x, dest_y) pair than T1/T2's, checking the byte-offset tile lookup
    // (dest_x<<11 + dest_y<<3, 0x0043352e-0x0043353c) against the SAME tile the passability check
    // reads -- a translation using the wrong stride would pass T1/T2 (both use the same tile) but
    // disagree with the fixture's own tile_object_at() indexing here.
    {
        tact_fixture fx;
        g_rec.reset();
        constexpr int32_t OTHER_X                             = 3;
        constexpr int32_t OTHER_Y                             = 5;
        fx.planes().tile_object_at(OTHER_X, OTHER_Y).building = 0;
        tact_store own                                        = fx.store();
        int32_t    result                                     = detail::teleport_cmdqueue_jump(own, fx.planes(), rec_calls(), UNIT_ID,
                                                                                               OTHER_X, OTHER_Y);
        ck_eq((uint32_t)result, 1u, "T3: distinct (x,y) tile lookup matches tile_object_at's own indexing");
    }
}

} // namespace mh::tact::test
