//
// sim_storage_cancel_pending_docked_selftest.cpp -- `simtest` offline oracle for
// llm_storage_cancel_pending_docked @0x0046ca99 (sim/sim_storage_cancel_pending_docked.h/.cpp,
// RI-SIM / SIM1D).
//
// This is the FIRST independent read of this body: the ledger row records it as armed-but-UNREACHED
// (zero live calls under the shadow arm) and it triggered no reimpl-verify review at all. Every case
// below was re-derived from the raw disassembly
// (tmp/decomp_sim/llm_storage_cancel_pending_docked_0046ca99.asm), not from the header banner's prose
// or the .cpp, though both were cross-checked and found to agree with the asm (no divergence found).
//
// EXPECTED BEHAVIOUR, re-derived from the .asm:
//   0x0046cac9: sub_id = buildings[player][building_index].sub_id (BYTE), read ONCE before the loop.
//   0x0046cada-0x0046caf6: for (i = 0; i < unit_storage[player][sub_id].docked_count; ++i)          [JL]
//   0x0046cb1d: unit_index = unit_storage[player][sub_id].docked_units[i]                    (DWORD, i*4)
//   0x0046cb39-0x0046cb41: if (units[player][unit_index].state == PARKED(0x1f))                     [JNZ]
//   0x0046cb4a: llm_unit_force_disembark(player, unit_index)  -- player passed as the SAME narrowed
//     16-bit value used for every row index (0x0046cb46 re-loads word [EBP-0x14]).
//   0x0046cafa/0x0046cb00: i++ and loop back UNCONDITIONALLY -- no break on a hit, every entry up to
//     docked_count is visited.
//   `player` is narrowed to uint16_t before every row-index multiply (0x0046cab6/0x0046cada/
//   0x0046cb02/0x0046cb26); `building_index` and `unit_index` stay full 32-bit width throughout.
//
#include "sim/sim_storage_cancel_pending_docked.h"

#include <vector>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

struct disembark_call {
    uint32_t player;
    int32_t  unit_index;
};
std::vector<disembark_call> g_disembark_calls;

// T9-only mutation hook: on the FIRST recorded call, if armed, mutate a building's sub_id in the
// fixture. Lets a case observe whether the function re-derives sub_id from the building record on
// each loop iteration (bug) or cached it once before the loop (correct, per the asm).
sim_fixture *g_mutate_fx             = nullptr;
uint32_t     g_mutate_player         = 0;
int32_t      g_mutate_building_index = 0;
uint8_t      g_mutate_sub_id_to      = 0;
bool         g_mutate_armed          = false;

void rec_unit_force_disembark(uint32_t player, int32_t unit_index) {
    g_disembark_calls.push_back({player, unit_index});
    if (g_mutate_armed && g_disembark_calls.size() == 1) {
        g_mutate_armed = false;
        g_mutate_fx->b(static_cast<int32_t>(g_mutate_player), g_mutate_building_index).sub_id =
            g_mutate_sub_id_to;
    }
}

const storage_cancel_pending_docked_calls g_calls = {
    &rec_unit_force_disembark,
};

void reset_recorders() {
    g_disembark_calls.clear();
    g_mutate_fx             = nullptr;
    g_mutate_player         = 0;
    g_mutate_building_index = 0;
    g_mutate_sub_id_to      = 0;
    g_mutate_armed          = false;
}

} // namespace

