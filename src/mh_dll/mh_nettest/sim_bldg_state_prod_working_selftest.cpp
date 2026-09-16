//
// sim_bldg_state_prod_working_selftest.cpp -- `simtest` cases for llm_strat_bldg_state_prod_working
// (sim/sim_bldg_state_prod.h/.cpp @0x00473ca8), SIM1-G4 second-slice building_tick machinery -- the
// "production cycle in progress" per-tick handler for a building whose state dispatches here.
//
// THIS FUNCTION IS DO-NOT-ARM (its write closure reaches game_SetEvent's ~780-function UI/gfx/snd/
// menu-teardown over-approximation) -- there is no rig run backing this up, so this file is written to
// be thorough rather than minimal.
//
// SCOPE: pins the completion gate's exact `>=` boundary (from below/at/above), the unconditional
// cycle_progress accumulation, the RATE-ZERO-GUARD's plain-bit `==0.0` substitution of 1.0, the
// LOCAL-vs-OTHER-PLAYER split, the SIM_ACTIVE gate on the voice line only, the queued_count[0]==0 gate
// on the whole local block, the race-dependent voice-line id, the soldier_count-keyed
// voice-id/message-reason pairing, param_1/param_2 being dead, param_3/param_4 being forwarded
// VERBATIM to completion_dispatch (unlike construction's register-reuse hazard), the negative-fine
// truncating fine_to_tile(), and -- the crux of this file -- TWO cases (DIVERGENCE-A / DIVERGENCE-B
// below, names kept for the git-blame trail) that were ORIGINALLY WRITTEN AS FAILING FINDINGS: the
// asm re-reads cur_building->sub_id and productions[...].active_unit_type FRESH FROM MEMORY, several
// times, AFTER llm_strat_bldg_completion_dispatch has already run, and the .cpp under test at the time
// this oracle was authored (2026-08-22) instead captured both ONCE at the top of the function, before
// that call, reusing the stale snapshot. FIXED THE SAME SESSION (2026-08-22, conductor): the .cpp now
// re-derives the whole `productions[cur_player][b.sub_id]` row independently at each of the three
// post-dispatch sites (see sim_bldg_state_prod.cpp's own comment on the fix), so DIVERGENCE-A/-B now
// PASS -- they still pin the asm's re-read behaviour, just no longer as a documented failure.
//
// EXPECTED BEHAVIOUR HAND-DERIVED FROM THE DISASSEMBLY
// (tmp/decomp_sim/llm_strat_bldg_state_prod_working_00473ca8.asm) -- NOT read off the .cpp body alone:
//
//   rate = cur_building->efficiency * Building[bid].unit_quant[active_unit_type] (0x00473cc0-0x00473d20,
//   unconditional). cycle_progress += tick_budget * rate (same range).
//   Completion gate (0x00473d51-0x00473d63): `if (build_time <= cycle_progress)` -- FCOMP/FNSTSW/SAHF/JC,
//   JC (taken on `<` OR unordered) skips straight to LAB_00473feb (tick_budget=0.0, no dispatch at all).
//   Inside the gate (0x00473d69-0x00473d83): completion_dispatch(cur_player, cur_index, param_3, param_4,
//   *game_clock) UNCONDITIONALLY -- param_3/param_4 (EBX/ECX) are never written between entry and this
//   call, so they are the caller's own incoming values, verbatim.
//   THEN (0x00473d88-0x00473dc3), LOCAL PLAYER (cur_player==PlayerSide) AND queued_count[0]==0 only:
//     SIM_ACTIVE-gated voice line (0x00473dc9-0x00473e6b): snd_play(id=race_off+(soldier_count<1?9:0xc),
//     volume=100), race_off = player_race==2 ? 0x12 : 0.
//     THEN (unconditional in this local sub-block) bldg_get_coords writes fine coords into
//     CAM_PAN_TARGET_COL/_ROW (0x00473e83), truncated fine->tile in place (0x00473e88-0x00473eb9).
//     THEN a "%s: %s" message (name=Building[bid].id, reason=text_ptrs[soldier_count<1?0x76:0x9d]) via
//     w_sprintf__vss + game_ui_PrintTextMessage (0x00473ebe-0x00473f6c).
//   UNCONDITIONALLY inside the gate (0x00473f71-0x00473fe9, both local and other player, queued or not):
//     RATE-ZERO-GUARD: divisor = rate; if (divisor==0.0) divisor=1.0 (plain bit-exact test, no x87
//     landmine). tick_budget = (cycle_progress - build_time) / divisor. state = PROD_PICK_NEXT(0x6c).
//     game_SetEvent(MAP_OBJECTS_REFRESH=0xe).
//   TAIL (0x00473fff-0x0047400d, ALWAYS runs, gate taken or not): bldg_notify_ui(cur_player, cur_index).
//   b.online_state is NEVER written anywhere in this function.
//
#include "sim/sim_bldg_state_prod.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <vector>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// ---- llm_strat_bldg_state values / text ids / voice constants, redeclared file-local per this
// project's established per-TU precedent (no real C++ enum exists yet -- see the module header). ------
inline constexpr uint32_t BLDG_STATE_PROD_PICK_NEXT = 0x6c;
inline constexpr uint16_t TEXT_UNIT_READY           = 0x76;
inline constexpr uint16_t TEXT_SOLDIERS_READY       = 0x9d;
inline constexpr int32_t  VOICE_RACE2_OFFSET        = 0x12;
inline constexpr int32_t  VOICE_BASE_UNIT_READY     = 9;
inline constexpr int32_t  VOICE_BASE_SOLDIER_LOAD   = 0xc;
inline constexpr int32_t  VOICE_VOLUME              = 100;
inline constexpr uint32_t MAP_OBJECTS_REFRESH       = 14u;

