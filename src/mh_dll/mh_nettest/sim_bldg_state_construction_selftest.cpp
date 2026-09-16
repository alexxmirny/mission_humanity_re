//
// sim_bldg_state_construction_selftest.cpp -- `simtest` cases for llm_strat_bldg_state_construction
// (sim/sim_bldg_state_charge.h/.cpp @0x004724af), SIM1B building_tick machinery -- the "construction
// in progress" per-tick handler for a building whose state dispatches here.
//
// SCOPE: this file pins the full call/write order, the completion gate's exact `<=` boundary, the
// cycle_progress accumulation arithmetic, the LOCAL-vs-OTHER-PLAYER split (voice line + camera pan +
// message vs. none of those), the SIM_ACTIVE gate on the voice line only, the race-dependent voice-line
// sound id, and -- the crux of this function -- THE REGISTER-REUSE HAZARD: on the LOCAL-PLAYER branch,
// llm_strat_bldg_construction_complete's param_3/param_4 are overwritten with the ADDRESSES of
// _G_LLM_STRAT_CAM_PAN_TARGET_COL/_ROW before the call (0x00472551/0x00472556, never reloaded), while on
// the OTHER-PLAYER branch (which skips that whole block via the early JNZ @0x00472514) they carry THIS
// function's own incoming param_3/param_4 verbatim. It does NOT reproduce w_sprintf's actual text
// formatting content beyond which text_ptrs entries feed it (the formatting path itself is presentation).
//
// EXPECTED BEHAVIOUR HAND-DERIVED FROM THE DISASSEMBLY
// (tmp/decomp_sim/llm_strat_bldg_state_construction_004724af.asm), cross-checked against the .cpp/.h's
// own per-line address citations -- NOT read off the .cpp body alone:
//
//   cycle_progress += tick_budget * efficiency (0x004724c7-0x004724de, unconditional, runs even when the
//   completion gate below is not taken).
//   Completion gate (0x004724e1-0x00472501): `if (build_time_2 <= cycle_progress)` (JC @0x00472501 skips
//   the whole block straight to the tail when build_time_2 > cycle_progress).
//   Inside the gate, LOCAL PLAYER ONLY (cur_player == PlayerSide, 0x00472507-0x00472514):
//     SIM_ACTIVE-gated voice line (0x0047251a-0x0047254c): snd_play(id=(race==2?0x12:0)+4, volume=100).
//     THEN (unconditional in the local-player arm) param_3/param_4 REASSIGNED to the ADDRESSES of
//     CAM_PAN_TARGET_COL/_ROW (0x00472551/0x00472556) -- the register-reuse hazard.
//     THEN bldg_get_coords writes fine coords DIRECTLY into CAM_PAN_TARGET_COL/_ROW (0x0047255b-0x00472569),
//     truncated fine->tile in place (0x0047256e-0x0047259f).
//     THEN a "%s: %s" message (name=Building[bid].id, reason=text_ptrs[0x74]) via w_sprintf__vss +
//     game_ui_PrintTextMessage (0x004725a4-0x004725e1).
//   UNCONDITIONALLY inside the gate (0x004725e6-0x0047263e, both local and other player):
//     bldg_construction_complete(cur_player, cur_index, param_3, param_4) -- param_3/param_4 per the
//     branch taken above.
//     ai_notify_bldg_constructed(cur_player, b.x, cur_index, b.building_id, b.y, 1).
//     refresh_building(cur_player, cur_index).
//   TAIL (0x00472643-0x0047266a, ALWAYS runs, gate taken or not): tick_budget = 0.0;
//   bldg_notify_ui(cur_player, cur_index).
//
#include "sim/sim_bldg_state_charge.h"

#include <cstdint>
#include <cstring>
#include <vector>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// ---- shared trace: one sequence proves CALL ORDER across all 8 callees -------------------------
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

// ---- per-callee recorders (8, one per bldg_state_construction_calls member) --------------------
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
};
std::vector<CompleteCall> g_complete_calls;
void                      rec_construction_complete(uint32_t player, uint32_t index, uint32_t param_3, uint32_t param_4) {
    tr("construction_complete");
    g_complete_calls.push_back({player, index, param_3, param_4});
}

struct NotifyConstructedCall {
    uint32_t player, x_b, param_3, building_id, y_b, param_6;
};
std::vector<NotifyConstructedCall> g_notify_constructed_calls;
void                               rec_ai_notify_bldg_constructed(uint32_t player, uint32_t x_b, uint32_t param_3, uint32_t building_id,
                                                                  uint32_t y_b, uint32_t param_6) {
    tr("ai_notify_bldg_constructed");
    g_notify_constructed_calls.push_back({player, x_b, param_3, building_id, y_b, param_6});
}

