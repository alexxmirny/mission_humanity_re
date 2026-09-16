//
// sim_bldg_scrap_stored_units_selftest.cpp -- `simtest` cases for llm_bldg_scrap_stored_units
// (sim/sim_bldg_scrap_stored_units.h/.cpp), RI-SIM SIM1B.
//
// The function does no sim-state writes of its own (it only reads buildings[]/storage[] and calls
// two already-migrated SIM1A callees), so every assertion here is on the RECORDED CALL SEQUENCE --
// how many times each callee fired, in what order, and the exact (player, unit_index) each carries.
// Own recording calls struct, same shape as sim_unit_refund_selftest.cpp's `refund_log` /
// recording_calls() (non-capturing lambdas assigned to the plain-C-function-pointer struct members).
//
// EXPECTED BEHAVIOUR HAND-DERIVED FROM THE DISASSEMBLY
// (tmp/decomp/llm_bldg_scrap_stored_units_0048d7d5.asm), not from the .cpp:
//
//   sub_id is read ONCE, before the loop (0x0048d7ec MOV [ebp-0x14],EAX (player) / 0x0048d7ef MOV
//   [ebp-0x20],EDX (building_index) / 0x0048d7f2-0x0048d80c: building row = player*0x6aa4 +
//   building_index*0x111, MOVZX BYTE [.. + 0xc3d366] -> buildings[player][building_index].sub_id,
//   stored once into the [ebp-0x1c] local). Every iteration below reuses that ONE local -- it is
//   never re-read off the building record inside the loop.
//
//   The loop bound is NOT cached: LAB_0048d816 (0x0048d829-0x0048d832) recomputes the storage row
//   from player*0x17d4 + sub_id*0xf4 and CMPs the running index against dword [row + 0xc727c4]
//   (docked_count, storage offset 0x4) FRESH every pass -- so a case that mutates docked_count out
//   from under the loop would see the live value, though none of these cases needs that to prove the
//   re-read (the .cpp's own comment already carries that finding; this file's S1 proves the COUNT is
//   honoured for a plain, unmutated fixture).
//
//   Per iteration (LAB_0048d836/LAB_0048d83e): docked_units[i] is read via a PLAIN 32-bit MOV
//   (0x0048d859, index pre-scaled SHL EAX,2 -- stride 4, matching int32_t docked_units[50] in
//   mh_structs.gen.h, offset 0x8 off the storage record) into a stack local at [ebp-0x18], THEN:
//     1. llm_strat_unit_refund_build_cost_by_health(player, unit_index) -- 0x0048d862 MOV EDX,dword
//        ptr [ebp-0x18] (plain 32-bit load, FULL WIDTH, no MOVZX/MOVSX) / 0x0048d865 MOVZX EAX,word
//        [ebp-0x14] (player) / 0x0048d869 CALL 0x0048d706.
//     2. llm_strat_unit_teardown(player, unit_index) -- 0x0048d86e MOVZX EDX,word ptr [ebp-0x18]
//        (TRUNCATED to the low 16 bits of the SAME local call 1 just read full-width) / 0x0048d872
//        MOVZX EAX,word [ebp-0x14] (player) / 0x0048d876 CALL 0x00487ba5.
//   Then INC the running index (LAB_0048d836, 0x0048d839) and loop. No early return, no state
//   written by this function itself -- purely a read + two outward calls per docked unit, in the
//   order refund-then-teardown, never batched or reordered.
//
#include "sim/sim_bldg_scrap_stored_units.h"

#include <cstdint>
#include <initializer_list>
#include <vector>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// player < MAX_PLAYERS (8), building_index < BUILDINGS_PER_PLAYER (100), distinct from each other
// and from 0/the sub_ids used below so a swapped-operand translation lands on the wrong record.
constexpr uint32_t PLAYER         = 4;
constexpr int32_t  BUILDING_INDEX = 2;

// One recorded call, tagged by which of the two callees fired. `unit_index` is widened to int64_t
// so it can hold either the FULL-WIDTH int32_t refund argument or the 16-bit-TRUNCATED teardown
// argument without narrowing either side -- the whole point of these cases is to see the two differ.
struct call_log {
    enum kind_t { REFUND,
                  TEARDOWN };
    struct ev {
        kind_t   kind;
        uint32_t player;
        int64_t  unit_index;
    };
    std::vector<ev> events;
    void            reset() { events.clear(); }
};
call_log g_log;

