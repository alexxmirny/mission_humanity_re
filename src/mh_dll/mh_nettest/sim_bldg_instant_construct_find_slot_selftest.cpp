//
// sim_bldg_instant_construct_find_slot_selftest.cpp -- `simtest` oracle for
// llm_strat_bldg_instant_construct_find_slot @0x0046d229
// (sim/sim_bldg_instant_construct_find_slot.h/.cpp), X-TL-DRAIN step 4.
//
// WHY IT EXISTS BESIDE AN ARMABLE SHADOW SITE. The site compares the return value and
// _G_LLM_STRAT_ORDER_SCRATCH_ARGS, which is a real and well-aimed comparison -- but it is reachable
// only by a player clicking "confirm" on a valid building footprint, and no determinism or AI
// scenario in the rig does that. This file reaches every branch with no game at all: all six switch
// arms, both hardcoded scan bounds, the default types, the two `initial_workers` sentinels, and the
// scratch field->value pairing the committed plate warns about.
//
// EVERY EXPECTED VALUE IS DERIVED FROM THE DISASSEMBLY
// (tmp/decomp/llm_strat_bldg_instant_construct_find_slot_0046d229.asm) and from a BYTE READ of the
// jump table at 0x0046d24f -- not from the C++ under test, and not from the Ghidra .c draft (whose
// case labels could not, on their own, tell the mines arm from the turrets arm: both bounds are 32
// and only the base addresses differ).
//
#include <array>
#include <vector>

#include "sim/sim_bldg_instant_construct_find_slot.h"

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// Distinct, non-symmetric seeds -- no two share a value, so a transposition among the four staged
// scratch values or the five parameters cannot pass by accident. TILE_COL != TILE_ROW specifically
// separates scratch field 4 from field 5.
constexpr int32_t  TILE_COL        = 41;
constexpr int32_t  TILE_ROW        = 77;
constexpr int32_t  INITIAL_WORKERS = 13;
constexpr uint32_t PLAYER          = 3;
constexpr uint16_t ORDER_CODE      = 0x00ea;
constexpr uint32_t OWNER_TAG       = 0x40;

// cfg_enum_E_BUILDING members, one representative per jump-table arm.
constexpr uint8_t T_A_PRODUCTION = 1;
constexpr uint8_t T_A_MINE       = 2;
constexpr uint8_t T_A_PLANT      = 3; // the SINGLE group
constexpr uint8_t T_A_TURRET     = 5;
constexpr uint8_t T_A_BARRAKS    = 7; // the storage group
constexpr uint8_t T_A_LAB        = 11;
constexpr uint8_t T_H_PRODUCTION = 21;
constexpr uint8_t T_H_MINE       = 22;
constexpr uint8_t T_H_TURRET     = 25;
constexpr uint8_t T_A_BIURO      = 17; // in range, but a DEFAULT table entry
constexpr uint8_t T_H_BYURO      = 37; // ditto
constexpr uint8_t T_UNUSED_19    = 19; // ditto (no enum member at all)
constexpr uint8_t T_UNDEFINED    = 0;

// The cfg row the tests select the arm through. Arbitrary but fixed; distinct from every type value
// above so a translation that switched on the INDEX rather than on Building[index].type fails.
constexpr int32_t BUILDING_TYPE_ID = 64;

// ---- recorder ----------------------------------------------------------------------------------

struct fs_recorder {
    int32_t                              reset_n = 0;
    std::vector<std::array<int32_t, 2>>  set_fields; // (index, value), in call order
    int32_t                              dispatch_n = 0;
    std::vector<std::array<uint32_t, 4>> dispatch_args; // (unit_id, player, op_code, arg)
    int32_t                              dispatch_ret = 0;
    void                                 reset() { *this = fs_recorder{}; }
};
fs_recorder g_fs;

