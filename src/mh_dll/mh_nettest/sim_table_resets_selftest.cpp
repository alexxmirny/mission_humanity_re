#include "sim/resid/sim_table_resets.h"

#include <cstdio>
#include <vector>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

struct shuttle_call {
    int32_t player;
    int32_t slot;
};
std::vector<shuttle_call> g_shuttle_calls;

void rec_prod_shuttle_slot_release(int32_t player, int32_t slot) {
    g_shuttle_calls.push_back({player, slot});
}

const prod_reset_system_calls g_calls = {
    &rec_prod_shuttle_slot_release,
};

void reset_recorders() { g_shuttle_calls.clear(); }

// ---- helpers for run_tech_tables_reset_tests() ----------------------------------------------------

// True iff every byte of [p, p+n) is zero. Used for the bulk zero-checks below (depend[32],
// objects[64]) instead of per-byte ck_eq calls, which would otherwise be several hundred extra
// checks per row/entry for no additional mutation-catching power (a single differing byte anywhere
// in the range still fails this).
bool bytes_all_zero(const void *p, size_t n) {
    const uint8_t *b = static_cast<const uint8_t *>(p);
    for (size_t i = 0; i < n; ++i)
        if (b[i] != 0) return false;
    return true;
}

// Progress[i] (cfg_invention): depend[]/f2/type/index, checked for every row. some_trash's
// placeholder-name copy is checked separately (only at the two named rows) since it needs a
// byte-exact literal comparison, not a zero-check.
void check_invention_zeroed(sim_store &own, int32_t i) {
    cfg_invention &invn = own.cfg_invention_at(i);
    char           m1[112];
    std::snprintf(m1, sizeof(m1), "T1: Progress[%d].depend[0..31] all zeroed, 0x00455bab", i);
    ck(bytes_all_zero(invn.depend, sizeof(invn.depend)), m1);
    char m2[96];
    std::snprintf(m2, sizeof(m2), "T1: Progress[%d].f2 = 0, 0x00455c12", i);
    ck_eq((uint32_t)invn.f2, 0u, m2);
    char m3[96];
    std::snprintf(m3, sizeof(m3), "T1: Progress[%d].type = UNDEFINED(0), 0x00455c5c", i);
    ck_eq((uint32_t)invn.type, 0u, m3);
    char m4[96];
    std::snprintf(m4, sizeof(m4), "T1: Progress[%d].index = 0, 0x00455c67", i);
    ck_eq((uint32_t)invn.index, 0u, m4);
}

// progress[player][row] (player_progress), checked for one cell.
void check_progress_cell_zeroed(sim_store &own, int32_t player, int32_t row) {
    player_progress &pr = own.progress_at((uint32_t)player, row);
    char             m1[160];
    std::snprintf(m1, sizeof(m1), "T1: progress[%d][%d].available = 0, 0x00455bd9", player, row);
    ck_eq((uint32_t)pr.available, 0u, m1);
    char m2[160];
    std::snprintf(m2, sizeof(m2), "T1: progress[%d][%d].acquired = 0, 0x00455bef", player, row);
    ck_eq((uint32_t)pr.acquired, 0u, m2);
    char m3[160];
    std::snprintf(m3, sizeof(m3), "T1: progress[%d][%d].f3 = 0, 0x00455c05", player, row);
    ck_eq((uint32_t)pr.f3, 0u, m3);
}

} // namespace