struct RefreshCall {
    uint16_t player;
    int32_t  index;
};
std::vector<RefreshCall> g_refresh_calls;
void                     rec_refresh_building(uint16_t player, int32_t index) {
    tr("refresh_building");
    g_refresh_calls.push_back({player, index});
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

const bldg_state_construction_calls g_calls = {
    &rec_get_coords,
    &rec_snd_play,
    &rec_sprintf,
    &rec_print_text_message,
    &rec_construction_complete,
    &rec_ai_notify_bldg_constructed,
    &rec_refresh_building,
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
    g_notify_constructed_calls.clear();
    g_refresh_calls.clear();
    g_notify_ui_calls.clear();
}

// ---- text pointers used to pin WHICH text_ptrs slots feed the "%s: %s" message -----------------
// Distinct addresses (not dereferenced by the recorder) so a translation that read the wrong index
// (or swapped name/reason) is caught by pointer identity, without depending on w_sprintf's actual
// formatting (presentation, out of scope per the header banner above).
const wchar_t k_name_text[]   = L"NAME_TXT";
const wchar_t k_reason_text[] = L"REASON_TXT";

// ---- fixture seeding -----------------------------------------------------------------------------
struct Seed {
    uint16_t player  = 0;
    int32_t  index   = 1;
    uint16_t cfg_row = 7;      // building_id -> cfg_buildings index
    int32_t  text_id = 40;     // cfg_building.id, the "name" half of the message (distinct from 0x74)
    uint8_t  bx = 21, by = 33; // building's OWN tile position, distinct from the fine/tile coords below

    double efficiency        = 2.0;
    double cycle_progress_in = 5.0;
    double tick_budget       = 10.0; // delta = tick_budget*efficiency = 20.0 -> cycle_progress = 25.0
    double build_time_2      = 25.0; // gate threshold; case overrides to probe both sides of <=

    int16_t player_side = 0; // == player -> "local"
    int32_t player_race = 0;
    int32_t sim_active  = 0;

    int32_t fine_x = 1600; // -> tile 50 (1600 / 32, exact)
    int32_t fine_y = 1760; // -> tile 55 (1760 / 32, exact)

    int32_t cam_pan_col_seed = -777;
    int32_t cam_pan_row_seed = -888;

    // Sentinel INCOMING param_3/param_4 -- deliberately far from any plausible CAM_PAN_TARGET address
    // so a bug that forwards them verbatim on the local-player branch (instead of overwriting them with
    // the globals' addresses) is caught, and so a bug that overwrites them on the OTHER-player branch
    // (instead of leaving them verbatim) is caught too.
    uint32_t param_3_in = 0xDEADBEEFu;
    uint32_t param_4_in = 0xCAFEBABEu;
};

void seed_and_run(sim_fixture &fx, const Seed &s) {
    fx.reset();

    building &b      = fx.b(s.player, s.index);
    b.building_id    = s.cfg_row;
    b.cycle_progress = s.cycle_progress_in;
    b.efficiency     = s.efficiency;
    b.x              = s.bx;
    b.y              = s.by;

    fx.cur_building_ptr = &b;
    fx.view_cur_player  = s.player;
    fx.view_cur_index   = (uint16_t)s.index;

    cfg_building &cb = fx.cfg_buildings[s.cfg_row];
    cb.id            = s.text_id;
    cb.build_time_2  = s.build_time_2;

    fx.player_side = s.player_side;
    fx.player_race = s.player_race;
    fx.sim_active  = s.sim_active;
    fx.tick_budget = s.tick_budget;

    fx.cam_pan_target_col = s.cam_pan_col_seed;
    fx.cam_pan_target_row = s.cam_pan_row_seed;

    fx.text_ptrs[(size_t)s.text_id] = k_name_text;   // "name" half -- Building[bid].id
    fx.text_ptrs[0x74]              = k_reason_text; // "reason" half -- literal TEXT_ID_CONSTRUCTION_COMPLETE

    g_get_coords_fine_x = s.fine_x;
    g_get_coords_fine_y = s.fine_y;

    reset_observations();

    sim_store own = fx.store();
    // param_1(EAX)/param_2(EDX) are dead in the original (never read) -- distinct sentinel values so a
    // translation that accidentally started reading them would show up as a wrong branch, not a crash.
    detail::bldg_state_construction(fx.view(), own, g_calls, 0x11111111u, 0x22222222u, s.param_3_in,
                                    s.param_4_in);
}

} // namespace