// ---- shared trace: one sequence proves CALL ORDER across all 7 callees -------------------------
std::vector<const char *> g_trace;
void                      tr(const char *tag) { g_trace.push_back(tag); }

bool trace_eq(const std::vector<const char *> &want) {
    if (g_trace.size() != want.size()) return false;
    for (size_t i = 0; i < want.size(); ++i)
        if (std::strcmp(g_trace[i], want[i]) != 0) return false;
    return true;
}

// ---- return-value knob for bldg_get_coords (settable per case BEFORE seed_and_run) -------------
int32_t g_get_coords_fine_x = 0;
int32_t g_get_coords_fine_y = 0;

// ---- ORDER-DIVERGENCE knobs (see DIVERGENCE-A/-B below): let the completion_dispatch mock mutate
// ambient state MID-CALL, so a case can observe whether the rest of the function re-reads that state
// AFTER the callee (the asm's own behaviour) or used a value captured before it (what the .cpp under
// test does). Both default to "no mutation"; reset_observations() clears them between cases.
uint8_t    *g_dispatch_mutate_sub_id_ptr      = nullptr;
uint8_t     g_dispatch_mutate_sub_id_new      = 0;
production *g_dispatch_mutate_prod_ptr        = nullptr;
uint8_t     g_dispatch_mutate_active_type_new = 0;

// ---- per-callee recorders (7, one per bldg_state_prod_working_calls member) ---------------------
struct CompletionDispatchCall {
    uint32_t player, index, param_3, param_4;
    double   game_clock;
};
std::vector<CompletionDispatchCall> g_dispatch_calls;
void                                rec_completion_dispatch(uint32_t player, uint32_t index, uint32_t param_3, uint32_t param_4,
                                                            double game_clock) {
    tr("completion_dispatch");
    g_dispatch_calls.push_back({player, index, param_3, param_4, game_clock});
    // ---- ORDER-DIVERGENCE mutation point: applied WHILE the callee is "running", exactly like a
    // real effectful original would mutate shared state before returning. ------------------------
    if (g_dispatch_mutate_sub_id_ptr != nullptr) *g_dispatch_mutate_sub_id_ptr = g_dispatch_mutate_sub_id_new;
    if (g_dispatch_mutate_prod_ptr != nullptr)
        g_dispatch_mutate_prod_ptr->active_unit_type = g_dispatch_mutate_active_type_new;
}

struct SndCall {
    int32_t id, volume;
};
std::vector<SndCall> g_snd_play;
void                 rec_snd_play(int32_t id, int32_t volume) {
    tr("snd_play");
    g_snd_play.push_back({id, volume});
}

struct CoordsCall {
    uint16_t player;
    int32_t  index;
};
std::vector<CoordsCall> g_coords;
void                    rec_get_coords(uint16_t player, int32_t index, int32_t *out_x, int32_t *out_y) {
    tr("get_coords");
    g_coords.push_back({player, index});
    *out_x = g_get_coords_fine_x;
    *out_y = g_get_coords_fine_y;
}

struct SprintfCall {
    const wchar_t *dst;
    const wchar_t *fmt;
    const wchar_t *a0;
    const wchar_t *a1;
};
std::vector<SprintfCall> g_sprintf_calls;
int32_t                  rec_sprintf(void *dst, const wchar_t *fmt, const wchar_t *a0, const wchar_t *a1) {
    tr("sprintf");
    g_sprintf_calls.push_back({static_cast<const wchar_t *>(dst), fmt, a0, a1});
    return 0;
}

int                 g_print_text_message_count = 0;
std::vector<void *> g_print_text_message_ptrs;
uint32_t            rec_print_text_message(void *text) {
    tr("print_text_message");
    ++g_print_text_message_count;
    g_print_text_message_ptrs.push_back(text);
    return 0;
}

std::vector<uint32_t> g_set_event_calls;
uint32_t              rec_set_event(uint32_t type) {
    tr("set_event");
    g_set_event_calls.push_back(type);
    return 0;
}

struct NotifyUiCall {
    uint16_t player;
    uint32_t index;
};
std::vector<NotifyUiCall> g_notify_ui_calls;
void                      rec_bldg_notify_ui(uint16_t player, uint32_t index) {
    tr("notify_ui");
    g_notify_ui_calls.push_back({player, index});
}

const bldg_state_prod_working_calls g_calls = {
    &rec_completion_dispatch,
    &rec_snd_play,
    &rec_get_coords,
    &rec_sprintf,
    &rec_print_text_message,
    &rec_set_event,
    &rec_bldg_notify_ui,
};

void reset_observations() {
    g_trace.clear();
    g_dispatch_calls.clear();
    g_snd_play.clear();
    g_coords.clear();
    g_sprintf_calls.clear();
    g_print_text_message_count = 0;
    g_print_text_message_ptrs.clear();
    g_set_event_calls.clear();
    g_notify_ui_calls.clear();
    g_dispatch_mutate_sub_id_ptr      = nullptr;
    g_dispatch_mutate_sub_id_new      = 0;
    g_dispatch_mutate_prod_ptr        = nullptr;
    g_dispatch_mutate_active_type_new = 0;
}

