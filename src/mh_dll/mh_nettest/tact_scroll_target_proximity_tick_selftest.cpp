//
// tact_scroll_target_proximity_tick_selftest.cpp -- offline oracle for
// llm_tact_scroll_target_proximity_tick (TACT1E, 2026-08-27/28). See
// tact/tact_scroll_target_proximity_tick.h for the derivation.
//
// WHY OFFLINE: its sole write region (_G_LLM_SQUAD_BB_TARGET_ENERGY_PCT) is OWN_SHARED (cross-mode)
// -- arm_ready:false, proven here only (proof:OFFLINE(tacttest, shared:1) per tact_migration.json).
// Zero outward calls (MEASURED via shadow_region_closure.py: 1 function reachable, itself only), so
// no callee mock seam is needed at all.
//
#include "tact/tact_scroll_target_proximity_tick.h"
#include "tact_test_support.h"

namespace mh::tact::test {

void run_scroll_target_proximity_tick_tests() {
    // Fixed target throughout: target_col=100, target_row=200.
    //   hard box: col in [95,105],   row in [193,207]
    //   soft box: col in [90,110],   row in [188,212]

    // T1: blast dead-center of the hard box -> reset to 0, regardless of prior energy. 0x004390a3.
    {
        tact_fixture fx;
        fx.target_tile_col            = 100;
        fx.target_tile_row            = 200;
        fx.blast_marker_col           = 100;
        fx.blast_marker_row           = 200;
        fx.squad_bb_target_energy_pct = 77;

        tact_store own = fx.store();
        detail::scroll_target_proximity_tick(own);

        ck_eq((uint32_t)own.squad_bb_target_energy_pct(), 0u,
              "T1: blast at target center is inside the hard box -> reset to 0, 0x004390a3");
    }

    // T2: blast far outside both boxes -> no write at all (the third, no-op exit). 0x004390f9.
    {
        tact_fixture fx;
        fx.target_tile_col            = 100;
        fx.target_tile_row            = 200;
        fx.blast_marker_col           = 300; // col 300 >> soft_hi(110)
        fx.blast_marker_row           = 200;
        fx.squad_bb_target_energy_pct = 42;

        tact_store own = fx.store();
        detail::scroll_target_proximity_tick(own);

        ck_eq((uint32_t)own.squad_bb_target_energy_pct(), 42u,
              "T2: outside both boxes -> energy_pct untouched, 0x004390db->0x004390f9");
    }

    // T3: hard-box LOW boundary, both axes exactly at col_hard_lo=95/row_hard_lo=193 (JL is
    // strict-less, so == the lo edge still counts as inside). Reset fires.
    {
        tact_fixture fx;
        fx.target_tile_col            = 100;
        fx.target_tile_row            = 200;
        fx.blast_marker_col           = 95;
        fx.blast_marker_row           = 193;
        fx.squad_bb_target_energy_pct = 88;

        tact_store own = fx.store();
        detail::scroll_target_proximity_tick(own);

        ck_eq((uint32_t)own.squad_bb_target_energy_pct(), 0u,
              "T3: exact hard-box lo boundary (col=95,row=193) still counts as inside, 0x0043907d/0x00439087");
    }

    // T4: hard-box HIGH boundary, col_hard_hi=105/row_hard_hi=207 (JLE, so == the hi edge counts).
    {
        tact_fixture fx;
        fx.target_tile_col            = 100;
        fx.target_tile_row            = 200;
        fx.blast_marker_col           = 105;
        fx.blast_marker_row           = 207;
        fx.squad_bb_target_energy_pct = 99;

        tact_store own = fx.store();
        detail::scroll_target_proximity_tick(own);

        ck_eq((uint32_t)own.squad_bb_target_energy_pct(), 0u,
              "T4: exact hard-box hi boundary (col=105,row=207) still counts as inside, 0x00439093/0x0043909f");
    }

    // T5: just outside the hard box on col only (col=94 < col_hard_lo=95), row still hard-in-range
    // -> NOT hard box; col=94 IS within soft [90,110] -> soft box; energy(77) >= 50 -> subtract 50.
    {
        tact_fixture fx;
        fx.target_tile_col            = 100;
        fx.target_tile_row            = 200;
        fx.blast_marker_col           = 94;
        fx.blast_marker_row           = 200;
        fx.squad_bb_target_energy_pct = 77;

        tact_store own = fx.store();
        detail::scroll_target_proximity_tick(own);

        ck_eq((uint32_t)own.squad_bb_target_energy_pct(), 27u,
              "T5: soft-not-hard with energy>=50 -> subtract 0x32 literally (77-50=27), 0x004390f2");
    }

    // T6: just outside the hard box on col only (col=106 > col_hard_hi=105), still within soft ->
    // soft box; energy(10) < 50 -> reset to 0 (NOT the hard-box exit).
    {
        tact_fixture fx;
        fx.target_tile_col            = 100;
        fx.target_tile_row            = 200;
        fx.blast_marker_col           = 106;
        fx.blast_marker_row           = 200;
        fx.squad_bb_target_energy_pct = 10;

        tact_store own = fx.store();
        detail::scroll_target_proximity_tick(own);

        ck_eq((uint32_t)own.squad_bb_target_energy_pct(), 0u,
              "T6: soft-not-hard with energy<50 -> reset to 0, 0x004390e6");
    }

    // T7: soft-box LOW boundary exactly (col=90), energy==50 EXACTLY. Regression pin only -- MUTATION
    // TESTED (2026-08-28) and found NOT to distinguish JGE from JG at this input: reset(50)=0 and
    // subtract(50)=50-50=0 coincide numerically, so a `< 0x32` -> `<= 0x32` mutation still passes this
    // one check. The real boundary evidence is T8 (energy=49, reset->0) bracketed against T12
    // (energy=51, subtract->1) below -- those two DO diverge observably and together pin the
    // threshold to exactly 50, since 0x004390dd's CMP/JGE is unambiguous from the raw bytes.
    {
        tact_fixture fx;
        fx.target_tile_col            = 100;
        fx.target_tile_row            = 200;
        fx.blast_marker_col           = 90;
        fx.blast_marker_row           = 200;
        fx.squad_bb_target_energy_pct = 50;

        tact_store own = fx.store();
        detail::scroll_target_proximity_tick(own);

        ck_eq((uint32_t)own.squad_bb_target_energy_pct(), 0u,
              "T7: energy==50 exactly -> 0 either way (reset or subtract-to-zero coincide), 0x004390dd-0x004390f2");
    }

    // T8: soft-box HIGH boundary exactly (col=110), energy==49 (just below 50) -> reset to 0.
    {
        tact_fixture fx;
        fx.target_tile_col            = 100;
        fx.target_tile_row            = 200;
        fx.blast_marker_col           = 110;
        fx.blast_marker_row           = 200;
        fx.squad_bb_target_energy_pct = 49;

        tact_store own = fx.store();
        detail::scroll_target_proximity_tick(own);

        ck_eq((uint32_t)own.squad_bb_target_energy_pct(), 0u,
              "T8: exact soft-box hi boundary (col=110) still inside soft; energy=49<50 -> reset, 0x004390e6");
    }

    // T9: just OUTSIDE the soft box lo edge (col=89) -> no-op regardless of row or energy.
    {
        tact_fixture fx;
        fx.target_tile_col            = 100;
        fx.target_tile_row            = 200;
        fx.blast_marker_col           = 89;
        fx.blast_marker_row           = 200;
        fx.squad_bb_target_energy_pct = 63;

        tact_store own = fx.store();
        detail::scroll_target_proximity_tick(own);

        ck_eq((uint32_t)own.squad_bb_target_energy_pct(), 63u,
              "T9: col=89 is outside the soft box (lo=90) -> untouched, 0x004390b7");
    }

    // T10: just OUTSIDE the soft box hi edge (col=111) -> no-op.
    {
        tact_fixture fx;
        fx.target_tile_col            = 100;
        fx.target_tile_row            = 200;
        fx.blast_marker_col           = 111;
        fx.blast_marker_row           = 200;
        fx.squad_bb_target_energy_pct = 15;

        tact_store own = fx.store();
        detail::scroll_target_proximity_tick(own);

        ck_eq((uint32_t)own.squad_bb_target_energy_pct(), 15u,
              "T10: col=111 is outside the soft box (hi=110) -> untouched, 0x004390cd");
    }

    // T11: col is comfortably inside soft range, but row is far outside soft range -- proves the
    // soft-box test requires ALL FOUR conditions together, not just the col pair.
    {
        tact_fixture fx;
        fx.target_tile_col            = 100;
        fx.target_tile_row            = 200;
        fx.blast_marker_col           = 100; // dead-center col
        fx.blast_marker_row           = 0;   // row 0 << soft row lo (188)
        fx.squad_bb_target_energy_pct = 55;

        tact_store own = fx.store();
        detail::scroll_target_proximity_tick(own);

        ck_eq((uint32_t)own.squad_bb_target_energy_pct(), 55u,
              "T11: col-in-range but row-out-of-range must still be a no-op (all 4 conditions AND'd), "
              "0x004390b9/0x004390c1");
    }

    // T12: row just outside the hard box only (row=192 < row_hard_lo=193), col hard-in-range -> not
    // hard box; row IS within soft [188,212] -> soft box; energy(51) >= 50 -> subtract.
    {
        tact_fixture fx;
        fx.target_tile_col            = 100;
        fx.target_tile_row            = 200;
        fx.blast_marker_col           = 100;
        fx.blast_marker_row           = 192;
        fx.squad_bb_target_energy_pct = 51;

        tact_store own = fx.store();
        detail::scroll_target_proximity_tick(own);

        ck_eq((uint32_t)own.squad_bb_target_energy_pct(), 1u,
              "T12: row-only miss on the hard box still reaches the soft-box decay path (51-50=1), "
              "0x0043907d and 0x004390f2");
    }
}

} // namespace mh::tact::test
