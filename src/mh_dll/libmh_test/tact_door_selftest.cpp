//
// tact_door_selftest.cpp -- offline oracle for the six llm_tact_door_* functions (TACT1D,
// 2026-08-27). See tact/tact_door.h for the derivation.
//
// WHY OFFLINE: door_update_tile_state/door_apply_to_map are REVIEW-flagged (writes_shared:
// tile_objects, _G_LLM_TILE_VIS_MAP) and the domain's own manifest classifies them proof:OFFLINE;
// door_path_clear has ZERO writes (not shadowable by construction); door_tick/door_anim_start/
// door_parse_definition have not had their region closures freshly re-derived (see
// the shadow manifest's why_extra for each). All six are proven here.
//
// `mission_parse_int_token` is used FOR REAL (mh::tact::mission_parse_int_token, already
// verified/T2 in TACT1A) rather than mocked -- it is a pure parser with no outward effects of its
// own, so routing through it exercises the real integration rather than a second guess at its
// behavior. Only the genuinely side-effecting/would-terminate calls (time_now, fatal_cleanup,
// utils_abort) are mocked.
//
#include "tact/tact_door.h"
#include "tact/tact_mission_parse.h"
#include "tact_test_support.h"

namespace mh::tact::test {

namespace {

struct call_log {
    double  now            = 0.0;
    bool    cleanup_called = false;
    bool    abort_called   = false;
    int32_t abort_status   = 0x5a5a5a5a;
};

call_log &log() {
    static call_log l;
    return l;
}

void reset_log() { log() = call_log{}; }

double mock_time_now() { return log().now; }
void   mock_fatal_cleanup() { log().cleanup_called = true; }
void   mock_utils_abort(int32_t status) {
    log().abort_called = true;
    log().abort_status = status;
}

// mission_parse_int_token is a PURE parser (no side effects of its own beyond a fatal path on
// malformed input), but its PUBLIC wrapper (mh::tact::mission_parse_int_token) binds through
// state()'s LIVE globals -- wrong for an offline test. This adapter goes through detail:: with a
// throwaway mission_parse_calls instead, so a malformed-token fatal (never exercised by the
// well-formed inputs below, but a real possibility) cannot reach a live game-side function pointer.
const mission_parse_calls &int_token_calls() {
    static const mission_parse_calls c = {[]() {}, [](int32_t) {}, [](double x) { return x; }};
    return c;
}
int32_t mock_int_token(char *line, uint32_t *io_pos, int32_t *out_value) {
    return mh::tact::detail::mission_parse_int_token(int_token_calls(), line, io_pos, out_value);
}

door_calls mock_calls() {
    return {mock_time_now, mock_fatal_cleanup, mock_utils_abort, mock_int_token};
}

// ---- whole-table region assert (TACT1D acceptance clause: "Door state is asserted over the WHOLE
// _G_LLM_TACT_DOOR_TABLE region, not one entry") ----------------------------------------------
//
// Every case above asserts a single slot (own.door_table_at(3), fx.door_table[3], ...). A
// translation that writes the right fields into the right slot but ALSO corrupts a NEIGHBOURING
// slot -- wrong stride, an off-by-one base, a loop that runs one entry too far -- passes every one
// of them. This walks the fixture's REAL 32-slot extent (the door_table sized in
// tact_test_support.h to cover exactly what door_tick/door_apply_to_map's literal 0x20 loop bound
// reaches: `for (i=1; i<0x20; ++i)`, indices 1..31 inclusive -- see tact_door.h's header comment
// and tact_test_support.h's own door_table derivation) field-by-field against an expected image,
// naming the SLOT and the FIELD for every mismatch (mirroring tact_fov_raycast_selftest.cpp's
// assert_stencil_exact: loud per-mismatch, one summary ck_eq).
constexpr int32_t DOOR_REGION_SLOTS = 32;

int32_t door_field_mismatch(bool eq, int32_t slot, const char *field, const char *what) {
    if (eq) return 0;
    printf("  T-REGION: slot %d %s, .%s\n", slot, what, field);
    fflush(stdout);
    return 1;
}

void assert_door_region(tact_store &own, const std::vector<door_record> &expected, const char *what) {
    int32_t mismatches = 0;
    for (int32_t i = 0; i < DOOR_REGION_SLOTS; ++i) {
        const door_record &got  = own.door_table_at(i);
        const door_record &want = expected[(size_t)i];

        mismatches += door_field_mismatch(got.id == want.id, i, "id", what);
        mismatches += door_field_mismatch(got.direct_mode == want.direct_mode, i, "direct_mode", what);
        mismatches += door_field_mismatch(got.state == want.state, i, "state", what);
        mismatches += door_field_mismatch(got.frame_index == want.frame_index, i, "frame_index", what);
        mismatches += door_field_mismatch(got.tile_x == want.tile_x, i, "tile_x", what);
        mismatches += door_field_mismatch(got.tile_y == want.tile_y, i, "tile_y", what);
        mismatches += door_field_mismatch(got.right_frame_count == want.right_frame_count, i,
                                          "right_frame_count", what);
        mismatches += door_field_mismatch(got.right_col_count == want.right_col_count, i,
                                          "right_col_count", what);
        mismatches += door_field_mismatch(got.left_frame_count == want.left_frame_count, i,
                                          "left_frame_count", what);
        mismatches += door_field_mismatch(got.left_col_count == want.left_col_count, i,
                                          "left_col_count", what);
        mismatches += door_field_mismatch(got.speed == want.speed, i, "speed", what);
        mismatches += door_field_mismatch(got.last_step_time == want.last_step_time, i,
                                          "last_step_time", what);
        mismatches += door_field_mismatch(got.opened_at_time == want.opened_at_time, i,
                                          "opened_at_time", what);
        for (int32_t r = 0; r < 16; ++r) {
            char right_field[24];
            snprintf(right_field, sizeof(right_field), "right_rows[%d]", r);
            mismatches += door_field_mismatch(got.right_rows[(size_t)r] == want.right_rows[(size_t)r],
                                              i, right_field, what);
            char left_field[24];
            snprintf(left_field, sizeof(left_field), "left_rows[%d]", r);
            mismatches += door_field_mismatch(got.left_rows[(size_t)r] == want.left_rows[(size_t)r], i,
                                              left_field, what);
        }
    }
    ck_eq((uint32_t)mismatches, 0u, what);
}

// Fills every fixture door slot with fully DISTINCT, non-symmetric field values (a function of the
// slot index alone) -- tact_test_support.h's own fixture-rules banner: two slots holding the same
// number make an off-by-one/stride bug pass silently. `state` is left 0 (idle/inert -- door_tick's
// own id==0||state==0 skip, 0x004322f7) so a case can drive exactly the slot(s) it names active;
// every OTHER field stays a distinct nonzero/non-repeating value per slot so a stride bug that
// lands on the wrong slot's DATA (not just control flow) is also observable. Leaf counts are
// zeroed so the region-assert cases below stay scoped to _G_LLM_TACT_DOOR_TABLE itself (no
// incidental door_update_tile_state leaf-walk into tile_objects to reason about).
void seed_distinct_door_table(std::vector<door_record> &table) {
    for (size_t i = 0; i < table.size(); ++i) {
        door_record &d      = table[i];
        d                   = door_record{};
        const int32_t si    = (int32_t)i;
        d.id                = (uint8_t)(si * 5 + 11); // distinct, nonzero for every i in [0,31]
        d.direct_mode       = (uint8_t)(si % 3);
        d.state             = 0; // caller overrides for the driven slot(s)
        d.frame_index       = (uint8_t)(si * 3 + 1);
        d.tile_x            = (uint16_t)(si * 17 + 200);
        d.tile_y            = (uint16_t)(si * 23 + 300);
        d.right_frame_count = (uint16_t)(si * 7 + 400);
        d.right_col_count   = 0; // no leaf walk by default
        d.left_frame_count  = (uint16_t)(si * 13 + 500);
        d.left_col_count    = 0; // no leaf walk by default
        d.speed             = 1000.0 + si * 1.5;
        d.last_step_time    = 2000.0 + si * 2.5;
        d.opened_at_time    = 3000.0 + si * 3.5;
        for (int32_t r = 0; r < 16; ++r) {
            d.right_rows[r] = (uint16_t)(si * 100 + r * 2 + 1);
            d.left_rows[r]  = (uint16_t)(si * 100 + r * 2 + 2);
        }
    }
}

} // namespace

void run_door_tests() {
    // ================= door_tick =================

    // T1: state==1 (opening), deadline NOT yet elapsed -- no advance, no write.
    {
        tact_fixture fx;
        door_record &d      = fx.door_table[3];
        d.id                = 3;
        d.state             = 1;
        d.frame_index       = 1;
        d.right_frame_count = 5;
        d.speed             = 10.0;
        d.last_step_time    = 100.0;
        d.right_col_count   = 0; // no leaf writes to worry about in this case
        d.left_col_count    = 0;

        tact_store             own    = fx.store();
        mh::state::mode_planes planes = fx.planes();
        reset_log();
        log().now = 105.0; // 100+10=110, not yet elapsed
        detail::door_tick(own, planes, fx.view(), mock_calls());

        ck_eq(own.door_table_at(3).frame_index, 1u, "T1: not yet -> no frame advance, 0x0043236f");
        ck_eq(own.door_table_at(3).state, 1u, "T1: still opening");
    }

    // T2: state==1, deadline elapsed, frame_index reaches right_frame_count -> fully open (state 2).
    {
        tact_fixture fx;
        door_record &d      = fx.door_table[3];
        d.id                = 3;
        d.state             = 1;
        d.frame_index       = 4;
        d.right_frame_count = 4; // ALREADY equal -- the "fully open" branch, not the advance branch
        d.speed             = 10.0;
        d.last_step_time    = 100.0;
        d.right_col_count   = 0;
        d.left_col_count    = 0;

        tact_store             own    = fx.store();
        mh::state::mode_planes planes = fx.planes();
        reset_log();
        log().now = 999.0;
        detail::door_tick(own, planes, fx.view(), mock_calls());

        ck_eq(own.door_table_at(3).state, 2u, "T2: frame_index==right_frame_count -> state 2, 0x00432341");
        ck_eq_d(own.door_table_at(3).opened_at_time, 999.0, "T2: opened_at_time stamped, 0x00432351");
    }

    // T3: state==1, deadline elapsed, frame_index < right_frame_count -- advance + re-stamp
    // last_step_time as (speed + last_step_time), not `now`.
    {
        tact_fixture fx;
        door_record &d      = fx.door_table[3];
        d.id                = 3;
        d.state             = 1;
        d.frame_index       = 1;
        d.right_frame_count = 5;
        d.speed             = 10.0;
        d.last_step_time    = 100.0;
        d.right_col_count   = 0;
        d.left_col_count    = 0;

        tact_store             own    = fx.store();
        mh::state::mode_planes planes = fx.planes();
        reset_log();
        log().now = 111.0; // > 100+10
        detail::door_tick(own, planes, fx.view(), mock_calls());

        ck_eq(own.door_table_at(3).frame_index, 2u, "T3: frame_index++, 0x00432389");
        ck_eq_d(own.door_table_at(3).last_step_time, 110.0,
                "T3: last_step_time = speed+last_step_time, NOT now, 0x004323ab");
    }

    // T4: state==2 (open/holding), hold time elapsed -- SAME-TICK fallthrough into state==3's
    // closing body (door_path_clear called, blocked in this case so only last_step_time changes).
    {
        tact_fixture fx;
        door_record &d                              = fx.door_table[3];
        d.id                                        = 3;
        d.state                                     = 2;
        d.opened_at_time                            = 100.0; // + door_open_hold_time_sec(2.0) = 102.0
        d.right_col_count                           = 1;
        d.left_col_count                            = 0;
        d.direct_mode                               = 2;
        d.tile_x                                    = 60;
        d.tile_y                                    = 60;
        fx.planes().tile_object_at(60, 60).building = 7; // occupied -- door_path_clear must return 0

        tact_store             own    = fx.store();
        mh::state::mode_planes planes = fx.planes();
        reset_log();
        log().now = 200.0; // well past 102.0
        detail::door_tick(own, planes, fx.view(), mock_calls());

        ck_eq(own.door_table_at(3).state, 3u, "T4: hold expired -> state 3, 0x004323e7 (same tick)");
        ck_eq_d(own.door_table_at(3).last_step_time, 200.0,
                "T4: blocked close -> last_step_time = now only, 0x004324a2");
    }

    // T5: state==3 (closing), path IS clear, frame_index==1 -> fully closed (state 0, frame_index 0).
    {
        tact_fixture fx;
        door_record &d                              = fx.door_table[3];
        d.id                                        = 3;
        d.state                                     = 3;
        d.frame_index                               = 1;
        d.right_col_count                           = 1;
        d.left_col_count                            = 0;
        d.direct_mode                               = 2;
        d.tile_x                                    = 60;
        d.tile_y                                    = 60;
        fx.planes().tile_object_at(60, 60).building = 0; // clear

        tact_store             own    = fx.store();
        mh::state::mode_planes planes = fx.planes();
        reset_log();
        detail::door_tick(own, planes, fx.view(), mock_calls());

        ck_eq(own.door_table_at(3).state, 0u, "T5: fully closed -> state 0, 0x00432442");
        ck_eq(own.door_table_at(3).frame_index, 0u, "T5: frame_index reset, 0x00432437");
    }

    // T6: an inactive slot (id==0) is skipped entirely -- no field of it is touched.
    {
        tact_fixture fx;
        door_record &d      = fx.door_table[5];
        d                   = door_record{}; // id=0
        d.state             = 1;             // would advance if the id==0 skip did not fire
        d.frame_index       = 1;
        d.right_frame_count = 99;

        tact_store             own    = fx.store();
        mh::state::mode_planes planes = fx.planes();
        reset_log();
        log().now = 99999.0;
        detail::door_tick(own, planes, fx.view(), mock_calls());

        ck_eq(own.door_table_at(5).frame_index, 1u, "T6: id==0 -> untouched, 0x004322f7");
    }

    // T7: slot 31 (the innermost bound of the preserved 16-vs-31 overrun, uncertainties[0] in the
    // translation report) is REACHED by the loop -- pins the literal 0x20 bound rather than 16.
    {
        tact_fixture fx;
        door_record &d      = fx.door_table[31];
        d.id                = 31;
        d.state             = 1;
        d.frame_index       = 1;
        d.right_frame_count = 5;
        d.speed             = 10.0;
        d.last_step_time    = 0.0;
        d.right_col_count   = 0;
        d.left_col_count    = 0;

        tact_store             own    = fx.store();
        mh::state::mode_planes planes = fx.planes();
        reset_log();
        log().now = 999.0;
        detail::door_tick(own, planes, fx.view(), mock_calls());

        ck_eq(own.door_table_at(31).frame_index, 2u,
              "T7: slot 31 IS reached (loop bound is 0x20, not 16), 0x004322d9");
    }

    // ================= door_anim_start =================

    // T8: fires from fully-idle.
    {
        tact_fixture fx;
        door_record &d = fx.door_table[2];
        d.frame_index  = 0;
        d.state        = 0;

        tact_store own = fx.store();
        reset_log();
        log().now = 42.0;
        detail::door_anim_start(own, mock_calls(), 2);

        ck_eq(own.door_table_at(2).state, 1u, "T8: idle -> opening, 0x004322a1");
        ck_eq_d(own.door_table_at(2).last_step_time, 42.0, "T8: last_step_time stamped, 0x004322b1");
    }

    // T9: refuses when frame_index != 0, even with state==0.
    {
        tact_fixture fx;
        door_record &d = fx.door_table[2];
        d.frame_index  = 3;
        d.state        = 0;

        tact_store own = fx.store();
        reset_log();
        detail::door_anim_start(own, mock_calls(), 2);

        ck_eq(own.door_table_at(2).state, 0u, "T9: frame_index!=0 -> refused, 0x00432285");
    }

    // T10: refuses when state != 0, even with frame_index==0.
    {
        tact_fixture fx;
        door_record &d = fx.door_table[2];
        d.frame_index  = 0;
        d.state        = 2;

        tact_store own = fx.store();
        reset_log();
        detail::door_anim_start(own, mock_calls(), 2);

        ck_eq(own.door_table_at(2).state, 2u, "T10: state!=0 -> refused, 0x00432292");
    }

    // ================= door_update_tile_state =================

    // T11: CLEAR branch (row_value==0) zeroes class_owner at (col,row) AND its flat-predecessor.
    {
        tact_fixture fx;
        door_record &d                                 = fx.door_table[4];
        d.id                                           = 4;
        d.tile_x                                       = 60;
        d.tile_y                                       = 60;
        d.direct_mode                                  = 2; // Normal: col+1 per step, row fixed
        d.frame_index                                  = 1;
        d.right_col_count                              = 1;
        d.right_rows[0]                                = 0; // CLEAR
        d.left_col_count                               = 0;
        fx.planes().tile_object_at(60, 60).class_owner = 9; // poisoned nonzero
        fx.planes().tile_object_at(60, 59).class_owner = 9; // the flat-predecessor (row-1)

        tact_store             own    = fx.store();
        mh::state::mode_planes planes = fx.planes();
        detail::door_update_tile_state(own, planes, fx.view(), 4);

        ck_eq(planes.tile_object_at(60, 60).class_owner, 0u, "T11: (col,row).class_owner cleared, 0x0043257f");
        ck_eq(planes.tile_object_at(60, 59).class_owner, 0u,
              "T11: flat-predecessor (row-1) ALSO cleared -- the -2-byte bias twin, 0x00432659");
    }

    // T12: MARK branch (row_value!=0) stamps class_owner=door_idx at both tiles.
    {
        tact_fixture fx;
        door_record &d    = fx.door_table[4];
        d.id              = 4;
        d.tile_x          = 60;
        d.tile_y          = 60;
        d.direct_mode     = 2;
        d.frame_index     = 1;
        d.right_col_count = 1;
        // row_value=5: chain_val(0)==k(0) so the loop body runs once (height_row[0]=5), then
        // frame_val = 5-20 = -15 < 0 breaks BEFORE any sprite-bank byte is ever read -- so this
        // case needs no sprite-bank mock data at all.
        d.right_rows[0]  = 5; // MARK
        d.left_col_count = 0;

        tact_store             own    = fx.store();
        mh::state::mode_planes planes = fx.planes();
        detail::door_update_tile_state(own, planes, fx.view(), 4);

        ck_eq(planes.tile_object_at(60, 60).class_owner, 4u, "T12: (col,row).class_owner = door_idx, 0x00432642");
        ck_eq(planes.tile_object_at(60, 59).class_owner, 4u,
              "T12: flat-predecessor ALSO stamped door_idx, 0x00432659");
    }

    // ================= door_path_clear =================

    // T13: nothing on the footprint -- returns 1.
    {
        tact_fixture fx;
        door_record &d                              = fx.door_table[6];
        d.tile_x                                    = 70;
        d.tile_y                                    = 70;
        d.direct_mode                               = 2;
        d.right_col_count                           = 2;
        d.left_col_count                            = 0;
        fx.planes().tile_object_at(70, 70).building = 0;
        fx.planes().tile_object_at(71, 70).building = 0;
        fx.planes().tile_object_at(70, 69).building = 0; // the -1-row bias-read neighbours
        fx.planes().tile_object_at(71, 69).building = 0;

        tact_store    own = fx.store();
        const int32_t r   = detail::door_path_clear(own, fx.view(), 6);
        ck_eq((uint32_t)r, 1u, "T13: nothing on the footprint -> clear, 0x00432b8d");
    }

    // T14: a unit at the SECOND right-leaf tile blocks -- returns 0 (cascade stops at first hit).
    {
        tact_fixture fx;
        door_record &d                              = fx.door_table[6];
        d.tile_x                                    = 70;
        d.tile_y                                    = 70;
        d.direct_mode                               = 2;
        d.right_col_count                           = 2;
        d.left_col_count                            = 0;
        fx.planes().tile_object_at(70, 70).building = 0;
        fx.planes().tile_object_at(70, 69).building = 0;
        fx.planes().tile_object_at(71, 70).building = 9; // occupied
        fx.planes().tile_object_at(71, 69).building = 0;

        tact_store    own = fx.store();
        const int32_t r   = detail::door_path_clear(own, fx.view(), 6);
        ck_eq((uint32_t)r, 0u, "T14: blocked at the direct tile -> not clear, 0x00432a76");
    }

    // T15: the flat-predecessor bias read (NOT the direct tile) is what blocks.
    {
        tact_fixture fx;
        door_record &d                              = fx.door_table[6];
        d.tile_x                                    = 70;
        d.tile_y                                    = 70;
        d.direct_mode                               = 2;
        d.right_col_count                           = 1;
        d.left_col_count                            = 0;
        fx.planes().tile_object_at(70, 70).building = 0;
        fx.planes().tile_object_at(70, 69).building = 9; // the -6-byte bias neighbour, occupied

        tact_store    own = fx.store();
        const int32_t r   = detail::door_path_clear(own, fx.view(), 6);
        ck_eq((uint32_t)r, 0u, "T15: blocked via the -6-byte bias read, not the direct tile, 0x00432a8e");
    }

    // ================= door_apply_to_map =================

    // T16: self-consistency skip -- a slot whose .id does not match its own index is never walked.
    {
        tact_fixture fx;
        for (auto &d : fx.door_table) d = door_record{};
        door_record &d                                 = fx.door_table[8];
        d.id                                           = 99; // MISMATCHED -- must be skipped
        d.tile_x                                       = 80;
        d.tile_y                                       = 80;
        d.direct_mode                                  = 2;
        d.right_col_count                              = 1;
        d.right_rows[0]                                = 5;
        d.left_col_count                               = 0;
        fx.planes().tile_object_at(80, 80).class_owner = 0;

        tact_store             own    = fx.store();
        mh::state::mode_planes planes = fx.planes();
        detail::door_apply_to_map(own, planes, fx.view());

        ck_eq(planes.tile_object_at(80, 80).class_owner, 0u,
              "T16: id != slot index -> skipped entirely, 0x00439d2b-0x00439d35");
    }

    // T17: a self-consistent slot ALWAYS marks (row 0, not frame_index-selected -- frame_index is
    // never read by this function at all).
    {
        tact_fixture fx;
        for (auto &d : fx.door_table) d = door_record{};
        door_record &d    = fx.door_table[8];
        d.id              = 8;
        d.frame_index     = 200; // deliberately absurd -- must have NO effect (unlike update_tile_state)
        d.tile_x          = 80;
        d.tile_y          = 80;
        d.direct_mode     = 2;
        d.right_col_count = 1;
        d.right_rows[0]   = 5; // row 0 -- what apply_to_map actually reads
        d.left_col_count  = 0;

        tact_store             own    = fx.store();
        mh::state::mode_planes planes = fx.planes();
        detail::door_apply_to_map(own, planes, fx.view());

        ck_eq(planes.tile_object_at(80, 80).class_owner, 8u,
              "T17: self-consistent slot ALWAYS marks via rows[0], frame_index ignored, 0x00439d86");
    }

    // T22: reimpl-verify (2026-08-27) divergence -- door_apply_to_map must NEVER touch
    // _G_LLM_TILE_VIS_MAP or read map_cam_col/row, unlike door_update_tile_state's structurally
    // similar mark loop. Poison the camera + the vis-map cell the mark would stamp under the
    // door_update_tile_state formula, and confirm door_apply_to_map leaves both untouched.
    {
        tact_fixture fx;
        for (auto &d : fx.door_table) d = door_record{};
        door_record &d    = fx.door_table[9];
        d.id              = 9;
        d.tile_x          = 60;
        d.tile_y          = 60;
        d.direct_mode     = 2;
        d.right_col_count = 1;
        d.right_rows[0]   = 5; // MARK
        d.left_col_count  = 0;
        // RID_TILE_VIS_MAP's real extent is 300 B (tact_fixture::tile_vis_map) -- camera placed so
        // the door's screen-space cell is small and in-bounds for BOTH the vis-map buffer and the
        // door_update_tile_state on-screen check, not just "large enough to look plausible".
        fx.map_cam_col  = 55;
        fx.map_cam_row  = 55;
        fx.view_tiles_w = 20; // wide enough that (60,60) would be ON-SCREEN if this were update_tile_state
        fx.view_tiles_h = 20;

        tact_store             own       = fx.store();
        mh::state::mode_planes planes    = fx.planes();
        const int32_t          vis_index = 5 * fx.view_tiles_w + 5; // door_update_tile_state's own stamp formula (60-55=5)
        own.tile_vis_map_at(vis_index)   = 0xaa;                    // poison -- must survive unchanged
        detail::door_apply_to_map(own, planes, fx.view());

        ck_eq(planes.tile_object_at(60, 60).class_owner, 9u, "T22: still marks class_owner, 0x00439dac");
        ck_eq(own.tile_vis_map_at(vis_index), (uint8_t)0xaa,
              "T22: door_apply_to_map must NOT stamp TILE_VIS_MAP (0x713d20 absent from its .asm)");
    }

    // T23: reimpl-verify (2026-08-27) divergence -- the CLEAR leaf's k-sub-loop bound differs per
    // leaf in door_update_tile_state: right=6 (0x004325a5), left=8 (0x00432827). A door_y high enough
    // that k=6/k=7 are not cut short by the `row-k<0` early break must clear height_row[6]/[7] for
    // the LEFT leaf but NOT for the RIGHT leaf.
    {
        tact_fixture fx;
        door_record &d    = fx.door_table[4];
        d                 = door_record{};
        d.id              = 4;
        d.tile_x          = 60;
        d.tile_y          = 60; // row=60 >> 8, so k=0..7 never hit the `row-k<0` early break
        d.direct_mode     = 2;  // Normal step: col+1 per column, row fixed
        d.frame_index     = 1;
        d.right_col_count = 1;
        d.right_rows[0]   = 0; // CLEAR
        d.left_col_count  = 1;
        d.left_rows[0]    = 0; // CLEAR

        tact_store             own     = fx.store();
        mh::state::mode_planes planes  = fx.planes();
        uint16_t              *right_h = own.map_tile_height_sprites() + (60 << 10) + (60 << 3);
        uint16_t              *left_h  = own.map_tile_height_sprites() + (61 << 10) + (60 << 3); // left leaf steps col+1
        for (int k = 0; k < 8; ++k) {
            right_h[k] = 0xbeef;
            left_h[k]  = 0xbeef;
        }
        detail::door_update_tile_state(own, planes, fx.view(), 4);

        ck_eq((uint32_t)right_h[5], 0u, "T23: right leaf clears k=5 (within its bound 6), 0x0043261f");
        ck_eq((uint32_t)right_h[6], 0xbeefu,
              "T23: right leaf must NOT clear k=6 -- its bound is 6, not 8, 0x004325a5");
        ck_eq((uint32_t)left_h[6], 0u, "T23: left leaf DOES clear k=6 -- its bound is 8, 0x00432827");
        ck_eq((uint32_t)left_h[7], 0u, "T23: left leaf clears k=7 too (bound 8, exclusive), 0x00432827");
    }

    // ================= whole-table region (TACT1D acceptance clause) =================
    //
    // T1-T23 above each assert a single slot. These pin the acceptance clause literally: seed the
    // WHOLE 32-slot region distinctly, drive exactly one function call, then assert the ENTIRE
    // region -- not just the touched slot -- matches expectations. Done at BOTH ends of the table
    // (slot 0 / slot 1 and slot 30 / slot 31) since a stride or bound error shows there first.

    // T24: door_tick's loop starts at slot 1, not 0 (@0x004322d9: MOV [EBP-0x18],0x1). Slot 0 is
    // given the SAME active/advance-eligible shape as slot 1 -- if the loop's start were shifted to
    // include it, it would visibly advance too. Only slot 1 may change.
    {
        tact_fixture fx;
        seed_distinct_door_table(fx.door_table);

        door_record &d0      = fx.door_table[0];
        d0.state             = 1;
        d0.frame_index       = 1;
        d0.right_frame_count = 9;
        d0.speed             = 10.0;
        d0.last_step_time    = 0.0;

        door_record &d1      = fx.door_table[1];
        d1.state             = 1;
        d1.frame_index       = 1;
        d1.right_frame_count = 9;
        d1.speed             = 10.0;
        d1.last_step_time    = 0.0;

        std::vector<door_record> expected = fx.door_table;
        expected[1].frame_index           = 2;    // T3's advance-branch transform
        expected[1].last_step_time        = 10.0; // speed + last_step_time

        tact_store             own    = fx.store();
        mh::state::mode_planes planes = fx.planes();
        reset_log();
        log().now = 20.0; // > last_step_time(0)+speed(10) for both slot 0 and slot 1
        detail::door_tick(own, planes, fx.view(), mock_calls());

        assert_door_region(
            own, expected,
            "T24: door_tick loop starts at slot 1 (0x004322d9) -- slot 0 stays byte-identical to "
            "its seed even though it was ALSO configured to advance (stride/base check); the whole "
            "table region, not just slot 1, is asserted");
    }

    // T25: door_tick's loop reaches slot 31, the last index before the CMP ...,0x20 stop
    // (0x004322e0), and does not leak the write into slot 30. Slot 30 stays fully inert (state==0
    // from the seed) so a base-shift bug that redirected slot 31's write onto slot 30 is directly
    // observable.
    {
        tact_fixture fx;
        seed_distinct_door_table(fx.door_table);

        door_record &d31      = fx.door_table[31];
        d31.state             = 1;
        d31.frame_index       = 1;
        d31.right_frame_count = 9;
        d31.speed             = 10.0;
        d31.last_step_time    = 0.0;

        std::vector<door_record> expected = fx.door_table;
        expected[31].frame_index          = 2;
        expected[31].last_step_time       = 10.0;

        tact_store             own    = fx.store();
        mh::state::mode_planes planes = fx.planes();
        reset_log();
        log().now = 20.0;
        detail::door_tick(own, planes, fx.view(), mock_calls());

        assert_door_region(
            own, expected,
            "T25: door_tick reaches slot 31 (last index before the CMP ...,0x20 stop at "
            "0x004322e0) and slot 30 stays byte-identical to its seed (stride/base check); the "
            "whole table region, not just slot 31, is asserted");
    }

    // T26: door_update_tile_state(1) -- door_idx 1, the LOW end of the range door_tick/
    // door_apply_to_map actually walk (their loops start at slot 1, not 0) -- never writes ANY
    // field of _G_LLM_TACT_DOOR_TABLE (it only reads its own door_idx's record and writes
    // tile_objects / TILE_VIS_MAP / the height-sprite cache). Whole-region check catches any stray
    // write near the table, not just at the driven slot. (door_idx==1 also keeps the MARK write's
    // class_owner==1 a genuine sanity signal -- door_idx==0 would write a literal 0,
    // indistinguishable from "nothing happened".)
    {
        tact_fixture fx;
        seed_distinct_door_table(fx.door_table);
        door_record &d1    = fx.door_table[1];
        d1.tile_x          = 90;
        d1.tile_y          = 90;
        d1.direct_mode     = 2;
        d1.frame_index     = 1;
        d1.right_col_count = 1;
        d1.right_rows[0]   = 5;
        d1.left_col_count  = 0;

        std::vector<door_record> expected = fx.door_table; // no field of the table should move

        tact_store             own    = fx.store();
        mh::state::mode_planes planes = fx.planes();
        detail::door_update_tile_state(own, planes, fx.view(), 1);

        ck_eq(planes.tile_object_at(90, 90).class_owner, 1u,
              "T26: sanity -- door_update_tile_state(1) still does its normal MARK write");
        assert_door_region(own, expected,
                           "T26: door_update_tile_state(1) (real region LOW end) never mutates "
                           "_G_LLM_TACT_DOOR_TABLE anywhere in the region");
    }

    // T27: same invariant at the HIGH end of the real region, door_idx 31.
    {
        tact_fixture fx;
        seed_distinct_door_table(fx.door_table);
        door_record &d31    = fx.door_table[31];
        d31.tile_x          = 95;
        d31.tile_y          = 95;
        d31.direct_mode     = 2;
        d31.frame_index     = 1;
        d31.right_col_count = 1;
        d31.right_rows[0]   = 5;
        d31.left_col_count  = 0;

        std::vector<door_record> expected = fx.door_table;

        tact_store             own    = fx.store();
        mh::state::mode_planes planes = fx.planes();
        detail::door_update_tile_state(own, planes, fx.view(), 31);

        ck_eq(planes.tile_object_at(95, 95).class_owner, 31u,
              "T27: sanity -- door_update_tile_state(31) still does its normal MARK write");
        assert_door_region(own, expected,
                           "T27: door_update_tile_state(31) (real region HIGH end) never mutates "
                           "_G_LLM_TACT_DOOR_TABLE anywhere in the region");
    }

    // T28: door_apply_to_map's loop also starts at slot 1 (@0x00439d0d: MOV [EBP-0x34],0x1) and
    // self-consistency-skips everyone but the one slot whose .id matches its own index
    // (0x00439d2b-0x00439d35). Only slot 1 is made self-consistent; every id is otherwise forced to
    // 0 (mismatched against every possible index) so a stride/base bug in the id-vs-index compare
    // shows as an unexpected mark. Whole-region check: apply_to_map writes NO field of the door
    // table itself (only tile_objects / the height-sprite cache), at either end.
    {
        tact_fixture fx;
        seed_distinct_door_table(fx.door_table);
        for (auto &d : fx.door_table) d.id = 0;
        door_record &d1    = fx.door_table[1];
        d1.id              = 1; // self-consistent -- the only slot the walk should mark
        d1.tile_x          = 91;
        d1.tile_y          = 91;
        d1.direct_mode     = 2;
        d1.right_col_count = 1;
        d1.right_rows[0]   = 5;
        d1.left_col_count  = 0;

        std::vector<door_record> expected = fx.door_table; // apply_to_map never writes door_table

        tact_store             own    = fx.store();
        mh::state::mode_planes planes = fx.planes();
        detail::door_apply_to_map(own, planes, fx.view());

        ck_eq(planes.tile_object_at(91, 91).class_owner, 1u,
              "T28: slot 1 (loop's real first index, 0x00439d0d) marks its own tile");
        assert_door_region(own, expected,
                           "T28: door_apply_to_map (real region LOW end, slot 1) never mutates "
                           "_G_LLM_TACT_DOOR_TABLE anywhere in the region");
    }

    // T29: same shape at the HIGH end, slot 31.
    {
        tact_fixture fx;
        seed_distinct_door_table(fx.door_table);
        for (auto &d : fx.door_table) d.id = 0;
        door_record &d31    = fx.door_table[31];
        d31.id              = 31;
        d31.tile_x          = 96;
        d31.tile_y          = 96;
        d31.direct_mode     = 2;
        d31.right_col_count = 1;
        d31.right_rows[0]   = 5;
        d31.left_col_count  = 0;

        std::vector<door_record> expected = fx.door_table;

        tact_store             own    = fx.store();
        mh::state::mode_planes planes = fx.planes();
        detail::door_apply_to_map(own, planes, fx.view());

        ck_eq(planes.tile_object_at(96, 96).class_owner, 31u,
              "T29: slot 31 (loop's real last index, before the CMP ...,0x20 stop at 0x00439d14) "
              "marks its own tile");
        assert_door_region(own, expected,
                           "T29: door_apply_to_map (real region HIGH end, slot 31) never mutates "
                           "_G_LLM_TACT_DOOR_TABLE anywhere in the region");
    }

    // ================= door_parse_definition =================

    // T18: a RIGHT line. The FIRST token written (slot==1) becomes right_col_count -- ONLY after
    // that does every later write land in right_rows[]. "2,10,20,30,40": col_count=2, then FOUR
    // single (comma-delimited) row values -- 4 rows / col_count 2 = 2, no remainder.
    {
        tact_fixture fx;
        door_record &d    = fx.door_table[1];
        d                 = door_record{};
        char       line[] = "1RIGHT{2,10,20,30,40}";
        tact_store own    = fx.store();
        reset_log();
        detail::door_parse_definition(own, mock_calls(), line, 1);

        ck_eq(own.door_table_at(1).right_col_count, 2u, "T18: slot==1 -> right_col_count = FIRST token, 0x00439a4a");
        ck_eq(own.door_table_at(1).right_rows[0], 10u, "T18: right_rows[0]=10, slot==2");
        ck_eq(own.door_table_at(1).right_rows[1], 20u, "T18: right_rows[1]=20");
        ck_eq(own.door_table_at(1).right_rows[2], 30u, "T18: right_rows[2]=30");
        ck_eq(own.door_table_at(1).right_rows[3], 40u, "T18: right_rows[3]=40");
        ck_eq(own.door_table_at(1).right_frame_count, 2u, "T18: entries(4)/col_count(2)=2, no remainder");
        ck_eq(log().abort_called, false, "T18: no fatal");
    }

    // T19: a LEFT line whose SECOND token opens an inclusive DASH range. "1,5-8": token1(1) is a
    // plain comma-terminated single -> col_count=1 (slot==1); token2(5) is followed by '-' -> stash
    // range_start=5 (NO write yet, slot stays at 2); token3(8) closes the range -> fills
    // [5,6,7,8], FOUR writes landing at slot 2..5 (left_rows[0..3]). col_count=1 divides any count.
    {
        tact_fixture fx;
        door_record &d    = fx.door_table[1];
        d                 = door_record{};
        char       line[] = "1LEFT{1,5-8}";
        tact_store own    = fx.store();
        reset_log();
        detail::door_parse_definition(own, mock_calls(), line, 1);

        ck_eq(own.door_table_at(1).left_col_count, 1u, "T19: slot==1 -> left_col_count = FIRST token, 0x00439ad8");
        ck_eq(own.door_table_at(1).left_rows[0], 5u, "T19: range fill start, 0x00439ae3-0x00439ae6");
        ck_eq(own.door_table_at(1).left_rows[1], 6u, "T19: range fill +1");
        ck_eq(own.door_table_at(1).left_rows[2], 7u, "T19: range fill +2");
        ck_eq(own.door_table_at(1).left_rows[3], 8u, "T19: range fill end (inclusive)");
        ck_eq(own.door_table_at(1).left_frame_count, 4u, "T19: entries(4)/col_count(1)=4");
        ck_eq(log().abort_called, false, "T19: no fatal");
    }

    // T20: an unrecognized keyword is fatal.
    {
        tact_fixture fx;
        char         line[] = "1UPDOWN{3}";
        tact_store   own    = fx.store();
        reset_log();
        detail::door_parse_definition(own, mock_calls(), line, 1);

        ck_eq(log().cleanup_called, true, "T20: bad keyword -> llm_fatal_cleanup, 0x004399e6");
        ck_eq(log().abort_called, true, "T20: bad keyword -> utils_abort");
        ck_eq((uint32_t)log().abort_status, 0u, "T20: abort status 0");
    }

    // T21: a nonzero remainder (entries not evenly divisible by col_count) is fatal. First token
    // (3) is consumed as col_count; the remaining two tokens (5,7) are row entries -- 2 rows is not
    // evenly divisible by col_count=3.
    {
        tact_fixture fx;
        char         line[] = "1RIGHT{3,5,7}";
        tact_store   own    = fx.store();
        reset_log();
        detail::door_parse_definition(own, mock_calls(), line, 1);

        ck_eq(log().abort_called, true, "T21: 2 rows not divisible by col_count=3 -> fatal, 0x00439af8");
    }
}

} // namespace mh::tact::test