void run_table_resets_tests() {
    sim_fixture fx;

    // =================================================================================================
    // T1 -- exhaustive: every player x every prod_queue_slot cell zeroed; both loop bounds pinned by
    // name; the two neighbouring struct fields (mother_established just before @off 0x190,
    // primary_mother_bldg[0] just after @off 0x214) survive untouched; the shuttle-release mock sees
    // exactly the 8x10 cartesian product, in original order, with player/slot never transposed.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();

        // Seed every player's every prod_queue_slot[0..31] with a DISTINCT, non-zero value so a wrong
        // loop bound / stride / base leaves an identifiable survivor.
        for (int32_t player = 0; player < 8; ++player) {
            for (int32_t slot = 0; slot < 32; ++slot) fx.profiles[player].prod_queue_slot[slot] = player * 100 + slot + 1;
            // Neighbours of the prod_queue_slot[32] array within mh_llm_strat_player_profile (real
            // offsets from mh_structs.gen.h's own offsetof static_asserts): mother_established is the
            // scalar field immediately BEFORE (offset 0x190, ends exactly at 0x194), and
            // primary_mother_bldg[32] is the array immediately AFTER (offset 0x214, prod_queue_slot's
            // own end). A single-cell zero-check cannot distinguish "zeroed exactly prod_queue_slot"
            // from "zeroed a superset that spilled into either neighbour".
            fx.profiles[player].mother_established     = player * 1000 + 777; // sentinel, just BEFORE
            fx.profiles[player].primary_mother_bldg[0] = player * 1000 + 888; // sentinel, just AFTER
        }

        sim_store own = fx.store();
        detail::prod_reset_system(own, g_calls);

        // ---- prod_queue_slot: exhaustive zero check, all 8 players x 32 slots ----------------------
        for (int32_t player = 0; player < 8; ++player) {
            for (int32_t slot = 0; slot < 32; ++slot) {
                char msg[160];
                std::snprintf(msg, sizeof(msg),
                              "T1: prod_queue_slot[%d] zeroed for player %d, 0x00490041", slot, player);
                ck_eq((uint32_t)fx.profiles[player].prod_queue_slot[slot], 0u, msg);
            }
        }
        // Both loop ends, named individually per the mutation-testing brief.
        ck_eq((uint32_t)fx.profiles[0].prod_queue_slot[0], 0u,
              "T1: outer loop's FIRST cell (player 0, slot 0) zeroed -- player bound `CMP ...,0x8` @0x00490008, "
              "slot bound `CMP ...,0x20` @0x00490022");
        ck_eq((uint32_t)fx.profiles[7].prod_queue_slot[31], 0u,
              "T1: outer loop's LAST cell (player 7, slot 31) zeroed -- player bound `CMP ...,0x8` @0x00490008, "
              "slot bound `CMP ...,0x20` @0x00490022");

        // ---- neighbouring fields: unchanged (proves the zero write did NOT overrun prod_queue_slot) -
        for (int32_t player = 0; player < 8; ++player) {
            char msg_before[160];
            std::snprintf(msg_before, sizeof(msg_before),
                          "T1: mother_established (offset 0x190, just BEFORE prod_queue_slot) unchanged for "
                          "player %d -- no store spilled backward past 0x00490041",
                          player);
            ck_eq((uint32_t)fx.profiles[player].mother_established, (uint32_t)(player * 1000 + 777), msg_before);

            char msg_after[160];
            std::snprintf(msg_after, sizeof(msg_after),
                          "T1: primary_mother_bldg[0] (offset 0x214, just AFTER prod_queue_slot) unchanged for "
                          "player %d -- no store spilled forward past 0x00490041",
                          player);
            ck_eq((uint32_t)fx.profiles[player].primary_mother_bldg[0], (uint32_t)(player * 1000 + 888), msg_after);
        }

        // ---- shuttle-release mock: exactly the 8x10 cartesian product, in original order -----------
        ck_eq((uint32_t)g_shuttle_calls.size(), 80u,
              "T1: prod_shuttle_slot_release called exactly 80 times (8 players x 10 slots), 0x0049006a");

        for (int32_t player = 0; player < 8 && (size_t)(player * 10) < g_shuttle_calls.size(); ++player) {
            for (int32_t slot = 0; slot < 10; ++slot) {
                size_t idx = (size_t)(player * 10 + slot);
                if (idx >= g_shuttle_calls.size()) continue;
                char msg[200];
                std::snprintf(msg, sizeof(msg),
                              "T1: call #%zu is (player=%d, slot=%d) in original order -- EAX=player @0x00490067, "
                              "EDX=slot @0x00490064",
                              idx, player, slot);
                ck(g_shuttle_calls[idx].player == player && g_shuttle_calls[idx].slot == slot, msg);
            }
        }

        // Order/first/last named explicitly, plus the argument-transposition guard: player's domain
        // (0..7) and slot's domain (0..9) are DISTINGUISHABLE, so a swapped-argument bug (player and
        // slot transposed at the call site) would still yield 80 calls but violate one of these.
        ck(!g_shuttle_calls.empty() && g_shuttle_calls.front().player == 0 && g_shuttle_calls.front().slot == 0,
           "T1: FIRST shuttle call is (player=0, slot=0) -- all 10 slots of player 0 fire before player 1 starts");
        ck(!g_shuttle_calls.empty() && g_shuttle_calls.back().player == 7 && g_shuttle_calls.back().slot == 9,
           "T1: LAST shuttle call is (player=7, slot=9) -- shuttle loop bound `CMP ...,0xa` @0x00490058");

        bool any_player_over_7 = false;
        bool any_slot_eq_9     = false;
        for (const shuttle_call &c : g_shuttle_calls) {
            if (c.player > 7) any_player_over_7 = true;
            if (c.slot == 9) any_slot_eq_9 = true;
        }
        ck(!any_player_over_7,
           "T1: no recorded pair has player > 7 -- rules out player/slot argument transposition "
           "(player's real domain is 0..7, slot's is 0..9), 0x00490067");
        ck(any_slot_eq_9,
           "T1: at least one recorded pair has slot == 9 -- the slot domain reaches past player's max (7), "
           "which a transposed call could not produce, 0x00490064");
    }
}

