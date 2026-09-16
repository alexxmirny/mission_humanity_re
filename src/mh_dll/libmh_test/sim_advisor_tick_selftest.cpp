//
// sim_advisor_tick_selftest.cpp -- `simtest` offline oracle for llm_strat_advisor_tick @0x0049b5b0
// (sim/resid/sim_advisor_tick.h/.cpp, RI-SIM sim_resid batch -- ONE OF ONLY TWO PER-TICK members of
// that batch).
//
// NO SHADOW SITE (see the header banner): both advisories reach game_ui_PrintTextMessage, an
// EFFECTFUL WALL a shadow arm would DOUBLE-FIRE. This offline oracle (net_selftest simtest) is the
// ONLY verification (proof:OFFLINE).
//
// EXPECTED BEHAVIOUR, from the DISASSEMBLY (tmp/decomp_sim_resid/llm_strat_advisor_tick_0049b5b0.asm)
// -- the .asm is the spec, never the .c beside it (the .c has silently lied elsewhere in this
// project):
//   0x0049b5cf-0x0049b5e1: due-time gate -- skip the WHOLE body (including the CUR_PLAYER relatch)
//     unless ADVISOR_NEXT_TIME + ADVISOR_DUE_DELAY < now.
//   0x0049b5e7-0x0049b5ed: CUR_PLAYER <- PlayerSide, a RAW 16-BIT COPY (not sign-extended), runs
//     once the gate passes, before either advisory.
//   0x0049b5fb-0x0049b60b: ADVISOR_PHASE selects exactly one of two advisories: ==0 -> phase 0,
//     ==1 -> phase 1, any OTHER value (unreachable given the mod-2 advance below, but a real branch)
//     runs NEITHER.
//   PHASE 0 (0x0049b615-0x0049b693): scans player_resources[player][0..9] against
//     storage_stats[player].cap_prev[0..9]; cap_prev[i] < player_resource[i] for ANY i fires the
//     "storage overflow" message (text id 0x324).
//   PHASE 1 (0x0049b698-0x0049b8b6): 0x0049b698-0x0049b6ba unconditionally stamps CUR_INDEX=1 /
//     CUR_BUILDING=&buildings[player][1] BEFORE the scan even starts. The scan walks
//     buildings[player][1..], budget = buildings[player][0].index (a per-player COUNT, not an
//     index): 0x0049b6fe empty slot (building_id==0) is skipped WITHOUT spending budget;
//     0x0049b704-0x0049b707 spends budget the INSTANT building_id!=0, BEFORE the energy/built_flags
//     gate (PRESERVE-BUG: an occupied-but-dead slot still counts). 0x0049b717/0x0049b722 requires
//     energy>0.0 AND built_flags==3. 0x0049b762-0x0049b7ab: CONSTRUCTION/UPGRADING/DISMANTLING/
//     CHARGE_STEP share one ratio = current_workers/builder_count, except UPGRADING
//     (0x0049b76d-0x0049b791) reads builder_count through the UPGRADE TARGET's own cfg record
//     (Building[Building[id].upgrade_index].builder_count); builder_count==0
//     (0x0049b7af/0x0049b7ca) is a SOFT skip (no ratio, not a hit) that still lets the scan continue
//     normally. 0x0049b7d1-0x0049b80e: any other state is gated by bldg_uses_workers(player,idx);
//     ratio = current_workers/Building[id].worker_count. 0x0049b818-0x0049b824: ratio < threshold ->
//     understaffed, pans the camera (0x0049b845 bldg_get_coords, then the classic Watcom
//     SAR/SHL/SBB/SAR /32 fine-to-tile conversion on BOTH col and row) and FORCES the scan to stop
//     (remaining=0) -- the FIRST hit wins. 0x0049b88c: if any hit was found, fires the "needs
//     workers" message (text id 0x321) AFTER the camera pan.
//   0x0049b8b6-0x0049b8e3: UNCONDITIONAL housekeeping, regardless of which (if any) advisory ran --
//     ADVISOR_PHASE = (ADVISOR_PHASE+1) % 2, ADVISOR_NEXT_TIME += ADVISOR_INTERVAL.
//
// PRESERVE-BUG cases (T9) assert the BUGGY result -- an occupied-but-dead slot silently consumes the
// per-player building budget even though it is never itself a staffing candidate, which can hide a
// real understaffed building sitting right behind it in the roster. Do not "fix" this in the .cpp.
//
#include "sim/resid/sim_advisor_tick.h"

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// llm_strat_bldg_state members this function reads -- same values sim_advisor_tick.cpp's own
// anonymous namespace uses; re-declared here per this project's per-TU convention.
constexpr uint16_t BLDG_STATE_CONSTRUCTION = 0x64;
constexpr uint16_t BLDG_STATE_CHARGE_STEP  = 0x6a;
constexpr uint16_t BLDG_STATE_DISMANTLING  = 0x6b;
constexpr uint16_t BLDG_STATE_UPGRADING    = 0x82;
constexpr uint8_t  BUILT_FLAGS_OPERATIONAL = 3;

// G_TEXT_PTRS indices the two floating messages use (0x0058509c / 0x00585090 in the raw assembly).
constexpr int32_t TEXT_ID_STORAGE_OVERFLOW = 0x324;
constexpr int32_t TEXT_ID_NEEDS_WORKERS    = 0x321;

const wchar_t g_overflow_text[]      = L"OVERFLOW";
const wchar_t g_needs_workers_text[] = L"NEEDS_WORKERS";

// ---- recorded outward calls ------------------------------------------------------------------------
int32_t g_seq = 0; // global call-order counter, shared by every mock below

int32_t     g_str_copy_calls = 0;
int32_t     g_str_copy_seq   = -1;
const void *g_str_copy_src   = nullptr;
const void *g_str_copy_dst   = nullptr;

int32_t     g_print_text_calls = 0;
int32_t     g_print_text_seq   = -1;
const void *g_print_text_arg   = nullptr;

struct UsesWorkersCall {
    uint32_t player;
    int32_t  idx;
};
std::vector<UsesWorkersCall> g_uses_workers_calls;
int32_t                      g_uses_workers_result = 0;
// T19 (order pin): when set, the mock mutates *g_mutate_fixture's building [g_mutate_player]
// [g_mutate_idx].current_workers to g_mutate_new_workers DURING the call, before returning --
// proving the caller reads current_workers AFTER this call returns, not a value cached earlier.
sim_fixture *g_mutate_fixture     = nullptr;
int32_t      g_mutate_player      = 0;
int32_t      g_mutate_idx         = 0;
uint16_t     g_mutate_new_workers = 0;

