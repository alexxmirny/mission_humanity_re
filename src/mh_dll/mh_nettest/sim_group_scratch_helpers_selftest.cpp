//
// sim_group_scratch_helpers_selftest.cpp -- offline `simtest` oracle for TWO paired group-scratch
// registration helpers (sim/sim_group_move_register_member.h/.cpp and
// sim/sim_group_scratch_add_unit.h/.cpp, RI-SIM / SIM1-G1 tail slice, 2026-08-20):
//
//   llm_strat_group_move_register_member                       @0x0048d16b
//   llm_strat_group_scratch_add_unit_and_normalize_heading      @0x0048d283
//
// WHY OFFLINE, NOT A RIG ARM: both functions take a CALLER-OWNED in/out counter pointer
// (`int *scratch_count` / `int *io_count`): the counter lives on the
// caller's own stack, not in any restorable region, so the shadow snapshot/restore mechanism
// cannot reach it -- arming these two on the live rig hung the process (2026-08-20) even though
// both translations are correct (independently adversarially reviewed, zero divergences). This
// file is their evidence instead: it seeds and reads `*scratch_count`/`*io_count` directly, with
// no restore ambiguity.
//
// EXPECTED BEHAVIOUR HAND-DERIVED FROM
// tmp/decomp_sim/llm_strat_group_move_register_member_0048d16b.asm and
// tmp/decomp_sim/llm_strat_group_scratch_add_unit_and_normalize_heading_0048d283.asm -- every
// assertion below cites the instruction address(es) it pins. NOT read off the .cpp bodies.
//
// Both functions are leaf, straight-line bodies (no branches, no callees besides the inert
// `utils_assert_stack_capacity` prologue) -- so unlike most sim selftests there is no recorder
// struct here, just fixture seeding + direct calls, same shape as sim_selftest.cpp's
// test_group_scratch_centroid().
//
#include "sim/sim_group_move_register_member.h"
#include "sim/sim_group_scratch_add_unit.h"

#include <cstdint>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// =====================================================================================================
// llm_strat_group_move_register_member @0x0048d16b
// =====================================================================================================

struct MoveRegSeed {
    int32_t player   = 2;
    int32_t unit_idx = 5;

    // Distinct, non-symmetric so a swapped x/y translation disagrees with the fixture.
    uint8_t tile_x = 30;
    uint8_t tile_y = 90;

    // THE ASYMMETRY under test: passable_pre is what's CURRENTLY in passable[x][y] before the call
    // (must be READ into row.saved_passable, 0x0048d199-0x0048d1d9); origin_tile_was_passable is a
    // DIFFERENT field on the unit that gets WRITTEN into passable[x][y] afterward
    // (0x0048d1df-0x0048d22a). Distinct values so mixing up which feeds which side is observable.
    uint8_t passable_pre             = 0x42;
    uint8_t origin_tile_was_passable = 0x99;

    int32_t start_count = 0; // *scratch_count on entry
};

void seed_and_run_move_reg(sim_fixture &fx, const MoveRegSeed &s, int32_t &scratch_count) {
    fx.reset();
    unit &u                                                   = fx.u(s.player, s.unit_idx);
    u.x                                                       = s.tile_x;
    u.y                                                       = s.tile_y;
    u.origin_tile_was_passable                                = s.origin_tile_was_passable;
    fx.passable[((int32_t)s.tile_x << 8) | (int32_t)s.tile_y] = s.passable_pre;

    scratch_count = s.start_count;

    sim_store own = fx.store();
    detail::group_move_register_member(fx.view(), own, s.player, s.unit_idx, &scratch_count);
}

