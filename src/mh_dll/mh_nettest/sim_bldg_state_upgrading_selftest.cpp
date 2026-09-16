//
// sim_bldg_state_upgrading_selftest.cpp -- `simtest` cases for llm_strat_bldg_state_upgrading
// (sim/sim_bldg_state_upgrade_research.h/.cpp @0x00472c42), SIM1-G4 building_tick machinery -- the
// "upgrade in progress" per-tick handler for a building whose state dispatches here.
//
// THIS FUNCTION IS DO-NOT-ARM (its write closure reaches game_SetEvent's ~780-function UI/gfx/snd/
// menu-teardown over-approximation) -- there is no rig run backing this up. This file is its ONLY
// evidence.
//
// SCOPE: this file pins the cycle_progress accumulation, the completion gate's DOUBLE-INDIRECTED
// threshold (Building[Building[bid].upgrade_index].build_time_2 -- NOT this building's own
// build_time_2), the UNCONDITIONAL (pre-local/other-split) ai_queue_release_order(mode=2) call, the
// LOCAL-vs-OTHER-PLAYER split (voice line + camera pan + message vs. none of those), the SIM_ACTIVE
// gate on the voice line only, the race-dependent voice-line sound id (base 0xf, DIFFERENT from
// construction's 4), THE REGISTER-REUSE HAZARD (completion_dispatch's param_3/param_4 are the
// ADDRESSES of CAM_PAN_TARGET_COL/_ROW on the local-player branch, and the MODELED-INERT constant 0
// on the other-player branch -- this is void(void), so unlike construction there is no real caller
// param to forward verbatim), the state_transition_ids[1] LOW-16-BIT mask, and the production-in-
// progress override (roster-derived building type H_PRODUCTION/A_PRODUCTION + nonzero
// productions[player][sub_id].queued_count[0] forces state = PROD_PICK_NEXT, keyed by `sub_id` NOT by
// `cur_index`). Does not reproduce w_sprintf's actual text formatting content beyond which text_ptrs
// entries feed it (presentation, out of scope).
//
// EXPECTED BEHAVIOUR HAND-DERIVED FROM THE DISASSEMBLY
// (tmp/decomp_sim/llm_strat_bldg_state_upgrading_00472c42.asm), NOT read off the .c draft (project
// policy: the .c has silently lied before):
//
//   cycle_progress += tick_budget * efficiency (0x00472c5a-0x00472c71, unconditional, runs even when
//   the completion gate below is not taken).
//   Completion gate (0x00472c74-0x00472c9e): `if (Building[Building[bid].upgrade_index].build_time_2
//   <= cycle_progress)` (JC @0x00472c9e skips the whole block straight to the tail). This is a JC gate
//   (not JBE) -- per the header's NaN-SAFE RESTATEMENT note, the naive `<=` is ALREADY NaN-safe here
//   (JC-taken/skip already corresponds to hardware's "!(threshold<=cycle_progress)", and C++'s `<=` is
//   also false for NaN) -- no restatement needed. A real NaN cannot be constructed through this
//   function's public C++ API (no path takes an outside double straight into the compare), so instead
//   U1/U2 below pin the ORDERED-input boundary as tightly as representable (nextafter(25.0, +inf) on
//   the not-taken side, exactly 25.0 on the taken side).
//   Inside the gate: FIRST, UNCONDITIONALLY (0x00472ca4-0x00472cb7, BEFORE the local/other split, unlike
//   charge_gate's equivalent call which is part of the shared tail AFTER the message):
//     ai_queue_release_order(cur_player, cur_index, mode=2).
//   LOCAL PLAYER ONLY (cur_player == PlayerSide, 0x00472cbc-0x00472cc9):
//     SIM_ACTIVE-gated voice line (0x00472ccf-0x00472d01): snd_play(id=(race==2?0x12:0)+0xf, volume=100).
//     THEN (unconditional in the local-player arm) param_3/param_4 REASSIGNED to the ADDRESSES of
//     CAM_PAN_TARGET_COL/_ROW (0x00472d06/0x00472d0b) -- the register-reuse hazard. Since this function
//     is void(void) (no caller-supplied EBX/ECX), the OTHER-player branch's param_3/param_4 are whatever
//     garbage the CALLER left there -- proven inert (completion_dispatch never reads them) and MODELED
//     as the constant 0 in the .cpp, NOT as a real captured sentinel (there is nothing to capture).
//     THEN bldg_get_coords writes fine coords DIRECTLY into CAM_PAN_TARGET_COL/_ROW (0x00472d10-
//     0x00472d1e), truncated fine->tile in place (0x00472d23-0x00472d54, truncating toward zero).
//     THEN a "%s: %s" message (name=Building[bid].id, reason=text_ptrs[0x79]) via w_sprintf__vss +
//     game_ui_PrintTextMessage (0x00472d59-0x00472d96).
//   UNCONDITIONALLY inside the gate (0x00472d9b-0x00472e73, both local and other player):
//     completion_dispatch(cur_player, cur_index, param_3, param_4, game_clock) -- param_3/param_4 per
//     the branch taken above; game_clock (a DOUBLE, param_5) is ALWAYS forwarded, unlike construction's
//     equivalent call which has no 5th argument.
//     cur_building->state = Building[bid].state_transition_ids[1] LOW 16 BITS ONLY (0x00472dba-
//     0x00472dd6).
//     IF Building[roster_bid].type (roster-derived, but == bid's own type since cur_building always
//     aliases the roster slot -- see the header's "two building_id expressions" note) is
//     H_PRODUCTION(0x15) or A_PRODUCTION(0x01) AND productions[cur_player][cur_building->sub_id].
//     queued_count[0] != 0 (0x00472dda-0x00472e6d, KEYED BY sub_id, NOT cur_index): override
//     cur_building->state = PROD_PICK_NEXT (0x6c).
//     refresh_building(cur_player, cur_index) (0x00472e81) -- ONLY reached when the completion gate
//     fired (no separate "skip refresh_building" arm, unlike charge_gate/construction).
//   TAIL (0x00472e86-0x00472ea8, ALWAYS runs, gate taken or not): tick_budget = 0.0 (both dwords);
//   bldg_notify_ui(cur_player, cur_index).
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