struct CoordsCall {
    uint16_t player;
    int32_t  idx;
};
std::vector<CoordsCall> g_coords_calls;
int32_t                 g_coords_seq     = -1;
int32_t                 g_coords_out_col = 0;
int32_t                 g_coords_out_row = 0;

void *stub_str_copy(void *src, void *dst) {
    g_str_copy_calls++;
    g_str_copy_seq = g_seq++;
    g_str_copy_src = src;
    g_str_copy_dst = dst;
    return dst;
}
uint32_t stub_print_text(void *text) {
    g_print_text_calls++;
    g_print_text_seq = g_seq++;
    g_print_text_arg = text;
    return 0;
}
int32_t stub_uses_workers(uint32_t player, int32_t building_index) {
    g_uses_workers_calls.push_back({player, building_index});
    if (g_mutate_fixture != nullptr && (int32_t)player == g_mutate_player &&
        building_index == g_mutate_idx) {
        g_mutate_fixture->b(g_mutate_player, g_mutate_idx).current_workers = g_mutate_new_workers;
    }
    return g_uses_workers_result;
}
void stub_get_coords(uint16_t player, int32_t building_index, int32_t *out_col, int32_t *out_row) {
    g_coords_calls.push_back({player, building_index});
    g_coords_seq = g_seq++;
    *out_col     = g_coords_out_col;
    *out_row     = g_coords_out_row;
}

const advisor_tick_calls g_calls = {stub_str_copy, stub_print_text, stub_uses_workers,
                                    stub_get_coords};

// CONFIGURATION the stubs read to decide their behaviour (g_uses_workers_result, the g_mutate_*
// group, g_coords_out_col/row) is DELIBERATELY NOT reset here -- a test sets it AFTER fx.reset()
// (via reset_mock_config() below) but BEFORE run(), and run() must not clobber it. Only the
// RECORDING state (what the stubs observed) is cleared, immediately before every call.
void clear_calls() {
    g_seq              = 0;
    g_str_copy_calls   = 0;
    g_str_copy_seq     = -1;
    g_str_copy_src     = nullptr;
    g_str_copy_dst     = nullptr;
    g_print_text_calls = 0;
    g_print_text_seq   = -1;
    g_print_text_arg   = nullptr;
    g_uses_workers_calls.clear();
    g_coords_calls.clear();
    g_coords_seq = -1;
}

// The CONFIGURATION half -- defaults every stub-input back to its neutral value. Call this once per
// test case, right after fx.reset(), BEFORE seeding any test-specific config (g_uses_workers_result,
// g_mutate_*, g_coords_out_col/row) and before run().
void reset_mock_config() {
    g_uses_workers_result = 0;
    g_mutate_fixture      = nullptr;
    g_mutate_player       = 0;
    g_mutate_idx          = 0;
    g_mutate_new_workers  = 0;
    g_coords_out_col      = 0;
    g_coords_out_row      = 0;
}

void run(sim_fixture &fx, double now) {
    clear_calls();
    sim_store own = fx.store();
    detail::advisor_tick(fx.view(), own, g_calls, now);
}

} // namespace