void run_group_move_register_member_tests(sim_fixture &fx) {
    // =================================================================================================
    // T1 -- the full row write at index 0: unit_idx (0x0048d18d-0x0048d193), saved_passable pulled
    // from the tile's CURRENT passable[] byte read BEFORE the overwrite (0x0048d199-0x0048d1d9,
    // MOVZX@0x0048d1cc off SHL EBX,0x8@0x0048d1b0 / ADD@0x0048d1ca -- the (x<<8)|y indexing), the
    // passable[] overwrite from the unit's OWN origin_tile_was_passable field, a DIFFERENT field than
    // what fed saved_passable (0x0048d1df-0x0048d22a, final store at 0x0048d22a), and tile_col/
    // tile_row re-read fresh from the roster (0x0048d230-0x0048d24d / 0x0048d253-0x0048d270).
    // =================================================================================================
    {
        MoveRegSeed s;
        s.start_count = 0;
        int32_t count;
        seed_and_run_move_reg(fx, s, count);

        const group_scratch_member &row = fx.group_move_scratch[0];
        ck_eq((uint32_t)row.unit_idx, (uint32_t)s.unit_idx,
              "move_reg T1: scratch[0].unit_idx == unit_idx (0x0048d18d-0x0048d193)");
        ck_eq((uint32_t)row.saved_passable, (uint32_t)s.passable_pre,
              "move_reg T1: scratch[0].saved_passable == the PRE-overwrite passable[x][y] byte, read "
              "before the overwrite below (0x0048d199-0x0048d1d9)");
        ck_eq((uint32_t)fx.passable[((int32_t)s.tile_x << 8) | (int32_t)s.tile_y],
              (uint32_t)s.origin_tile_was_passable,
              "move_reg T1: passable[x][y] overwritten with unit.origin_tile_was_passable -- a "
              "DIFFERENT field than what fed saved_passable above (0x0048d1df-0x0048d22a)");
        ck_eq((uint32_t)row.tile_col, (uint32_t)s.tile_x,
              "move_reg T1: scratch[0].tile_col == unit.x (0x0048d230-0x0048d24d)");
        ck_eq((uint32_t)row.tile_row, (uint32_t)s.tile_y,
              "move_reg T1: scratch[0].tile_row == unit.y (0x0048d253-0x0048d270)");
        ck_eq((uint32_t)count, (uint32_t)(s.start_count + 1),
              "move_reg T1: *scratch_count incremented by exactly 1 (INC @0x0048d276-0x0048d279)");
    }

    // =================================================================================================
    // T2 -- wave_rank (the row's 5th field, array_base+0x8) is NEVER written by this function: no
    // store in the whole body (0x0048d178-0x0048d282) targets that offset -- the only offsets touched
    // are +0xc (unit_idx, 0x0048d193), +0x10 (saved_passable, 0x0048d1d9), +0x0 (tile_col, 0x0048d24d),
    // +0x4 (tile_row, 0x0048d270). A pre-seeded sentinel at the target row must survive unchanged.
    // =================================================================================================
    {
        fx.reset();
        MoveRegSeed s;
        s.start_count                                             = 0;
        fx.group_move_scratch[0].wave_rank                        = -777; // sentinel, set AFTER fx.reset() zeroed it
        unit &u                                                   = fx.u(s.player, s.unit_idx);
        u.x                                                       = s.tile_x;
        u.y                                                       = s.tile_y;
        u.origin_tile_was_passable                                = s.origin_tile_was_passable;
        fx.passable[((int32_t)s.tile_x << 8) | (int32_t)s.tile_y] = s.passable_pre;
        int32_t   count                                           = s.start_count;
        sim_store own                                             = fx.store();
        detail::group_move_register_member(fx.view(), own, s.player, s.unit_idx, &count);

        ck_eq((uint32_t)fx.group_move_scratch[0].wave_rank, (uint32_t)-777,
              "move_reg T2: scratch[0].wave_rank sentinel survives -- no write ever targets that "
              "offset in this function's body (0x0048d178-0x0048d282)");
    }

    // =================================================================================================
    // T3 -- NONZERO starting *scratch_count: the function writes to the row named by the CURRENT
    // count, not always row 0, and increments from wherever it started (all four scratch-row stores
    // and the INC re-derive their array index from *scratch_count at 0x0048d18d/0x0048d1d3/
    // 0x0048d247/0x0048d26a/0x0048d276 -- IMUL by 0x14, not a hardcoded 0).
    // =================================================================================================
    {
        MoveRegSeed s;
        s.player                   = 3;
        s.unit_idx                 = 9;
        s.tile_x                   = 12;
        s.tile_y                   = 34;
        s.passable_pre             = 0x11;
        s.origin_tile_was_passable = 0x22;
        s.start_count              = 7;
        int32_t count;
        seed_and_run_move_reg(fx, s, count);

        const group_scratch_member &row7 = fx.group_move_scratch[7];
        ck_eq((uint32_t)row7.unit_idx, (uint32_t)s.unit_idx,
              "move_reg T3: nonzero start_count=7 -> writes land at scratch[7], not scratch[0] "
              "(0x0048d18d, 0x0048d24d, 0x0048d270)");
        ck_eq((uint32_t)count, 8u,
              "move_reg T3: *scratch_count increments FROM its starting value (7 -> 8, 0x0048d276)");
        // Row 0 must be untouched by this run (still whatever fx.reset() zeroed it to).
        ck_eq((uint32_t)fx.group_move_scratch[0].unit_idx, 0u,
              "move_reg T3: scratch[0] untouched when start_count=7 -- confirms the index is really "
              "*scratch_count-driven, not hardcoded to row 0");
    }

    // =================================================================================================
    // T4 -- non-corruption: a guard scratch row at a DIFFERENT index than any row this file's cases
    // write to, seeded with distinct sentinel values across all five fields, read back unchanged.
    // =================================================================================================
    {
        constexpr int32_t GUARD_ROW = 95;
        fx.reset();
        group_scratch_member &g = fx.group_move_scratch[GUARD_ROW];
        g.tile_col              = 111;
        g.tile_row              = 222;
        g.wave_rank             = 333;
        g.unit_idx              = 444;
        g.saved_passable        = 555;

        MoveRegSeed s;
        s.start_count                                             = 0;
        unit &u                                                   = fx.u(s.player, s.unit_idx);
        u.x                                                       = s.tile_x;
        u.y                                                       = s.tile_y;
        u.origin_tile_was_passable                                = s.origin_tile_was_passable;
        fx.passable[((int32_t)s.tile_x << 8) | (int32_t)s.tile_y] = s.passable_pre;
        int32_t   count                                           = s.start_count;
        sim_store own                                             = fx.store();
        detail::group_move_register_member(fx.view(), own, s.player, s.unit_idx, &count);

        ck_eq((uint32_t)fx.group_move_scratch[GUARD_ROW].tile_col, 111u,
              "move_reg T4: guard row's tile_col untouched");
        ck_eq((uint32_t)fx.group_move_scratch[GUARD_ROW].tile_row, 222u,
              "move_reg T4: guard row's tile_row untouched");
        ck_eq((uint32_t)fx.group_move_scratch[GUARD_ROW].wave_rank, 333u,
              "move_reg T4: guard row's wave_rank untouched");
        ck_eq((uint32_t)fx.group_move_scratch[GUARD_ROW].unit_idx, 444u,
              "move_reg T4: guard row's unit_idx untouched");
        ck_eq((uint32_t)fx.group_move_scratch[GUARD_ROW].saved_passable, 555u,
              "move_reg T4: guard row's saved_passable untouched");
    }
}