const scrap_stored_units_calls &recording_calls() {
    static const scrap_stored_units_calls c = {
        // refund_build_cost_by_health(int32_t player, int32_t unit_index) -- FULL WIDTH.
        [](int32_t player, int32_t unit_index) -> void {
            g_log.events.push_back(call_log::ev{call_log::REFUND, (uint32_t)player, unit_index});
        },
        // unit_teardown(uint32_t player, uint16_t unit_index) -- the committed prototype already
        // narrows the parameter to uint16_t, so whatever arrives here IS the truncated value.
        [](uint32_t player, uint16_t unit_index) -> void {
            g_log.events.push_back(call_log::ev{call_log::TEARDOWN, player, unit_index});
        },
    };
    return c;
}

// Assert the recorded call at `n` is exactly (kind, player, unit_index).
void ck_ev(int32_t n, call_log::kind_t kind, uint32_t player, int64_t unit_index, const char *what) {
    if ((size_t)n >= g_log.events.size()) {
        ck(false, what);
        return;
    }
    const call_log::ev &e = g_log.events[(size_t)n];
    ck(e.kind == kind && e.player == player && e.unit_index == unit_index, what);
}

// Seed storage[player][slot].docked_count/docked_units in order. `units` must not exceed the real
// 50-entry extent (mh_map_object_unit_storage::docked_units[50]).
void put_storage(sim_fixture &f, uint32_t player, int32_t slot, int32_t docked_count,
                 std::initializer_list<int32_t> units) {
    unit_storage &s = f.storage[(size_t)player * (size_t)STORAGE_PER_PLAYER + (size_t)slot];
    s.docked_count  = docked_count;
    int32_t i       = 0;
    for (int32_t u : units) s.docked_units[i++] = u;
}

} // namespace

// ---- S1: three docked units, distinct non-symmetric values -> three refund+teardown pairs, IN
// ORDER, each pair's teardown right after its refund (not batched: refund,refund,refund,teardown,...
// would also pass a plain count check but fail this ordering check). sub_id == BUILDING_INDEX here
// (no indirection yet -- that is S4's job).
void test_three_docked_units_refund_then_teardown_in_order() {
    sim_fixture fx;
    fx.b(PLAYER, BUILDING_INDEX).sub_id = (uint8_t)BUILDING_INDEX;
    put_storage(fx, PLAYER, BUILDING_INDEX, /*docked_count=*/3, {11, 22, 33});

    const sim_view v = fx.view();
    g_log.reset();
    detail::bldg_scrap_stored_units(v, recording_calls(), PLAYER, BUILDING_INDEX);

    ck_eq((uint32_t)g_log.events.size(), 6u,
          "S1: docked_count=3 -> 3 refund + 3 teardown = 6 recorded calls");
    ck_ev(0, call_log::REFUND, PLAYER, 11, "S1[0]: refund(player, 11) for slot 0");
    ck_ev(1, call_log::TEARDOWN, PLAYER, 11, "S1[1]: teardown(player, 11) right after its refund");
    ck_ev(2, call_log::REFUND, PLAYER, 22, "S1[2]: refund(player, 22) for slot 1");
    ck_ev(3, call_log::TEARDOWN, PLAYER, 22, "S1[3]: teardown(player, 22)");
    ck_ev(4, call_log::REFUND, PLAYER, 33, "S1[4]: refund(player, 33) for slot 2");
    ck_ev(5, call_log::TEARDOWN, PLAYER, 33, "S1[5]: teardown(player, 33)");
}

// ---- S2: docked_count == 0 -> the loop condition fails on its first test -> zero calls at all,
// not even a spurious read of docked_units[0].
void test_zero_docked_count_no_calls() {
    sim_fixture fx;
    fx.b(PLAYER, BUILDING_INDEX).sub_id = (uint8_t)BUILDING_INDEX;
    put_storage(fx, PLAYER, BUILDING_INDEX, /*docked_count=*/0, {});
    // Poison docked_units so a bound-check bug that walked anyway would be caught by S1-style values
    // never appearing.
    unit_storage &s   = fx.storage[(size_t)PLAYER * (size_t)STORAGE_PER_PLAYER + (size_t)BUILDING_INDEX];
    s.docked_units[0] = 0x7fffffff;

    const sim_view v = fx.view();
    g_log.reset();
    detail::bldg_scrap_stored_units(v, recording_calls(), PLAYER, BUILDING_INDEX);

    ck_eq((uint32_t)g_log.events.size(), 0u, "S2: docked_count=0 -> no refund/teardown calls at all");
}

