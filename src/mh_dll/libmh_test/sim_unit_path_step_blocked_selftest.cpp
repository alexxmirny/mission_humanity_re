//
// sim_unit_path_step_blocked_selftest.cpp -- `simtest` offline oracle for
// llm_strat_unit_path_step_blocked (sim/sim_unit_path_step_blocked.h/.cpp, RI-SIM / SIM1-G2).
//
// NOT SHADOWABLE (0 tracked write cells -- pure query, writes nothing) -- this offline oracle is
// its only evidence.
//
// Cases (c)-(h) below are exactly what caught a real bug: an earlier draft of the reimplementation
// read tile_object::flags (+0x0) and ::unit[0] (+0x4) where the original reads ::building (+0x2)
// and ::class_owner (+0x6) -- both wrong fields happened to compile, since flags/unit are also
// uint8_t-based, but they read the wrong bytes of the same 8-byte struct. Caught by reimpl-verify
// (2026-08-20), re-derived independently against the .asm's literal offsets (0xd1ec82/0xd1ec86 vs
// the region base 0xd1ec80) before fixing. See tools/data/ghidra_findings.json.
//
#include "sim/sim_unit_path_step_blocked.h"

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

struct log_t {
    int      facing24_calls    = 0;
    uint32_t last_facing24_arg = 0;
    int32_t  facing24_out_dx   = 0; // what the mock WRITES
    int32_t  facing24_out_dy   = 0;

    void reset() { *this = log_t{}; }
};
log_t g_log;

const unit_path_step_blocked_calls &recording_calls() {
    static const unit_path_step_blocked_calls c = {
        [](uint32_t facing24, int32_t *out_dx, int32_t *out_dy) -> void {
            ++g_log.facing24_calls;
            g_log.last_facing24_arg = facing24;
            *out_dx                 = g_log.facing24_out_dx;
            *out_dy                 = g_log.facing24_out_dy;
        },
    };
    return c;
}

} // namespace