// ---- text pointers used to pin WHICH text_ptrs slots feed the "%s: %s" message -----------------
// Distinct addresses (not dereferenced by the recorder) so a translation that read the wrong index
// (or swapped name/reason) is caught by pointer identity, without depending on w_sprintf's actual
// formatting (presentation, out of scope).
const wchar_t k_name_text[]             = L"NAME_TXT";
const wchar_t k_reason_unit_ready[]     = L"UNIT_READY_TXT";
const wchar_t k_reason_soldiers_ready[] = L"SOLDIERS_READY_TXT";

// ---- fixture seeding -----------------------------------------------------------------------------
struct Seed {
    uint16_t player           = 1;
    int32_t  index            = 2;
    uint8_t  sub_id           = 3;  // b.sub_id -> productions[player][sub_id]
    uint16_t cfg_row          = 9;  // building_id -> cfg_buildings index
    int32_t  text_id          = 77; // cfg_building.id, the "name" half of the message
    uint8_t  active_unit_type = 40; // production.active_unit_type / cfg_units index

    double efficiency        = 2.0;
    double unit_quant        = 3.0; // cfg_buildings[cfg_row].unit_quant[active_unit_type]; rate = 6.0
    double cycle_progress_in = 4.0;
    double tick_budget_in    = 5.0;  // delta = tick_budget_in*rate = 30.0 -> cycle_progress = 34.0
    double build_time        = 25.0; // gate threshold; 34.0 >= 25.0 -> TAKEN with margin

    int32_t soldier_count = 0; // <1 -> "unit ready" family (BASE_UNIT_READY / TEXT_UNIT_READY)
    int16_t player_side   = 1; // == player -> "local"
    int32_t player_race   = 2;
    int32_t sim_active    = 1;
    int32_t queued_count0 = 0; // ==0 -> local block eligible

    int32_t fine_x = 1600; // -> tile 50 (1600/32, exact)
    int32_t fine_y = 1760; // -> tile 55 (1760/32, exact)

    int32_t cam_pan_col_seed = -4001;
    int32_t cam_pan_row_seed = -4002;

    double game_clock = 123456.5;

    // Dead in the original -- distinct sentinels so a translation that started reading them shows up
    // as a wrong branch, not a crash.
    uint32_t param_1_in = 0x11111111u;
    uint32_t param_2_in = 0x22222222u;
    // Forwarded VERBATIM to completion_dispatch (NO register-reuse hazard here, unlike construction's
    // own four-parameter sibling) -- sentinels deliberately far from any plausible address.
    uint32_t param_3_in = 0x33333333u;
    uint32_t param_4_in = 0x44444444u;

    // ---- ORDER-DIVERGENCE support (DIVERGENCE-A/-B below) -----------------------------------------
    bool    dispatch_mutates_sub_id      = false;
    uint8_t dispatch_new_sub_id          = 0;
    bool    dispatch_mutates_active_type = false;
    uint8_t dispatch_new_active_type     = 0;
    bool    seed_row_b                   = false;
    uint8_t row_b_sub_id                 = 0;
    int32_t row_b_queued_count0          = 0;
    uint8_t row_b_active_unit_type       = 0;

    // A one-off extra fixture poke a case needs before the call (e.g. a SECOND cfg_unit slot for the
    // divergence cases) -- a test-file-local convenience, not a fixture gap.
    std::function<void(sim_fixture &)> extra_seed = nullptr;
};

void seed_and_run(sim_fixture &fx, const Seed &s) {
    fx.reset();

    building &b      = fx.b(s.player, s.index);
    b.building_id    = s.cfg_row;
    b.sub_id         = s.sub_id;
    b.cycle_progress = s.cycle_progress_in;
    b.efficiency     = s.efficiency;
    // Sentinels distinct from any real state/online_state value this function writes, so "unchanged"
    // checks are meaningful.
    b.state        = 0x1234;
    b.online_state = (int16_t)0x2345;

    fx.cur_building_ptr = &b;
    fx.view_cur_player  = s.player;
    fx.view_cur_index   = (uint16_t)s.index;

    cfg_building &cb                  = fx.cfg_buildings[s.cfg_row];
    cb.id                             = s.text_id;
    cb.unit_quant[s.active_unit_type] = s.unit_quant;

    cfg_unit &cu     = fx.cfg_units[s.active_unit_type];
    cu.build_time    = s.build_time;
    cu.soldier_count = s.soldier_count;

    production &prod      = fx.productions[(size_t)s.player * PRODUCTIONS_PER_PLAYER + s.sub_id];
    prod.active_unit_type = s.active_unit_type;
    prod.queued_count[0]  = s.queued_count0;

    if (s.seed_row_b) {
        production &prod_b      = fx.productions[(size_t)s.player * PRODUCTIONS_PER_PLAYER + s.row_b_sub_id];
        prod_b.queued_count[0]  = s.row_b_queued_count0;
        prod_b.active_unit_type = s.row_b_active_unit_type;
    }

    fx.player_side = s.player_side;
    fx.player_race = s.player_race;
    fx.sim_active  = s.sim_active;
    fx.tick_budget = s.tick_budget_in;
    fx.game_clock  = s.game_clock;

    fx.cam_pan_target_col = s.cam_pan_col_seed;
    fx.cam_pan_target_row = s.cam_pan_row_seed;

    fx.text_ptrs[(size_t)s.text_id]   = k_name_text;
    fx.text_ptrs[TEXT_UNIT_READY]     = k_reason_unit_ready;
    fx.text_ptrs[TEXT_SOLDIERS_READY] = k_reason_soldiers_ready;

    g_get_coords_fine_x = s.fine_x;
    g_get_coords_fine_y = s.fine_y;

    if (s.extra_seed) s.extra_seed(fx);

    reset_observations();
    g_dispatch_mutate_sub_id_ptr      = s.dispatch_mutates_sub_id ? &b.sub_id : nullptr;
    g_dispatch_mutate_sub_id_new      = s.dispatch_new_sub_id;
    g_dispatch_mutate_prod_ptr        = s.dispatch_mutates_active_type ? &prod : nullptr;
    g_dispatch_mutate_active_type_new = s.dispatch_new_active_type;

    sim_store own = fx.store();
    detail::bldg_state_prod_working(fx.view(), own, g_calls, s.param_1_in, s.param_2_in, s.param_3_in,
                                    s.param_4_in);
}

} // namespace