const bldg_instant_construct_find_slot_calls &rec_fs_calls() {
    static const bldg_instant_construct_find_slot_calls c = {
        []() { g_fs.reset_n++; },
        [](int32_t index, int32_t value) { g_fs.set_fields.push_back({index, value}); },
        [](uint16_t unit_id, uint32_t player, uint16_t op_code, uint16_t arg) -> int32_t {
            g_fs.dispatch_n++;
            g_fs.dispatch_args.push_back({(uint32_t)unit_id, player, (uint32_t)op_code,
                                          (uint32_t)arg});
            return g_fs.dispatch_ret;
        },
    };
    return c;
}

// ---- fixture helpers ---------------------------------------------------------------------------
//
// "Occupied" is b_index != 0 for a sub-roster slot and building_id != 0 for a roster slot, straight
// off the CMP/JNZ pairs (0x0046d342, 0x0046d381, 0x0046d3c1, 0x0046d403, 0x0046d441, 0x0046d49c).

void fill_productions(sim_fixture &f, int32_t lo, int32_t hi, int32_t val) {
    for (int32_t i = lo; i < hi; ++i)
        f.productions[(size_t)((int32_t)PLAYER * PRODUCTIONS_PER_PLAYER + i)].b_index = val;
}
void fill_mines(sim_fixture &f, int32_t lo, int32_t hi, int32_t val) {
    for (int32_t i = lo; i < hi; ++i)
        f.mines[(size_t)((int32_t)PLAYER * MINES_PER_PLAYER + i)].b_index = val;
}
void fill_turrets(sim_fixture &f, int32_t lo, int32_t hi, int16_t val) {
    for (int32_t i = lo; i < hi; ++i)
        f.turrets[(size_t)((int32_t)PLAYER * TURRETS_PER_PLAYER + i)].b_index = val;
}
void fill_storage(sim_fixture &f, int32_t lo, int32_t hi, int32_t val) {
    for (int32_t i = lo; i < hi; ++i)
        f.storage[(size_t)((int32_t)PLAYER * STORAGE_PER_PLAYER + i)].b_index = val;
}
void fill_labs(sim_fixture &f, int32_t lo, int32_t hi, int32_t val) {
    for (int32_t i = lo; i < hi; ++i)
        f.labs[(size_t)((int32_t)PLAYER * LABS_PER_PLAYER + i)].b_index = val;
}
void fill_roster(sim_fixture &f, int32_t lo, int32_t hi, uint16_t val) {
    for (int32_t i = lo; i < hi; ++i) f.b((int32_t)PLAYER, i).building_id = val;
}

int32_t call(sim_fixture &f, uint8_t type, int32_t initial_workers = INITIAL_WORKERS) {
    f.cfg_buildings[BUILDING_TYPE_ID].type = type;
    sim_view v                             = f.view();
    return detail::bldg_instant_construct_find_slot(v, rec_fs_calls(), TILE_COL, TILE_ROW,
                                                    BUILDING_TYPE_ID, initial_workers, PLAYER);
}

void ck_nothing_happened(const char *what) {
    ck(g_fs.reset_n == 0 && g_fs.set_fields.empty() && g_fs.dispatch_n == 0, what);
}

// ==== the type switch: each arm reads ITS OWN sub-roster =========================================
//
// The shape of every case below is the same and it is chosen to SEPARATE the arms rather than merely
// exercise them: the arm's own sub-roster is FULL while every other sub-roster is EMPTY. A correct
// body returns 0; a body wired to the wrong array finds a free slot and dispatches. This is what
// settles the mines/turrets question a decompile alone could not (see the file banner).

void test_production_arm_reads_productions() {
    sim_fixture f;
    g_fs.reset();
    fill_productions(f, 1, PRODUCTIONS_PER_PLAYER, 9); // full
    const int32_t r = call(f, T_A_PRODUCTION);
    ck_eq((uint32_t)r, 0u, "A_PRODUCTION with productions[] full -> 0 (caseD_1 @0x0046d318)");
    ck_nothing_happened("... and nothing is staged or dispatched");
}