void run_unit_path_step_blocked_tests() {
    constexpr uint32_t PLAYER   = 1;
    constexpr int32_t  UNIT_IDX = 2;

    // ---- (a) heading out of [1, 24]: blocked WITHOUT ever calling facing24_to_delta. ----------------
    for (int32_t bad_heading : {-1, 0, 25, 0x7f}) {
        sim_fixture fx;
        fx.reset();
        g_log.reset();
        sim_view v = fx.view();

        unit &u        = fx.units[PLAYER * UNITS_PER_PLAYER + (size_t)UNIT_IDX];
        u.path_slot_id = 5;
        const uint32_t path_index =
            PLAYER * PATH_WAYPOINTS_PER_PLAYER + (uint32_t)u.path_slot_id * PATH_WAYPOINTS_PER_SLOT;
        fx.path_buffers[path_index].heading = (uint8_t)bad_heading;

        const int32_t r = detail::unit_path_step_blocked(v, recording_calls(), PLAYER, UNIT_IDX);
        ck_eq(r, 1, "unit_path_step_blocked: heading out of [1,24] -> blocked");
        ck_eq(g_log.facing24_calls, 0,
              "unit_path_step_blocked: out-of-range heading never reaches facing24_to_delta");
    }

    // Shared setup for the in-range-heading cases below: heading=5 -> facing24_to_delta returns
    // (dx,dy)=(3,-2); unit at tile (10,20) so the candidate tile is (13,18).
    auto setup = [&](sim_fixture &fx) -> unit & {
        fx.reset();
        g_log.reset();
        g_log.facing24_out_dx = 3;
        g_log.facing24_out_dy = -2;

        unit &u        = fx.units[PLAYER * UNITS_PER_PLAYER + (size_t)UNIT_IDX];
        u.path_slot_id = 5;
        u.x            = 10;
        u.y            = 20;
        const uint32_t path_index =
            PLAYER * PATH_WAYPOINTS_PER_PLAYER + (uint32_t)u.path_slot_id * PATH_WAYPOINTS_PER_SLOT;
        fx.path_buffers[path_index].heading = 5;
        return u;
    };
    constexpr int32_t CAND_X = 13, CAND_Y = 18; // (10+3, 20-2), both well inside the masks

    // ---- (b) candidate PASSABLE and UNOCCUPIED (.building == 0): free (0). ------------------------
    {
        sim_fixture fx;
        setup(fx);
        sim_view v = fx.view();

        fx.passable[(size_t)((CAND_X << 8) | CAND_Y)] = 1; // passable
        tile_object &t                                = fx.tile_objects[(size_t)((CAND_X << 8) | CAND_Y)];
        t.building                                    = 0; // unoccupied

        const int32_t r = detail::unit_path_step_blocked(v, recording_calls(), PLAYER, UNIT_IDX);
        ck_eq(r, 0, "unit_path_step_blocked: passable + building==0 -> free");
        ck_eq(g_log.facing24_calls, 1, "unit_path_step_blocked: in-range heading calls facing24_to_delta once");
        ck_eq(g_log.last_facing24_arg, 5u, "unit_path_step_blocked: facing24_to_delta called with the path heading");
    }

    // ---- (c) candidate PASSABLE but .building != 0 (looks occupied), and the occupant bit
    // (.class_owner & 0x80) is CLEAR -> not a valid occupant record -> blocked. ---------------------
    {
        sim_fixture fx;
        setup(fx);
        sim_view v = fx.view();

        fx.passable[(size_t)((CAND_X << 8) | CAND_Y)] = 1;
        tile_object &t                                = fx.tile_objects[(size_t)((CAND_X << 8) | CAND_Y)];
        t.building                                    = 7;    // building != 0 -> the passable+unoccupied shortcut does NOT fire
        t.class_owner                                 = 0x00; // 0x80 clear -> no valid occupant

        const int32_t r = detail::unit_path_step_blocked(v, recording_calls(), PLAYER, UNIT_IDX);
        ck_eq(r, 1, "unit_path_step_blocked: passable + occupied-looking but no valid occupant bit -> blocked");
    }

    // ---- (d) candidate NOT passable: the passable+unoccupied shortcut is skipped entirely, but the
    // occupant-bit path is STILL reached (passability does not gate it). ---------------------------
    {
        sim_fixture fx;
        setup(fx);
        sim_view v = fx.view();

        fx.passable[(size_t)((CAND_X << 8) | CAND_Y)] = 0; // NOT passable
        tile_object &t                                = fx.tile_objects[(size_t)((CAND_X << 8) | CAND_Y)];
        t.building                                    = 9;        // occupant_idx = 9
        t.class_owner                                 = 0x80 | 3; // valid occupant, owner (low nibble) = player 3

        unit &occ                    = fx.units[3 * UNITS_PER_PLAYER + 9];
        occ.unit_proto_id            = 0;
        fx.cfg_units[0].move_op_arg  = 0xa;
        fx.cfg_units[0].move_op_code = 0xf;
        occ.state                    = 0xa; // == move_op_arg

        const int32_t r = detail::unit_path_step_blocked(v, recording_calls(), PLAYER, UNIT_IDX);
        ck_eq(r, 0, "unit_path_step_blocked: impassable candidate still reaches the occupant-state check, "
                    "and a move_op_arg match is free");
    }

    // ---- (e) occupant.state matches move_op_code (and NOT move_op_arg) -> free. -------------------
    {
        sim_fixture fx;
        setup(fx);
        sim_view v = fx.view();

        fx.passable[(size_t)((CAND_X << 8) | CAND_Y)] = 1;
        tile_object &t                                = fx.tile_objects[(size_t)((CAND_X << 8) | CAND_Y)];
        t.building                                    = 9;
        t.class_owner                                 = 0x80 | 3;

        unit &occ                    = fx.units[3 * UNITS_PER_PLAYER + 9];
        occ.unit_proto_id            = 0;
        fx.cfg_units[0].move_op_arg  = 0xa;
        fx.cfg_units[0].move_op_code = 0xf;
        occ.state                    = 0xf; // == move_op_code, != move_op_arg

        const int32_t r = detail::unit_path_step_blocked(v, recording_calls(), PLAYER, UNIT_IDX);
        ck_eq(r, 0, "unit_path_step_blocked: occupant.state == move_op_code -> free");
    }

    // ---- (f) occupant.state == the literal 0xc (matches neither move_op_arg nor move_op_code) ------
    // -> free.
    {
        sim_fixture fx;
        setup(fx);
        sim_view v = fx.view();

        fx.passable[(size_t)((CAND_X << 8) | CAND_Y)] = 1;
        tile_object &t                                = fx.tile_objects[(size_t)((CAND_X << 8) | CAND_Y)];
        t.building                                    = 9;
        t.class_owner                                 = 0x80 | 3;

        unit &occ                    = fx.units[3 * UNITS_PER_PLAYER + 9];
        occ.unit_proto_id            = 0;
        fx.cfg_units[0].move_op_arg  = 0xa;
        fx.cfg_units[0].move_op_code = 0xf;
        occ.state                    = 0xc; // matches neither proto field, but the literal 0xc arm

        const int32_t r = detail::unit_path_step_blocked(v, recording_calls(), PLAYER, UNIT_IDX);
        ck_eq(r, 0, "unit_path_step_blocked: occupant.state == literal 0xc -> free");
    }

    // ---- (g) occupant.state matches NONE of the three -> blocked. ---------------------------------
    {
        sim_fixture fx;
        setup(fx);
        sim_view v = fx.view();

        fx.passable[(size_t)((CAND_X << 8) | CAND_Y)] = 1;
        tile_object &t                                = fx.tile_objects[(size_t)((CAND_X << 8) | CAND_Y)];
        t.building                                    = 9;
        t.class_owner                                 = 0x80 | 3;

        unit &occ                    = fx.units[3 * UNITS_PER_PLAYER + 9];
        occ.unit_proto_id            = 0;
        fx.cfg_units[0].move_op_arg  = 0xa;
        fx.cfg_units[0].move_op_code = 0xf;
        occ.state                    = 0x99; // matches nothing

        const int32_t r = detail::unit_path_step_blocked(v, recording_calls(), PLAYER, UNIT_IDX);
        ck_eq(r, 1, "unit_path_step_blocked: occupant.state matches nothing -> blocked");
    }

    // ---- (h) owner is read from the LOW NIBBLE of .class_owner, and occupant_idx from .building --
    // NOT from the same byte -- swap a plausible-but-wrong pairing and confirm it disagrees. -------
    {
        sim_fixture fx;
        setup(fx);
        sim_view v = fx.view();

        fx.passable[(size_t)((CAND_X << 8) | CAND_Y)] = 1;
        tile_object &t                                = fx.tile_objects[(size_t)((CAND_X << 8) | CAND_Y)];
        t.building                                    = 5;        // occupant_idx = 5
        t.class_owner                                 = 0x80 | 3; // owner = 3 (low nibble)

        // Real occupant at (owner=3, idx=5): matches.
        unit &real_occ         = fx.units[3 * UNITS_PER_PLAYER + 5];
        real_occ.unit_proto_id = 0;
        // A DIFFERENT record at a swapped (owner=5, idx=3) that would ALSO match if the fields were
        // read in the wrong order -- left at a mismatching state so a swap bug shows up as "blocked"
        // instead of silently agreeing. Both indices stay < MAX_PLAYERS(8) so the swap is a real,
        // in-bounds alternate record, not an out-of-bounds access.
        unit &swapped                = fx.units[5 * UNITS_PER_PLAYER + 3];
        swapped.unit_proto_id        = 1;
        fx.cfg_units[0].move_op_arg  = 0xa;
        fx.cfg_units[0].move_op_code = 0xf;
        fx.cfg_units[1].move_op_arg  = 0x55;
        fx.cfg_units[1].move_op_code = 0x56;
        real_occ.state               = 0xa;  // matches proto 0's move_op_arg
        swapped.state                = 0x77; // matches NEITHER of proto 1's fields

        const int32_t r = detail::unit_path_step_blocked(v, recording_calls(), PLAYER, UNIT_IDX);
        ck_eq(r, 0, "unit_path_step_blocked: owner=class_owner&0xf, occupant_idx=building (not swapped)");
    }
}

} // namespace mh::sim::test