void run_bldg_state_prod_working_tests() {
    sim_fixture fx;

    // =================================================================================================
    // C1 -- completion gate NOT taken (build_time clearly above cycle_progress after accumulation,
    // 0x00473d63 JC taken): tick_budget zeroes unconditionally, notify_ui fires at the tail, and
    // NOTHING ELSE (no dispatch/voice/coords/message/set_event -- proves those are all INSIDE the gate).
    // =================================================================================================
    {
        Seed s;
        s.build_time = 999.0; // 34.0 (accumulated) < 999.0 -> gate NOT taken
        seed_and_run(fx, s);

        ck(trace_eq({"notify_ui"}), "C1: gate not taken -- only the tail's notify_ui fires (0x00473fff-0x0047400d)");
        ck_eq_d(fx.b(s.player, s.index).cycle_progress, 34.0,
                "C1: cycle_progress = tick_budget*rate + cycle_progress_in = 5.0*(2.0*3.0)+4.0 = 34.0 "
                "(0x00473cc0-0x00473d20, accumulates even though the gate below is not taken)");
        ck_eq_d(fx.tick_budget, 0.0, "C1: tick_budget zeroed unconditionally on the not-taken arm (0x00473feb/0x00473ff5)");
        ck_eq((uint32_t)fx.b(s.player, s.index).state, 0x1234u,
              "C1: b.state UNCHANGED (sentinel) -- the not-taken arm never writes state");
        ck_eq((uint32_t)(uint16_t)fx.b(s.player, s.index).online_state, 0x2345u,
              "C1: b.online_state UNCHANGED -- prod_working never writes online_state anywhere in the body");
        ck(g_dispatch_calls.empty() && g_snd_play.empty() && g_coords.empty() && g_sprintf_calls.empty() &&
               g_print_text_message_count == 0 && g_set_event_calls.empty(),
           "C1: none of the completion-gate callees fire when the gate is skipped");
        ck_eq((uint32_t)fx.cam_pan_target_col, (uint32_t)s.cam_pan_col_seed, "C1: cam_pan_target_col UNCHANGED");
        ck_eq((uint32_t)fx.cam_pan_target_row, (uint32_t)s.cam_pan_row_seed, "C1: cam_pan_target_row UNCHANGED");
    }

    // =================================================================================================
    // C2/C3/C4 -- the completion gate's exact boundary, pinned from below/at/above. The asm's
    // FCOMP/FNSTSW/SAHF/JC treats an UNORDERED compare as "JC taken" (as if cycle_progress<build_time);
    // negating that gives `!(cycle_progress>=build_time)` for the not-taken arm -- see the module
    // header's NAN-SAFE NEGATION note. A real NaN can't be driven through this public API, so these
    // three pin the ORDERED-input boundary precisely instead (value exactly at the threshold, and one
    // ULP on each side), isolated on the OTHER-PLAYER branch so only the gate itself is in play.
    // =================================================================================================
    {
        Seed s;
        s.efficiency        = 2.0;
        s.unit_quant        = 5.0; // rate = 10.0 (nonzero -- isolates the gate from the RATE-ZERO-GUARD)
        s.tick_budget_in    = 0.0; // delta = 0 -> cycle_progress stays exactly cycle_progress_in
        s.cycle_progress_in = 40.0;
        s.build_time        = 40.0;                    // == cycle_progress -> gate TAKEN at the boundary
        s.player_side       = (int16_t)(s.player + 5); // != player -> "other" (isolates just the gate)
        seed_and_run(fx, s);

        ck(trace_eq({"completion_dispatch", "set_event", "notify_ui"}),
           "C2: gate taken AT EXACT EQUALITY (0x00473d5a FCOMP, JC NOT taken at equality) -- pins `>=`, not `>`");
        ck_eq((uint32_t)fx.b(s.player, s.index).state, BLDG_STATE_PROD_PICK_NEXT,
              "C2: b.state = PROD_PICK_NEXT(0x6c) when the gate is taken (0x00473fd9)");
    }
    {
        Seed s;
        s.efficiency        = 2.0;
        s.unit_quant        = 5.0;
        s.tick_budget_in    = 0.0;
        s.cycle_progress_in = 40.0;
        s.build_time        = std::nextafter(40.0, HUGE_VAL); // one ULP ABOVE cycle_progress
        s.player_side       = (int16_t)(s.player + 5);
        seed_and_run(fx, s);

        ck(trace_eq({"notify_ui"}),
           "C3: build_time ONE ULP above cycle_progress -- cycle_progress strictly < build_time -> gate "
           "NOT taken (0x00473d63 JC taken)");
        ck_eq((uint32_t)fx.b(s.player, s.index).state, 0x1234u, "C3: b.state UNCHANGED when the gate is not taken");
    }
    {
        Seed s;
        s.efficiency        = 2.0;
        s.unit_quant        = 5.0;
        s.tick_budget_in    = 0.0;
        s.cycle_progress_in = 40.0;
        s.build_time        = std::nextafter(40.0, -HUGE_VAL); // one ULP BELOW cycle_progress
        s.player_side       = (int16_t)(s.player + 5);
        seed_and_run(fx, s);

        ck(trace_eq({"completion_dispatch", "set_event", "notify_ui"}),
           "C4: build_time ONE ULP below cycle_progress -- gate taken (0x00473d5a/0x00473d63, JC not taken), "
           "completes the below/at/above triple with C2/C3");
    }

    // =================================================================================================
    // C5 -- the full local-player arm: gate taken, cur_player==PlayerSide, queued_count[0]==0,
    // SIM_ACTIVE!=0, soldier_count<1, race==2. Pins the exact call order, the dispatch args (cur_player/
    // cur_index/param_3-verbatim/param_4-verbatim/game_clock), the voice id, the camera-pan tile
    // conversion, the sprintf name/reason args + dest buffer, the RATE-ZERO-GUARD's non-triggered
    // formula, the resulting state, and that online_state is STILL untouched even on this full arm.
    // =================================================================================================
    {
        Seed s; // defaults already encode this scenario -- see the Seed comments above
        seed_and_run(fx, s);

        ck(trace_eq({"completion_dispatch", "snd_play", "get_coords", "sprintf", "print_text_message",
                     "set_event", "notify_ui"}),
           "C5: exact call order for the local/sim-active/queued-empty arm (0x00473d83 dispatch -> "
           "0x00473e5d voice -> 0x00473e83 coords -> 0x00473f25 message -> 0x00473fe4 set_event -> "
           "0x0047400d tail)");

        ck_eq_d(fx.b(s.player, s.index).cycle_progress, 34.0, "C5: cycle_progress = 5.0*(2.0*3.0)+4.0 = 34.0");

        ck(g_dispatch_calls.size() == 1, "C5: completion_dispatch fires exactly once (0x00473d83)");
        if (g_dispatch_calls.size() == 1) {
            const auto &dc = g_dispatch_calls[0];
            ck(dc.player == s.player && dc.index == (uint32_t)s.index,
               "C5: completion_dispatch(cur_player, cur_index, ...) (0x00473d75-0x00473d83)");
            ck_eq(dc.param_3, s.param_3_in,
                  "C5: completion_dispatch's param_3 = THIS function's own incoming param_3 VERBATIM -- "
                  "EBX untouched between entry and 0x00473d83 (unlike construction's register-reuse hazard)");
            ck_eq(dc.param_4, s.param_4_in,
                  "C5: completion_dispatch's param_4 = incoming param_4 VERBATIM (ECX untouched)");
            ck_eq_d(dc.game_clock, s.game_clock,
                    "C5: completion_dispatch's 5th arg = *game_clock, pushed as an 8-byte double "
                    "(0x00473d69/0x00473d6f)");
        }

        ck(g_snd_play.size() == 1 && g_snd_play[0].id == VOICE_BASE_UNIT_READY + VOICE_RACE2_OFFSET &&
               g_snd_play[0].volume == VOICE_VOLUME,
           "C5: soldier_count<1, race==2 -> snd_play(id=9+0x12=0x1b, volume=100) (0x00473e3d-0x00473e66)");

        ck(g_coords.size() == 1 && g_coords[0].player == s.player && g_coords[0].index == s.index,
           "C5: bldg_get_coords(cur_player, cur_index, &cam_pan_target_col, &cam_pan_target_row) (0x00473e83)");
        ck_eq((uint32_t)fx.cam_pan_target_col, 50u,
              "C5: cam_pan_target_col = tile(1600) = 1600/32 = 50 (0x00473e88-0x00473e9e)");
        ck_eq((uint32_t)fx.cam_pan_target_row, 55u,
              "C5: cam_pan_target_row = tile(1760) = 1760/32 = 55 (0x00473ea3-0x00473eb9)");

        ck(g_sprintf_calls.size() == 1, "C5: w_sprintf__vss fires once (0x00473f25)");
        if (g_sprintf_calls.size() == 1) {
            const auto &sp = g_sprintf_calls[0];
            ck(sp.a0 == k_name_text, "C5: sprintf name-arg = text_ptrs[Building[bid].id] (0x00473f0a/0x00473f13)");
            ck(sp.a1 == k_reason_unit_ready, "C5: soldier_count<1 -> sprintf reason-arg = text_ptrs[0x76] (0x00473ef5)");
            ck(sp.dst == fx.text_scratch.data(), "C5: sprintf writes into own.text_scratch() (0xe15178)");
        }
        ck(g_print_text_message_count == 1 &&
               (g_print_text_message_ptrs.empty() || g_print_text_message_ptrs[0] == (void *)fx.text_scratch.data()),
           "C5: game_ui_PrintTextMessage(own.text_scratch()) fires once, same buffer sprintf wrote (0x00473f6c)");

        ck(g_set_event_calls.size() == 1 && g_set_event_calls[0] == MAP_OBJECTS_REFRESH,
           "C5: game_SetEvent(MAP_OBJECTS_REFRESH=0xe) fires once (0x00473fe4)");
        ck_eq_d(fx.tick_budget, 1.5,
                "C5: tick_budget = (cycle_progress-build_time)/rate = (34.0-25.0)/6.0 = 1.5 (0x00473fc2-0x00473fce)");
        ck_eq((uint32_t)fx.b(s.player, s.index).state, BLDG_STATE_PROD_PICK_NEXT,
              "C5: b.state = PROD_PICK_NEXT(0x6c) (0x00473fd9)");
        ck_eq((uint32_t)(uint16_t)fx.b(s.player, s.index).online_state, 0x2345u,
              "C5: b.online_state UNCHANGED -- prod_working never writes it, even on the full local arm");

        ck(g_notify_ui_calls.size() == 1 && g_notify_ui_calls[0].player == s.player &&
               g_notify_ui_calls[0].index == (uint32_t)s.index,
           "C5: bldg_notify_ui(cur_player, cur_index) fires at the shared tail (0x0047400d)");
    }

    // =================================================================================================
    // C6 -- same as C5 but soldier_count>=1: the "soldier load" family (BASE_SOLDIER_LOAD/SOLDIERS_READY)
    // instead of the "unit ready" one -- pins the OTHER half of the soldier_count<1 ternary.
    // =================================================================================================
    {
        Seed s;
        s.soldier_count = 5; // >= 1 -> "soldier load" family
        seed_and_run(fx, s);

        ck(g_snd_play.size() == 1 && g_snd_play[0].id == VOICE_BASE_SOLDIER_LOAD + VOICE_RACE2_OFFSET,
           "C6: soldier_count>=1, race==2 -> snd_play(id=0xc+0x12=0x1e) (0x00473e0d-0x00473e3b)");
        ck(g_sprintf_calls.size() == 1 && g_sprintf_calls[0].a1 == k_reason_soldiers_ready,
           "C6: soldier_count>=1 -> sprintf reason-arg = text_ptrs[0x9d] (0x00473f2f)");
    }

    // =================================================================================================
    // C7 -- race!=2: no 0x12 offset added to either voice id.
    // =================================================================================================
    {
        Seed s;
        s.player_race = 0; // != 2 -> no race offset
        seed_and_run(fx, s);

        ck(g_snd_play.size() == 1 && g_snd_play[0].id == VOICE_BASE_UNIT_READY,
           "C7: race!=2 -> snd_play id = base(9)+0, no 0x12 offset (0x00473e14 JNZ taken -> LAB_00473e26)");
    }

    // =================================================================================================
    // C8 -- SIM_ACTIVE=0: the voice line is skipped (SIM_ACTIVE gate, 0x00473dc9 JZ), but
    // coords/message/set_event/tail still fire -- only snd_play is gated by SIM_ACTIVE.
    // =================================================================================================
    {
        Seed s;
        s.sim_active = 0;
        seed_and_run(fx, s);

        ck(trace_eq({"completion_dispatch", "get_coords", "sprintf", "print_text_message", "set_event", "notify_ui"}),
           "C8: SIM_ACTIVE=0 -- snd_play does NOT fire, but coords/message/set_event/tail still do "
           "(0x00473dc9 JZ @0x00473dd0 skips only the voice-line block)");
        ck(g_snd_play.empty(), "C8: no voice line at all when SIM_ACTIVE=0");
        ck(g_coords.size() == 1, "C8: bldg_get_coords still fires (SIM_ACTIVE does not gate it)");
        ck_eq((uint32_t)fx.cam_pan_target_col, 50u, "C8: cam_pan_target still written when SIM_ACTIVE=0");
    }

    // =================================================================================================
    // C9 -- queued_count[0]!=0: the ENTIRE local block (voice/coords/message) is skipped, but
    // completion_dispatch/RATE-ZERO-GUARD/state/set_event/tail -- all UNCONDITIONAL inside the gate --
    // still fire. Proves queued_count[0] gates only the local-only sub-block, not the whole gate arm.
    // =================================================================================================
    {
        Seed s;
        s.queued_count0 = 7; // != 0 -> the whole local block is skipped (0x00473dbc CMP / 0x00473dc3 JNZ)
        seed_and_run(fx, s);

        ck(trace_eq({"completion_dispatch", "set_event", "notify_ui"}),
           "C9: queued_count[0]!=0 -- voice/coords/message ALL skipped, dispatch/set_event/tail still fire "
           "(0x00473dbc CMP / 0x00473dc3 JNZ -> LAB_00473f71)");
        ck_eq((uint32_t)fx.cam_pan_target_col, (uint32_t)s.cam_pan_col_seed, "C9: cam_pan_target_col UNCHANGED");
        ck_eq((uint32_t)fx.cam_pan_target_row, (uint32_t)s.cam_pan_row_seed, "C9: cam_pan_target_row UNCHANGED");
        ck_eq((uint32_t)fx.b(s.player, s.index).state, BLDG_STATE_PROD_PICK_NEXT,
              "C9: b.state STILL becomes PROD_PICK_NEXT -- unconditional inside the gate regardless of "
              "the local-only queued_count[0] check");
        ck_eq((uint32_t)(uint16_t)fx.b(s.player, s.index).online_state, 0x2345u, "C9: b.online_state still UNCHANGED");
    }

    // =================================================================================================
    // C10 -- other player (cur_player != PlayerSide): same shape as C9 via a DIFFERENT gate
    // (0x00473d95 JNZ, before the queued_count[0] check is even reached).
    // =================================================================================================
    {
        Seed s;
        s.player_side = (int16_t)(s.player + 9); // != player -> "other"
        seed_and_run(fx, s);

        ck(trace_eq({"completion_dispatch", "set_event", "notify_ui"}),
           "C10: other-player branch -- voice/coords/message never fire (0x00473d95 JNZ -> LAB_00473f71); "
           "dispatch + tail still do");
        ck_eq((uint32_t)fx.cam_pan_target_col, (uint32_t)s.cam_pan_col_seed, "C10: cam_pan_target_col UNCHANGED");
        ck_eq((uint32_t)fx.cam_pan_target_row, (uint32_t)s.cam_pan_row_seed, "C10: cam_pan_target_row UNCHANGED");
    }

    // =================================================================================================
    // C11 -- RATE-ZERO-GUARD: rate==0.0 (efficiency==0.0) forces the divisor to 1.0 instead of dividing
    // by zero -- a plain bit-exact `==0.0` test, no x87 landmine (see the module header). Proven by an
    // exact, otherwise-impossible result: without the guard, (40.0-30.0)/0.0 is +inf, not 10.0.
    // =================================================================================================
    {
        Seed s;
        s.efficiency        = 0.0;  // rate = 0.0 * unit_quant = 0.0
        s.tick_budget_in    = 5.0;  // delta = tick_budget*rate = 0.0 regardless of tick_budget_in
        s.cycle_progress_in = 40.0; // cycle_progress stays 40.0 (delta is 0)
        s.build_time        = 30.0; // gate taken: 40.0 >= 30.0
        seed_and_run(fx, s);

        ck_eq_d(fx.tick_budget, 10.0,
                "C11 RATE-ZERO-GUARD: rate==0.0 -> divisor forced to 1.0 -> tick_budget = (40.0-30.0)/1.0 "
                "= 10.0, NOT +inf (0x00473f71-0x00473f8e bit-test, 0x00473fcb FDIV)");
    }
    // (C5 above already pins the divisor==rate, non-guarded formula: (34.0-25.0)/6.0 = 1.5.)

    // =================================================================================================
    // C12 -- param_1(EAX)/param_2(EDX) are DEAD: running the identical scenario with two different
    // pairs of incoming param_1/param_2 must produce byte-identical results and the identical call
    // trace, since nothing in the body ever reads them.
    // =================================================================================================
    {
        Seed sa;
        sa.param_1_in = 0xAAAAAAAAu;
        sa.param_2_in = 0xBBBBBBBBu;
        seed_and_run(fx, sa);
        const double   cp_a    = fx.b(sa.player, sa.index).cycle_progress;
        const double   tb_a    = fx.tick_budget;
        const uint32_t state_a = fx.b(sa.player, sa.index).state;
        const size_t   trace_a = g_trace.size();

        Seed sb;
        sb.param_1_in = 0x55555555u;
        sb.param_2_in = 0x66666666u;
        seed_and_run(fx, sb);
        const double   cp_b    = fx.b(sb.player, sb.index).cycle_progress;
        const double   tb_b    = fx.tick_budget;
        const uint32_t state_b = fx.b(sb.player, sb.index).state;
        const size_t   trace_b = g_trace.size();

        ck_eq_d(cp_a, cp_b, "C12: param_1/param_2 are DEAD -- cycle_progress identical regardless of their value");
        ck_eq_d(tb_a, tb_b, "C12: param_1/param_2 dead -- tick_budget identical");
        ck_eq(state_a, state_b, "C12: param_1/param_2 dead -- b.state identical");
        ck_eq((uint32_t)trace_a, (uint32_t)trace_b, "C12: param_1/param_2 dead -- same call-trace length");
    }

    // =================================================================================================
    // C13 -- fine_to_tile's truncating (toward zero, not floor) division by 32, pinned on NEGATIVE
    // inputs (the positive case is already pinned by C5's 1600->50/1760->55).
    // =================================================================================================
    {
        Seed s;
        s.fine_x = -1;  // -1/32 truncates TOWARD ZERO -> tile 0 (NOT floor's -1)
        s.fine_y = -33; // -33/32 truncates TOWARD ZERO -> tile -1 (NOT floor's -2)
        seed_and_run(fx, s);

        ck_eq((uint32_t)fx.cam_pan_target_col, 0u,
              "C13: fine_to_tile(-1) = -1/32 = 0, truncating toward zero (0x00473e93-0x00473e9e "
              "SAR/SHL/SBB/SAR idiom)");
        ck_eq((uint32_t)fx.cam_pan_target_row, (uint32_t)-1,
              "C13: fine_to_tile(-33) = -33/32 = -1 (truncating), NOT floor's -2 (0x00473eae-0x00473eb9)");
    }

    // =================================================================================================
    // DIVERGENCE-A [FINDING, not a normal pin] -- the asm re-reads cur_building->sub_id FRESH FROM
    // MEMORY to recompute the production row for the queued_count[0] gate AFTER
    // llm_strat_bldg_completion_dispatch has already run (0x00473d9b-0x00473dc3, entirely after the
    // 0x00473d83 CALL) -- sub_id is never kept in a register across the call. The .cpp under test
    // instead binds `prod` (and, transitively, the whole local-block gate) to
    // `v.productions[cur_player*8 + b.sub_id]` ONCE, evaluated at the TOP of the function, BEFORE
    // completion_dispatch runs, and never re-reads it. If completion_dispatch mutates
    // cur_building->sub_id (plausible for "a production/order just completed" dispatch), the asm and
    // the .cpp disagree about WHICH production row's queued_count[0] gates the local block.
    //
    // This case pins the ASM's fresh-read behaviour via a mock that mutates b.sub_id from 3 to 6 DURING
    // completion_dispatch. Row A (sub_id=3, what a STALE pre-dispatch `prod` reference would read) is
    // seeded with queued_count[0]=7 (nonzero -> would SKIP the local block); row B (sub_id=6, what the
    // fresh post-dispatch re-read the fixed .cpp performs actually sees) is seeded with
    // queued_count[0]=0 (-> local block eligible). PASSES against the .cpp as fixed 2026-08-22.
    // =================================================================================================
    {
        Seed s;
        s.sub_id                  = 3;
        s.queued_count0           = 7; // row A (what the .cpp's stale `prod` reference sees): nonzero
        s.dispatch_mutates_sub_id = true;
        s.dispatch_new_sub_id     = 6; // row B, what a fresh post-dispatch re-read would see
        s.seed_row_b              = true;
        s.row_b_sub_id            = 6;
        s.row_b_queued_count0     = 0;                  // row B: zero -> local block eligible per the asm
        s.row_b_active_unit_type  = s.active_unit_type; // same unit-type family in both rows -- isolates
                                                        // this case to JUST the row-gating question
        seed_and_run(fx, s);

        ck(trace_eq({"completion_dispatch", "snd_play", "get_coords", "sprintf", "print_text_message",
                     "set_event", "notify_ui"}),
           "DIVERGENCE-A: per the asm's POST-DISPATCH fresh re-read of cur_building->sub_id "
           "(0x00473d9b-0x00473dc3), the local block runs against row B (queued_count[0]==0) -- a stale "
           "pre-dispatch `prod` reference would instead see row A's queued_count[0]==7 and SKIP the "
           "local block entirely. Fixed 2026-08-22 (sim_bldg_state_prod.cpp re-derives the row fresh "
           "at each post-dispatch site); this pins that against regression.");
    }

    // =================================================================================================
    // DIVERGENCE-B [FINDING] -- narrower than A: sub_id is FIXED (no row change), but the asm re-reads
    // productions[...].active_unit_type FRESH FROM MEMORY, twice, AFTER completion_dispatch
    // (0x00473dd6-0x00473e04 for the voice-line's soldier_count check, and again at
    // 0x00473ebe-0x00473eec for the message's), while the .cpp under test binds `active_type` (and
    // `cu = cfg_units[active_type]`) ONCE at the top of the function, before completion_dispatch runs,
    // and reuses that same snapshot for BOTH checks. If completion_dispatch mutates
    // productions[...].active_unit_type -- exactly what THIS STATE FAMILY'S OWN sibling function,
    // llm_strat_bldg_state_prod_pick_next, does to this very field -- the asm and the .cpp choose
    // different voice ids / message reasons.
    //
    // This case pins the ASM's fresh-read behaviour: the completion_dispatch mock mutates
    // active_unit_type from 40 (soldier_count=0, "unit ready" family) to 77 (soldier_count=5, "soldier
    // load" family). PASSES against the .cpp as fixed 2026-08-22 (picks the "soldier load" id/text,
    // matching type 77's post-dispatch soldier_count==5, not the stale type 40 reading).
    // =================================================================================================
    {
        Seed s;
        s.active_unit_type             = 40;
        s.soldier_count                = 0; // cfg_units[40]: "unit ready" family (what the .cpp's stale `cu` sees)
        s.dispatch_mutates_active_type = true;
        s.dispatch_new_active_type     = 77;
        s.extra_seed                   = [](sim_fixture &fixture) {
            fixture.cfg_units[77].soldier_count = 5; // "soldier load" family -- what a fresh post-dispatch
                                                     // re-read of active_unit_type=77 would see
        };
        seed_and_run(fx, s);

        ck(g_snd_play.size() == 1, "DIVERGENCE-B: snd_play fires once regardless of which side of the divergence wins");
        if (g_snd_play.size() == 1) {
            ck_eq((uint32_t)g_snd_play[0].id, (uint32_t)(VOICE_BASE_SOLDIER_LOAD + VOICE_RACE2_OFFSET),
                  "DIVERGENCE-B: per the asm's post-dispatch fresh re-read of active_unit_type "
                  "(now 77, soldier_count=5>=1), the voice id is BASE_SOLDIER_LOAD(0xc)+0x12=0x1e -- a "
                  "stale pre-dispatch `cu` (type 40, soldier_count=0) would instead pick "
                  "BASE_UNIT_READY(9)+0x12=0x1b. Fixed 2026-08-22; this pins that against regression.");
        }
        ck(g_sprintf_calls.size() == 1, "DIVERGENCE-B: sprintf fires once regardless of which side of the divergence wins");
        if (g_sprintf_calls.size() == 1) {
            ck(g_sprintf_calls[0].a1 == k_reason_soldiers_ready,
               "DIVERGENCE-B: same fresh-vs-stale question for the message reason text -- "
               "text_ptrs[0x9d] (SOLDIERS_READY) per the asm's fresh re-read; a stale reading would "
               "produce text_ptrs[0x76] (UNIT_READY) instead. Fixed 2026-08-22.");
        }
    }
}

} // namespace mh::sim::test