void test_mine_arm_reads_mines_not_turrets() {
    sim_fixture f;
    g_fs.reset();
    fill_mines(f, 1, MINES_PER_PLAYER, 9); // mines FULL, turrets untouched (all free)
    const int32_t r = call(f, T_A_MINE);
    ck_eq((uint32_t)r, 0u,
          "A_MINE with mines[] full and turrets[] EMPTY -> 0. caseD_2 @0x0046d39a has base "
          "0x00d03480 == `mines`; a body wired to turrets would have found slot 1 here");
    ck_nothing_happened("... and nothing is staged or dispatched");
}

void test_turret_arm_reads_turrets_not_mines() {
    sim_fixture f;
    g_fs.reset();
    fill_turrets(f, 1, TURRETS_PER_PLAYER, 9); // turrets FULL, mines untouched
    const int32_t r = call(f, T_A_TURRET);
    ck_eq((uint32_t)r, 0u,
          "A_TURRET with turrets[] full and mines[] EMPTY -> 0. caseD_5 @0x0046d35a has base "
          "0x00cc0fe0 == `turrets` and a WORD compare (map_object_turret::b_index is int16_t)");
    ck_nothing_happened("... and nothing is staged or dispatched");
}

void test_storage_arm_reads_unit_storage() {
    sim_fixture f;
    g_fs.reset();
    fill_storage(f, 1, STORAGE_PER_PLAYER, 9);
    const int32_t r = call(f, T_A_BARRAKS);
    ck_eq((uint32_t)r, 0u, "A_BARRAKS with unit_storage[] full -> 0 (caseD_7 @0x0046d3d9)");
    ck_nothing_happened("... and nothing is staged or dispatched");
}

void test_lab_arm_reads_labs() {
    sim_fixture f;
    g_fs.reset();
    fill_labs(f, 1, LABS_PER_PLAYER, 9);
    const int32_t r = call(f, T_A_LAB);
    ck_eq((uint32_t)r, 0u, "A_LAB with labs[] full -> 0 (caseD_b @0x0046d418)");
    ck_nothing_happened("... and nothing is staged or dispatched");
}

void test_single_group_needs_no_sub_roster_at_all() {
    sim_fixture f;
    g_fs.reset();
    // EVERY sub-roster full. The SINGLE arm sets sub_slot = 1 unconditionally, so the roster scan
    // still runs and still dispatches.
    fill_productions(f, 0, PRODUCTIONS_PER_PLAYER, 9);
    fill_mines(f, 0, MINES_PER_PLAYER, 9);
    fill_turrets(f, 0, TURRETS_PER_PLAYER, 9);
    fill_storage(f, 0, STORAGE_PER_PLAYER, 9);
    fill_labs(f, 0, LABS_PER_PLAYER, 9);
    const int32_t r = call(f, T_A_PLANT);
    ck_eq((uint32_t)r, 1u,
          "A_PLANT ignores every sub-roster -- sub_slot = 1 unconditionally (caseD_3 @0x0046d456) -- "
          "so it dispatches at the first free roster slot, 1");
    ck_eq((uint32_t)g_fs.dispatch_n, 1u, "... exactly one dispatch");
}

void test_both_races_take_the_same_arm() {
    // The jump table is symmetric with an offset of 20 (A_x at index x-1, H_x at index x+19). Three
    // spot checks, one per scanning arm that has a race pair, using the same full-sub-roster shape.
    {
        sim_fixture f;
        g_fs.reset();
        fill_productions(f, 1, PRODUCTIONS_PER_PLAYER, 9);
        ck_eq((uint32_t)call(f, T_H_PRODUCTION), 0u, "H_PRODUCTION shares caseD_1 with A_PRODUCTION");
    }
    {
        sim_fixture f;
        g_fs.reset();
        fill_mines(f, 1, MINES_PER_PLAYER, 9);
        ck_eq((uint32_t)call(f, T_H_MINE), 0u, "H_MINE shares caseD_2 with A_MINE");
    }
    {
        sim_fixture f;
        g_fs.reset();
        fill_turrets(f, 1, TURRETS_PER_PLAYER, 9);
        ck_eq((uint32_t)call(f, T_H_TURRET), 0u, "H_TURRET shares caseD_5 with A_TURRET");
    }
}

