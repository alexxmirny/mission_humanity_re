//
// sim_revoke_invention_selftest.cpp -- `simtest` offline oracle for llm_strat_revoke_invention
// (sim/sim_revoke_invention.{h,cpp}, RI-SIM / SIM1F). The function is a single byte store --
// `own.progress_at(player, progress_id).acquired = false` -- with a redundant `player & 0xffff`
// re-mask (see the .cpp's derivation). ZERO CALLS were seen in two live scenarios (sp_det + all-AI
// soak, 15000 steps each, 2026-08-16), a genuine scenario gap: invention-revoke is a rare event.
// So the per-call shadow site cannot cover it and this offline oracle is what promotes it to T1.
//
// EXPECTED BEHAVIOUR HAND-DERIVED FROM THE DISASSEMBLY
// (tmp/decomp/llm_strat_revoke_invention_0044048c.asm): clears exactly ONE progress record's
// `.acquired` byte to 0; touches nothing else (not `.available`, not `.f3`, not a neighbouring row
// or player). The `player & 0xffff` mask means a caller-supplied player value with high bits set is
// truncated to 16 bits before indexing.
//
#include "sim/sim_revoke_invention.h"

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

} // namespace

void run_revoke_invention_tests() {
    sim_fixture fx;

    // ---- C1: clears exactly the target record's `.acquired`, leaves `.available`/`.f3` alone -------
    fx.reset();
    {
        // Seed the target (player 2, invention 5) fully acquired, plus distinct neighbours so a
        // wrong index is caught: same player different row, same row different player.
        player_progress &tgt        = fx.store().progress_at(2, 5);
        tgt.available               = 1;
        tgt.acquired                = 1;
        tgt.f3                      = 0x7f;
        player_progress &nbr_row    = fx.store().progress_at(2, 6);
        nbr_row.acquired            = 1;
        player_progress &nbr_player = fx.store().progress_at(3, 5);
        nbr_player.acquired         = 1;

        sim_store own = fx.store();
        detail::revoke_invention(own, /*player*/ 2, /*progress_id*/ 5);

        ck_eq((uint32_t)fx.store().progress_at(2, 5).acquired, 0u, "C1: target .acquired cleared to 0");
        ck_eq((uint32_t)fx.store().progress_at(2, 5).available, 1u, "C1: target .available untouched");
        ck_eq((uint32_t)fx.store().progress_at(2, 5).f3, 0x7fu, "C1: target .f3 untouched");
        ck_eq((uint32_t)fx.store().progress_at(2, 6).acquired, 1u, "C1: same-player neighbour row untouched");
        ck_eq((uint32_t)fx.store().progress_at(3, 5).acquired, 1u, "C1: same-row neighbour player untouched");
    }

    // ---- C2: idempotent on an already-cleared record (still 0, no crash) --------------------------
    fx.reset();
    {
        fx.store().progress_at(1, 0).acquired = 0;
        sim_store own                         = fx.store();
        detail::revoke_invention(own, /*player*/ 1, /*progress_id*/ 0);
        ck_eq((uint32_t)fx.store().progress_at(1, 0).acquired, 0u, "C2: already-cleared stays 0");
    }

    // ---- C3: the highest valid player/row slot clears cleanly (no off-by-one past the array) -------
    // The body's `player & 0xffffu` re-mask is redundant here -- `player` is already a uint16_t
    // parameter, so a high-bit value truncates at the call boundary before the mask ever runs (see
    // the .cpp: "mirrors the original's own redundant re-masking of an already-16-bit value"). So
    // this case just confirms the top-of-range indexing lands on exactly the target record.
    fx.reset();
    {
        fx.store().progress_at(7, 9).acquired = 1;
        sim_store own                         = fx.store();
        detail::revoke_invention(own, /*player*/ (uint16_t)7, /*progress_id*/ (uint16_t)9);
        ck_eq((uint32_t)fx.store().progress_at(7, 9).acquired, 0u, "C3: player 7 row 9 cleared");
        ck_eq((uint32_t)fx.store().progress_at(6, 9).acquired, 0u, "C3: player 6 row 9 was never set (untouched)");
    }
}

} // namespace mh::sim::test
