//
// sim_bldg_state_researching_selftest.cpp -- `simtest` cases for llm_strat_bldg_state_researching
// (sim/sim_bldg_state_upgrade_research.h/.cpp @0x00472f3c), SIM1-G4 building_tick machinery -- the
// "research in progress" per-tick handler for a building whose state dispatches here.
//
// SCOPE: this file pins the full call/write order, BOTH FP-comparison gates' exact boundaries (the
// first-stage completion gate, JC, `<=`; and the SECOND-stage overflow-carry gate, JBE, `>`), the
// cycle_progress accumulation arithmetic, the LOCAL-vs-OTHER-PLAYER split, the SIM_ACTIVE gate on the
// voice line only, the race-dependent voice-line sound id, the state-transition LOW-16-BIT mask, and
// -- the crux of this function, distinguishing it from every sibling in the closure -- that the
// completion threshold is `Projects[labs[cur_player][cur_building->sub_id].active_project_id].
// build_time`, RE-READ FRESH at each of its three use sites (completion gate / message name /
// overflow gate+subtraction) rather than cached once. It does NOT reproduce w_sprintf's actual text
// formatting content beyond which text_ptrs entries feed it (the formatting path itself is
// presentation, same posture as every sibling oracle in this closure).
//
// EXPECTED BEHAVIOUR HAND-DERIVED FROM THE DISASSEMBLY
// (tmp/decomp_sim/llm_strat_bldg_state_researching_00472f3c.asm), cross-checked against the .cpp/.h's
// own per-line address citations -- NOT read off the .cpp body alone:
//
//   cycle_progress += tick_budget * efficiency (0x00472f54-0x00472f6b, unconditional, runs even when
//   the completion gate below is not taken).
//   Completion gate (0x00472f6e-0x00472fa8): `if (Projects[labs[cur_player][sub_id].active_project_id]
//   .build_time <= cycle_progress)` (JC @0x00472fa8 skips the whole block straight to LAB_004730e4,
//   the SECOND stage, when NOT taken -- unlike construction/upgrading, skipping this gate does NOT
//   skip the whole function: the second stage always runs).
//   Inside the gate, LOCAL PLAYER ONLY (cur_player == PlayerSide, 0x00472fae-0x00472fbb):
//     SIM_ACTIVE-gated voice line (0x00472fc1-0x00472ff3): snd_play(id=(race==2?0x12:0)+0xa,
//     volume=100) -- base id 0xa, DIFFERENT from upgrading's 0xf and construction's 4.
//     THEN (unconditional in the local-player arm) bldg_get_coords writes fine coords DIRECTLY into
//     CAM_PAN_TARGET_COL/_ROW (0x00472ffd-0x00473046), truncated fine->tile in place.
//     THEN a "%s: %s" message (name=Projects[...].name -- the PROJECT's OWN name, NOT the building's,
//     unlike every other sibling in this closure -- reason=text_ptrs[0x7a]) via w_sprintf__vss +
//     game_ui_PrintTextMessage (0x0047304b-0x004730a1).
//   UNCONDITIONALLY inside the gate, join point LAB_004730a6 (both local and other player,
//   0x004730a6-0x004730c0): completion_dispatch(cur_player, cur_index, param_3, param_4, game_clock)
//   -- param_3/param_4 are the CAM_PAN_TARGET addresses on the local branch, the literal constant 0 on
//   the other-player branch (this is void(void): there is no caller-supplied EBX/ECX to forward, so
//   there is no "verbatim incoming sentinel" half of this hazard the way construction/upgrading have
//   -- see sim_bldg_state_upgrade_research.h/.cpp's COMPLETION_DISPATCH_PARAM_UNRESOLVED note).
//     Then cur_building->state = Building[bid].state_transition_ids[1] LOW 16 BITS (0x004730c5-
//     0x004730e0). NO production-type override, NO refresh_building call anywhere in this function
//     (confirmed absent from the disassembly -- see the header).
//   THE SECOND STAGE, join point LAB_004730e4, ALWAYS REACHED (gate taken or not, 0x004730e4-
//   0x00473177): the SAME threshold is RECOMPUTED FRESH (Projects[labs[...].active_project_id].
//   build_time, re-read via the SAME byte-offset arithmetic as the first gate) and compared via JBE
//   (not JC) against cycle_progress: if `cycle_progress > threshold` (ordered, NaN-safe restatement of
//   "JBE not taken"): `tick_budget = cycle_progress - threshold`; ELSE (`cycle_progress <= threshold`,
//   OR unordered): `tick_budget = 0.0`. `bldg_notify_ui(cur_player, cur_index)` always follows,
//   unconditionally.
//
#include "sim/sim_bldg_state_upgrade_research.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// ---- shared trace: one sequence proves CALL ORDER across all 6 callees -------------------------
std::vector<const char *> g_trace;
void                      tr(const char *tag) { g_trace.push_back(tag); }