// ==== the default arm ============================================================================

void test_default_types_return_zero_with_every_roster_free() {
    // Four values that land on the table's DEFAULT entries or outside its range, with EVERY roster
    // completely empty -- so the only reason to return 0 is the switch itself.
    const uint8_t     types[] = {T_UNDEFINED, T_A_BIURO, T_H_BYURO, T_UNUSED_19, 39, 255};
    const char *const why[]   = {
        "type 0 (UNDEFINED): DEC AL wraps to 0xff and the CMP/JA bound check rejects it (0x0046d300)",
        "A_BIURO (17) is a DEFAULT entry in the jump table -- real, not a transcription gap",
        "H_BYURO (37) likewise",
        "type 19 has no enum member and a DEFAULT table entry",
        "type 39 is one past the table's last index (0x25 == 37, i.e. type 38)",
        "type 255 is far out of range",
    };
    for (size_t k = 0; k < sizeof(types) / sizeof(types[0]); ++k) {
        sim_fixture f;
        g_fs.reset();
        ck_eq((uint32_t)call(f, types[k]), 0u, why[k]);
        ck_nothing_happened("... default arm stages nothing and dispatches nothing");
    }
}

// ==== the two hardcoded scan bounds ==============================================================

void test_storage_scan_stops_at_16_not_at_the_array_extent() {
    sim_fixture f;
    g_fs.reset();
    // Slots 1..15 OCCUPIED, 16..24 FREE. The original's bound is `CMP dword ptr [EBP-0x10],0x10`
    // (0x0046d3e0), so it never looks at 16 or beyond even though map_object_unit_storage[8][25]
    // holds 25. A body written against caps.storage would find slot 16 and dispatch.
    fill_storage(f, 1, 16, 9);
    const int32_t r = call(f, T_A_BARRAKS);
    ck_eq((uint32_t)r, 0u,
          "unit_storage slots 16..24 are FREE and still nothing is found: the scan bound is a "
          "HARDCODED 16 (0x0046d3e0 CMP ...,0x10), not the array's 25");
    ck_nothing_happened("... and nothing is staged or dispatched");
}

void test_roster_scan_stops_at_91_not_at_the_array_extent() {
    sim_fixture f;
    g_fs.reset();
    // Roster slots 1..90 OCCUPIED, 91..99 FREE. Bound is `CMP dword ptr [EBP-0x14],0x5b`
    // (0x0046d476). A body written against caps.buildings would find slot 91 and dispatch.
    fill_roster(f, 1, 91, 0x1234);
    const int32_t r = call(f, T_A_PLANT);
    ck_eq((uint32_t)r, 0u,
          "buildings slots 91..99 are FREE and still nothing is found: the roster scan bound is a "
          "HARDCODED 91 (0x0046d476 CMP ...,0x5b), not the array's 100");
    ck_nothing_happened("... and nothing is staged or dispatched -- the scratch is NOT reset on the "
                        "exhausted-roster path either (0x0046d501)");
}

void test_roster_scan_returns_the_first_free_slot() {
    sim_fixture f;
    g_fs.reset();
    fill_roster(f, 1, 5, 0x1234); // 1..4 occupied, 5 free
    const int32_t r = call(f, T_A_PLANT);
    ck_eq((uint32_t)r, 5u, "the roster scan returns the FIRST free slot (>= 1), not the last");
    ck(g_fs.dispatch_args.size() == 1 && g_fs.dispatch_args[0][0] == 5u,
       "... and dispatch's unit_id argument is that same slot (0x0046d4ee MOVZX EAX,word ptr "
       "[EBP-0x14])");
}

void test_roster_slot_zero_is_never_offered() {
    sim_fixture f;
    g_fs.reset();
    // Slot 0 free (the default), slot 1 occupied, slot 2 free. The loop starts at 1 (0x0046d46f), so
    // the answer must be 2 and never 0.
    f.b((int32_t)PLAYER, 1).building_id = 0x1234;
    const int32_t r                     = call(f, T_A_PLANT);
    ck_eq((uint32_t)r, 2u, "roster slot 0 is never a candidate (loop starts at 1, 0x0046d46f)");
}