// ---- S3: the 16-bit TRUNCATION finding. docked_units[0] = 0x10005 (> 16 bits). refund must see the
// value FULL WIDTH (0x10005); teardown must see it TRUNCATED to its low 16 bits (0x0005). This is
// the case that fails if a translation accidentally widens/narrows either call's argument to match
// the other.
void test_teardown_truncates_to_16_bits_refund_stays_full_width() {
    sim_fixture fx;
    fx.b(PLAYER, BUILDING_INDEX).sub_id = (uint8_t)BUILDING_INDEX;
    put_storage(fx, PLAYER, BUILDING_INDEX, /*docked_count=*/1, {0x10005});

    const sim_view v = fx.view();
    g_log.reset();
    detail::bldg_scrap_stored_units(v, recording_calls(), PLAYER, BUILDING_INDEX);

    ck_eq((uint32_t)g_log.events.size(), 2u, "S3: one docked unit -> one refund + one teardown");
    ck_ev(0, call_log::REFUND, PLAYER, 0x10005,
          "S3[0]: refund gets the FULL 32-bit value 0x10005, not truncated");
    ck_ev(1, call_log::TEARDOWN, PLAYER, 0x0005,
          "S3[1] FINDING: teardown gets only the low 16 bits (0x10005 & 0xffff = 0x0005)");
}

// ---- S4: sub_id INDIRECTION. buildings[player][building_index].sub_id is a DIFFERENT value (9) from
// building_index (2) itself, so the storage row the function actually walks is
// storage[player][9] -- NOT storage[player][building_index]. A decoy record is planted at
// storage[player][building_index] with different count/values; if the translation used
// building_index instead of sub_id it would fire on the decoy's 2 units instead of the real slot's 1.
constexpr int32_t S4_SUB_ID = 9; // distinct from BUILDING_INDEX (2) and from 0
void              test_sub_id_indirection_not_building_index() {
    sim_fixture fx;
    fx.b(PLAYER, BUILDING_INDEX).sub_id = (uint8_t)S4_SUB_ID;
    // The REAL slot (storage[player][sub_id]): one docked unit.
    put_storage(fx, PLAYER, S4_SUB_ID, /*docked_count=*/1, {77});
    // The DECOY slot (storage[player][building_index]), same slot number as building_index but a
    // DIFFERENT storage row from the real one -- distinct count and distinct, non-overlapping values
    // so any leakage from the wrong row is directly observable.
    put_storage(fx, PLAYER, BUILDING_INDEX, /*docked_count=*/2, {1, 2});

    const sim_view v = fx.view();
    g_log.reset();
    detail::bldg_scrap_stored_units(v, recording_calls(), PLAYER, BUILDING_INDEX);

    ck_eq((uint32_t)g_log.events.size(), 2u,
                       "S4: only the sub_id=9 row's ONE docked unit is walked (1 refund + 1 teardown), "
                                    "not the decoy building_index=2 row's two");
    ck_ev(0, call_log::REFUND, PLAYER, 77, "S4[0]: refund(player, 77) from the sub_id-indexed row");
    ck_ev(1, call_log::TEARDOWN, PLAYER, 77, "S4[1]: teardown(player, 77)");
}

// ---- S5: PER-PLAYER row isolation. storage_of strides by player (player * STORAGE_PER_PLAYER +
// sub_id) -- a decoy at the SAME sub_id but a DIFFERENT player row must not be read while PLAYER's
// own row is walked. Also exercises a second, distinct sub_id (6) from every other case.
constexpr int32_t  S5_SUB_ID   = 6;
constexpr uint32_t S5_DECOY_PL = 0; // distinct from PLAYER (4)
void               test_storage_row_is_scoped_to_the_players_own_row() {
    sim_fixture fx;
    fx.b(PLAYER, BUILDING_INDEX).sub_id = (uint8_t)S5_SUB_ID;
    put_storage(fx, PLAYER, S5_SUB_ID, /*docked_count=*/1, {55});
    // Decoy: player 0's row at the SAME sub_id, larger count and disjoint values.
    put_storage(fx, S5_DECOY_PL, S5_SUB_ID, /*docked_count=*/3, {101, 102, 103});

    const sim_view v = fx.view();
    g_log.reset();
    detail::bldg_scrap_stored_units(v, recording_calls(), PLAYER, BUILDING_INDEX);

    ck_eq((uint32_t)g_log.events.size(), 2u,
                        "S5: PLAYER's own row (1 docked unit) is walked, not player 0's decoy row (3 units)");
    ck_ev(0, call_log::REFUND, PLAYER, 55, "S5[0]: refund(PLAYER, 55) from PLAYER's own row");
    ck_ev(1, call_log::TEARDOWN, PLAYER, 55, "S5[1]: teardown(PLAYER, 55)");
}

void run_bldg_scrap_stored_units_tests() {
    test_three_docked_units_refund_then_teardown_in_order();
    test_zero_docked_count_no_calls();
    test_teardown_truncates_to_16_bits_refund_stays_full_width();
    test_sub_id_indirection_not_building_index();
    test_storage_row_is_scoped_to_the_players_own_row();
}

} // namespace mh::sim::test