// ---- shared trace: one sequence proves CALL ORDER across all 8 callees -------------------------
std::vector<const char *> g_trace;
void                      tr(const char *tag) { g_trace.push_back(tag); }

bool trace_eq(const std::vector<const char *> &want) {
    if (g_trace.size() != want.size()) return false;
    for (size_t i = 0; i < want.size(); ++i)
        if (std::strcmp(g_trace[i], want[i]) != 0) return false;
    return true;
}

// BLDG_STATE_PROD_PICK_NEXT: the .cpp's own copy is file-local (anonymous namespace, not exported via
// the header) -- redeclared here per this project's established per-TU convention (the .cpp's own
// comment lists FIVE other sim/ TUs already doing the same for this exact constant).
inline constexpr uint16_t BLDG_STATE_PROD_PICK_NEXT = 0x6c;

// ---- return-value knobs (settable per case BEFORE seed_and_run) --------------------------------
int32_t g_get_coords_fine_x = 0;
int32_t g_get_coords_fine_y = 0;

// ---- per-callee recorders (8, one per bldg_state_upgrading_calls member) -----------------------
struct ReleaseOrderCall {
    int32_t player, index, mode;
};
std::vector<ReleaseOrderCall> g_release_order_calls;
void                          rec_ai_queue_release_order(int32_t player, int32_t index, int32_t mode) {
    tr("ai_queue_release_order");
    g_release_order_calls.push_back({player, index, mode});
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

struct CompleteCall {
    uint32_t player, index, param_3, param_4;
    double   param_5;
};
std::vector<CompleteCall> g_complete_calls;
void                      rec_completion_dispatch(uint32_t player, uint32_t index, uint32_t param_3, uint32_t param_4,
                                                  double param_5) {
    tr("completion_dispatch");
    g_complete_calls.push_back({player, index, param_3, param_4, param_5});
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

const bldg_state_upgrading_calls g_calls = {
    &rec_ai_queue_release_order,
    &rec_snd_play,
    &rec_get_coords,
    &rec_sprintf,
    &rec_print_text_message,
    &rec_completion_dispatch,
    &rec_refresh_building,
    &rec_bldg_notify_ui,
};

void reset_observations() {
    g_trace.clear();
    g_release_order_calls.clear();
    g_snd_play.clear();
    g_coords.clear();
    g_sprintf_calls.clear();
    g_print_text_message_count = 0;
    g_print_text_message_ptrs.clear();
    g_complete_calls.clear();
    g_refresh_calls.clear();
    g_notify_ui_calls.clear();
}

// ---- text pointers used to pin WHICH text_ptrs slots feed the "%s: %s" message -----------------
// Distinct addresses (not dereferenced by the recorder) so a translation that read the wrong index
// (or swapped name/reason) is caught by pointer identity, without depending on w_sprintf's actual
// formatting.
const wchar_t k_name_text[]   = L"NAME_TXT";
const wchar_t k_reason_text[] = L"REASON_TXT";

// ---- fixture seeding -----------------------------------------------------------------------------
struct Seed {
    uint16_t player      = 0;
    int32_t  index       = 1;
    uint16_t cfg_row     = 7; // building_id -> cfg_buildings index (the "bid" expression)
    int32_t  upgrade_row = 3; // cfg_buildings[cfg_row].upgrade_index -- DISTINCT from cfg_row, so a
                              // translation that dropped the double indirection reads the wrong row.
    int32_t text_id = 40;     // cfg_building.id, the "name" half of the message
    uint8_t sub_id  = 5;      // DISTINCT from `index` -- keys the production lookup, not cur_index

    double efficiency        = 2.0;
    double cycle_progress_in = 5.0;
    double tick_budget       = 10.0; // delta = tick_budget*efficiency = 20.0 -> cycle_progress = 25.0

    // The completion gate reads Building[Building[bid].upgrade_index].build_time_2 -- NOT
    // Building[bid].build_time_2 directly. `build_time_2_decoy` sits on the BID's own row and is
    // always set to the value that would make the gate come out WRONG if a translation used single
    // indirection; `build_time_2_target` sits on the upgrade_row and is the real threshold.
    double build_time_2_decoy  = 0.0;
    double build_time_2_target = 25.0;

    int16_t player_side = 0; // == player -> "local"
    int32_t player_race = 0;
    int32_t sim_active  = 0;

    int32_t fine_x = 1600; // -> tile 50 (1600 / 32, exact)
    int32_t fine_y = 1760; // -> tile 55 (1760 / 32, exact)

    int32_t cam_pan_col_seed = -777;
    int32_t cam_pan_row_seed = -888;

    // state_transition_ids[1]'s FULL 32 BITS -- only the LOW 16 must survive into b.state. High bits
    // set so a translation that forgot the `& 0xffff` mask is caught.
    int32_t state_transition_id1 = static_cast<int32_t>(0xABCD1234u); // low16 = 0x1234

    uint8_t bldg_type             = 0; // cfg_buildings[cfg_row].type -- the production-override gate
    int32_t prod_queued_at_sub_id = 0; // productions[player][sub_id].queued_count[0]
    int32_t prod_queued_at_index  = 0; // productions[player][index].queued_count[0] -- the WRONG key,
                                       // for U10's "keyed by sub_id, not cur_index" pin

    double game_clock = 987.5; // distinct, nonzero -- pins param_5 forwarding
};

void seed_and_run(sim_fixture &fx, const Seed &s) {
    fx.reset();

    building &b      = fx.b(s.player, s.index);
    b.building_id    = s.cfg_row;
    b.cycle_progress = s.cycle_progress_in;
    b.efficiency     = s.efficiency;
    b.sub_id         = s.sub_id;

    fx.cur_building_ptr = &b;
    fx.view_cur_player  = s.player;
    fx.view_cur_index   = (uint16_t)s.index;

    cfg_building &cb           = fx.cfg_buildings[s.cfg_row];
    cb.id                      = s.text_id;
    cb.upgrade_index           = s.upgrade_row;
    cb.build_time_2            = s.build_time_2_decoy; // NOT the real gate -- see Seed's comment
    cb.type                    = s.bldg_type;
    cb.state_transition_ids[1] = s.state_transition_id1;

    cfg_building &cbt = fx.cfg_buildings[s.upgrade_row];
    cbt.build_time_2  = s.build_time_2_target; // the REAL gate threshold, via double indirection

    fx.player_side = s.player_side;
    fx.player_race = s.player_race;
    fx.sim_active  = s.sim_active;
    fx.tick_budget = s.tick_budget;
    fx.game_clock  = s.game_clock;

    fx.cam_pan_target_col = s.cam_pan_col_seed;
    fx.cam_pan_target_row = s.cam_pan_row_seed;

    fx.text_ptrs[(size_t)s.text_id] = k_name_text;   // "name" half -- Building[bid].id
    fx.text_ptrs[0x79]              = k_reason_text; // "reason" half -- literal TEXT_ID_UPGRADE_COMPLETE

    fx.productions[(size_t)s.player * PRODUCTIONS_PER_PLAYER + s.sub_id].queued_count[0] =
        s.prod_queued_at_sub_id;
    fx.productions[(size_t)s.player * PRODUCTIONS_PER_PLAYER + (size_t)s.index].queued_count[0] =
        s.prod_queued_at_index;

    g_get_coords_fine_x = s.fine_x;
    g_get_coords_fine_y = s.fine_y;

    reset_observations();

    sim_store own = fx.store();
    detail::bldg_state_upgrading(fx.view(), own, g_calls);
}

} // namespace

void run_bldg_state_upgrading_tests() {
    sim_fixture fx;

    // =================================================================================================
    // U1 -- completion gate NOT taken, pinned at the tightest representable boundary from the untaken
    // side (build_time_2_target = nextafter(25.0, +inf), one ULP above the accumulated cycle_progress):
    // ONLY the unconditional accumulation + tail run. ai_queue_release_order does NOT fire here (unlike
    // charge_gate/construction, where release_order sits OUTSIDE this function's gate; here it is
    // INSIDE, so a not-taken gate must suppress it too). The decoy on cfg_buildings[cfg_row].build_time_2
    // is 0.0 -- if a translation read that row directly instead of double-indirecting through
    // upgrade_index, it would (wrongly) see 0.0 <= 25.0 and take the gate; this case fails if that bug
    // is present.
    // =================================================================================================
    {
        Seed s;
        s.build_time_2_target = std::nextafter(25.0, 100.0); // one ULP above 25.0 -> gate NOT taken
        s.build_time_2_decoy  = 0.0;                         // wrong-indirection decoy: looks "taken"
        seed_and_run(fx, s);

        ck(trace_eq({"notify_ui"}),
           "U1: gate not taken -- only the tail's notify_ui fires (0x00472e86-0x00472ea8); "
           "ai_queue_release_order (0x00472cb7) does NOT fire because it is INSIDE the gate here");
        ck_eq_d(fx.b(s.player, s.index).cycle_progress, 25.0,
                "U1: cycle_progress = tick_budget*efficiency + cycle_progress_in = 10.0*2.0+5.0 = 25.0 "
                "(0x00472c5a-0x00472c71, accumulates even though the gate below is not taken)");
        ck_eq_d(fx.tick_budget, 0.0, "U1: tick_budget zeroed unconditionally (0x00472e86/0x00472e90)");
        ck(g_notify_ui_calls.size() == 1 && g_notify_ui_calls[0].player == s.player &&
               g_notify_ui_calls[0].index == (uint32_t)s.index,
           "U1: bldg_notify_ui(cur_player, cur_index) (0x00472ea1-0x00472ea8)");
        ck(g_release_order_calls.empty() && g_snd_play.empty() && g_coords.empty() &&
               g_sprintf_calls.empty() && g_print_text_message_count == 0 && g_complete_calls.empty() &&
               g_refresh_calls.empty(),
           "U1: none of the completion-gate callees fire when the DOUBLE-INDIRECTED gate "
           "(Building[Building[bid].upgrade_index].build_time_2, 0x00472c74-0x00472c9e) is skipped -- "
           "including the decoy row's build_time_2=0.0, which would wrongly pass a single-indirected read");
        ck_eq((uint32_t)fx.cam_pan_target_col, (uint32_t)s.cam_pan_col_seed,
              "U1: cam_pan_target_col UNCHANGED -- only the local-player arm inside the gate writes it");
        ck_eq((uint32_t)fx.cam_pan_target_row, (uint32_t)s.cam_pan_row_seed, "U1: cam_pan_target_row UNCHANGED");
        ck_eq((uint32_t)fx.b(s.player, s.index).state, 0u,
              "U1: cur_building->state UNTOUCHED (still its zero-init value) -- the state_transition_ids "
              "write at 0x00472dcf only happens inside the gate");
    }

    // =================================================================================================
    // U2 -- completion gate TAKEN at the exact `<=` boundary via the DOUBLE INDIRECTION
    // (Building[Building[bid].upgrade_index].build_time_2 == cycle_progress, 25.0), LOCAL player,
    // SIM_ACTIVE=1, race==2: the full local-player arm fires in order, ai_queue_release_order(mode=2)
    // fires FIRST (before the local/other split), completion_dispatch receives the ADDRESSES of
    // cam_pan_target_col/_row as param_3/param_4 (the register-reuse hazard) plus game_clock as
    // param_5, cur_building->state takes ONLY the low 16 bits of state_transition_ids[1], and the
    // production override does NOT fire (type mismatch, bldg_type=0 by default). The decoy on
    // cfg_buildings[cfg_row].build_time_2 is 999.0 -- a single-indirection bug would wrongly see
    // 999.0 <= 25.0 as false and skip the gate; this case fails if that bug is present.
    // =================================================================================================
    {
        Seed s;
        s.build_time_2_target = 25.0;     // == cycle_progress after accumulation -- boundary: gate IS taken
        s.build_time_2_decoy  = 999.0;    // wrong-indirection decoy: looks "not taken"
        s.player_side         = s.player; // 0 == 0 -> local
        s.sim_active          = 1;
        s.player_race         = 2;
        seed_and_run(fx, s);

        ck(trace_eq({"ai_queue_release_order", "snd_play", "get_coords", "sprintf", "print_text_message",
                     "completion_dispatch", "refresh_building", "notify_ui"}),
           "U2: exact call order -- release_order FIRST and unconditional (0x00472ca4-0x00472cb7), THEN "
           "the local/sim-active/race2 arm (0x00472ccf voice -> 0x00472d06 coords -> 0x00472d59 message "
           "-> 0x00472d9b completion+state+refresh -> 0x00472e86 tail)");

        ck_eq_d(fx.b(s.player, s.index).cycle_progress, 25.0,
                "U2: cycle_progress = 10.0*2.0+5.0 = 25.0, exactly at the DOUBLE-INDIRECTED build_time_2 "
                "-- gate taken (0x00472c95: `build_time_2 <= cycle_progress`, JC @0x00472c9e NOT taken at "
                "equality)");

        ck(g_release_order_calls.size() == 1, "U2: ai_queue_release_order fires exactly once (0x00472cb7)");
        if (g_release_order_calls.size() == 1) {
            const auto &rc = g_release_order_calls[0];
            ck(rc.player == s.player && rc.index == s.index && rc.mode == 2,
               "U2: ai_queue_release_order(cur_player, cur_index, mode=2) -- exact args (0x00472ca9-"
               "0x00472cb7, MOV EBX,0x2 @0x00472ca4)");
        }

        ck(g_snd_play.size() == 1 && g_snd_play[0].id == 0x21 && g_snd_play[0].volume == 100,
           "U2: snd_play(id=0x12+0xf=0x21 [race==2], volume=100) (0x00472cd8-0x00472d01) -- UPGRADING's "
           "OWN base sound id 0xf, different from construction's 4");

        ck(g_coords.size() == 1 && g_coords[0].player == s.player && g_coords[0].index == s.index,
           "U2: bldg_get_coords(cur_player, cur_index, &cam_pan_target_col, &cam_pan_target_row) "
           "(0x00472d06-0x00472d1e)");
        ck_eq((uint32_t)fx.cam_pan_target_col, 50u,
              "U2: cam_pan_target_col = tile(fine_x) = 1600/32 = 50 (0x00472d23-0x00472d39)");
        ck_eq((uint32_t)fx.cam_pan_target_row, 55u,
              "U2: cam_pan_target_row = tile(fine_y) = 1760/32 = 55 (0x00472d3e-0x00472d54)");

        ck(g_sprintf_calls.size() == 1, "U2: w_sprintf__vss fires once (0x00472d59-0x00472d8e)");
        if (g_sprintf_calls.size() == 1) {
            const auto &sp = g_sprintf_calls[0];
            ck(sp.a0 == k_name_text, "U2: sprintf name-arg = text_ptrs[Building[bid].id]");
            ck(sp.a1 == k_reason_text,
               "U2: sprintf reason-arg = text_ptrs[0x79] (TEXT_ID_UPGRADE_COMPLETE, literal push in "
               "0x00472d59-0x00472d82) -- DIFFERENT slot from construction's 0x74");
            ck(sp.dst == fx.text_scratch.data(), "U2: sprintf writes into own.text_scratch() (0xe15178)");
        }
        ck(g_print_text_message_count == 1 &&
               (g_print_text_message_ptrs.empty() ||
                g_print_text_message_ptrs[0] == (void *)fx.text_scratch.data()),
           "U2: game_ui_PrintTextMessage(own.text_scratch()) fires once, same buffer as sprintf's dst "
           "(0x00472d91-0x00472d96)");

        ck(g_complete_calls.size() == 1, "U2: completion_dispatch fires once (0x00472db5)");
        if (g_complete_calls.size() == 1) {
            const auto    &cc            = g_complete_calls[0];
            const uint32_t want_col_addr = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&fx.cam_pan_target_col));
            const uint32_t want_row_addr = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&fx.cam_pan_target_row));
            ck(cc.player == s.player && cc.index == (uint32_t)s.index,
               "U2: completion_dispatch(cur_player, cur_index, ...)");
            ck_eq(cc.param_3, want_col_addr,
                  "U2 REGISTER-REUSE HAZARD: completion_dispatch's param_3 = &cam_pan_target_col "
                  "(0x00472d06 `MOV ECX,0xe587e5`/0x00472d0b `MOV EBX,0xe587e1`, never reloaded before "
                  "0x00472db5)");
            ck_eq(cc.param_4, want_row_addr,
                  "U2 REGISTER-REUSE HAZARD: completion_dispatch's param_4 = &cam_pan_target_row, same "
                  "reasoning as param_3");
            ck_eq_d(cc.param_5, s.game_clock,
                    "U2: completion_dispatch's param_5 = game_clock (0x00472d9b-0x00472da7 pushes "
                    "_G_LLM_STRAT_GAME_CLOCK's two dwords) -- the 5th argument construction's own "
                    "equivalent call does NOT have");
        }

        ck_eq((uint32_t)fx.b(s.player, s.index).state, 0x1234u,
              "U2: cur_building->state = Building[bid].state_transition_ids[1] LOW 16 BITS ONLY "
              "(0xABCD1234 -> 0x1234, 0x00472dcf `MOV AX, word ptr[...]` / 0x00472dd6 `MOV word ptr[EDX+"
              "0xd],AX`) -- production override does NOT fire (bldg_type=0 matches neither "
              "H_PRODUCTION(0x15) nor A_PRODUCTION(0x01))");

        ck(g_refresh_calls.size() == 1 && g_refresh_calls[0].player == s.player &&
               g_refresh_calls[0].index == s.index,
           "U2: refresh_building(cur_player, cur_index) fires (0x00472e81) -- ONLY reached because the "
           "gate was taken; upgrading has no separate 'skip refresh_building' arm");

        ck(g_notify_ui_calls.size() == 1 && g_notify_ui_calls[0].player == s.player &&
               g_notify_ui_calls[0].index == (uint32_t)s.index,
           "U2: bldg_notify_ui(cur_player, cur_index) fires at the shared tail too (0x00472ea8)");
        ck_eq_d(fx.tick_budget, 0.0, "U2: tick_budget zeroed at the tail even though the gate was taken");
    }

    // =================================================================================================
    // U3 -- same as U2 but race != 2: voice-line sound id is the BASE id (0xf), no 0x12 offset.
    // =================================================================================================
    {
        Seed s;
        s.build_time_2_target = 25.0;
        s.player_side         = s.player;
        s.sim_active          = 1;
        s.player_race         = 0;
        seed_and_run(fx, s);

        ck(g_snd_play.size() == 1 && g_snd_play[0].id == 0xf && g_snd_play[0].volume == 100,
           "U3: race!=2 -> snd_play id = base(0xf) + 0, no 0x12 offset (0x00472ce6 JNZ taken -> "
           "LAB_00472cf1)");
    }

    // =================================================================================================
    // U4 -- gate taken, LOCAL player, SIM_ACTIVE=0: the voice line is skipped (SIM_ACTIVE gate,
    // 0x00472ccf JZ), but ai_queue_release_order/get_coords/message/completion chain still fire --
    // only snd_play is gated.
    // =================================================================================================
    {
        Seed s;
        s.build_time_2_target = 25.0;
        s.player_side         = s.player;
        s.sim_active          = 0;
        s.player_race         = 2; // irrelevant here since the whole voice-line block is skipped
        seed_and_run(fx, s);

        ck(trace_eq({"ai_queue_release_order", "get_coords", "sprintf", "print_text_message",
                     "completion_dispatch", "refresh_building", "notify_ui"}),
           "U4: SIM_ACTIVE=0 -- snd_play does NOT fire, but release_order/coords/message/completion chain "
           "still do (0x00472ccf JZ @0x00472cd6 skips only the voice-line block)");
        ck(g_snd_play.empty(), "U4: no voice line at all when SIM_ACTIVE=0");
        ck(g_coords.size() == 1, "U4: bldg_get_coords still fires (SIM_ACTIVE does not gate it)");
        ck_eq((uint32_t)fx.cam_pan_target_col, 50u, "U4: cam_pan_target still written when SIM_ACTIVE=0");
    }

    // =================================================================================================
    // U5 -- gate taken, OTHER player (cur_player != PlayerSide, 0x00472cc9 JNZ taken -> skips straight
    // to LAB_00472d9b): NONE of voice/coords/message fire (cam_pan_target UNCHANGED), but
    // ai_queue_release_order/completion_dispatch/refresh_building STILL fire, and completion_dispatch's
    // param_3/param_4 are the MODELED-INERT constant 0 (COMPLETION_DISPATCH_PARAM_UNRESOLVED) -- NOT any
    // address -- because this function is void(void) and has no caller-supplied EBX/ECX to forward
    // (the exact inverse of construction's equivalent test, which DOES have real incoming params).
    // =================================================================================================
    {
        Seed s;
        s.build_time_2_target = 25.0;
        s.player_side         = (int16_t)(s.player + 5); // != player -> "other"
        s.sim_active          = 1;                       // would matter if the voice line fired -- must not
        s.player_race         = 2;
        seed_and_run(fx, s);

        ck(trace_eq({"ai_queue_release_order", "completion_dispatch", "refresh_building", "notify_ui"}),
           "U5: other-player branch -- voice/coords/message never fire; release_order (unconditional, "
           "pre-split) + completion chain + tail still do (0x00472cc9 JNZ -> LAB_00472d9b skips "
           "0x00472ccf-0x00472d96 entirely)");
        ck(g_snd_play.empty() && g_coords.empty() && g_sprintf_calls.empty() && g_print_text_message_count == 0,
           "U5: none of the local-only calls fire on the other-player branch");
        ck_eq((uint32_t)fx.cam_pan_target_col, (uint32_t)s.cam_pan_col_seed,
              "U5: cam_pan_target_col UNCHANGED -- only the local-player arm writes it");
        ck_eq((uint32_t)fx.cam_pan_target_row, (uint32_t)s.cam_pan_row_seed, "U5: cam_pan_target_row UNCHANGED");

        ck(g_complete_calls.size() == 1, "U5: completion_dispatch still fires (unconditional in the gate)");
        if (g_complete_calls.size() == 1) {
            const auto &cc = g_complete_calls[0];
            ck(cc.player == s.player && cc.index == (uint32_t)s.index,
               "U5: completion_dispatch(cur_player, cur_index, ...)");
            ck_eq(cc.param_3, 0u,
                  "U5 REGISTER-REUSE HAZARD (inverse of U2): completion_dispatch's param_3 = the "
                  "MODELED-INERT constant 0 (COMPLETION_DISPATCH_PARAM_UNRESOLVED) on the other-player "
                  "branch -- this is void(void), so unlike construction there is no real incoming EBX to "
                  "forward verbatim; the .cpp's own comment documents this is provably inert regardless "
                  "of value");
            ck_eq(cc.param_4, 0u,
                  "U5 REGISTER-REUSE HAZARD (inverse of U2): completion_dispatch's param_4 = 0, same "
                  "reasoning as param_3");
            ck_eq_d(cc.param_5, s.game_clock,
                    "U5: game_clock is STILL forwarded as param_5 on the other-player branch -- it is not "
                    "part of the register-reuse hazard, it is pushed unconditionally at 0x00472d9b");
        }

        ck(g_release_order_calls.size() == 1,
           "U5: ai_queue_release_order still fires (0x00472cb7 is BEFORE the local/other split at "
           "0x00472cc9, so the other-player branch does not skip it)");
        ck(g_refresh_calls.size() == 1, "U5: refresh_building still fires");
        ck(g_notify_ui_calls.size() == 1, "U5: bldg_notify_ui still fires at the tail");
    }

    // =================================================================================================
    // U6 -- production-in-progress override APPLIES for H_PRODUCTION(0x15) with nonzero
    // queued_count[0]: cur_building->state is forced to PROD_PICK_NEXT (0x6c), OVERRIDING the
    // state_transition_ids[1] value that would otherwise have been written (0x1234, per U2). Run on the
    // other-player branch to isolate this mechanism from the local-player arm.
    // =================================================================================================
    {
        Seed s;
        s.build_time_2_target   = 25.0;
        s.player_side           = (int16_t)(s.player + 5); // "other" -- isolates the override mechanism
        s.bldg_type             = 0x15;                    // H_PRODUCTION
        s.prod_queued_at_sub_id = 5;                       // nonzero
        seed_and_run(fx, s);

        ck_eq((uint32_t)fx.b(s.player, s.index).state, BLDG_STATE_PROD_PICK_NEXT,
              "U6: type==H_PRODUCTION(0x15) AND productions[player][sub_id].queued_count[0]!=0 -> "
              "state forced to PROD_PICK_NEXT (0x6c), overriding state_transition_ids[1]'s 0x1234 "
              "(0x00472e03 CMP...0x15; JZ @0x00472e0a taken -> LAB_00472e3e -> 0x00472e6d MOV word "
              "ptr[EAX+0xd],0x6c)");
    }

    // =================================================================================================
    // U7 -- production-in-progress override APPLIES for A_PRODUCTION(0x01) too -- the OTHER half of the
    // OR condition (0x00472e0c-0x00472e3c), a distinct code path from U6's H_PRODUCTION arm.
    // =================================================================================================
    {
        Seed s;
        s.build_time_2_target   = 25.0;
        s.player_side           = (int16_t)(s.player + 5);
        s.bldg_type             = 0x01; // A_PRODUCTION
        s.prod_queued_at_sub_id = 7;
        seed_and_run(fx, s);

        ck_eq((uint32_t)fx.b(s.player, s.index).state, BLDG_STATE_PROD_PICK_NEXT,
              "U7: type==A_PRODUCTION(0x01) AND queued_count[0]!=0 -> state forced to PROD_PICK_NEXT "
              "(0x00472e0c-0x00472e35 CMP...0x01 JNZ @0x00472e3c NOT taken -> falls into LAB_00472e3e)");
    }

    // =================================================================================================
    // U8 -- production override does NOT apply when the type matches (H_PRODUCTION) but
    // queued_count[0]==0: state stays exactly the state_transition_ids[1] low16 value (0x1234).
    // =================================================================================================
    {
        Seed s;
        s.build_time_2_target   = 25.0;
        s.player_side           = (int16_t)(s.player + 5);
        s.bldg_type             = 0x15;
        s.prod_queued_at_sub_id = 0; // zero -- the CMP...0x0 JZ @0x00472e66 skips the override
        seed_and_run(fx, s);

        ck_eq((uint32_t)fx.b(s.player, s.index).state, 0x1234u,
              "U8: type matches but queued_count[0]==0 -- override does NOT fire "
              "(0x00472e5f CMP dword ptr[...],0x0 / 0x00472e66 JZ -> LAB_00472e73, state stays "
              "state_transition_ids[1]'s low16 = 0x1234)");
    }

    // =================================================================================================
    // U9 -- production override does NOT apply when the type does not match either production type,
    // even with a nonzero queued_count[0]: state stays 0x1234.
    // =================================================================================================
    {
        Seed s;
        s.build_time_2_target   = 25.0;
        s.player_side           = (int16_t)(s.player + 5);
        s.bldg_type             = 0x02; // neither H_PRODUCTION(0x15) nor A_PRODUCTION(0x01)
        s.prod_queued_at_sub_id = 9;    // nonzero -- irrelevant, type gate fails first
        seed_and_run(fx, s);

        ck_eq((uint32_t)fx.b(s.player, s.index).state, 0x1234u,
              "U9: type (0x02) matches neither production type -- override does NOT fire regardless of "
              "queued_count (0x00472e03 JZ not taken, 0x00472e35 JNZ taken -> LAB_00472e73 directly), "
              "state stays 0x1234");
    }

    // =================================================================================================
    // U10 -- the production lookup is KEYED BY cur_building->sub_id, NOT by cur_index: a nonzero
    // queued_count sitting at the `index`-keyed slot (the wrong key) must NOT trigger the override,
    // while the correct sub_id-keyed slot stays zero. Catches a translation that read
    // productions[player][cur_index] instead of productions[player][sub_id].
    // =================================================================================================
    {
        Seed s;
        s.build_time_2_target   = 25.0;
        s.player_side           = (int16_t)(s.player + 5);
        s.bldg_type             = 0x15; // type matches
        s.prod_queued_at_sub_id = 0;    // the CORRECT key -- zero
        s.prod_queued_at_index  = 42;   // the WRONG key (cur_index, != sub_id) -- nonzero
        seed_and_run(fx, s);

        ck_eq((uint32_t)fx.b(s.player, s.index).state, 0x1234u,
              "U10: override reads productions[player][cur_building->sub_id] (0x00472e3e-0x00472e43 "
              "`MOVZX EAX, byte ptr[EAX+0xc6]` = sub_id, then 0x00472e50-0x00472e5f indexes by cur_player), "
              "NOT productions[player][cur_index] -- the sub_id-keyed slot is zero, so no override even "
              "though the index-keyed slot is nonzero");
    }

    // =================================================================================================
    // U11 -- fine->tile truncation is TOWARD ZERO, both for a negative fine coordinate and for a
    // positive non-exact one, reproducing the SAR/SHL/SBB/SAR idiom's signed-correct truncating divide
    // (the SAME fine_to_tile() helper every sibling handler in this closure shares).
    // =================================================================================================
    {
        Seed s;
        s.build_time_2_target = 25.0;
        s.player_side         = s.player; // local -- exercises the coords write
        s.sim_active          = 0;        // voice line irrelevant here
        s.fine_x              = -63;      // -63/32 truncated toward zero = -1 (not -2, the floor result)
        s.fine_y              = 95;       // 95/32 truncated toward zero = 2
        seed_and_run(fx, s);

        ck_eq((uint32_t)fx.cam_pan_target_col, (uint32_t)(-1),
              "U11: cam_pan_target_col = fine_to_tile(-63) = -1 (truncate toward zero, NOT floor's -2) "
              "(0x00472d23-0x00472d39 SAR/SHL/SBB/SAR idiom)");
        ck_eq((uint32_t)fx.cam_pan_target_row, 2u,
              "U11: cam_pan_target_row = fine_to_tile(95) = 2 (95/32 = 2.96875, truncated) "
              "(0x00472d3e-0x00472d54)");
    }
}

} // namespace mh::sim::test