void run_storage_cancel_pending_docked_tests() {
    sim_fixture fx;

    // =================================================================================================
    // T1 -- single docked entry, unit IS PARKED(0x1f): the one call fires with (player, unit_index),
    // 0x0046cb39-0x0046cb4a.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        constexpr uint32_t PLAYER         = 5;
        constexpr int32_t  BUILDING_INDEX = 42;
        constexpr uint8_t  SUB_ID         = 11;
        constexpr int32_t  UNIT_INDEX     = 13;

        fx.b(PLAYER, BUILDING_INDEX).sub_id = SUB_ID;
        unit_storage &st                    = fx.storage[PLAYER * STORAGE_PER_PLAYER + SUB_ID];
        st.docked_count                     = 1;
        st.docked_units[0]                  = UNIT_INDEX;
        fx.u(PLAYER, UNIT_INDEX).state      = 0x1f;

        detail::storage_cancel_pending_docked(fx.view(), g_calls, PLAYER, BUILDING_INDEX);

        ck_eq((uint32_t)g_disembark_calls.size(), 1u,
              "T1: exactly one call for one PARKED docked entry, 0x0046cb39-0x0046cb4a");
        if (g_disembark_calls.size() == 1) {
            ck_eq(g_disembark_calls[0].player, PLAYER, "T1: call player == the (narrowed) player, 0x0046cb46");
            ck_eq((uint32_t)g_disembark_calls[0].unit_index, (uint32_t)UNIT_INDEX,
                  "T1: call unit_index == docked_units[0], 0x0046cb1d/0x0046cb43");
        }
    }

    // =================================================================================================
    // T2 -- state == 0x1e, one BELOW the PARKED sentinel: gate is exact equality, not >=, so no call.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        constexpr uint32_t PLAYER         = 1;
        constexpr int32_t  BUILDING_INDEX = 7;
        constexpr uint8_t  SUB_ID         = 4;
        constexpr int32_t  UNIT_INDEX     = 22;

        fx.b(PLAYER, BUILDING_INDEX).sub_id = SUB_ID;
        unit_storage &st                    = fx.storage[PLAYER * STORAGE_PER_PLAYER + SUB_ID];
        st.docked_count                     = 1;
        st.docked_units[0]                  = UNIT_INDEX;
        fx.u(PLAYER, UNIT_INDEX).state      = 0x1e;

        detail::storage_cancel_pending_docked(fx.view(), g_calls, PLAYER, BUILDING_INDEX);

        ck_eq((uint32_t)g_disembark_calls.size(), 0u,
              "T2: state 0x1e (PARKED - 1) -- no call, gate is == not >=, 0x0046cb39-0x0046cb41");
    }

    // =================================================================================================
    // T3 -- state == 0x20, one ABOVE the PARKED sentinel: gate is exact equality, not <=, so no call.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        constexpr uint32_t PLAYER         = 1;
        constexpr int32_t  BUILDING_INDEX = 7;
        constexpr uint8_t  SUB_ID         = 4;
        constexpr int32_t  UNIT_INDEX     = 22;

        fx.b(PLAYER, BUILDING_INDEX).sub_id = SUB_ID;
        unit_storage &st                    = fx.storage[PLAYER * STORAGE_PER_PLAYER + SUB_ID];
        st.docked_count                     = 1;
        st.docked_units[0]                  = UNIT_INDEX;
        fx.u(PLAYER, UNIT_INDEX).state      = 0x20;

        detail::storage_cancel_pending_docked(fx.view(), g_calls, PLAYER, BUILDING_INDEX);

        ck_eq((uint32_t)g_disembark_calls.size(), 0u,
              "T3: state 0x20 (PARKED + 1) -- no call, gate is == not <=, 0x0046cb39-0x0046cb41");
    }

    // =================================================================================================
    // T4 -- docked_count == 0: the loop body never runs at all, even though docked_units[0] names a
    // real PARKED unit -- pins the loop-bound compare being < (0x0046caf0/0x0046caf6 JL), not <=.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        constexpr uint32_t PLAYER         = 2;
        constexpr int32_t  BUILDING_INDEX = 9;
        constexpr uint8_t  SUB_ID         = 2;
        constexpr int32_t  UNIT_INDEX     = 30;

        fx.b(PLAYER, BUILDING_INDEX).sub_id = SUB_ID;
        unit_storage &st                    = fx.storage[PLAYER * STORAGE_PER_PLAYER + SUB_ID];
        st.docked_count                     = 0;
        st.docked_units[0]                  = UNIT_INDEX; // would be a hit if ever read
        fx.u(PLAYER, UNIT_INDEX).state      = 0x1f;       // PARKED -- proves it is never consulted

        detail::storage_cancel_pending_docked(fx.view(), g_calls, PLAYER, BUILDING_INDEX);

        ck_eq((uint32_t)g_disembark_calls.size(), 0u,
              "T4: docked_count==0 -- zero iterations, docked_units[0] never read, 0x0046caf0-0x0046caf6");
    }

    // =================================================================================================
    // T5 -- three entries, PARKED/not-PARKED/PARKED: the loop does NOT break on a hit (unconditional
    // i++ + JMP back at 0x0046cafa/0x0046cb00) -- both PARKED entries fire, the middle miss is skipped,
    // and the calls arrive in docked_units[] order.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        constexpr uint32_t PLAYER         = 6;
        constexpr int32_t  BUILDING_INDEX = 50;
        constexpr uint8_t  SUB_ID         = 20;
        constexpr int32_t  UNIT_A         = 10;
        constexpr int32_t  UNIT_B         = 40;
        constexpr int32_t  UNIT_C         = 70;

        fx.b(PLAYER, BUILDING_INDEX).sub_id = SUB_ID;
        unit_storage &st                    = fx.storage[PLAYER * STORAGE_PER_PLAYER + SUB_ID];
        st.docked_count                     = 3;
        st.docked_units[0]                  = UNIT_A;
        st.docked_units[1]                  = UNIT_B;
        st.docked_units[2]                  = UNIT_C;
        fx.u(PLAYER, UNIT_A).state          = 0x1f; // hit
        fx.u(PLAYER, UNIT_B).state          = 0x05; // miss
        fx.u(PLAYER, UNIT_C).state          = 0x1f; // hit

        detail::storage_cancel_pending_docked(fx.view(), g_calls, PLAYER, BUILDING_INDEX);

        ck_eq((uint32_t)g_disembark_calls.size(), 2u,
              "T5: 2 of 3 entries PARKED -- the miss does not stop the loop, 0x0046cafa/0x0046cb00");
        if (g_disembark_calls.size() == 2) {
            ck_eq((uint32_t)g_disembark_calls[0].unit_index, (uint32_t)UNIT_A,
                  "T5: first call is docked_units[0] (the middle miss is skipped, not visited-and-passed)");
            ck_eq((uint32_t)g_disembark_calls[1].unit_index, (uint32_t)UNIT_C,
                  "T5: second call is docked_units[2], preserving iteration order");
        }
    }

    // =================================================================================================
    // T6 -- the storage row is selected by buildings[player][building_index].SUB_ID, never by
    // building_index itself. Seed BOTH storage[building_index] (the wrong-if-confused row) and
    // storage[sub_id] (the correct row) with DIFFERENT PARKED hits; only the sub_id row's hit must
    // fire. Pins 0x0046cac9 (sub_id read) feeding 0x0046cada/0x0046cb02 (row base), not building_index.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        constexpr uint32_t PLAYER                 = 3;
        constexpr int32_t  BUILDING_INDEX         = 6;  // deliberately ALSO a valid storage slot number
        constexpr uint8_t  SUB_ID                 = 19; // the REAL row, distinct from building_index
        constexpr int32_t  UNIT_IF_INDEX_CONFUSED = 77;
        constexpr int32_t  UNIT_IF_SUB_ID_USED    = 55;

        fx.b(PLAYER, BUILDING_INDEX).sub_id = SUB_ID;

        // The row a building_index-confused translation would read from.
        unit_storage &wrong_row                    = fx.storage[PLAYER * STORAGE_PER_PLAYER + BUILDING_INDEX];
        wrong_row.docked_count                     = 1;
        wrong_row.docked_units[0]                  = UNIT_IF_INDEX_CONFUSED;
        fx.u(PLAYER, UNIT_IF_INDEX_CONFUSED).state = 0x1f;

        // The row the correct translation reads from (buildings[..].sub_id == 19).
        unit_storage &right_row                 = fx.storage[PLAYER * STORAGE_PER_PLAYER + SUB_ID];
        right_row.docked_count                  = 1;
        right_row.docked_units[0]               = UNIT_IF_SUB_ID_USED;
        fx.u(PLAYER, UNIT_IF_SUB_ID_USED).state = 0x1f;

        detail::storage_cancel_pending_docked(fx.view(), g_calls, PLAYER, BUILDING_INDEX);

        ck_eq((uint32_t)g_disembark_calls.size(), 1u,
              "T6: exactly one call -- only the sub_id row's docked list is walked, 0x0046cac9");
        if (g_disembark_calls.size() == 1) {
            ck_eq((uint32_t)g_disembark_calls[0].unit_index, (uint32_t)UNIT_IF_SUB_ID_USED,
                  "T6: the call uses the SUB_ID row's unit, not the building_index row's, 0x0046cada/0x0046cb02");
        }
    }

    // =================================================================================================
    // T7 -- `player` is narrowed to its low 16 bits before every row index (0x0046cab6). Pass a raw
    // player value with garbage set above bit 15; the function must behave exactly as though only the
    // low 16 bits were given (row lookups AND the value handed to the callee).
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        constexpr uint32_t PLAYER_LOW16   = 5;
        constexpr uint32_t PLAYER_RAW     = 0x00070005u; // low 16 bits == 5, garbage above bit 15
        constexpr int32_t  BUILDING_INDEX = 13;
        constexpr uint8_t  SUB_ID         = 8;
        constexpr int32_t  UNIT_INDEX     = 61;

        fx.b(static_cast<int32_t>(PLAYER_LOW16), BUILDING_INDEX).sub_id = SUB_ID;
        unit_storage &st                                                = fx.storage[PLAYER_LOW16 * STORAGE_PER_PLAYER + SUB_ID];
        st.docked_count                                                 = 1;
        st.docked_units[0]                                              = UNIT_INDEX;
        fx.u(static_cast<int32_t>(PLAYER_LOW16), UNIT_INDEX).state      = 0x1f;

        detail::storage_cancel_pending_docked(fx.view(), g_calls, PLAYER_RAW, BUILDING_INDEX);

        ck_eq((uint32_t)g_disembark_calls.size(), 1u,
              "T7: raw player with high garbage bits still resolves the low-16 row, 0x0046cab6");
        if (g_disembark_calls.size() == 1) {
            ck_eq(g_disembark_calls[0].player, PLAYER_LOW16,
                  "T7: callee receives the NARROWED player (5), not the raw value, 0x0046cb46");
            ck_eq((uint32_t)g_disembark_calls[0].unit_index, (uint32_t)UNIT_INDEX,
                  "T7: unit_index found via the correctly-narrowed row");
        }
    }

    // =================================================================================================
    // T8 -- `unit_index` (docked_units[i]) is used at FULL 32-bit width, never narrowed to a byte or
    // word: 325 (0x145) does not fit in a byte, so a translation that truncated it would land on a
    // different (miss) unit and/or pass the wrong value to the callee.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        constexpr uint32_t PLAYER         = 2;
        constexpr int32_t  BUILDING_INDEX = 30;
        constexpr uint8_t  SUB_ID         = 14;
        constexpr int32_t  UNIT_INDEX     = 325; // 0x145 -- exceeds a byte

        fx.b(PLAYER, BUILDING_INDEX).sub_id = SUB_ID;
        unit_storage &st                    = fx.storage[PLAYER * STORAGE_PER_PLAYER + SUB_ID];
        st.docked_count                     = 1;
        st.docked_units[0]                  = UNIT_INDEX;
        fx.u(PLAYER, UNIT_INDEX).state      = 0x1f;

        detail::storage_cancel_pending_docked(fx.view(), g_calls, PLAYER, BUILDING_INDEX);

        ck_eq((uint32_t)g_disembark_calls.size(), 1u,
              "T8: the full-width unit_index (325) resolves to a real PARKED unit, 0x0046cb1d");
        if (g_disembark_calls.size() == 1) {
            ck_eq((uint32_t)g_disembark_calls[0].unit_index, (uint32_t)UNIT_INDEX,
                  "T8: callee receives unit_index==325 UNTRUNCATED (not &0xff==69, not &0xffff), 0x0046cb43");
        }
    }

    // =================================================================================================
    // T9 -- ORDER: sub_id is read from buildings[player][building_index] ONCE, before the loop starts
    // (0x0046cac9), and every subsequent row access reuses that cached value -- it is never re-derived
    // from the building record inside the loop. The first callback mutates the building's sub_id to
    // point at an (empty, docked_count==0) row; the SECOND docked entry must still fire, which is only
    // possible if the row lookup for i==1 still uses the ORIGINAL sub_id.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        constexpr uint32_t PLAYER            = 4;
        constexpr int32_t  BUILDING_INDEX    = 21;
        constexpr uint8_t  SUB_ID_ORIGINAL   = 3;
        constexpr uint8_t  SUB_ID_MUTATED_TO = 17; // left at its reset() default: docked_count == 0
        constexpr int32_t  UNIT_FIRST        = 88;
        constexpr int32_t  UNIT_SECOND       = 99;

        fx.b(PLAYER, BUILDING_INDEX).sub_id = SUB_ID_ORIGINAL;
        unit_storage &st                    = fx.storage[PLAYER * STORAGE_PER_PLAYER + SUB_ID_ORIGINAL];
        st.docked_count                     = 2;
        st.docked_units[0]                  = UNIT_FIRST;
        st.docked_units[1]                  = UNIT_SECOND;
        fx.u(PLAYER, UNIT_FIRST).state      = 0x1f;
        fx.u(PLAYER, UNIT_SECOND).state     = 0x1f;

        g_mutate_fx             = &fx;
        g_mutate_player         = PLAYER;
        g_mutate_building_index = BUILDING_INDEX;
        g_mutate_sub_id_to      = SUB_ID_MUTATED_TO;
        g_mutate_armed          = true;

        detail::storage_cancel_pending_docked(fx.view(), g_calls, PLAYER, BUILDING_INDEX);

        ck_eq((uint32_t)g_disembark_calls.size(), 2u,
              "T9: BOTH entries fire -- sub_id cached once before the loop, 0x0046cac9, not re-read per "
              "iteration (a re-read would see docked_count==0 on the mutated row and stop after entry 0)");
        if (g_disembark_calls.size() == 2) {
            ck_eq((uint32_t)g_disembark_calls[0].unit_index, (uint32_t)UNIT_FIRST,
                  "T9: first call is docked_units[0] from the ORIGINAL sub_id row");
            ck_eq((uint32_t)g_disembark_calls[1].unit_index, (uint32_t)UNIT_SECOND,
                  "T9: second call is docked_units[1], still from the ORIGINAL sub_id row after the mutation");
        }
    }
}

} // namespace mh::sim::test