bool trace_eq(const std::vector<const char *> &want) {
    if (g_trace.size() != want.size()) return false;
    for (size_t i = 0; i < want.size(); ++i)
        if (std::strcmp(g_trace[i], want[i]) != 0) return false;
    return true;
}

// ---- return-value knobs (settable per case BEFORE seed_and_run) --------------------------------
int32_t g_get_coords_fine_x = 0;
int32_t g_get_coords_fine_y = 0;

// O1's re-read probe: if non-null/non-negative, rec_get_coords mutates the lab's active_project_id
// DURING the call -- i.e. strictly BETWEEN the completion gate's read (which already ran, above this
// call site) and the message-name/overflow-gate reads (which run AFTER it). A translation that
// hoisted `active_project()` into a local ONCE at the top of the function would still see the OLD
// project here; the real disassembly re-reads the global each time, so it must see the NEW one.
lab    *g_switch_lab        = nullptr;
int32_t g_switch_to_project = -1;

// ---- per-callee recorders (6, one per bldg_state_researching_calls member) ---------------------
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
    if (g_switch_lab != nullptr && g_switch_to_project >= 0) {
        g_switch_lab->active_project_id = g_switch_to_project;
    }
}

struct SndCall {
    int32_t id, volume;
};
std::vector<SndCall> g_snd_play;
void                 rec_snd_play(int32_t id, int32_t volume) {
    tr("snd_play");
    g_snd_play.push_back({id, volume});
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

struct CompleteCall {
    uint32_t player, index, param_3, param_4;
    double   game_clock;
};
std::vector<CompleteCall> g_complete_calls;
void                      rec_completion_dispatch(uint32_t player, uint32_t index, uint32_t param_3, uint32_t param_4,
                                                  double game_clock) {
    tr("completion_dispatch");
    g_complete_calls.push_back({player, index, param_3, param_4, game_clock});
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

const bldg_state_researching_calls g_calls = {
    &rec_snd_play,
    &rec_get_coords,
    &rec_sprintf,
    &rec_print_text_message,
    &rec_completion_dispatch,
    &rec_bldg_notify_ui,
};

void reset_observations() {
    g_trace.clear();
    g_coords.clear();
    g_snd_play.clear();
    g_sprintf_calls.clear();
    g_print_text_message_count = 0;
    g_print_text_message_ptrs.clear();
    g_complete_calls.clear();
    g_notify_ui_calls.clear();
    g_switch_lab        = nullptr;
    g_switch_to_project = -1;
}

// ---- text pointers used to pin WHICH text_ptrs slots feed the "%s: %s" message -----------------
// Distinct addresses (not dereferenced by the recorder) so a translation that read the wrong index
// (or swapped name/reason, or read the BUILDING's id instead of the PROJECT's name -- the crux
// divergence of this function vs every sibling) is caught by pointer identity.
const wchar_t k_name_text_a[] = L"NAME_TXT_PROJECT_A";
const wchar_t k_name_text_b[] = L"NAME_TXT_PROJECT_B";
const wchar_t k_reason_text[] = L"REASON_TXT";

// ---- fixture seeding -----------------------------------------------------------------------------
struct Seed {
    uint16_t player  = 0;
    int32_t  index   = 1;
    uint16_t cfg_row = 7; // building_id -> cfg_buildings index (state_transition_ids source)
    uint8_t  sub_id  = 3; // building's own sub_id -> labs[player][sub_id] row

    int32_t active_project_id = 12; // labs[...].active_project_id -> cfg_projects row (Project A)
    int32_t proj_text_id      = 55; // Projects[A].name, the "name" half of the message

    // The state-transition value deliberately carries GARBAGE in its upper 16 bits (0x1234) so a
    // translation that forgot the `& 0xffff` mask disagrees with the fixture; only the low 16 bits
    // (0xabcd) are the real next-state.
    int32_t  state_transition_raw = 0x1234abcd;
    uint16_t initial_state        = 0x9999; // sentinel BEFORE the call -- distinct from 0xabcd

    double efficiency        = 2.0;
    double cycle_progress_in = 5.0;
    double tick_budget       = 10.0; // delta = tick_budget*efficiency = 20.0 -> cycle_progress = 25.0
    double build_time        = 25.0; // gate threshold (Project A's); case overrides to probe both sides

    int16_t player_side = 0; // == player -> "local"
    int32_t player_race = 0;
    int32_t sim_active  = 0;

    double game_clock = 123.5; // distinct nonzero, forwarded verbatim to completion_dispatch's param_5

    int32_t fine_x = 1600; // -> tile 50 (1600 / 32, exact)
    int32_t fine_y = 1760; // -> tile 55 (1760 / 32, exact)

    int32_t cam_pan_col_seed = -777;
    int32_t cam_pan_row_seed = -888;

    // O1's re-read probe (disabled unless probe_project_id >= 0). See O1's own comment below for the
    // full derivation. When armed, rec_get_coords mutates labs[...].active_project_id to this project
    // DURING the call, strictly between the completion gate's read (already past) and the
    // message-name/overflow-gate reads (still to come).
    int32_t probe_project_id         = -1;
    int32_t probe_project_text_id    = -1;
    double  probe_project_build_time = 0.0;
};

void seed_and_run(sim_fixture &fx, const Seed &s) {
    fx.reset();

    building &b      = fx.b(s.player, s.index);
    b.building_id    = s.cfg_row;
    b.sub_id         = s.sub_id;
    b.cycle_progress = s.cycle_progress_in;
    b.efficiency     = s.efficiency;
    b.state          = s.initial_state;

    fx.cur_building_ptr = &b;
    fx.view_cur_player  = s.player;
    fx.view_cur_index   = (uint16_t)s.index;

    cfg_building &cb           = fx.cfg_buildings[s.cfg_row];
    cb.state_transition_ids[1] = s.state_transition_raw;

    lab &l              = fx.labs[(size_t)((int32_t)s.player * LABS_PER_PLAYER + s.sub_id)];
    l.active_project_id = s.active_project_id;

    cfg_project &proj = fx.cfg_projects[(size_t)s.active_project_id];
    proj.name         = s.proj_text_id;
    proj.build_time   = s.build_time;

    fx.player_side = s.player_side;
    fx.player_race = s.player_race;
    fx.sim_active  = s.sim_active;
    fx.tick_budget = s.tick_budget;
    fx.game_clock  = s.game_clock;

    fx.cam_pan_target_col = s.cam_pan_col_seed;
    fx.cam_pan_target_row = s.cam_pan_row_seed;

    fx.text_ptrs[(size_t)s.proj_text_id] = k_name_text_a; // "name" half -- Projects[A].name
    fx.text_ptrs[0x7a]                   = k_reason_text; // "reason" half -- TEXT_ID_RESEARCH_COMPLETE

    g_get_coords_fine_x = s.fine_x;
    g_get_coords_fine_y = s.fine_y;

    reset_observations(); // clears g_switch_lab/g_switch_to_project too -- arm the probe AFTER this

    if (s.probe_project_id >= 0) {
        fx.cfg_projects[(size_t)s.probe_project_id].name       = s.probe_project_text_id;
        fx.cfg_projects[(size_t)s.probe_project_id].build_time = s.probe_project_build_time;
        fx.text_ptrs[(size_t)s.probe_project_text_id]          = k_name_text_b;
        g_switch_lab                                           = &l;
        g_switch_to_project                                    = s.probe_project_id;
    }

    sim_store own = fx.store();
    detail::bldg_state_researching(fx.view(), own, g_calls);
}

} // namespace

void run_bldg_state_researching_tests() {
    sim_fixture fx;

    // =================================================================================================
    // R1 -- completion gate NOT taken, CLEAR gap (build_time (26.0) > cycle_progress (25.0) after
    // accumulation, JC @0x00472fa8 taken): ONLY the unconditional accumulation + the SECOND stage
    // (which is ALWAYS reached, unlike construction/upgrading's simple tail) run. Since cycle_progress
    // < threshold, the overflow gate's `>` is also false -> tick_budget zeroes. NOTHING from the
    // completion-gate body fires, and state stays at the sentinel (UNCHANGED).
    // =================================================================================================
    {
        Seed s;
        s.build_time = 26.0; // 25.0 (accumulated) < 26.0 -> completion gate NOT taken
        seed_and_run(fx, s);

        ck(trace_eq({"notify_ui"}),
           "R1: completion gate not taken -- only the always-reached second stage's notify_ui fires "
           "(0x00473185)");
        ck_eq_d(fx.b(s.player, s.index).cycle_progress, 25.0,
                "R1: cycle_progress = tick_budget*efficiency + cycle_progress_in = 10.0*2.0+5.0 = 25.0 "
                "(0x00472f54-0x00472f6b, accumulates even though the gate below is not taken)");
        ck_eq((uint32_t)fx.b(s.player, s.index).state, (uint32_t)s.initial_state,
              "R1: state UNCHANGED -- the state-transition write (0x004730e0) is INSIDE the completion "
              "gate, never reached when the gate is skipped");
        ck_eq_d(fx.tick_budget, 0.0,
                "R1: overflow gate (0x00473120 JBE) -- cycle_progress(25.0) <= threshold(26.0) -> "
                "tick_budget = 0.0 (0x00473163-0x0047316d)");
        ck(g_notify_ui_calls.size() == 1 && g_notify_ui_calls[0].player == s.player &&
               g_notify_ui_calls[0].index == (uint32_t)s.index,
           "R1: bldg_notify_ui(cur_player, cur_index) (0x00473177-0x00473185)");
        ck(g_coords.empty() && g_snd_play.empty() && g_sprintf_calls.empty() &&
               g_print_text_message_count == 0 && g_complete_calls.empty(),
           "R1: none of the completion-gate callees fire when the gate is skipped");
        ck_eq((uint32_t)fx.cam_pan_target_col, (uint32_t)s.cam_pan_col_seed,
              "R1: cam_pan_target_col UNCHANGED -- only the local-player arm inside the gate writes it");
        ck_eq((uint32_t)fx.cam_pan_target_row, (uint32_t)s.cam_pan_row_seed,
              "R1: cam_pan_target_row UNCHANGED");
    }

    // =================================================================================================
    // R1b -- completion gate NOT taken at the SMALLEST possible margin (build_time one ULP ABOVE
    // cycle_progress): pins the JC gate's `<=` boundary from the "not taken" side at maximum precision
    // (per the header's NaN-SAFE RESTATEMENT note -- this gate's naive `<=` is already NaN-safe, but
    // every FP-comparison boundary still gets its own pin here).
    // =================================================================================================
    {
        Seed s;
        s.build_time = std::nextafter(25.0, INFINITY); // one ULP above 25.0 -- still NOT taken
        seed_and_run(fx, s);

        ck(trace_eq({"notify_ui"}),
           "R1b: build_time one ULP above cycle_progress -- gate still NOT taken (0x00472fa8 JC), only "
           "notify_ui fires");
        ck_eq((uint32_t)fx.b(s.player, s.index).state, (uint32_t)s.initial_state,
              "R1b: state UNCHANGED at the one-ULP-not-taken boundary");
        ck_eq_d(fx.tick_budget, 0.0, "R1b: overflow gate also not-taken at this margin -> tick_budget=0.0");
    }

    // =================================================================================================
    // R2 -- completion gate TAKEN at the EXACT `<=` boundary (build_time == cycle_progress, pins the
    // gate as `<=` not `<`, from the "taken" side), LOCAL player, SIM_ACTIVE=1, race==2: the full
    // local-player arm fires, in order, message uses the PROJECT's name (not the building's -- the
    // crux divergence from every sibling), and completion_dispatch's param_3/param_4 are THE ADDRESSES
    // of cam_pan_target_col/_row. Simultaneously pins the SECOND stage's JBE boundary from the "zero"
    // side: at EXACT equality cycle_progress is NOT > threshold, so tick_budget must be 0.0 even though
    // the first gate WAS taken (a naive same-boundary-both-gates assumption would get this right by
    // luck for ordered inputs; the two gates are genuinely independent comparisons on the same values).
    // =================================================================================================
    {
        Seed s;
        s.build_time  = 25.0;     // == cycle_progress after accumulation (25.0) -- boundary
        s.player_side = s.player; // 0 == 0 -> local
        s.sim_active  = 1;
        s.player_race = 2;
        seed_and_run(fx, s);

        ck(trace_eq({"snd_play", "get_coords", "sprintf", "print_text_message", "completion_dispatch",
                     "notify_ui"}),
           "R2: exact call order for the local/sim-active/race2 completion arm (0x00472fc1 voice -> "
           "0x00472ffd coords -> 0x0047304b message -> 0x004730a6 completion_dispatch -> 0x00473177 "
           "second-stage tail)");

        ck_eq_d(fx.b(s.player, s.index).cycle_progress, 25.0,
                "R2: cycle_progress = 10.0*2.0+5.0 = 25.0, exactly at build_time -- gate taken "
                "(0x00472fa8: `build_time <= cycle_progress`, JC NOT taken at equality)");

        ck(g_snd_play.size() == 1 && g_snd_play[0].id == 0x1c && g_snd_play[0].volume == 100,
           "R2: snd_play(id=0xa+0x12=0x1c [race==2], volume=100) (0x00472fda-0x00472ff3) -- 0xa is "
           "researching's OWN base id, distinct from upgrading's 0xf and construction's 4");

        ck(g_coords.size() == 1 && g_coords[0].player == s.player && g_coords[0].index == s.index,
           "R2: bldg_get_coords(cur_player, cur_index, &cam_pan_target_col, &cam_pan_target_row) "
           "(0x00472ffd-0x00473010)");
        ck_eq((uint32_t)fx.cam_pan_target_col, 50u,
              "R2: cam_pan_target_col = tile(fine_x) = 1600/32 = 50 (0x00473015-0x0047302b)");
        ck_eq((uint32_t)fx.cam_pan_target_row, 55u,
              "R2: cam_pan_target_row = tile(fine_y) = 1760/32 = 55 (0x00473030-0x00473046)");

        ck(g_sprintf_calls.size() == 1, "R2: w_sprintf__vss fires once (0x0047304b-0x00473094)");
        if (g_sprintf_calls.size() == 1) {
            const auto &sp = g_sprintf_calls[0];
            ck(sp.a0 == k_name_text_a,
               "R2 CRUX: sprintf name-arg = text_ptrs[Projects[active_project_id].name] -- the "
               "PROJECT's own name (0x00473079 read of Project.name@0x4, 0x00473082 push), NOT "
               "Building[bid].id the way every other sibling handler does");
            ck(sp.a1 == k_reason_text,
               "R2: sprintf reason-arg = text_ptrs[0x7a] (TEXT_ID_RESEARCH_COMPLETE, literal dword ptr "
               "[0x005845f4] push @0x0047304b)");
            ck(sp.dst == fx.text_scratch.data(), "R2: sprintf writes into own.text_scratch() (0xe15178)");
        }
        ck(g_print_text_message_count == 1 &&
               (g_print_text_message_ptrs.empty() ||
                g_print_text_message_ptrs[0] == (void *)fx.text_scratch.data()),
           "R2: game_ui_PrintTextMessage(own.text_scratch()) fires once, same buffer as sprintf's dst "
           "(0x0047309c-0x004730a1)");

        ck(g_complete_calls.size() == 1, "R2: completion_dispatch fires once (0x004730c0)");
        if (g_complete_calls.size() == 1) {
            const auto    &cc            = g_complete_calls[0];
            const uint32_t want_col_addr = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&fx.cam_pan_target_col));
            const uint32_t want_row_addr = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&fx.cam_pan_target_row));
            ck(cc.player == s.player && cc.index == (uint32_t)s.index,
               "R2: completion_dispatch(cur_player, cur_index, ...)");
            ck_eq(cc.param_3, want_col_addr,
                  "R2 REGISTER-REUSE HAZARD: completion_dispatch's param_3 = &cam_pan_target_col "
                  "(never reloaded before 0x004730c0)");
            ck_eq(cc.param_4, want_row_addr,
                  "R2 REGISTER-REUSE HAZARD: completion_dispatch's param_4 = &cam_pan_target_row "
                  "(never reloaded before 0x004730c0)");
            ck_eq_d(cc.game_clock, s.game_clock,
                    "R2: completion_dispatch's param_5 = game_clock, forwarded verbatim "
                    "(0x004730a6/0x004730ac push GAME_CLOCK's two dwords)");
        }

        ck_eq((uint32_t)fx.b(s.player, s.index).state, 0xabcdu,
              "R2: state = Building[bid].state_transition_ids[1] LOW 16 BITS (0x1234abcd & 0xffff = "
              "0xabcd) -- the upper 0x1234 garbage must be masked off (0x004730d9 `MOV DX, word ptr "
              "[...]`, 16-bit load)");

        ck(g_notify_ui_calls.size() == 1 && g_notify_ui_calls[0].player == s.player &&
               g_notify_ui_calls[0].index == (uint32_t)s.index,
           "R2: bldg_notify_ui(cur_player, cur_index) fires at the second-stage tail too (0x00473185)");
        ck_eq_d(fx.tick_budget, 0.0,
                "R2 SECOND-STAGE JBE BOUNDARY (zero side): cycle_progress(25.0) is NOT > "
                "threshold(25.0) at exact equality -- tick_budget = 0.0 even though the FIRST gate was "
                "taken (0x00473120 JBE taken at equality: CF=0,ZF=1 both set the 'not greater' path)");
    }

    // =================================================================================================
    // R3 -- same as R2 but race != 2: voice-line sound id is the BASE id (0xa), no 0x12 offset.
    // =================================================================================================
    {
        Seed s;
        s.build_time  = 25.0;
        s.player_side = s.player;
        s.sim_active  = 1;
        s.player_race = 0;
        seed_and_run(fx, s);

        ck(g_snd_play.size() == 1 && g_snd_play[0].id == 0xa && g_snd_play[0].volume == 100,
           "R3: race!=2 -> snd_play id = base(0xa) + 0, no 0x12 offset (0x00472fd8 JNZ taken -> "
           "LAB_00472fe3)");
    }

    // =================================================================================================
    // R4 -- gate taken, LOCAL player, SIM_ACTIVE=0: the voice line is skipped (SIM_ACTIVE gate,
    // 0x00472fc8 JZ), but get_coords/message/completion_dispatch still fire -- only snd_play is gated.
    // =================================================================================================
    {
        Seed s;
        s.build_time  = 25.0;
        s.player_side = s.player;
        s.sim_active  = 0;
        s.player_race = 2; // irrelevant here since the whole voice-line block is skipped
        seed_and_run(fx, s);

        ck(trace_eq({"get_coords", "sprintf", "print_text_message", "completion_dispatch", "notify_ui"}),
           "R4: SIM_ACTIVE=0 -- snd_play does NOT fire, but coords/message/completion_dispatch still do "
           "(0x00472fc8 JZ @0x00472fc8 skips only the voice-line block, not the rest of the local arm)");
        ck(g_snd_play.empty(), "R4: no voice line at all when SIM_ACTIVE=0");
        ck(g_coords.size() == 1, "R4: bldg_get_coords still fires (SIM_ACTIVE does not gate it)");
        ck_eq((uint32_t)fx.cam_pan_target_col, 50u, "R4: cam_pan_target still written when SIM_ACTIVE=0");
    }

    // =================================================================================================
    // R5 -- gate taken, OTHER player (cur_player != PlayerSide, 0x00472fbb JNZ taken -> skips straight
    // to LAB_004730a6): NONE of voice/coords/message fire (cam_pan_target UNCHANGED), but
    // completion_dispatch AND the state transition STILL fire, and completion_dispatch's param_3/
    // param_4 are the LITERAL CONSTANT 0 (COMPLETION_DISPATCH_PARAM_UNRESOLVED) -- NOT the cam_pan
    // addresses, and (unlike construction/upgrading, which are real __watcall functions with genuine
    // incoming EBX/ECX) there is no caller-forwarded sentinel to check either, because this function
    // is void(void).
    // =================================================================================================
    {
        Seed s;
        s.build_time  = 25.0;
        s.player_side = (int16_t)(s.player + 5); // != player -> "other"
        s.sim_active  = 1;                       // would matter if the voice line fired -- it must not
        s.player_race = 2;
        seed_and_run(fx, s);

        ck(trace_eq({"completion_dispatch", "notify_ui"}),
           "R5: other-player branch -- voice/coords/message never fire; completion_dispatch + the "
           "second-stage tail still do (0x00472fbb JNZ -> LAB_004730a6 skips 0x00472fc1-0x004730a1 "
           "entirely)");
        ck(g_snd_play.empty() && g_coords.empty() && g_sprintf_calls.empty() && g_print_text_message_count == 0,
           "R5: none of the local-only calls fire on the other-player branch");
        ck_eq((uint32_t)fx.cam_pan_target_col, (uint32_t)s.cam_pan_col_seed,
              "R5: cam_pan_target_col UNCHANGED -- only the local-player arm writes it");
        ck_eq((uint32_t)fx.cam_pan_target_row, (uint32_t)s.cam_pan_row_seed,
              "R5: cam_pan_target_row UNCHANGED");

        ck(g_complete_calls.size() == 1, "R5: completion_dispatch still fires (unconditional in the gate)");
        if (g_complete_calls.size() == 1) {
            const auto &cc = g_complete_calls[0];
            ck(cc.player == s.player && cc.index == (uint32_t)s.index,
               "R5: completion_dispatch(cur_player, cur_index, ...)");
            ck_eq(cc.param_3, 0u,
                  "R5 (inverse of R2): completion_dispatch's param_3 = the literal constant 0 -- this "
                  "void(void) function has no caller-supplied EBX to forward on the other-player "
                  "branch, unlike construction/upgrading's genuine incoming params");
            ck_eq(cc.param_4, 0u,
                  "R5 (inverse of R2): completion_dispatch's param_4 = the literal constant 0, same "
                  "reasoning");
            ck_eq_d(cc.game_clock, s.game_clock, "R5: game_clock still forwarded on the other-player branch");
        }

        ck_eq((uint32_t)fx.b(s.player, s.index).state, 0xabcdu,
              "R5: state transition still fires on the other-player branch (unconditional inside the "
              "gate, only the local sub-block is skipped)");
        ck(g_notify_ui_calls.size() == 1, "R5: bldg_notify_ui still fires at the second-stage tail");
        ck_eq_d(fx.tick_budget, 0.0, "R5: second-stage JBE boundary (zero side) at equality, same as R2");
    }

    // =================================================================================================
    // R6 -- SECOND-STAGE overflow gate SUBTRACT branch, clear margin: build_time (20.0) well below
    // cycle_progress (25.0) -- `cycle_progress > threshold` is unambiguously true, so
    // tick_budget = cycle_progress - threshold = 5.0 exactly (both exactly representable). Other
    // player, to isolate the arithmetic from the local-arm noise already covered above.
    // =================================================================================================
    {
        Seed s;
        s.build_time  = 20.0; // cycle_progress(25.0) - build_time(20.0) = 5.0
        s.player_side = (int16_t)(s.player + 5);
        seed_and_run(fx, s);

        ck(trace_eq({"completion_dispatch", "notify_ui"}), "R6: other-player branch, minimal trace");
        ck_eq_d(fx.tick_budget, 5.0,
                "R6 SECOND-STAGE JBE BOUNDARY (subtract side, clear margin): cycle_progress(25.0) > "
                "threshold(20.0) -> tick_budget = cycle_progress - threshold = 5.0 exactly "
                "(0x00473152-0x0047315b `FSUB`/`FSTP`)");
    }

    // =================================================================================================
    // R7 -- SECOND-STAGE overflow gate SUBTRACT branch, SMALLEST possible margin (build_time one ULP
    // BELOW cycle_progress): pins the JBE boundary from the "subtract" side at maximum precision, the
    // mirror of R2's "zero side at exact equality" pin -- per the header's NaN-SAFE RESTATEMENT note,
    // the `.cpp` writes this branch as `cycle_progress > threshold` (matching hardware's JBE-not-taken
    // exactly), NOT the naive `cycle_progress <= threshold ? zero : subtract` (whose C++ `<=` would
    // ALSO be false for a NaN operand and wrongly select subtract) -- this case cannot construct a
    // real NaN through the public API, so it pins the ORDERED-input behaviour precisely at the
    // boundary instead, one ULP on the side that must NOT be zero.
    // =================================================================================================
    {
        Seed s;
        s.build_time  = std::nextafter(25.0, -INFINITY); // one ULP below 25.0
        s.player_side = (int16_t)(s.player + 5);
        seed_and_run(fx, s);

        const double want_budget = 25.0 - s.build_time; // exactly one ULP of 25.0, tiny but nonzero
        ck(want_budget > 0.0,
           "R7 self-check: the fixture's own arithmetic produces a strictly positive gap");
        ck_eq_d(fx.tick_budget, want_budget,
                "R7 SECOND-STAGE JBE BOUNDARY (subtract side, one ULP): build_time one ULP below "
                "cycle_progress -- gate1 taken (0x00472fa8, threshold < cycle_progress) AND the "
                "overflow gate takes the subtract branch (0x00473120 JBE NOT taken) for the smallest "
                "possible positive margin, not the zero branch a naive `<=` restatement would pick at "
                "this boundary under NaN");
        ck_eq((uint32_t)fx.b(s.player, s.index).state, 0xabcdu,
              "R7: state transition still fires (gate1 taken just above the boundary)");
    }

    // =================================================================================================
    // O1 -- ORDER PROOF: the completion threshold is Projects[labs[...].active_project_id].build_time,
    // RE-READ FRESH at each of its three use sites (completion gate / message name / overflow gate),
    // NOT cached once into a local. rec_get_coords (which fires strictly BETWEEN the completion gate's
    // read and the message-name/overflow-gate reads) mutates labs[...].active_project_id from Project A
    // (build_time=25.0, name=55) to Project B (build_time=10.0, name=66) DURING the call. A correct
    // translation must therefore: (1) still have used Project A's build_time for the FIRST gate (it ran
    // BEFORE the mutation, so gate-taken depends on A's 25.0, not B's 10.0 -- if it had used B's 10.0
    // the gate would ALSO be taken, so this alone doesn't distinguish; the DISCRIMINATING checks are
    // (2) and (3)); (2) use Project B's NAME in the message (proves the message's active_project() call
    // re-reads after the mutation, not a value cached at function entry); (3) use Project B's
    // build_time in the overflow-gate arithmetic (proves the SAME re-read discipline there). A
    // translation that hoisted `const cfg_project &proj = active_project();` ONCE at the top would fail
    // both (2) and (3), landing on Project A's name (55) and build_time (25.0 -> tick_budget 0.0)
    // instead.
    // =================================================================================================
    {
        Seed s;
        s.build_time  = 25.0;     // Project A's threshold -- cycle_progress(25.0) <= 25.0 -> gate1 taken
        s.player_side = s.player; // local -- get_coords must fire to run the mutation
        s.sim_active  = 0;        // keep the trace minimal; irrelevant to the probe

        // Project B: a DIFFERENT row (13, vs A's 12) with a DIFFERENT name (66, vs A's 55) and a
        // build_time (10.0) that yields a DIFFERENT, DISTINGUISHABLE overflow-gate result (subtract
        // 15.0, not A's "zero at exact equality").
        s.probe_project_id         = 13;
        s.probe_project_text_id    = 66;
        s.probe_project_build_time = 10.0;
        seed_and_run(fx, s);

        ck(trace_eq({"get_coords", "sprintf", "print_text_message", "completion_dispatch", "notify_ui"}),
           "O1: local arm fires (sim_active=0 skips only snd_play)");
        ck(g_sprintf_calls.size() == 1 && g_sprintf_calls[0].a0 == k_name_text_b,
           "O1 ORDER PROOF (message): sprintf's name-arg is Project B's name (66), NOT Project A's (55) "
           "-- active_project() is RE-EVALUATED for the message AFTER get_coords mutated "
           "labs[...].active_project_id, not cached from the completion-gate check above it");
        ck_eq_d(fx.tick_budget, 25.0 - 10.0,
                "O1 ORDER PROOF (overflow gate): tick_budget = cycle_progress(25.0) - Project B's "
                "build_time(10.0) = 15.0, NOT Project A's build_time(25.0) (which would give 0.0) -- "
                "the overflow-gate's active_project() call also re-reads after the mutation, the SAME "
                "re-read discipline as the message-name check above");
        ck_eq((uint32_t)fx.b(s.player, s.index).state, 0xabcdu,
              "O1: state transition still fires normally (gate1 used Project A's threshold, which had "
              "already been read before the mutation -- this checks the gate wasn't otherwise disturbed)");
    }
}

} // namespace mh::sim::test