// ==== the REAL OUTPUT: the order-scratch staging =================================================

void test_scratch_staging_field_to_value_pairing() {
    sim_fixture f;
    g_fs.reset();
    const int32_t r = call(f, T_A_PLANT);

    ck_eq((uint32_t)r, 1u, "precondition: a free roster slot was found");
    ck_eq((uint32_t)g_fs.reset_n, 1u,
          "llm_strat_order_scratch_reset called exactly ONCE, before the stores (0x0046d4a6)");
    ck(g_fs.set_fields.size() == 4, "exactly four llm_strat_order_scratch_set_field calls");
    // THE PLATE'S WARNING, ASSERTED. The four indices are not ascending and do not follow the
    // parameter order; the index->value pairing is what the 0xea handler reads back, so a
    // transposition here places the building at the mirrored tile with the wrong worker count.
    ck(g_fs.set_fields.size() == 4 && g_fs.set_fields[0][0] == 4 &&
           g_fs.set_fields[0][1] == TILE_COL,
       "call 1: set_field(4, tile_col) (0x0046d4ab MOV EDX,[EBP-0x28] / 0x0046d4ae MOV EAX,0x4)");
    ck(g_fs.set_fields.size() == 4 && g_fs.set_fields[1][0] == 5 &&
           g_fs.set_fields[1][1] == TILE_ROW,
       "call 2: set_field(5, tile_row) (0x0046d4b8 / 0x0046d4bb MOV EAX,0x5)");
    ck(g_fs.set_fields.size() == 4 && g_fs.set_fields[2][0] == 1 &&
           g_fs.set_fields[2][1] == INITIAL_WORKERS,
       "call 3: set_field(1, initial_workers) (0x0046d4c5 / 0x0046d4c8 MOV EAX,0x1)");
    ck(g_fs.set_fields.size() == 4 && g_fs.set_fields[3][0] == 0 &&
           g_fs.set_fields[3][1] == BUILDING_TYPE_ID,
       "call 4: set_field(0, building_type_id) (0x0046d4d2 / 0x0046d4d5 XOR EAX,EAX) -- the cfg ROW "
       "INDEX, not the type value the switch dispatched on");
}

void test_initial_workers_sentinels_pass_through_untouched() {
    // The committed plate records two sentinels resolved three hops away in map_CreateBuilding:
    // -1 = "use the cfg builder_count default", -2 = "staff from free human population, capped at
    // it". THIS function must not interpret, clamp or validate either -- it stages the raw value.
    for (int32_t w : {-1, -2}) {
        sim_fixture f;
        g_fs.reset();
        const int32_t r = call(f, T_A_PLANT, w);
        ck_eq((uint32_t)r, 1u, "sentinel initial_workers still finds and dispatches at a slot");
        ck(g_fs.set_fields.size() == 4 && g_fs.set_fields[2][0] == 1 && g_fs.set_fields[2][1] == w,
           "initial_workers sentinel (-1 / -2) is staged into scratch field 1 VERBATIM -- it is "
           "map_CreateBuilding's to interpret, not this function's");
    }
}

void test_no_sub_slot_leaves_the_scratch_alone() {
    sim_fixture f;
    g_fs.reset();
    fill_labs(f, 1, LABS_PER_PLAYER, 9);
    const int32_t r = call(f, T_A_LAB);
    ck_eq((uint32_t)r, 0u, "precondition: no free lab slot");
    ck_eq((uint32_t)g_fs.reset_n, 0u,
          "the no-sub-slot path returns BEFORE touching the order scratch (0x0046d45d CMP / JNZ, "
          "0x0046d463 -> 0x0046d508) -- a caller's earlier staging survives");
}

// ==== the dispatch call ===========================================================================