// =====================================================================================================
// llm_strat_group_scratch_add_unit_and_normalize_heading @0x0048d283
// =====================================================================================================

struct ScratchAddSeed {
    int32_t player     = 3;
    int32_t unit_index = 8;

    // Index into the int32_t[24] remap table. PINNED TO 0 IN EVERY CASE BELOW, not because the table
    // is read-only or the callee only ever uses index 0 -- it doesn't, this is purely a fixture-side
    // constraint discovered while writing this file: sim_test_support.h declares
    // `std::vector<int32_t> group_step_heading_remap{24}` with BRACES, which -- exactly the
    // initializer_list-vs-size-constructor trap this same header's own banner warns about for scalar
    // element types -- binds the single-int overload and allocates a ONE-element vector holding the
    // value 24, not 24 zeroed elements (confirmed by ASan: ck at index 5 heap-buffer-overflowed 16
    // bytes past a 4-byte region). Every case here only varies the VALUE stored at index 0, which is
    // sufficient to prove the low-byte read/truncation; it does not exercise a different heading
    // INDEX. This file is restricted to touching only itself, sim_selftest.cpp, and
    // mh_nettest.vcxproj, so the fixture bug is reported here rather than fixed.
    uint8_t old_move_heading = 0;
    int32_t remap_value      = 0x12345678; // the table entry at [old_move_heading]

    int32_t start_count = 0; // *io_count on entry
};