void run_advisor_tick_tests() {
    sim_fixture fx;

    // =================================================================================================
    // T1 -- due-time gate, SKIP arm, exact boundary: NEXT_TIME+DUE_DELAY == now is NOT "< now", so the
    // WHOLE body (incl. the CUR_PLAYER relatch and the housekeeping) is skipped, 0x0049b5db/0x0049b5e1
    // JNC.
    // =================================================================================================
    {
        fx.reset();
        reset_mock_config();
        fx.advisor_next_time = 10.0;
        fx.advisor_due_delay = 5.0; // sum = 15.0
        fx.advisor_interval  = 3.0;
        fx.advisor_phase     = 7; // sentinel, must stay untouched
        fx.player_side       = 9;
        fx.view_cur_player   = 42; // sentinel, distinct from player_side -- must stay untouched

        run(fx, /*now=*/15.0);

        ck_eq(g_str_copy_calls, 0u, "T1: gate skip -- no w_str_copy, 0x0049b5e1 JNC");
        ck_eq(g_print_text_calls, 0u, "T1: gate skip -- no ui_print_text_message, same gate");
        ck_eq((uint32_t)fx.view_cur_player, 42u,
              "T1: gate skip -- CUR_PLAYER relatch (0x0049b5e7-0x0049b5ed) never runs");
        ck_eq((uint32_t)fx.advisor_phase, 7u,
              "T1: gate skip -- housekeeping (0x0049b8b6 INC) never runs, phase untouched");
        ck_eq_d(fx.advisor_next_time, 10.0,
                "T1: gate skip -- ADVISOR_NEXT_TIME untouched, 0x0049b8d7-0x0049b8e3 never runs");
    }

    // =================================================================================================
    // T2 -- due-time gate, RUN arm (sum < now), combined with the "OTHER phase value" branch
    // (0x0049b60b, a real branch though unreachable given the mod-2 advance): phase=7 selects
    // NEITHER advisory, yet the CUR_PLAYER relatch and the housekeeping still run unconditionally.
    // =================================================================================================
    {
        fx.reset();
        reset_mock_config();
        fx.advisor_next_time = 10.0;
        fx.advisor_due_delay = 5.0; // sum = 15.0, now(16.0) is clearly past it
        fx.advisor_interval  = 3.0;
        fx.advisor_phase     = 7;
        fx.player_side       = 9;
        fx.view_cur_player   = 42; // sentinel, must become player_side once the gate passes

        run(fx, /*now=*/16.0);

        ck_eq((uint32_t)fx.view_cur_player, 9u,
              "T2: gate RUN -- CUR_PLAYER relatched from PlayerSide, 0x0049b5e7-0x0049b5ed");
        ck_eq(g_str_copy_calls, 0u, "T2: ADVISOR_PHASE=7 (neither 0 nor 1) -- no advisory fires");
        ck_eq(g_print_text_calls, 0u, "T2: same -- ui_print_text_message not called either");
        ck_eq((uint32_t)g_coords_calls.size(), 0u, "T2: same -- bldg_get_coords not called either");
        ck_eq((uint32_t)fx.advisor_phase, 0u,
              "T2: housekeeping still runs -- (7+1)%2=0, 0x0049b8bc-0x0049b8d1 IDIV EBX(2)");
        ck_eq_d(fx.advisor_next_time, 13.0,
                "T2: housekeeping still runs -- NEXT_TIME=10.0+INTERVAL(3.0)=13.0, 0x0049b8d7-0x0049b8e3");
    }

    // =================================================================================================
    // T3 -- CUR_PLAYER <- PlayerSide is a RAW 16-BIT COPY (0x0049b5e7 MOV AX,[PlayerSide] / 0x0049b5ed
    // MOV [CUR_PLAYER],AX), not a sign-extending one: PlayerSide=-1 (int16_t) must land as 0xFFFF, not
    // as a sign-extended 0xFFFFFFFF truncated back down (same numeric result here, but a widening bug
    // that read PlayerSide as unsigned 16 first would also give 0xFFFF -- the real regression this
    // catches is a translation that narrows through a SIGNED 32-bit intermediate incorrectly, e.g.
    // clamping negative PlayerSide to 0).
    // =================================================================================================
    {
        fx.reset();
        reset_mock_config();
        fx.advisor_next_time = 0.0;
        fx.advisor_due_delay = 0.0;
        fx.advisor_interval  = 1.0;
        fx.advisor_phase     = 7; // neither advisory, isolates the relatch
        fx.player_side       = -1;

        run(fx, /*now=*/1.0);

        ck_eq((uint32_t)fx.view_cur_player, 0xFFFFu,
              "T3: PlayerSide=-1 -> CUR_PLAYER=0xFFFF, raw 16-bit copy, 0x0049b5e7-0x0049b5ed");
    }

    // =================================================================================================
    // T4 -- PHASE 0, boundary NEGATIVE arm: cap_prev[i] == player_resource[i] for every slot (equal,
    // not less) -- the comparison is strict '<', so equality across the whole 10-slot scan must NOT
    // fire the overflow message.
    // =================================================================================================
    {
        fx.reset();
        reset_mock_config();
        fx.advisor_next_time         = 0.0;
        fx.advisor_due_delay         = 0.0;
        fx.advisor_interval          = 5.0;
        fx.advisor_phase             = 0;
        fx.player_side               = 3;
        fx.floating_msg_queue_active = 9; // sentinel, must stay untouched
        for (int32_t i = 0; i < PLAYER_RESOURCE_SLOTS; ++i) {
            fx.storage_stats_rows[3].cap_prev[i]               = 100;
            fx.player_resources[3 * PLAYER_RESOURCE_SLOTS + i] = 100;
        }

        run(fx, /*now=*/1.0);

        ck_eq(g_str_copy_calls, 0u, "T4: cap_prev==resource everywhere -- strict '<' does not fire");
        ck_eq(g_print_text_calls, 0u, "T4: same -- ui_print_text_message not called");
        ck_eq((uint32_t)fx.floating_msg_queue_active, 9u, "T4: FLOATING_MSG_QUEUE_ACTIVE left untouched");
        ck_eq((uint32_t)fx.advisor_phase, 1u, "T4: housekeeping -- phase 0->1, 0x0049b8bc-0x0049b8d1");
        ck_eq_d(fx.advisor_next_time, 5.0, "T4: housekeeping -- NEXT_TIME 0.0+5.0=5.0");
    }

    // =================================================================================================
    // T5 -- PHASE 0, overflow at the LAST slot (i=9) only: pins the loop's UPPER bound (a '<9' bound
    // would miss this). Also pins the message plumbing: text id 0x324, the str_copy->print_text ORDER,
    // and the ASSIGNMENT (not OR) into FLOATING_MSG_QUEUE_ACTIVE.
    // =================================================================================================
    {
        fx.reset();
        reset_mock_config();
        fx.advisor_next_time         = 0.0;
        fx.advisor_due_delay         = 0.0;
        fx.advisor_interval          = 5.0;
        fx.advisor_phase             = 0;
        fx.player_side               = 3;
        fx.floating_msg_queue_active = 9; // sentinel, must become exactly 1
        for (int32_t i = 0; i < PLAYER_RESOURCE_SLOTS; ++i) {
            fx.storage_stats_rows[3].cap_prev[i]               = 100;
            fx.player_resources[3 * PLAYER_RESOURCE_SLOTS + i] = 100;
        }
        fx.storage_stats_rows[3].cap_prev[9]               = 50;
        fx.player_resources[3 * PLAYER_RESOURCE_SLOTS + 9] = 51; // 50 < 51 -- overflow only here
        fx.text_ptrs[TEXT_ID_STORAGE_OVERFLOW]             = g_overflow_text;

        run(fx, /*now=*/1.0);

        ck_eq((uint32_t)g_str_copy_calls, 1u, "T5: overflow at slot i=9 (loop upper bound), fires once");
        ck(g_str_copy_src == (const void *)g_overflow_text,
           "T5: w_str_copy src = text_ptrs[0x324], 0x0049b675/0x0049b67a");
        ck(g_str_copy_dst == (const void *)fx.text_scratch.data(),
           "T5: w_str_copy dst = own.text_scratch()");
        ck_eq((uint32_t)g_print_text_calls, 1u, "T5: ui_print_text_message fires once, 0x0049b68e");
        ck(g_print_text_arg == (const void *)fx.text_scratch.data(),
           "T5: ui_print_text_message arg = the SAME text_scratch buffer w_str_copy just filled");
        ck(g_str_copy_seq >= 0 && g_print_text_seq > g_str_copy_seq,
           "T5: ORDER -- w_str_copy (0x0049b67a) before ui_print_text_message (0x0049b68e)");
        ck_eq((uint32_t)fx.floating_msg_queue_active, 1u,
              "T5: FLOATING_MSG_QUEUE_ACTIVE = 1, an ASSIGNMENT not an OR, 0x0049b67f");
        ck_eq((uint32_t)fx.advisor_phase, 1u, "T5: housekeeping -- phase 0->1");
        ck_eq_d(fx.advisor_next_time, 5.0, "T5: housekeeping -- NEXT_TIME advances by INTERVAL");
    }

    // =================================================================================================
    // T6 -- PHASE 0, overflow at the FIRST slot (i=0) only: pins the loop's LOWER bound (a loop
    // starting at i=1 would miss this).
    // =================================================================================================
    {
        fx.reset();
        reset_mock_config();
        fx.advisor_next_time = 0.0;
        fx.advisor_due_delay = 0.0;
        fx.advisor_interval  = 5.0;
        fx.advisor_phase     = 0;
        fx.player_side       = 3;
        for (int32_t i = 0; i < PLAYER_RESOURCE_SLOTS; ++i) {
            fx.storage_stats_rows[3].cap_prev[i]               = 100;
            fx.player_resources[3 * PLAYER_RESOURCE_SLOTS + i] = 100;
        }
        fx.storage_stats_rows[3].cap_prev[0]               = 50;
        fx.player_resources[3 * PLAYER_RESOURCE_SLOTS + 0] = 51; // 50 < 51 -- overflow only at i=0

        run(fx, /*now=*/1.0);

        ck_eq((uint32_t)g_str_copy_calls, 1u, "T6: overflow at slot i=0 (loop lower bound), fires once");
        ck_eq((uint32_t)g_print_text_calls, 1u, "T6: message plumbing fires from the FIRST slot too");
    }

    // =================================================================================================
    // T7 -- PHASE 1, the UNCONDITIONAL pre-scan store (0x0049b698-0x0049b6ba): CUR_INDEX=1 and
    // CUR_BUILDING=&buildings[player][1] are stamped even when buildings[player][0].index==0 (zero
    // live buildings), so the scan loop body never runs at all.
    // =================================================================================================
    {
        fx.reset();
        reset_mock_config();
        fx.advisor_next_time = 0.0;
        fx.advisor_due_delay = 0.0;
        fx.advisor_interval  = 5.0;
        fx.advisor_phase     = 1;
        fx.player_side       = 4;
        fx.b(4, 0).index     = 0; // COUNT = 0 -- the scan loop body never executes

        run(fx, /*now=*/1.0);

        ck_eq((uint32_t)fx.view_cur_index, 1u,
              "T7: CUR_INDEX=1 stamped unconditionally, 0x0049b698, even with zero live buildings");
        ck(fx.cur_building_ptr == &fx.b(4, 1),
           "T7: CUR_BUILDING=&buildings[player][1] stamped unconditionally, 0x0049b6a1-0x0049b6ba");
        ck_eq(g_str_copy_calls, 0u, "T7: no live buildings -- no understaffed message");
        ck_eq((uint32_t)fx.advisor_phase, 0u, "T7: housekeeping -- phase 1->0");
        ck_eq_d(fx.advisor_next_time, 5.0, "T7: housekeeping -- NEXT_TIME advances");
    }

    // =================================================================================================
    // T8 -- PHASE 1, empty-slot SKIP (0x0049b6fe JZ): an empty slot (building_id==0) is walked over
    // WITHOUT spending the per-player budget, so the scan reaches and reports the REAL understaffed
    // building right behind it. Also pins bldg_get_coords's argument, the fine-to-tile /32 conversion
    // on POSITIVE values (col=64->2, row=96->3, both exact multiples so no truncation ambiguity), and
    // the pan-before-message ORDER (0x0049b845 before 0x0049b892).
    // =================================================================================================
    {
        fx.reset();
        reset_mock_config();
        fx.advisor_next_time         = 0.0;
        fx.advisor_due_delay         = 0.0;
        fx.advisor_interval          = 5.0;
        fx.advisor_phase             = 1;
        fx.player_side               = 4;
        fx.advisor_staff_threshold   = 0.9;
        fx.floating_msg_queue_active = 9; // sentinel

        fx.b(4, 0).index = 1; // budget = 1 -- exactly ONE real building expected

        // idx=1: EMPTY slot -- seeded with values that WOULD look like an understaffed hit, to prove
        // they are never even examined.
        fx.b(4, 1).building_id = 0;
        fx.b(4, 1).energy      = 999.0;
        fx.b(4, 1).built_flags = BUILT_FLAGS_OPERATIONAL;
        fx.b(4, 1).state       = BLDG_STATE_CONSTRUCTION;

        // idx=2: the real (understaffed) hit, reached only by walking PAST the empty idx=1 without
        // spending the budget.
        fx.b(4, 2).building_id             = 77;
        fx.b(4, 2).energy                  = 1.0;
        fx.b(4, 2).built_flags             = BUILT_FLAGS_OPERATIONAL;
        fx.b(4, 2).state                   = BLDG_STATE_CONSTRUCTION;
        fx.b(4, 2).current_workers         = 1;
        fx.cfg_buildings[77].builder_count = 2; // ratio 1/2 = 0.5 < 0.9 threshold

        fx.text_ptrs[TEXT_ID_NEEDS_WORKERS] = g_needs_workers_text;
        g_coords_out_col                    = 64;
        g_coords_out_row                    = 96;

        run(fx, /*now=*/1.0);

        ck_eq((uint32_t)g_coords_calls.size(), 1u,
              "T8: exactly one bldg_get_coords call -- the empty idx=1 slot is never a candidate");
        ck_eq((uint32_t)g_coords_calls[0].idx, 2u,
              "T8: bldg_get_coords called for idx=2 (the empty idx=1 did not consume the budget), "
              "0x0049b6fe skip-without-decrement");
        ck_eq((uint32_t)fx.cam_pan_target_col, 2u, "T8: fine_to_tile(64) = 2, 0x0049b84a-0x0049b85d");
        ck_eq((uint32_t)fx.cam_pan_target_row, 3u, "T8: fine_to_tile(96) = 3, 0x0049b865-0x0049b87b");
        ck(g_str_copy_src == (const void *)g_needs_workers_text,
           "T8: w_str_copy src = text_ptrs[0x321], 0x0049b892/0x0049b898");
        ck(g_coords_seq >= 0 && g_str_copy_seq > g_coords_seq,
           "T8: ORDER -- camera pan (0x0049b845-0x0049b87b) before the message (0x0049b892)");
        ck_eq((uint32_t)fx.floating_msg_queue_active, 1u, "T8: FLOATING_MSG_QUEUE_ACTIVE = 1, 0x0049b8a2");
    }

    // =================================================================================================
    // T9 -- PRESERVE-BUG: an occupied-but-DEAD slot (building_id!=0 but energy<=0) still spends the
    // per-player budget (0x0049b704-0x0049b707 decrements BEFORE the energy/built_flags gate at
    // 0x0049b717/0x0049b722), so a real understaffed building sitting immediately behind it in the
    // roster is NEVER SCANNED. This is the shipped behaviour -- assert the buggy (no message) result,
    // do not "fix" it.
    // =================================================================================================
    {
        fx.reset();
        reset_mock_config();
        fx.advisor_next_time       = 0.0;
        fx.advisor_due_delay       = 0.0;
        fx.advisor_interval        = 5.0;
        fx.advisor_phase           = 1;
        fx.player_side             = 4;
        fx.advisor_staff_threshold = 0.9;

        fx.b(4, 0).index = 1; // budget = 1 -- consumed entirely by the dead slot below

        // idx=1: occupied but DEAD (energy<=0) -- fails the gate, but still spends the ONLY budget
        // slot.
        fx.b(4, 1).building_id = 55;
        fx.b(4, 1).energy      = 0.0; // NOT > 0.0 -- fails 0x0049b717
        fx.b(4, 1).built_flags = BUILT_FLAGS_OPERATIONAL;

        // idx=2: a genuinely understaffed building that the bug prevents from ever being reached.
        fx.b(4, 2).building_id             = 66;
        fx.b(4, 2).energy                  = 1.0;
        fx.b(4, 2).built_flags             = BUILT_FLAGS_OPERATIONAL;
        fx.b(4, 2).state                   = BLDG_STATE_CONSTRUCTION;
        fx.b(4, 2).current_workers         = 1;
        fx.cfg_buildings[66].builder_count = 2; // ratio 0.5 < 0.9 -- WOULD be a hit if ever reached

        run(fx, /*now=*/1.0);

        ck_eq((uint32_t)g_coords_calls.size(), 0u,
              "T9 PRESERVE-BUG: idx=2's real understaffed hit is never scanned -- the dead idx=1 slot "
              "consumed the only budget slot before the energy gate, 0x0049b704-0x0049b707");
        ck_eq(g_str_copy_calls, 0u, "T9 PRESERVE-BUG: no message fires despite a real understaffed "
                                    "building existing in the roster (shipped bug, not fixed here)");
    }

    // =================================================================================================
    // T10 -- the OTHER half of the operational gate: built_flags != 3 blocks even when energy passes.
    // =================================================================================================
    {
        fx.reset();
        reset_mock_config();
        fx.advisor_next_time       = 0.0;
        fx.advisor_due_delay       = 0.0;
        fx.advisor_interval        = 5.0;
        fx.advisor_phase           = 1;
        fx.player_side             = 4;
        fx.advisor_staff_threshold = 0.9;

        fx.b(4, 0).index = 1;

        fx.b(4, 1).building_id             = 10;
        fx.b(4, 1).energy                  = 5.0; // > 0.0, passes
        fx.b(4, 1).built_flags             = 1;   // != 3, fails 0x0049b722
        fx.b(4, 1).state                   = BLDG_STATE_CONSTRUCTION;
        fx.b(4, 1).current_workers         = 1;
        fx.cfg_buildings[10].builder_count = 2; // would be a 0.5 < 0.9 hit if the gate did not block

        run(fx, /*now=*/1.0);

        ck_eq(g_str_copy_calls, 0u,
              "T10: built_flags=1 (!=3) blocks despite energy>0.0 and an understaffed-looking ratio, "
              "0x0049b71e-0x0049b722");
    }

    // =================================================================================================
    // T11 -- builder_count==0 in the construction-group is a SOFT skip (0x0049b7af/0x0049b7ca): no
    // ratio is computed and it is NOT a hit, but (unlike T9's dead-slot bug) the scan continues
    // NORMALLY to the next slot with the budget spent exactly once.
    // =================================================================================================
    {
        fx.reset();
        reset_mock_config();
        fx.advisor_next_time       = 0.0;
        fx.advisor_due_delay       = 0.0;
        fx.advisor_interval        = 5.0;
        fx.advisor_phase           = 1;
        fx.player_side             = 4;
        fx.advisor_staff_threshold = 0.9;

        fx.b(4, 0).index = 2; // budget = 2 -- both slots below get examined

        fx.b(4, 1).building_id             = 20;
        fx.b(4, 1).energy                  = 1.0;
        fx.b(4, 1).built_flags             = BUILT_FLAGS_OPERATIONAL;
        fx.b(4, 1).state                   = BLDG_STATE_CONSTRUCTION;
        fx.cfg_buildings[20].builder_count = 0; // builder_count==0 -- soft skip, no ratio

        fx.b(4, 2).building_id             = 30;
        fx.b(4, 2).energy                  = 1.0;
        fx.b(4, 2).built_flags             = BUILT_FLAGS_OPERATIONAL;
        fx.b(4, 2).state                   = BLDG_STATE_CONSTRUCTION;
        fx.b(4, 2).current_workers         = 1;
        fx.cfg_buildings[30].builder_count = 2; // ratio 0.5 < 0.9 -- the real hit

        run(fx, /*now=*/1.0);

        ck_eq((uint32_t)g_coords_calls.size(), 1u,
              "T11: builder_count==0 at idx=1 is a soft skip -- exactly one hit total");
        ck_eq((uint32_t)g_coords_calls[0].idx, 2u,
              "T11: the hit is idx=2, reached normally after idx=1's soft skip, 0x0049b7af/0x0049b7ca");
    }

    // =================================================================================================
    // T12 -- construction-group ratio, boundary EQUAL: current_workers/builder_count == threshold
    // exactly (2/4 = 0.5 == 0.5) is NOT "< threshold", so this is NOT a hit.
    // =================================================================================================
    {
        fx.reset();
        reset_mock_config();
        fx.advisor_next_time       = 0.0;
        fx.advisor_due_delay       = 0.0;
        fx.advisor_interval        = 5.0;
        fx.advisor_phase           = 1;
        fx.player_side             = 4;
        fx.advisor_staff_threshold = 0.5;

        fx.b(4, 0).index                   = 1;
        fx.b(4, 1).building_id             = 40;
        fx.b(4, 1).energy                  = 1.0;
        fx.b(4, 1).built_flags             = BUILT_FLAGS_OPERATIONAL;
        fx.b(4, 1).state                   = BLDG_STATE_CONSTRUCTION;
        fx.b(4, 1).current_workers         = 2;
        fx.cfg_buildings[40].builder_count = 4; // ratio 2/4 = 0.5, == threshold, strict '<' fails

        run(fx, /*now=*/1.0);

        ck_eq(g_str_copy_calls, 0u,
              "T12: ratio==threshold exactly (0.5==0.5) is NOT understaffed, strict '<' at 0x0049b824");
    }

    // =================================================================================================
    // T13 -- construction-group ratio, boundary BELOW threshold: 1/4=0.25 < 0.5 IS understaffed. Also
    // pins fine_to_tile on a NEGATIVE col (-100 -> -3, C-style truncation toward zero, matching the
    // SAR/SHL/SBB/SAR trick) paired with a DIFFERENT positive row (200 -> 6), so a translation that
    // consumed the wrong pair or truncated toward -infinity disagrees with one or the other.
    // =================================================================================================
    {
        fx.reset();
        reset_mock_config();
        fx.advisor_next_time       = 0.0;
        fx.advisor_due_delay       = 0.0;
        fx.advisor_interval        = 5.0;
        fx.advisor_phase           = 1;
        fx.player_side             = 4;
        fx.advisor_staff_threshold = 0.5;

        fx.b(4, 0).index                   = 1;
        fx.b(4, 1).building_id             = 40;
        fx.b(4, 1).energy                  = 1.0;
        fx.b(4, 1).built_flags             = BUILT_FLAGS_OPERATIONAL;
        fx.b(4, 1).state                   = BLDG_STATE_CONSTRUCTION;
        fx.b(4, 1).current_workers         = 1;
        fx.cfg_buildings[40].builder_count = 4; // ratio 1/4 = 0.25 < 0.5 -- understaffed

        g_coords_out_col = -100; // fine_to_tile(-100) = -3 (truncation toward zero, NOT floor(-3.125)=-4)
        g_coords_out_row = 200;  // fine_to_tile(200)  =  6

        run(fx, /*now=*/1.0);

        ck_eq((uint32_t)g_coords_calls.size(), 1u, "T13: ratio 0.25 < 0.5 -- understaffed hit fires");
        ck_eq((int32_t)fx.cam_pan_target_col, -3,
              "T13: fine_to_tile(-100) = -3, truncating /32 not floor, 0x0049b84a-0x0049b85d");
        ck_eq((int32_t)fx.cam_pan_target_row, 6,
              "T13: fine_to_tile(200) = 6, the OTHER axis's independent conversion, 0x0049b865-0x0049b87b");
    }

    // =================================================================================================
    // T14 -- UPGRADING's builder_count INDIRECTION (0x0049b76d-0x0049b791): reads builder_count
    // through Building[Building[id].upgrade_index], NOT the building's own record. The building's own
    // builder_count (100) would give ratio 1/100=0.01 < 0.5 (a false hit); the upgrade TARGET's
    // builder_count (2) gives ratio 1/2=0.5 == threshold (no hit) -- so a wrong-record translation
    // disagrees with this case.
    // =================================================================================================
    {
        fx.reset();
        reset_mock_config();
        fx.advisor_next_time       = 0.0;
        fx.advisor_due_delay       = 0.0;
        fx.advisor_interval        = 5.0;
        fx.advisor_phase           = 1;
        fx.player_side             = 4;
        fx.advisor_staff_threshold = 0.5;

        fx.b(4, 0).index                   = 1;
        fx.b(4, 1).building_id             = 50;
        fx.b(4, 1).energy                  = 1.0;
        fx.b(4, 1).built_flags             = BUILT_FLAGS_OPERATIONAL;
        fx.b(4, 1).state                   = BLDG_STATE_UPGRADING;
        fx.b(4, 1).current_workers         = 1;
        fx.cfg_buildings[50].upgrade_index = 60;
        fx.cfg_buildings[50].builder_count = 100; // WRONG record if used directly -> false hit
        fx.cfg_buildings[60].builder_count = 2;   // the CORRECT (upgrade target's) record

        run(fx, /*now=*/1.0);

        ck_eq(g_str_copy_calls, 0u,
              "T14: UPGRADING reads builder_count via Building[upgrade_index], ratio 1/2=0.5==threshold "
              "-- NOT understaffed; using the building's own builder_count(100) would wrongly fire");
    }

    // =================================================================================================
    // T15/T16 -- construction-group STATE-SET membership: CHARGE_STEP and DISMANTLING share the SAME
    // direct-builder_count formula as CONSTRUCTION (not the uses_workers-gated path). bldg_uses_workers
    // is left returning 0 (the default), so if either state were wrongly excluded from the OR-chain,
    // it would fall through to the gated path and be BLOCKED (no hit) instead.
    // =================================================================================================
    {
        fx.reset();
        reset_mock_config();
        fx.advisor_next_time       = 0.0;
        fx.advisor_due_delay       = 0.0;
        fx.advisor_interval        = 5.0;
        fx.advisor_phase           = 1;
        fx.player_side             = 4;
        fx.advisor_staff_threshold = 0.9;

        fx.b(4, 0).index                   = 1;
        fx.b(4, 1).building_id             = 51;
        fx.b(4, 1).energy                  = 1.0;
        fx.b(4, 1).built_flags             = BUILT_FLAGS_OPERATIONAL;
        fx.b(4, 1).state                   = BLDG_STATE_CHARGE_STEP;
        fx.b(4, 1).current_workers         = 1;
        fx.cfg_buildings[51].builder_count = 2; // ratio 0.5 < 0.9

        run(fx, /*now=*/1.0);

        ck_eq(g_str_copy_calls, 1u,
              "T15: CHARGE_STEP(0x6a) uses the direct construction-group formula (uses_workers stayed "
              "0/unset), 0x0049b757-0x0049b762 membership");
    }
    {
        fx.reset();
        reset_mock_config();
        fx.advisor_next_time       = 0.0;
        fx.advisor_due_delay       = 0.0;
        fx.advisor_interval        = 5.0;
        fx.advisor_phase           = 1;
        fx.player_side             = 4;
        fx.advisor_staff_threshold = 0.9;

        fx.b(4, 0).index                   = 1;
        fx.b(4, 1).building_id             = 52;
        fx.b(4, 1).energy                  = 1.0;
        fx.b(4, 1).built_flags             = BUILT_FLAGS_OPERATIONAL;
        fx.b(4, 1).state                   = BLDG_STATE_DISMANTLING;
        fx.b(4, 1).current_workers         = 1;
        fx.cfg_buildings[52].builder_count = 2; // ratio 0.5 < 0.9

        run(fx, /*now=*/1.0);

        ck_eq(g_str_copy_calls, 1u,
              "T16: DISMANTLING(0x6b) uses the direct construction-group formula too, "
              "0x0049b744-0x0049b762 membership");
    }

    // =================================================================================================
    // T17 -- non-construction-group state: bldg_uses_workers() returning 0 BLOCKS the hit even though
    // the ratio (0/100=0.0) would otherwise be maximally understaffed. Also pins the call args
    // (player, idx).
    // =================================================================================================
    {
        fx.reset();
        reset_mock_config();
        fx.advisor_next_time       = 0.0;
        fx.advisor_due_delay       = 0.0;
        fx.advisor_interval        = 5.0;
        fx.advisor_phase           = 1;
        fx.player_side             = 4;
        fx.advisor_staff_threshold = 0.9;
        g_uses_workers_result      = 0; // gate BLOCKS

        fx.b(4, 0).index                  = 1;
        fx.b(4, 1).building_id            = 53;
        fx.b(4, 1).energy                 = 1.0;
        fx.b(4, 1).built_flags            = BUILT_FLAGS_OPERATIONAL;
        fx.b(4, 1).state                  = 5; // not in the construction group
        fx.b(4, 1).current_workers        = 0;
        fx.cfg_buildings[53].worker_count = 100; // ratio would be 0/100=0.0 -- maximally understaffed

        run(fx, /*now=*/1.0);

        ck_eq((uint32_t)g_uses_workers_calls.size(), 1u, "T17: bldg_uses_workers IS called once");
        ck(g_uses_workers_calls[0].player == 4 && g_uses_workers_calls[0].idx == 1,
           "T17: bldg_uses_workers(player=4, idx=1), 0x0049b7d1-0x0049b7df args");
        ck_eq(g_str_copy_calls, 0u,
              "T17: bldg_uses_workers()==0 blocks the hit despite a 0.0 ratio, 0x0049b7e4-0x0049b7e6");
    }

    // =================================================================================================
    // T18 -- non-construction-group state, bldg_uses_workers() TRUE: ratio = current_workers /
    // Building[id].worker_count (the DIRECT cfg record, no indirection).
    // =================================================================================================
    {
        fx.reset();
        reset_mock_config();
        fx.advisor_next_time       = 0.0;
        fx.advisor_due_delay       = 0.0;
        fx.advisor_interval        = 5.0;
        fx.advisor_phase           = 1;
        fx.player_side             = 4;
        fx.advisor_staff_threshold = 0.5;
        g_uses_workers_result      = 1; // gate PASSES

        fx.b(4, 0).index                  = 1;
        fx.b(4, 1).building_id            = 54;
        fx.b(4, 1).energy                 = 1.0;
        fx.b(4, 1).built_flags            = BUILT_FLAGS_OPERATIONAL;
        fx.b(4, 1).state                  = 5;
        fx.b(4, 1).current_workers        = 1;
        fx.cfg_buildings[54].worker_count = 10; // ratio 1/10 = 0.1 < 0.5

        run(fx, /*now=*/1.0);

        ck_eq((uint32_t)g_uses_workers_calls.size(), 1u, "T18: bldg_uses_workers called once");
        ck(g_uses_workers_calls[0].player == 4 && g_uses_workers_calls[0].idx == 1,
           "T18: bldg_uses_workers(player=4, idx=1)");
        ck_eq(g_str_copy_calls, 1u,
              "T18: ratio 1/10=0.1 < 0.5 via Building[id].worker_count (direct, no indirection), "
              "0x0049b7d1-0x0049b80e");
    }

    // =================================================================================================
    // T19 -- ORDER: current_workers is read AFTER bldg_uses_workers() returns, not cached from before
    // the call. The mock MUTATES building[4][1].current_workers from 100 (would-be ratio 100/10=10.0,
    // NOT understaffed) to 1 (ratio 1/10=0.1, understaffed) DURING the call; only a post-call read
    // sees the mutated value and produces a hit.
    // =================================================================================================
    {
        fx.reset();
        reset_mock_config();
        fx.advisor_next_time       = 0.0;
        fx.advisor_due_delay       = 0.0;
        fx.advisor_interval        = 5.0;
        fx.advisor_phase           = 1;
        fx.player_side             = 4;
        fx.advisor_staff_threshold = 0.5;
        g_uses_workers_result      = 1;
        g_mutate_fixture           = &fx;
        g_mutate_player            = 4;
        g_mutate_idx               = 1;
        g_mutate_new_workers       = 1;

        fx.b(4, 0).index                  = 1;
        fx.b(4, 1).building_id            = 55;
        fx.b(4, 1).energy                 = 1.0;
        fx.b(4, 1).built_flags            = BUILT_FLAGS_OPERATIONAL;
        fx.b(4, 1).state                  = 5;
        fx.b(4, 1).current_workers        = 100; // would look fully staffed if read BEFORE the call
        fx.cfg_buildings[55].worker_count = 10;

        run(fx, /*now=*/1.0);

        ck_eq(g_str_copy_calls, 1u,
              "T19 ORDER: current_workers read AFTER bldg_uses_workers() mutates it (100->1) -- ratio "
              "computed from the POST-call value (0.1<0.5), proving no pre-call caching, 0x0049b7f7-0x0049b80e");
    }

    // =================================================================================================
    // T20 -- the scan stops at the FIRST understaffed hit: two qualifying buildings at idx=1 and idx=2,
    // only idx=1's coordinates get used.
    // =================================================================================================
    {
        fx.reset();
        reset_mock_config();
        fx.advisor_next_time       = 0.0;
        fx.advisor_due_delay       = 0.0;
        fx.advisor_interval        = 5.0;
        fx.advisor_phase           = 1;
        fx.player_side             = 4;
        fx.advisor_staff_threshold = 0.9;

        fx.b(4, 0).index = 2; // budget = 2 -- both slots WOULD qualify

        fx.b(4, 1).building_id             = 60;
        fx.b(4, 1).energy                  = 1.0;
        fx.b(4, 1).built_flags             = BUILT_FLAGS_OPERATIONAL;
        fx.b(4, 1).state                   = BLDG_STATE_CONSTRUCTION;
        fx.b(4, 1).current_workers         = 1;
        fx.cfg_buildings[60].builder_count = 2; // ratio 0.5 < 0.9 -- hit #1

        fx.b(4, 2).building_id             = 61;
        fx.b(4, 2).energy                  = 1.0;
        fx.b(4, 2).built_flags             = BUILT_FLAGS_OPERATIONAL;
        fx.b(4, 2).state                   = BLDG_STATE_CONSTRUCTION;
        fx.b(4, 2).current_workers         = 1;
        fx.cfg_buildings[61].builder_count = 2; // ratio 0.5 < 0.9 -- would ALSO be a hit, never reached

        run(fx, /*now=*/1.0);

        ck_eq((uint32_t)g_coords_calls.size(), 1u,
              "T20: exactly one bldg_get_coords call -- the scan stops at the first hit, remaining=0 "
              "forced at 0x0049b880");
        ck_eq((uint32_t)g_coords_calls[0].idx, 1u,
              "T20: the hit used is idx=1 (the FIRST), not idx=2");
    }

    // =================================================================================================
    // T21 -- clean phase-1 pass: a live, FULLY staffed building (ratio 1.0, nowhere near threshold) --
    // buildings are present and scanned, but none is understaffed, so no message fires.
    // =================================================================================================
    {
        fx.reset();
        reset_mock_config();
        fx.advisor_next_time       = 0.0;
        fx.advisor_due_delay       = 0.0;
        fx.advisor_interval        = 5.0;
        fx.advisor_phase           = 1;
        fx.player_side             = 4;
        fx.advisor_staff_threshold = 0.9;

        fx.b(4, 0).index                   = 1;
        fx.b(4, 1).building_id             = 70;
        fx.b(4, 1).energy                  = 1.0;
        fx.b(4, 1).built_flags             = BUILT_FLAGS_OPERATIONAL;
        fx.b(4, 1).state                   = BLDG_STATE_CONSTRUCTION;
        fx.b(4, 1).current_workers         = 4;
        fx.cfg_buildings[70].builder_count = 4; // ratio 1.0, not understaffed

        run(fx, /*now=*/1.0);

        ck_eq(g_str_copy_calls, 0u, "T21: fully staffed building (ratio 1.0) -- no message");
        ck_eq((uint32_t)fx.advisor_phase, 0u, "T21: housekeeping still runs -- phase 1->0");
        ck_eq_d(fx.advisor_next_time, 5.0, "T21: housekeeping still runs -- NEXT_TIME advances");
    }

    // =================================================================================================
    // T22 -- THE LOOP-EXIT ADVANCE, natural drain. LAB_0049b6e1 (`INC word ptr [CUR_INDEX]` /
    // `ADD dword ptr [CUR_BUILDING],0x111`) sits between the body and the `remaining == 0` test at
    // LAB_0049b6d6, and every body exit funnels into it -- so the scan leaves both ambient globals
    // ONE SLOT PAST the last slot it examined, never AT it. Two live slots, neither understaffed:
    // the count drains on slot 2, so the exit value is 3.
    // =================================================================================================
    {
        fx.reset();
        reset_mock_config();
        fx.advisor_next_time       = 0.0;
        fx.advisor_due_delay       = 0.0;
        fx.advisor_interval        = 5.0;
        fx.advisor_phase           = 1;
        fx.player_side             = 4;
        fx.advisor_staff_threshold = 0.9;

        fx.b(4, 0).index                   = 2; // COUNT = 2 -- slots 1 and 2 are both examined
        fx.cfg_buildings[70].builder_count = 4;
        for (int i = 1; i <= 2; ++i) {
            fx.b(4, i).building_id     = 70;
            fx.b(4, i).energy          = 1.0;
            fx.b(4, i).built_flags     = BUILT_FLAGS_OPERATIONAL;
            fx.b(4, i).state           = BLDG_STATE_CONSTRUCTION;
            fx.b(4, i).current_workers = 4; // ratio 1.0 -- no hit, the scan runs to the end
        }

        run(fx, /*now=*/1.0);

        ck_eq(g_str_copy_calls, 0u, "T22: neither slot understaffed -- no message");
        ck_eq((uint32_t)fx.view_cur_index, 3u,
              "T22: CUR_INDEX = last examined slot (2) + 1 -- the advance at LAB_0049b6e1 runs "
              "BEFORE the remaining==0 test at 0x0049b6d6, 0x0049b6e1");
        ck(fx.cur_building_ptr == &fx.b(4, 3),
           "T22: CUR_BUILDING advanced past slot 2 too (ADD 0x111 pairs with the INC), 0x0049b6e8");
    }

    // =================================================================================================
    // T23 -- THE LOOP-EXIT ADVANCE, forced stop. The understaffed hit zeroes `remaining` at
    // 0x0049b880 and then FALLS THROUGH LAB_0049b887 into the same advance, so even the arm that
    // stops the scan early leaves the globals one past the hit slot. Slot 1 is understaffed, so the
    // exit value is 2 -- with a live slot 2 present that the scan must never reach.
    // =================================================================================================
    {
        fx.reset();
        reset_mock_config();
        fx.advisor_next_time       = 0.0;
        fx.advisor_due_delay       = 0.0;
        fx.advisor_interval        = 5.0;
        fx.advisor_phase           = 1;
        fx.player_side             = 4;
        fx.advisor_staff_threshold = 0.9;

        fx.b(4, 0).index                   = 2;
        fx.cfg_buildings[70].builder_count = 4;
        for (int i = 1; i <= 2; ++i) {
            fx.b(4, i).building_id     = 70;
            fx.b(4, i).energy          = 1.0;
            fx.b(4, i).built_flags     = BUILT_FLAGS_OPERATIONAL;
            fx.b(4, i).state           = BLDG_STATE_CONSTRUCTION;
            fx.b(4, i).current_workers = 1; // ratio 0.25 < 0.9 -- slot 1 hits and stops the scan
        }

        run(fx, /*now=*/1.0);

        ck_eq((uint32_t)g_coords_calls.size(), 1u, "T23: the scan stopped at the first hit");
        ck_eq((uint32_t)fx.view_cur_index, 2u,
              "T23: CUR_INDEX = hit slot (1) + 1 -- remaining=0 at 0x0049b880 falls through "
              "LAB_0049b887 into the advance at 0x0049b6e1, it does not skip it");
        ck(fx.cur_building_ptr == &fx.b(4, 2),
           "T23: CUR_BUILDING advanced past the hit slot as well, 0x0049b6e8");
    }
}

} // namespace mh::sim::test