void test_dispatch_argument_values() {
    sim_fixture f;
    g_fs.reset();
    fill_roster(f, 1, 3, 0x1234); // first free roster slot is 3
    const int32_t r = call(f, T_A_PLANT);

    ck_eq((uint32_t)r, 3u, "precondition: roster slot 3");
    ck(g_fs.dispatch_args.size() == 1 && g_fs.dispatch_args[0][0] == 3u,
       "arg 1 (EAX) = the roster slot, low 16 bits (0x0046d4ee)");
    ck(g_fs.dispatch_args.size() == 1 && g_fs.dispatch_args[0][1] == (PLAYER | OWNER_TAG),
       "arg 2 (EDX) = (uint16_t)(player | 0x40) -- an OR, so PLAYER=3 gives 0x43, not 0x40 "
       "(0x0046d4e9 OR AL,0x40 / 0x0046d4eb MOVZX EDX,AX). NOTE the BUILDING tag is 0x40 where the "
       "unit-side wrapper llm_unit_order_disembark_soldiers uses 0x80");
    ck(g_fs.dispatch_args.size() == 1 && g_fs.dispatch_args[0][2] == ORDER_CODE &&
           g_fs.dispatch_args[0][3] == ORDER_CODE,
       "args 3 and 4 (EBX, ECX) are BOTH 0xea (0x0046d4dc / 0x0046d4e1)");
}

void test_dispatch_return_is_discarded() {
    sim_fixture f;
    g_fs.reset();
    g_fs.dispatch_ret = -999; // the original never reads EAX back
    const int32_t r   = call(f, T_A_PLANT);
    ck_eq((uint32_t)r, 1u,
          "the function returns the ROSTER SLOT, not dispatch's result (0x0046d4f7 MOV EAX,"
          "[EBP-0x14])");
}

void test_player_is_truncated_to_16_bits_everywhere() {
    sim_fixture f;
    g_fs.reset();
    // Every roster index expression reads `MOVZX EAX,word ptr [EBP+0x8]`. Occupying row 3's first
    // two roster slots must therefore be visible when the call passes 0x10003.
    fill_roster(f, 1, 3, 0x1234);
    f.cfg_buildings[BUILDING_TYPE_ID].type = T_A_PLANT;
    sim_view      v                        = f.view();
    const int32_t r                        = detail::bldg_instant_construct_find_slot(
        v, rec_fs_calls(), TILE_COL, TILE_ROW, BUILDING_TYPE_ID, INITIAL_WORKERS, 0x10000u + PLAYER);

    ck_eq((uint32_t)r, 3u,
          "player = 0x10003 indexes roster row 3 (the word truncation at 0x0046d489), so the two "
          "occupied slots are seen and the answer is 3");
    ck(g_fs.dispatch_args.size() == 1 && g_fs.dispatch_args[0][1] == (PLAYER | OWNER_TAG),
       "... and the dispatch tag is 16-bit too: (uint16_t)(0x10003 | 0x40) == 0x43");
}

} // namespace

void run_bldg_instant_construct_find_slot_tests() {
    test_production_arm_reads_productions();
    test_mine_arm_reads_mines_not_turrets();
    test_turret_arm_reads_turrets_not_mines();
    test_storage_arm_reads_unit_storage();
    test_lab_arm_reads_labs();
    test_single_group_needs_no_sub_roster_at_all();
    test_both_races_take_the_same_arm();

    test_default_types_return_zero_with_every_roster_free();

    test_storage_scan_stops_at_16_not_at_the_array_extent();
    test_roster_scan_stops_at_91_not_at_the_array_extent();
    test_roster_scan_returns_the_first_free_slot();
    test_roster_slot_zero_is_never_offered();

    test_scratch_staging_field_to_value_pairing();
    test_initial_workers_sentinels_pass_through_untouched();
    test_no_sub_slot_leaves_the_scratch_alone();

    test_dispatch_argument_values();
    test_dispatch_return_is_discarded();
    test_player_is_truncated_to_16_bits_everywhere();
}

} // namespace mh::sim::test