void seed_and_run_scratch_add(sim_fixture &fx, const ScratchAddSeed &s, int32_t &io_count) {
    fx.reset();
    unit &u                                         = fx.u(s.player, s.unit_index);
    u.move_heading                                  = s.old_move_heading;
    fx.group_step_heading_remap[s.old_move_heading] = s.remap_value;

    io_count = s.start_count;

    sim_store own = fx.store();
    detail::group_scratch_add_unit_and_normalize_heading(fx.view(), own, s.player, s.unit_index,
                                                         &io_count);
}

void run_group_scratch_add_unit_tests(sim_fixture &fx) {
    // =================================================================================================
    // T1 -- the row write at index 0 is unit_idx ONLY (0x0048d2a2-0x0048d2ab), and move_heading is
    // overwritten with the LOW BYTE of group_step_heading_remap[old_move_heading]
    // (0x0048d2b1-0x0048d2e7: MOVZX old heading@0x0048d2c1, SHL EAX,0x2@0x0048d2c8 -- the int32
    // element stride -- MOV AL, byte ptr [table+EAX]@0x0048d2db reading only byte 0 of the LE int32,
    // MOV [roster],AL@0x0048d2e1). remap_value=0x12345678 has a NON-ZERO high 3 bytes, so this
    // proves the single-byte truncation is real and not just a small value passing through unchanged
    // -- expected result is the low byte, 0x78.
    // =================================================================================================
    {
        ScratchAddSeed s;
        s.start_count = 0;
        int32_t count;
        seed_and_run_scratch_add(fx, s, count);

        ck_eq((uint32_t)fx.group_move_scratch[0].unit_idx, (uint32_t)s.unit_index,
              "scratch_add T1: scratch[0].unit_idx == unit_index (0x0048d2a2-0x0048d2ab)");
        ck_eq((uint32_t)fx.u(s.player, s.unit_index).move_heading, 0x78u,
              "scratch_add T1: move_heading == LOW BYTE of remap[old_heading] (0x12345678 -> 0x78), "
              "proving the byte truncation is real, not a pass-through of an already-small value "
              "(0x0048d2b1-0x0048d2e7)");
        ck_eq((uint32_t)count, (uint32_t)(s.start_count + 1),
              "scratch_add T1: *io_count incremented by exactly 1 (INC @0x0048d2e7-0x0048d2ea)");
    }

    // =================================================================================================
    // T2 -- a SMALL remap value (high bytes already zero) still passes through correctly -- confirms
    // the low-byte read itself, independent of T1's truncation case.
    // =================================================================================================
    {
        ScratchAddSeed s;
        s.remap_value = 5;
        s.start_count = 0;
        int32_t count;
        seed_and_run_scratch_add(fx, s, count);

        ck_eq((uint32_t)fx.u(s.player, s.unit_index).move_heading, 5u,
              "scratch_add T2: a small remap value (0x5, high bytes already 0) reads through as 5 "
              "(0x0048d2c1/0x0048d2db)");
    }

    // =================================================================================================
    // T3 -- the row write touches ONLY unit_idx (array_base+0xc, 0x0048d2ab): tile_col/tile_row/
    // saved_passable/wave_rank at the same row must survive pre-seeded sentinels unchanged -- this
    // function's whole body (0x0048d283-0x0048d2f3) has exactly one store into the scratch array.
    // =================================================================================================
    {
        fx.reset();
        group_scratch_member &row0 = fx.group_move_scratch[0];
        row0.tile_col              = 1001;
        row0.tile_row              = 1002;
        row0.wave_rank             = 1003;
        row0.saved_passable        = 1004;

        ScratchAddSeed s;
        s.start_count                                   = 0;
        unit &u                                         = fx.u(s.player, s.unit_index);
        u.move_heading                                  = s.old_move_heading;
        fx.group_step_heading_remap[s.old_move_heading] = s.remap_value;
        int32_t   count                                 = s.start_count;
        sim_store own                                   = fx.store();
        detail::group_scratch_add_unit_and_normalize_heading(fx.view(), own, s.player, s.unit_index,
                                                             &count);

        ck_eq((uint32_t)fx.group_move_scratch[0].tile_col, 1001u,
              "scratch_add T3: scratch[0].tile_col untouched -- not written by this function");
        ck_eq((uint32_t)fx.group_move_scratch[0].tile_row, 1002u,
              "scratch_add T3: scratch[0].tile_row untouched -- not written by this function");
        ck_eq((uint32_t)fx.group_move_scratch[0].wave_rank, 1003u,
              "scratch_add T3: scratch[0].wave_rank untouched -- not written by this function");
        ck_eq((uint32_t)fx.group_move_scratch[0].saved_passable, 1004u,
              "scratch_add T3: scratch[0].saved_passable untouched -- not written by this function");
    }

    // =================================================================================================
    // T4 -- NONZERO starting *io_count: the row write lands at the CURRENT count's row, not always
    // row 0, and *io_count increments from wherever it started (IMUL by 0x14 off *io_count at
    // 0x0048d2a5/0x0048d2ea -- not a hardcoded 0).
    // =================================================================================================
    {
        fx.reset();
        ScratchAddSeed s;
        s.player      = 4;
        s.unit_index  = 13;
        s.remap_value = 0xAABBCC42;
        s.start_count = 6;
        int32_t count;
        seed_and_run_scratch_add(fx, s, count);

        ck_eq((uint32_t)fx.group_move_scratch[6].unit_idx, (uint32_t)s.unit_index,
              "scratch_add T4: nonzero start_count=6 -> write lands at scratch[6] (0x0048d2a5/0x0048d2ab)");
        ck_eq((uint32_t)count, 7u,
              "scratch_add T4: *io_count increments FROM its starting value (6 -> 7, 0x0048d2ea)");
        ck_eq((uint32_t)fx.group_move_scratch[0].unit_idx, 0u,
              "scratch_add T4: scratch[0] untouched when start_count=6 -- confirms the index is "
              "really *io_count-driven, not hardcoded to row 0");
        ck_eq((uint32_t)fx.u(s.player, s.unit_index).move_heading, 0x42u,
              "scratch_add T4: move_heading still gets the low byte of the remap entry (0xAABBCC42 "
              "-> 0x42) even with a nonzero starting count");
    }

    // =================================================================================================
    // T5 -- non-corruption: a guard roster unit (different player/index than any case above) and a
    // guard scratch row (a different index than any row this file's cases write to), both seeded with
    // distinct sentinel values, read back unchanged.
    // =================================================================================================
    {
        constexpr int32_t GUARD_PLAYER = 6;
        constexpr int32_t GUARD_INDEX  = 40;
        constexpr int32_t GUARD_ROW    = 90;

        fx.reset();
        unit &gu        = fx.u(GUARD_PLAYER, GUARD_INDEX);
        gu.move_heading = 0x77;

        group_scratch_member &g = fx.group_move_scratch[GUARD_ROW];
        g.tile_col              = 11;
        g.tile_row              = 22;
        g.wave_rank             = 33;
        g.unit_idx              = 44;
        g.saved_passable        = 55;

        ScratchAddSeed s;
        s.start_count                                   = 0;
        unit &u                                         = fx.u(s.player, s.unit_index);
        u.move_heading                                  = s.old_move_heading;
        fx.group_step_heading_remap[s.old_move_heading] = s.remap_value;
        int32_t   count                                 = s.start_count;
        sim_store own                                   = fx.store();
        detail::group_scratch_add_unit_and_normalize_heading(fx.view(), own, s.player, s.unit_index,
                                                             &count);

        ck_eq((uint32_t)fx.u(GUARD_PLAYER, GUARD_INDEX).move_heading, 0x77u,
              "scratch_add T5: guard roster unit's move_heading untouched");
        ck_eq((uint32_t)fx.group_move_scratch[GUARD_ROW].tile_col, 11u,
              "scratch_add T5: guard row's tile_col untouched");
        ck_eq((uint32_t)fx.group_move_scratch[GUARD_ROW].tile_row, 22u,
              "scratch_add T5: guard row's tile_row untouched");
        ck_eq((uint32_t)fx.group_move_scratch[GUARD_ROW].wave_rank, 33u,
              "scratch_add T5: guard row's wave_rank untouched");
        ck_eq((uint32_t)fx.group_move_scratch[GUARD_ROW].unit_idx, 44u,
              "scratch_add T5: guard row's unit_idx untouched");
        ck_eq((uint32_t)fx.group_move_scratch[GUARD_ROW].saved_passable, 55u,
              "scratch_add T5: guard row's saved_passable untouched");
    }
}

} // namespace

void run_group_scratch_helpers_tests() {
    sim_fixture fx;
    run_group_move_register_member_tests(fx);
    run_group_scratch_add_unit_tests(fx);
}

} // namespace mh::sim::test