// `simtest` oracle for llm_strat_tech_tables_reset @0x00455b5a (sim_table_resets.h/.cpp,
// detail::tech_tables_reset). See this file's top banner for the full address map.
//
// REQUIRES sim_test_support.h's `cfg_upgrades` sized to >= 100 elements. It is currently
// `std::vector<cfg_upgrade> cfg_upgrades{99}` (the canonical extent) -- but
// detail::tech_tables_reset's third loop unconditionally writes Upgrades[0..99] INCLUSIVE (the
// PRESERVE-BUG, see sim_table_resets.h), so EVERY call in EVERY case below writes one cfg_upgrade
// past the end of a 99-element vector: a guaranteed heap-buffer-overflow (ASan) / heap corruption
// (plain build) regardless of which case is running, not just the ones that inspect entry 99. This
// is not a per-case gap this file can work around locally (the instructions this oracle was written
// under forbid that) -- it is a prerequisite fixture widening for the conductor.
void run_tech_tables_reset_tests() {
    sim_fixture fx;

    // =================================================================================================
    // T1 -- the FIRST outer loop (inv = 0..299, `CMP [ebp-0x18],0x12c` @0x00455b79): Progress[inv]
    // (cfg_invention) reset PLUS, nested inside it, progress[j][inv] (the per-player acquisition-state
    // array, j = 0..7, `CMP [ebp-0x1c],0x8` @0x00455bc1) -- the .asm interleaves both tables inside the
    // same inv loop, so both are exercised by one call here.
    // =================================================================================================
    {
        fx.reset();
        sim_store      own = fx.store();
        const sim_view v   = fx.view();

        // Seed every Progress[]/cfg_invention row with DISTINCT, non-zero garbage (never 0 -- the
        // "%250+1" keeps every byte/word in [1,250]) so a wrong bound leaves an identifiable survivor.
        for (int32_t i = 0; i < 300; ++i) {
            cfg_invention &invn = own.cfg_invention_at(i);
            for (int32_t b = 0; b < 32; ++b) invn.depend[b] = (uint8_t)(((i * 3 + b + 1) % 250) + 1);
            invn.type  = (uint8_t)((i % 250) + 1);
            invn.index = (uint16_t)(i + 1000);
            for (int32_t c = 0; c < 64; ++c) invn.some_trash[c] = (char)(((i * 5 + c + 1) % 250) + 1);
            invn.f2 = i * 7 + 13 + 1;
        }
        // Seed every progress[player][row] cell with DISTINCT, non-zero garbage (all 8 x 300 = 2400
        // cells) so a wrong bound / stride / accessor-argument-order bug leaves an identifiable
        // survivor -- see sim_table_resets.h's REGION IDENTIFICATION note on why player is the OUTER
        // byte-address axis (row stride 0x384 == 300*3): a bug that swapped progress_at's (player,row)
        // argument order would compute out-of-range offsets against this real-extent fixture (player's
        // domain is 8, row's is 300) and be caught as a heap-buffer-overflow rather than a value
        // mismatch, which is why the exhaustive zero-check below is the correct oracle for that class
        // of bug, not a hand-computed "expected transposed value".
        for (int32_t player = 0; player < 8; ++player) {
            for (int32_t row = 0; row < 300; ++row) {
                player_progress &pr = own.progress_at((uint32_t)player, row);
                pr.available        = (uint8_t)(((player * 37 + row + 1) % 250) + 1);
                pr.acquired         = (uint8_t)(((player * 53 + row + 2) % 250) + 1);
                pr.f3               = (uint8_t)(((player * 71 + row + 3) % 250) + 1);
            }
        }

        detail::tech_tables_reset(v, own);

        // ---- Progress[]: depend[]/f2/type/index, every row -----------------------------------------
        for (int32_t i = 0; i < 300; ++i) check_invention_zeroed(own, i);
        // Named corners, per the mutation-testing brief.
        ck_eq((uint32_t)own.cfg_invention_at(0).depend[0], 0u,
              "T1: outer loop's FIRST row (inv=0), depend[0] (k=0, first pair) zeroed -- inv bound "
              "`CMP ...,0x12c` @0x00455b79, k bound `CMP ...,0x10` @0x00455b97, store 0x00455bab");
        ck_eq((uint32_t)own.cfg_invention_at(299).depend[31], 0u,
              "T1: outer loop's LAST row (inv=299), depend[31] (k=15, last pair) zeroed -- inv bound "
              "`CMP ...,0x12c` @0x00455b79, k bound `CMP ...,0x10` @0x00455b97, store 0x00455bab");
        ck_eq((uint32_t)own.cfg_invention_at(0).f2, 0u, "T1: FIRST row (inv=0) f2 = 0, 0x00455c12");
        ck_eq((uint32_t)own.cfg_invention_at(299).f2, 0u, "T1: LAST row (inv=299) f2 = 0, 0x00455c12");

        // ---- Progress[inv].some_trash: the placeholder-name copy, exact bytes, first/last row ------
        // src is sim_test_support.h's invention_name_placeholder, " wynalazek \0" (12 bytes). The
        // per-byte-pair copy loop (0x00455c3f-0x00455c55) stops at the source's own NUL (byte 11), so
        // bytes [0..11] must match the literal exactly and bytes [12..63] must be UNTOUCHED -- the
        // copy must not have kept scanning/filling past the source NUL into the rest of the 64-byte
        // field (that would be a "fill the whole field" bug, not a "copy until NUL" one).
        //
        // THE TAIL CHECK ASSERTS THE SEEDED SENTINELS SURVIVE, NOT THAT THE TAIL IS ZERO, and the
        // difference is the case's whole point. This oracle's first draft asserted zero and failed
        // against its OWN seed: nothing in llm_strat_tech_tables_reset clears some_trash's tail --
        // the only writes to the field are the 12 bytes the copy loop stores, and the function
        // contains no utils_fill_data at all (checked over the whole listing). Bytes [12..63]
        // therefore keep whatever they held BEFORE the call, which in the real game is the previous
        // name's remainder. Asserting the seed survives is also the STRONGER claim: an all-zero
        // check passes vacuously on a fixture that started zeroed -- i.e. it would pass on exactly
        // the "fill the whole field" bug it exists to exclude.
        {
            static const char expect[12] = {' ', 'w', 'y', 'n', 'a', 'l', 'a', 'z', 'e', 'k', ' ', '\0'};
            for (int32_t i : {0, 299}) {
                const char *got   = own.cfg_invention_at(i).some_trash;
                const bool  exact = std::memcmp(got, expect, sizeof(expect)) == 0;
                char        msg[144];
                std::snprintf(msg, sizeof(msg),
                              "T1: Progress[%d].some_trash[0..11] == \" wynalazek \0\" exactly (the "
                              "12-byte placeholder), 0x00455c3f-0x00455c55",
                              i);
                ck(exact, msg);

                // The seed formula from the top of this case, re-evaluated rather than remembered.
                bool tail_intact = true;
                for (int32_t c = 12; c < 64; ++c)
                    if (got[c] != (char)(((i * 5 + c + 1) % 250) + 1)) tail_intact = false;
                char msg2[192];
                std::snprintf(msg2, sizeof(msg2),
                              "T1: Progress[%d].some_trash[12..63] still holds its PRE-CALL seed -- the "
                              "copy loop stopped at the source's NUL instead of filling the field, "
                              "0x00455c55",
                              i);
                ck(tail_intact, msg2);
            }
        }

        // ---- Progress[inv].type / .index, first/last named explicitly ------------------------------
        ck_eq((uint32_t)own.cfg_invention_at(0).type, 0u,
              "T1: FIRST row (inv=0) type = UNDEFINED(0), 0x00455c5c");
        ck_eq((uint32_t)own.cfg_invention_at(299).index, 0u,
              "T1: LAST row (inv=299) index = 0, 0x00455c67");

        // ---- progress[player][row]: exhaustive zero-check, all 8 players x 300 rows ----------------
        for (int32_t player = 0; player < 8; ++player)
            for (int32_t row = 0; row < 300; ++row) check_progress_cell_zeroed(own, player, row);
        // Named corners: both loop bounds AND the player-vs-row stride/axis in one assertion.
        ck_eq((uint32_t)own.progress_at(0, 0).available, 0u,
              "T1: progress[0][0].available = 0 -- outer inv bound `CMP ...,0x12c` @0x00455b79, "
              "inner j bound `CMP ...,0x8` @0x00455bc1");
        ck_eq((uint32_t)own.progress_at(7, 299).f3, 0u,
              "T1: progress[7][299].f3 = 0 -- LAST player x LAST row, proves the 900-byte row stride "
              "(300*3) puts player on the OUTER byte-address axis, not row, 0x00455bd1-0x00455bd9");
    }

    // =================================================================================================
    // T2 -- the SECOND outer loop, Projects[0..99] (`CMP [ebp-0x18],0x64` @0x00455c80), plus its
    // nested resource[0..6] loop (`CMP [ebp-0x1c],0x7` @0x00455cd4). Also pins the two fields the
    // .asm does NOT touch: name (offset 4) and icon (offset 8) must survive at their seeded sentinels.
    // =================================================================================================
    {
        fx.reset();
        sim_store      own = fx.store();
        const sim_view v   = fx.view();

        for (int32_t p = 0; p < 100; ++p) {
            cfg_project &proj = own.cfg_project_at(p);
            proj.invention    = p * 10 + 1;
            proj.name         = p * 10 + 2; // NOT touched by the reset -- sentinel, must survive
            proj.icon         = p * 10 + 3; // NOT touched by the reset -- sentinel, must survive
            proj.type         = p * 10 + 4;
            proj.build_time   = (double)p * 1.5 + 1.0;
            for (int32_t r = 0; r < 7; ++r) {
                proj.resource[r].id  = (uint32_t)(p * 100 + r * 2 + 1);
                proj.resource[r].val = p * 100 + r * 2 + 2;
            }
        }

        detail::tech_tables_reset(v, own);

        for (int32_t p = 0; p < 100; ++p) {
            const cfg_project &proj = own.cfg_project_at(p);
            char               m1[96], m2[96], m3[112], m4[128], m5[128];
            std::snprintf(m1, sizeof(m1), "T2: Projects[%d].invention = 0, 0x00455c93", p);
            std::snprintf(m2, sizeof(m2), "T2: Projects[%d].type = 0, 0x00455ca4", p);
            std::snprintf(m3, sizeof(m3), "T2: Projects[%d].build_time = 0.0, 0x00455cb5/0x00455cbf", p);
            std::snprintf(m4, sizeof(m4),
                          "T2: Projects[%d].name (offset 4) NOT touched by the reset -- sentinel "
                          "survives, per sim_table_resets.h",
                          p);
            std::snprintf(m5, sizeof(m5),
                          "T2: Projects[%d].icon (offset 8) NOT touched by the reset -- sentinel "
                          "survives, per sim_table_resets.h",
                          p);
            ck_eq((uint32_t)proj.invention, 0u, m1);
            ck_eq((uint32_t)proj.type, 0u, m2);
            ck_eq_d(proj.build_time, 0.0, m3);
            ck_eq((uint32_t)proj.name, (uint32_t)(p * 10 + 2), m4);
            ck_eq((uint32_t)proj.icon, (uint32_t)(p * 10 + 3), m5);

            for (int32_t r = 0; r < 7; ++r) {
                char mi[128], mv[128];
                std::snprintf(mi, sizeof(mi),
                              "T2: Projects[%d].resource[%d].id = UNDEFINED(0), 0x00455cec", p, r);
                std::snprintf(mv, sizeof(mv), "T2: Projects[%d].resource[%d].val = 0, 0x00455d05", p, r);
                ck_eq((uint32_t)proj.resource[r].id, 0u, mi);
                ck_eq((uint32_t)proj.resource[r].val, 0u, mv);
            }
        }
        // Named corners.
        ck_eq((uint32_t)own.cfg_project_at(0).invention, 0u,
              "T2: outer loop's FIRST entry (p=0) invention = 0 -- bound `CMP ...,0x64` @0x00455c80");
        ck_eq((uint32_t)own.cfg_project_at(99).invention, 0u,
              "T2: outer loop's LAST entry (p=99) invention = 0 -- bound `CMP ...,0x64` @0x00455c80");
        ck_eq((uint32_t)own.cfg_project_at(0).resource[0].id, 0u,
              "T2: resource loop's FIRST slot (p=0, r=0) id = 0 -- bound `CMP ...,0x7` @0x00455cd4");
        ck_eq((uint32_t)own.cfg_project_at(99).resource[6].val, 0u,
              "T2: resource loop's LAST slot (p=99, r=6) val = 0 -- bound `CMP ...,0x7` @0x00455cd4");
    }

    // =================================================================================================
    // T3 -- the THIRD outer loop, Upgrades[0..99] INCLUSIVE (`CMP [ebp-0x18],0x64` @0x00455d21) -- 100
    // writes against a canonical 99-entry array (mh_addrs.gen.h: cfg_final_struct_Upgrade[99]). This
    // is the documented PRESERVE-BUG (sim_table_resets.h): entry 99 (the 100th, ONE PAST the canonical
    // bound) IS written, and this test asserts that BUGGY behaviour -- it must NOT be "fixed" to
    // exclude entry 99, even though that is what a 99-entry array would suggest is correct.
    //
    // REQUIRES sim_test_support.h's `cfg_upgrades` sized to >= 100 -- see this function's own leading
    // comment and the report for why this is not optional for any case in this file, not only T3.
    // =================================================================================================
    {
        fx.reset();
        sim_store      own = fx.store();
        const sim_view v   = fx.view();

        for (int32_t u = 0; u < 100; ++u) {
            cfg_upgrade &up = own.cfg_upgrade_at(u);
            up.invention    = u * 20 + 1;
            up.type         = (uint32_t)(u * 20 + 2);
            for (int32_t k = 0; k < 64; ++k) up.objects[k] = (uint8_t)(((u * 3 + k + 1) % 250) + 1);
            up.energy     = u * 20 + 3;
            up.step_speed = (double)u * 1.25 + 1.0;
            up.range_min  = u * 20 + 4;
            up.range_max  = u * 20 + 5;
            up.missing    = u * 20 + 6;
            up.fire_range = u * 20 + 7;
            up.power      = u * 20 + 8;
        }

        detail::tech_tables_reset(v, own);

        for (int32_t u = 0; u < 100; ++u) {
            const cfg_upgrade &up = own.cfg_upgrade_at(u);
            char               m1[96], m2[96], m3[96], m4[112], m5[96], m6[96], m7[96], m8[112], m9[96];
            std::snprintf(m1, sizeof(m1), "T3: Upgrades[%d].invention = 0, 0x00455d31", u);
            std::snprintf(m2, sizeof(m2), "T3: Upgrades[%d].type = UNIT(0), 0x00455d3f", u);
            std::snprintf(m3, sizeof(m3), "T3: Upgrades[%d].energy = 0, 0x00455d79", u);
            std::snprintf(m4, sizeof(m4), "T3: Upgrades[%d].step_speed = 0.0, 0x00455d87/0x00455d91", u);
            std::snprintf(m5, sizeof(m5), "T3: Upgrades[%d].range_min = 0, 0x00455d9f", u);
            std::snprintf(m6, sizeof(m6), "T3: Upgrades[%d].range_max = 0, 0x00455dad", u);
            std::snprintf(m7, sizeof(m7), "T3: Upgrades[%d].missing = 0, 0x00455dbb", u);
            std::snprintf(m8, sizeof(m8), "T3: Upgrades[%d].fire_range = 0, 0x00455dd7", u);
            std::snprintf(m9, sizeof(m9), "T3: Upgrades[%d].power = 0, 0x00455dc9", u);
            ck_eq((uint32_t)up.invention, 0u, m1);
            ck_eq((uint32_t)up.type, 0u, m2);
            ck_eq((uint32_t)up.energy, 0u, m3);
            ck_eq_d(up.step_speed, 0.0, m4);
            ck_eq((uint32_t)up.range_min, 0u, m5);
            ck_eq((uint32_t)up.range_max, 0u, m6);
            ck_eq((uint32_t)up.missing, 0u, m7);
            ck_eq((uint32_t)up.fire_range, 0u, m8);
            ck_eq((uint32_t)up.power, 0u, m9);

            char mo[128];
            std::snprintf(mo, sizeof(mo), "T3: Upgrades[%d].objects[0..63] all zeroed, 0x00455d69", u);
            ck(bytes_all_zero(up.objects, sizeof(up.objects)), mo);
        }
        // Named corners, including the PRESERVE-BUG entry.
        ck_eq((uint32_t)own.cfg_upgrade_at(0).invention, 0u,
              "T3: outer loop's FIRST entry (u=0) invention = 0 -- bound `CMP ...,0x64` @0x00455d21");
        ck_eq((uint32_t)own.cfg_upgrade_at(0).objects[0], 0u,
              "T3: objects loop's FIRST slot (u=0, k=0) zeroed -- bound `CMP ...,0x10` @0x00455d54");
        ck_eq((uint32_t)own.cfg_upgrade_at(98).fire_range, 0u,
              "T3: LAST CANONICAL entry (u=98, cfg_final_struct_Upgrade[99]'s last real slot) "
              "fire_range = 0 -- bound `CMP ...,0x64` @0x00455d21");
        ck_eq((uint32_t)own.cfg_upgrade_at(99).objects[63], 0u,
              "T3: PRESERVE-BUG -- entry 99 (the 100th write) objects[63] IS written, ONE PAST the "
              "canonical 99-entry cfg_final_struct_Upgrade array (mh_addrs.gen.h) -- NOT clamped in "
              "the original; bound `CMP ...,0x64` @0x00455d21 takes the JL through u=99");
        ck_eq((uint32_t)own.cfg_upgrade_at(99).power, 0u,
              "T3: PRESERVE-BUG -- entry 99's power IS written too (the whole record, not just one "
              "field, gets the one-past-canonical write), 0x00455dc9");
    }
}

} // namespace mh::sim::test