void run_bldg_state_construction_tests() {
    sim_fixture fx;

    // =================================================================================================
    // C1 -- completion gate NOT taken (build_time_2 > cycle_progress after accumulation, 0x00472501 JC
    // taken): ONLY the unconditional accumulation + tail run -- tick_budget zeroes, notify_ui fires,
    // and NOTHING ELSE (no coords/voice/message/completion chain at all).
    // =================================================================================================
    {
        Seed s;
        s.build_time_2 = 26.0; // 25.0 (accumulated) < 26.0 -> gate NOT taken
        seed_and_run(fx, s);

        ck(trace_eq({"notify_ui"}), "C1: gate not taken -- only the tail's notify_ui fires (0x00472657-0x00472665)");
        ck_eq_d(fx.b(s.player, s.index).cycle_progress, 25.0,
                "C1: cycle_progress = tick_budget*efficiency + cycle_progress_in = 10.0*2.0+5.0 = 25.0 "
                "(0x004724c7-0x004724de, accumulates even though the gate below is not taken)");
        ck_eq_d(fx.tick_budget, 0.0, "C1: tick_budget zeroed unconditionally (0x00472643/0x0047264d)");
        ck(g_notify_ui_calls.size() == 1 && g_notify_ui_calls[0].player == s.player &&
               g_notify_ui_calls[0].index == (uint32_t)s.index,
           "C1: bldg_notify_ui(cur_player, cur_index) (0x00472657-0x00472665)");
        ck(g_coords.empty() && g_snd_play.empty() && g_sprintf_calls.empty() &&
               g_print_text_message_count == 0 && g_complete_calls.empty() &&
               g_notify_constructed_calls.empty() && g_refresh_calls.empty(),
           "C1: none of the completion-gate callees fire when the gate is skipped");
        ck_eq((uint32_t)fx.cam_pan_target_col, (uint32_t)s.cam_pan_col_seed,
              "C1: cam_pan_target_col UNCHANGED -- only the local-player arm inside the gate writes it");
        ck_eq((uint32_t)fx.cam_pan_target_row, (uint32_t)s.cam_pan_row_seed, "C1: cam_pan_target_row UNCHANGED");
    }

    // =================================================================================================
    // C2 -- completion gate TAKEN at the exact `<=` boundary (build_time_2 == cycle_progress, pins the
    // gate as `<=` not `<`), LOCAL player, SIM_ACTIVE=1, race==2: the full local-player arm fires, in
    // order, and construction_complete's param_3/param_4 are THE ADDRESSES of cam_pan_target_col/_row --
    // NOT the sentinel param_3_in/param_4_in this function was called with. THE register-reuse hazard.
    // =================================================================================================
    {
        Seed s;
        s.build_time_2 = 25.0;     // == cycle_progress after accumulation (25.0) -- boundary: gate IS taken
        s.player_side  = s.player; // 0 == 0 -> local
        s.sim_active   = 1;
        s.player_race  = 2;
        seed_and_run(fx, s);

        ck(trace_eq({"snd_play", "get_coords", "sprintf", "print_text_message", "construction_complete",
                     "ai_notify_bldg_constructed", "refresh_building", "notify_ui"}),
           "C2: exact call order for the local/sim-active/race2 completion arm "
           "(0x0047251a voice -> 0x00472551 coords -> 0x004725a4 message -> 0x004725e6 completion chain "
           "-> 0x00472643 tail)");

        ck_eq_d(fx.b(s.player, s.index).cycle_progress, 25.0,
                "C2: cycle_progress = 10.0*2.0+5.0 = 25.0, exactly at build_time_2 -- gate taken (0x004724e1: "
                "`build_time_2 <= cycle_progress`, JC @0x00472501 NOT taken at equality)");

        ck(g_snd_play.size() == 1 && g_snd_play[0].id == 0x16 && g_snd_play[0].volume == 100,
           "C2: snd_play(id=0x12+4=0x16 [race==2], volume=100) (0x0047252a-0x0047254c)");

        ck(g_coords.size() == 1 && g_coords[0].player == s.player && g_coords[0].index == s.index,
           "C2: bldg_get_coords(cur_player, cur_index, &cam_pan_target_col, &cam_pan_target_row) (0x00472551-0x00472569)");
        ck_eq((uint32_t)fx.cam_pan_target_col, 50u,
              "C2: cam_pan_target_col = tile(fine_x) = 1600/32 = 50 (0x0047256e-0x00472584)");
        ck_eq((uint32_t)fx.cam_pan_target_row, 55u,
              "C2: cam_pan_target_row = tile(fine_y) = 1760/32 = 55 (0x00472589-0x0047259f)");

        ck(g_sprintf_calls.size() == 1, "C2: w_sprintf__vss fires once (0x004725a4-0x004725d9)");
        if (g_sprintf_calls.size() == 1) {
            const auto &sp = g_sprintf_calls[0];
            ck(sp.a0 == k_name_text, "C2: sprintf name-arg = text_ptrs[Building[bid].id] (0x005845dc read)");
            ck(sp.a1 == k_reason_text,
               "C2: sprintf reason-arg = text_ptrs[0x74] (TEXT_ID_CONSTRUCTION_COMPLETE, literal push @0x004725a4)");
            ck(sp.dst == fx.text_scratch.data(), "C2: sprintf writes into own.text_scratch() (0xe15178)");
        }
        ck(g_print_text_message_count == 1 &&
               (g_print_text_message_ptrs.empty() ||
                g_print_text_message_ptrs[0] == (void *)fx.text_scratch.data()),
           "C2: game_ui_PrintTextMessage(own.text_scratch()) fires once, same buffer as sprintf's dst (0x004725dc-0x004725e1)");

        ck(g_complete_calls.size() == 1, "C2: bldg_construction_complete fires once (0x004725f4)");
        if (g_complete_calls.size() == 1) {
            const auto    &cc            = g_complete_calls[0];
            const uint32_t want_col_addr = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&fx.cam_pan_target_col));
            const uint32_t want_row_addr = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&fx.cam_pan_target_row));
            ck(cc.player == s.player && cc.index == (uint32_t)s.index,
               "C2: construction_complete(cur_player, cur_index, ...)");
            ck_eq(cc.param_3, want_col_addr,
                  "C2 REGISTER-REUSE HAZARD: construction_complete's param_3 = &cam_pan_target_col (0x00472551 "
                  "`MOV EBX,0xe587e1`, never reloaded before 0x004725f4) -- NOT the sentinel param_3_in");
            ck_eq(cc.param_4, want_row_addr,
                  "C2 REGISTER-REUSE HAZARD: construction_complete's param_4 = &cam_pan_target_row (0x00472556 "
                  "`MOV ECX,0xe587e5`, never reloaded before 0x004725f4) -- NOT the sentinel param_4_in");
            ck(cc.param_3 != s.param_3_in && cc.param_4 != s.param_4_in,
               "C2: param_3/param_4 are NOT the caller's own incoming sentinels on the local-player branch");
        }

        ck(g_notify_constructed_calls.size() == 1, "C2: ai_notify_bldg_constructed fires once (0x0047262b)");
        if (g_notify_constructed_calls.size() == 1) {
            const auto &nc = g_notify_constructed_calls[0];
            ck(nc.player == s.player && nc.x_b == s.bx && nc.param_3 == (uint32_t)s.index &&
                   nc.building_id == s.cfg_row && nc.y_b == s.by && nc.param_6 == 1u,
               "C2: ai_notify_bldg_constructed(cur_player, b.x, cur_index, b.building_id, b.y, 1) "
               "-- exact arg order (0x004725f9-0x0047262b) and the literal 6th-arg constant 1");
        }

        ck(g_refresh_calls.size() == 1 && g_refresh_calls[0].player == s.player &&
               g_refresh_calls[0].index == s.index,
           "C2: refresh_building(cur_player, cur_index) (0x0047263e)");

        ck(g_notify_ui_calls.size() == 1 && g_notify_ui_calls[0].player == s.player &&
               g_notify_ui_calls[0].index == (uint32_t)s.index,
           "C2: bldg_notify_ui(cur_player, cur_index) fires at the shared tail too (0x00472665)");
        ck_eq_d(fx.tick_budget, 0.0, "C2: tick_budget zeroed at the tail even though the gate was taken");
    }

    // =================================================================================================
    // C3 -- same as C2 but race != 2: voice-line sound id is the BASE id (4), no 0x12 offset.
    // =================================================================================================
    {
        Seed s;
        s.build_time_2 = 25.0;
        s.player_side  = s.player;
        s.sim_active   = 1;
        s.player_race  = 0;
        seed_and_run(fx, s);

        ck(g_snd_play.size() == 1 && g_snd_play[0].id == 4 && g_snd_play[0].volume == 100,
           "C3: race!=2 -> snd_play id = base(4) + 0, no 0x12 offset (0x0047252a JNZ taken -> LAB_0047253c)");
    }

    // =================================================================================================
    // C4 -- gate taken, LOCAL player, SIM_ACTIVE=0: the voice line is skipped (SIM_ACTIVE gate,
    // 0x0047251a JZ), but get_coords/message/completion chain still fire -- only snd_play is gated.
    // =================================================================================================
    {
        Seed s;
        s.build_time_2 = 25.0;
        s.player_side  = s.player;
        s.sim_active   = 0;
        s.player_race  = 2; // irrelevant here since the whole voice-line block is skipped
        seed_and_run(fx, s);

        ck(trace_eq({"get_coords", "sprintf", "print_text_message", "construction_complete",
                     "ai_notify_bldg_constructed", "refresh_building", "notify_ui"}),
           "C4: SIM_ACTIVE=0 -- snd_play does NOT fire, but coords/message/completion chain still do "
           "(0x0047251a JZ @0x00472521 skips only the voice-line block, not the rest of the local arm)");
        ck(g_snd_play.empty(), "C4: no voice line at all when SIM_ACTIVE=0");
        ck(g_coords.size() == 1, "C4: bldg_get_coords still fires (SIM_ACTIVE does not gate it)");
        ck_eq((uint32_t)fx.cam_pan_target_col, 50u, "C4: cam_pan_target still written when SIM_ACTIVE=0");
    }

    // =================================================================================================
    // C5 -- gate taken, OTHER player (cur_player != PlayerSide, 0x00472514 JNZ taken -> skips straight
    // to LAB_004725e6): NONE of voice/coords/message fire (cam_pan_target UNCHANGED), but
    // construction_complete/ai_notify_bldg_constructed/refresh_building STILL fire, and
    // construction_complete receives THIS function's own incoming param_3/param_4 VERBATIM -- the exact
    // inverse of C2, pinning the other half of the register-reuse hazard.
    // =================================================================================================
    {
        Seed s;
        s.build_time_2 = 25.0;
        s.player_side  = (int16_t)(s.player + 5); // != player -> "other"
        s.sim_active   = 1;                       // would matter if the voice line fired -- it must not
        s.player_race  = 2;
        seed_and_run(fx, s);

        ck(trace_eq({"construction_complete", "ai_notify_bldg_constructed", "refresh_building", "notify_ui"}),
           "C5: other-player branch -- voice/coords/message never fire; completion chain + tail still do "
           "(0x00472514 JNZ -> LAB_004725e6 skips 0x0047251a-0x004725e1 entirely)");
        ck(g_snd_play.empty() && g_coords.empty() && g_sprintf_calls.empty() && g_print_text_message_count == 0,
           "C5: none of the local-only calls fire on the other-player branch");
        ck_eq((uint32_t)fx.cam_pan_target_col, (uint32_t)s.cam_pan_col_seed,
              "C5: cam_pan_target_col UNCHANGED -- only the local-player arm writes it");
        ck_eq((uint32_t)fx.cam_pan_target_row, (uint32_t)s.cam_pan_row_seed, "C5: cam_pan_target_row UNCHANGED");

        ck(g_complete_calls.size() == 1, "C5: bldg_construction_complete still fires (unconditional in the gate)");
        if (g_complete_calls.size() == 1) {
            const auto &cc = g_complete_calls[0];
            ck(cc.player == s.player && cc.index == (uint32_t)s.index,
               "C5: construction_complete(cur_player, cur_index, ...)");
            ck_eq(cc.param_3, s.param_3_in,
                  "C5 REGISTER-REUSE HAZARD (inverse of C2): construction_complete's param_3 = THIS "
                  "function's own incoming param_3 VERBATIM -- EBX is untouched from entry on the "
                  "other-player branch (0x00472514 skips the 0x00472551 `MOV EBX,...` reassignment)");
            ck_eq(cc.param_4, s.param_4_in,
                  "C5 REGISTER-REUSE HAZARD (inverse of C2): construction_complete's param_4 = THIS "
                  "function's own incoming param_4 VERBATIM (ECX untouched, 0x00472556 skipped)");
        }

        ck(g_notify_constructed_calls.size() == 1, "C5: ai_notify_bldg_constructed still fires");
        if (g_notify_constructed_calls.size() == 1) {
            const auto &nc = g_notify_constructed_calls[0];
            ck(nc.player == s.player && nc.x_b == s.bx && nc.param_3 == (uint32_t)s.index &&
                   nc.building_id == s.cfg_row && nc.y_b == s.by && nc.param_6 == 1u,
               "C5: ai_notify_bldg_constructed(cur_player, b.x, cur_index, b.building_id, b.y, 1) "
               "same arg order/constant as C2, unaffected by the local/other split");
        }
        ck(g_refresh_calls.size() == 1, "C5: refresh_building still fires");
        ck(g_notify_ui_calls.size() == 1, "C5: bldg_notify_ui still fires at the tail");
    }
}

} // namespace mh::sim::test
