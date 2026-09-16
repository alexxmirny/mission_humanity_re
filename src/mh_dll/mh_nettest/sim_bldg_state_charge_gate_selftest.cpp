//
// sim_bldg_state_charge_gate_selftest.cpp -- `simtest` cases for llm_strat_bldg_state_charge_gate
// (sim/sim_bldg_state_charge.h/.cpp @0x00472674), SIM1B building_tick machinery -- the CHARGE_GATE
// per-state tick handler (`detail::bldg_state_charge_gate`).
//
// EXPECTED BEHAVIOUR HAND-DERIVED FROM THE DISASSEMBLY
// (tmp/decomp_sim/llm_strat_bldg_state_charge_gate_00472674.asm), cross-checked against the .cpp/.h's
// own per-line address citations -- NOT read off the .cpp body alone:
//
//   Gate (0x0047268c-0x004726ac): `if (cur_building->energy >= Building[bid].energy)` (JNC) -> charge
//   already at/above cap: run the SHARED TAIL directly (0x00472859), no pay_cycle_inputs call at all.
//   ELSE: `bldg_pay_cycle_inputs(cur_player, cur_index)` (0x004726b2-0x004726c5).
//     returns 0 (0x004726c8 JNZ not taken): state=CHARGE_STEP(0x6a) (0x004726ce),
//       ai_queue_release_order(cur_player, cur_index, mode=0) (0x004726db-0x004726e9), then JMP
//       0x00472854 -- straight to the FINAL TAIL, skipping the shared tail entirely.
//     returns nonzero (0x004726cc JNZ taken, LAB_004726f3): if cur_player==PlayerSide (0x004726f9
//       JNZ 0x00472764 not taken): a "%s: %s" message -- name via the ROSTER-derived
//       buildings[cur_player][cur_index].building_id (0x0047270e-0x0047272a, NOT cur_building's own
//       building_id), reason = G_TEXT_PTRS[paid] (0x472702-0x472708, `paid` -- the pay_cycle_inputs
//       return value ITSELF -- reused as the text id) via w_sprintf(0x472752)+PrintTextMessage(0x47275f).
//       THEN (regardless of local/other, LAB_00472764) the SHARED TAIL:
//         1. ai_queue_release_order(cur_player, cur_index, mode=1) (0x472769-0x472777)
//         2. bldg_completion_dispatch(cur_player, cur_index, 0, 0, (double)*game_clock)
//            (0x47277c-0x472796) -- param_3/param_4 ALWAYS 0 per the header's RESOLVED note (Ghidra's
//            own extraout_EBX/unaff_ECX placeholders, confirmed dead in the callee).
//         3. cur_building->state = Building[bid].state_transition_ids[1] LOW 16 BITS (0x47279b-0x4727b7,
//            `bid` via the cur_building POINTER's OWN building_id -- a different expression from the
//            roster-derived one used next).
//         4. if Building[roster_bid].type (ROSTER-derived, 0x4727bb-0x4727e4) is
//            H_PRODUCTION(0x15)/A_PRODUCTION(0x01) (0x4727e4/0x472816) AND
//            productions[cur_player][cur_building->sub_id].queued_count[0]!=0 (0x472840): OVERRIDE
//            state=PROD_PICK_NEXT(0x6c) (0x47284e) -- overwrites step 3's write.
//       The SAME shared tail is byte-identical at 0x00472859-0x00472949 for the energy>=cap arm.
//   FINAL TAIL (both the payment-failed skip AND the shared-tail fallthrough converge at
//   0x00472949): bldg_notify_ui(cur_player, cur_index) -> refresh_building(cur_player, cur_index).
//
// SCOPE (honest, not exhaustive): this file pins the call/write ORDER, every branch predicate, the
// mode values passed to ai_queue_release_order (0 on payment-failure, 1 on the shared tail), the
// 16-bit mask on the state_transition_ids read, the AND (not OR) gate on the PROD_PICK_NEXT override,
// and that completion_dispatch's param_3/param_4 are unconditionally 0. It does NOT reproduce
// w_sprintf's actual text formatting (asserted as "fired", not by content) -- the whole formatting
// path is presentation and behind the effect seam, same posture sim_bldg_state_destroyed_selftest.cpp
// documents for its own function.
//
#include "sim/sim_bldg_state_charge.h"

#include <cstdint>
#include <cstring>
#include <vector>

#include "sim/sim_order_enqueue.h" // BUILDING_TYPE_A_PRODUCTION (0x01) / BUILDING_TYPE_H_PRODUCTION (0x15)
#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// ---- literal constants this function's own .cpp declares file-local (mirrored here for asserts) --
constexpr uint16_t ST_CHARGE_STEP    = 0x6a;
constexpr uint16_t ST_PROD_PICK_NEXT = 0x6c;

// ---- shared trace: one sequence proves CALL ORDER across all 7 callees -------------------------
std::vector<const char *> g_trace;
void                      tr(const char *tag) { g_trace.push_back(tag); }

bool trace_eq(const std::vector<const char *> &want) {
    if (g_trace.size() != want.size()) return false;
    for (size_t i = 0; i < want.size(); ++i)
        if (std::strcmp(g_trace[i], want[i]) != 0) return false;
    return true;
}

// ---- return-value knob (settable per case BEFORE seed_and_run) ---------------------------------
int32_t g_pay_cycle_inputs_ret = 0;

// ---- per-callee recorders (7, one per bldg_state_charge_gate_calls member) ---------------------
struct PayCall {
    uint32_t player, index;
};
std::vector<PayCall> g_pay_calls;
int32_t              rec_pay_cycle_inputs(uint32_t player, uint32_t b_index) {
    tr("pay_cycle_inputs");
    g_pay_calls.push_back({player, b_index});
    return g_pay_cycle_inputs_ret;
}

struct ReleaseCall {
    int32_t player, index, mode;
};
std::vector<ReleaseCall> g_release_calls;
void                     rec_ai_queue_release_order(int32_t player, int32_t building_index, int32_t mode) {
    tr("ai_queue_release_order");
    g_release_calls.push_back({player, building_index, mode});
}

int            g_sprintf_count       = 0;
const wchar_t *g_sprintf_last_name   = nullptr;
const wchar_t *g_sprintf_last_reason = nullptr;
int32_t        rec_sprintf(void *, const wchar_t *, const wchar_t *a0, const wchar_t *a1) {
    tr("sprintf");
    ++g_sprintf_count;
    g_sprintf_last_name   = a0;
    g_sprintf_last_reason = a1;
    return 0;
}

int      g_print_text_message_count = 0;
uint32_t rec_print_text_message(void *) {
    tr("print_text_message");
    ++g_print_text_message_count;
    return 0;
}

struct DispatchCall {
    uint32_t player, index, param_3, param_4;
    double   clock;
};
std::vector<DispatchCall> g_dispatch_calls;
void                      rec_completion_dispatch(uint32_t param_1, uint32_t param_2, uint32_t param_3,
                                                  uint32_t param_4, double param_5) {
    tr("completion_dispatch");
    g_dispatch_calls.push_back({param_1, param_2, param_3, param_4, param_5});
}

struct NotifyCall {
    uint16_t player;
    uint32_t index;
};
std::vector<NotifyCall> g_notify_calls;
void                    rec_bldg_notify_ui(uint16_t player, uint32_t index) {
    tr("notify_ui");
    g_notify_calls.push_back({player, index});
}

struct RefreshCall {
    uint16_t player;
    int32_t  index;
};
std::vector<RefreshCall> g_refresh_calls;
void                     rec_refresh_building(uint16_t p_id, int32_t b_id) {
    tr("refresh_building");
    g_refresh_calls.push_back({p_id, b_id});
}

const bldg_state_charge_gate_calls g_calls = {
    &rec_pay_cycle_inputs,
    &rec_ai_queue_release_order,
    &rec_sprintf,
    &rec_print_text_message,
    &rec_completion_dispatch,
    &rec_bldg_notify_ui,
    &rec_refresh_building,
};

void reset_observations() {
    g_trace.clear();
    g_pay_calls.clear();
    g_release_calls.clear();
    g_sprintf_count            = 0;
    g_sprintf_last_name        = nullptr;
    g_sprintf_last_reason      = nullptr;
    g_print_text_message_count = 0;
    g_dispatch_calls.clear();
    g_notify_calls.clear();
    g_refresh_calls.clear();
}

// ---- fixture seeding -----------------------------------------------------------------------------
struct Seed {
    uint16_t player  = 0;
    int32_t  index   = 1;
    uint16_t cfg_row = 10; // cur_building's OWN building_id (Building[cfg_row]) -- see the note below.

    double bldg_energy = 5.0;  // cur_building->energy
    double cap_energy  = 10.0; // cfg_buildings[cfg_row].energy -- the completion threshold

    int32_t pay_cycle_inputs_ret = 1; // nonzero == payment succeeded

    int16_t player_side = 0; // == player -> "local"; != player -> "other"

    double game_clock = 100.0;

    uint32_t state_transition_lo   = 0x1234; // state_transition_ids[1] low 16 bits (expected next state)
    uint32_t state_transition_hi16 = 0xBEEF; // upper 16 bits of the SAME element -- must be masked OFF

    uint8_t roster_type   = 2; // NOT a production type by default
    uint8_t sub_id        = 3; // cur_building->sub_id -- indexes productions[player][sub_id]
    int32_t queued_count0 = 0; // productions[player][sub_id].queued_count[0]

    int32_t sentinel_state = 0xBEEF; // pre-call state sentinel, distinct from every real state value
};

void seed_and_run(sim_fixture &fx, const Seed &s) {
    fx.reset();

    building &b   = fx.b(s.player, s.index);
    b.building_id = s.cfg_row;
    b.energy      = s.bldg_energy;
    b.sub_id      = s.sub_id;
    b.state       = (uint16_t)s.sentinel_state;

    fx.cur_building_ptr = &b;
    fx.view_cur_player  = s.player;
    fx.view_cur_index   = (uint16_t)s.index;

    // NOTE on the "two building_id expressions" hazard the header documents: `cur_building()`'s own
    // building_id (step 1/2c's `bid`) and the ROSTER-derived `building_of(v, player, index).building_id`
    // (step 1's message name / step 2d's type check) are DIFFERENT C++ expressions in the .cpp, but in
    // THIS fixture shape they resolve to the SAME memory (`cur_building_ptr` is pointed at
    // `fx.b(player, index)`, the very record `building_of` also indexes) -- so both always read the
    // SAME building_id here, matching the precedent sim_bldg_state_destroyed_selftest.cpp's identical
    // aliasing already sets for the same hazard in a sibling function. Not exercised as a genuine
    // divergence (see the report's "not exercised" note).
    cfg_building &cb = fx.cfg_buildings[s.cfg_row];
    cb.energy        = s.cap_energy;
    cb.type          = s.roster_type;
    cb.state_transition_ids[1] =
        static_cast<int32_t>((s.state_transition_hi16 << 16) | (s.state_transition_lo & 0xffffu));

    fx.player_side = s.player_side;
    fx.game_clock  = s.game_clock;

    fx.productions[(size_t)s.player * PRODUCTIONS_PER_PLAYER + s.sub_id].queued_count[0] = s.queued_count0;

    g_pay_cycle_inputs_ret = s.pay_cycle_inputs_ret;

    reset_observations();

    sim_store own = fx.store();
    detail::bldg_state_charge_gate(fx.view(), own, g_calls);
}

} // namespace

void run_bldg_state_charge_gate_tests() {
    sim_fixture fx;

    // =================================================================================================
    // G1 -- ENERGY>=CAP GATE (0x0047268c JNC): charge already at/above cap -- pay_cycle_inputs is NOT
    // called at all, the shared tail runs directly (ai_queue_release_order mode=1, completion_dispatch
    // param_3=0/param_4=0, state write from state_transition_ids[1] low 16 bits since roster_type is
    // not a production type here), then the final tail.
    // =================================================================================================
    {
        Seed s;
        s.player      = 0;
        s.index       = 1;
        s.bldg_energy = 10.0;
        s.cap_energy  = 10.0; // energy >= cap -- JNC taken (>=, not >, per 0x004726ac's JNC on FCOMP/SAHF)
        seed_and_run(fx, s);

        ck(trace_eq({"ai_queue_release_order", "completion_dispatch", "notify_ui", "refresh_building"}),
           "G1 @0x0047268c/0x00472859: energy>=cap -- pay_cycle_inputs NEVER called, shared tail runs directly");
        ck(g_pay_calls.empty(), "G1: bldg_pay_cycle_inputs not called on the energy>=cap arm");

        ck(g_release_calls.size() == 1 && g_release_calls[0].player == 0 && g_release_calls[0].index == 1 &&
               g_release_calls[0].mode == 1,
           "G1 @0x00472769-0x472777: ai_queue_release_order(player, index, mode=1) on the shared tail");

        ck(g_dispatch_calls.size() == 1, "G1: bldg_completion_dispatch called once");
        if (g_dispatch_calls.size() == 1) {
            const auto &d = g_dispatch_calls[0];
            ck(d.player == 0 && d.index == 1, "G1 @0x47277c-0x472796: completion_dispatch(cur_player, cur_index, ...)");
            ck(d.param_3 == 0 && d.param_4 == 0,
               "G1: completion_dispatch param_3=0, param_4=0 -- ALWAYS, per the header's RESOLVED-dead note");
            ck_eq_d(d.clock, s.game_clock, "G1 @0x47277c/0x472782: completion_dispatch param_5 = *game_clock");
        }

        ck_eq((uint32_t)fx.b(0, 1).state, (uint32_t)(s.state_transition_lo & 0xffff),
              "G1 @0x4727b0/0x4727b7: state = state_transition_ids[1] low 16 bits (roster_type != production)");

        ck(g_notify_calls.size() == 1 && g_notify_calls[0].player == 0 && g_notify_calls[0].index == 1,
           "G1 @0x472949-0x472957: bldg_notify_ui(player, index) in the final tail");
        ck(g_refresh_calls.size() == 1 && g_refresh_calls[0].player == 0 && g_refresh_calls[0].index == 1,
           "G1 @0x47295c-0x47296a: refresh_building(player, index) in the final tail");
        ck(g_sprintf_count == 0 && g_print_text_message_count == 0,
           "G1: no message on the energy>=cap arm -- that block is only reached via the payment path");
    }

    // =================================================================================================
    // G1b -- energy STRICTLY above cap also takes the same arm (JNC is >=, confirm with a real margin,
    // not just the boundary equality G1 used).
    // =================================================================================================
    {
        Seed s;
        s.bldg_energy = 12.0;
        s.cap_energy  = 10.0;
        seed_and_run(fx, s);
        ck(g_pay_calls.empty(), "G1b @0x0047268c: energy > cap also skips pay_cycle_inputs (JNC, not JG)");
    }

    // =================================================================================================
    // G2 -- PAYMENT FAILED (pay_cycle_inputs returns 0, 0x004726c8 JNZ not taken): state=CHARGE_STEP
    // (0x6a), ai_queue_release_order(mode=0), then JUMP STRAIGHT to the final tail -- NO
    // completion_dispatch, NO state_transition_ids read, NO PROD_PICK_NEXT check, no message.
    // =================================================================================================
    {
        Seed s;
        s.player               = 2;
        s.index                = 4;
        s.bldg_energy          = 1.0; // below cap -- must take the pay_cycle_inputs branch at all
        s.cap_energy           = 10.0;
        s.pay_cycle_inputs_ret = 0; // payment failed
        seed_and_run(fx, s);

        ck(trace_eq({"pay_cycle_inputs", "ai_queue_release_order", "notify_ui", "refresh_building"}),
           "G2 @0x004726ce-0x4726ee: payment failed -- state write + release(mode=0) then straight to the "
           "final tail, skipping the shared tail entirely");

        ck(g_pay_calls.size() == 1 && g_pay_calls[0].player == 2 && g_pay_calls[0].index == 4,
           "G2 @0x4726b2-0x4726c0: bldg_pay_cycle_inputs(cur_player, cur_index)");

        ck_eq((uint32_t)fx.b(2, 4).state, (uint32_t)ST_CHARGE_STEP,
              "G2 @0x4726ce: state = CHARGE_STEP(0x6a) on payment failure");

        ck(g_release_calls.size() == 1 && g_release_calls[0].player == 2 && g_release_calls[0].index == 4 &&
               g_release_calls[0].mode == 0,
           "G2 @0x4726db-0x4726e9: ai_queue_release_order(player, index, mode=0) -- DIFFERENT mode than "
           "the shared tail's mode=1");

        ck(g_dispatch_calls.empty(),
           "G2: completion_dispatch does NOT fire on payment failure -- the shared tail is skipped");
        ck(g_sprintf_count == 0 && g_print_text_message_count == 0,
           "G2: no message on payment failure -- that block is only inside the payment-succeeded arm");

        ck(g_notify_calls.size() == 1 && g_refresh_calls.size() == 1,
           "G2: notify_ui + refresh_building STILL fire -- the final tail is unconditional");
    }

    // =================================================================================================
    // G3 -- PAYMENT SUCCEEDED, LOCAL PLAYER (cur_player == PlayerSide): the "%s: %s" message fires,
    // using the ROSTER-derived building_id for the name and `paid` ITSELF (the return value, reused)
    // as the reason text id -- then the shared tail runs exactly like G1's.
    // =================================================================================================
    {
        Seed s;
        s.player               = 3;
        s.index                = 6;
        s.player_side          = 3; // == player -> LOCAL
        s.bldg_energy          = 1.0;
        s.cap_energy           = 10.0;
        s.pay_cycle_inputs_ret = 77; // nonzero, distinct sentinel reused as the text id
        seed_and_run(fx, s);

        ck(trace_eq({"pay_cycle_inputs", "sprintf", "print_text_message", "ai_queue_release_order",
                     "completion_dispatch", "notify_ui", "refresh_building"}),
           "G3 @0x4726f3-0x472949: payment succeeded, local player -- message THEN the shared tail, in order");

        ck(g_sprintf_count == 1 && g_print_text_message_count == 1,
           "G3 @0x472752/0x47275f: local-player message fires exactly once");
        // v.text_ptrs is left null in the shared fixture (sim_bldg_state_destroyed_selftest.cpp's same
        // posture) -- the case only asserts WHICH index was chosen, not the string content, by
        // checking the pointer VALUES forwarded match text_ptrs[roster building_id's cfg id] /
        // text_ptrs[paid] exactly (both null here since text_ptrs is unseeded, but the INDEX choice is
        // what's under test, verified indirectly via the trace + the pay_cycle_inputs-return-as-id
        // wiring below).
        ck(g_release_calls.size() == 1 && g_release_calls[0].mode == 1,
           "G3: shared tail's ai_queue_release_order(mode=1) still fires after the message");
        ck(g_dispatch_calls.size() == 1 && g_dispatch_calls[0].param_3 == 0 && g_dispatch_calls[0].param_4 == 0,
           "G3: completion_dispatch param_3=0/param_4=0 holds on the payment-succeeded/local arm too");
    }

    // =================================================================================================
    // G3b -- the reason id is `paid` ITSELF: two otherwise-identical runs with DIFFERENT
    // pay_cycle_inputs return values must select DIFFERENT text_ptrs slots. Verified via a seeded
    // text_ptrs table (distinct non-null sentinels per index) so the actual pointer forwarded to
    // w_sprintf can be compared, not just "a message fired".
    // =================================================================================================
    {
        static const wchar_t *TXT_A = L"reason-A";
        static const wchar_t *TXT_B = L"reason-B";
        static const wchar_t *NAME  = L"building-name";

        Seed s;
        s.player               = 0;
        s.index                = 1;
        s.player_side          = 0; // local
        s.bldg_energy          = 1.0;
        s.cap_energy           = 10.0;
        s.cfg_row              = 10;
        s.pay_cycle_inputs_ret = 41;
        fx.reset();
        fx.cfg_buildings[s.cfg_row].id = 55; // the "name" define-index this row's cb.id resolves to
        fx.text_ptrs[55]               = NAME;
        fx.text_ptrs[41]               = TXT_A;
        fx.text_ptrs[42]               = TXT_B;

        building &b                        = fx.b(s.player, s.index);
        b.building_id                      = s.cfg_row;
        b.energy                           = s.bldg_energy;
        fx.cur_building_ptr                = &b;
        fx.view_cur_player                 = s.player;
        fx.view_cur_index                  = (uint16_t)s.index;
        fx.cfg_buildings[s.cfg_row].energy = s.cap_energy;
        fx.cfg_buildings[s.cfg_row].type   = 2; // not a production type
        fx.player_side                     = s.player_side;
        g_pay_cycle_inputs_ret             = s.pay_cycle_inputs_ret;
        reset_observations();
        {
            sim_store own = fx.store();
            detail::bldg_state_charge_gate(fx.view(), own, g_calls);
        }
        ck(g_sprintf_last_name == NAME,
           "G3b @0x47270e-0x47272a: message name = text_ptrs[buildings[player][index].building_id's cfg id] "
           "(ROSTER-derived)");
        ck(g_sprintf_last_reason == TXT_A,
           "G3b @0x472702-0x472708: message reason = text_ptrs[paid] -- paid(41) reused AS the text id");

        // Re-run with paid=42 (only the return value differs) -- the reason pointer must track it.
        g_pay_cycle_inputs_ret = 42;
        reset_observations();
        {
            sim_store own = fx.store();
            detail::bldg_state_charge_gate(fx.view(), own, g_calls);
        }
        ck(g_sprintf_last_reason == TXT_B,
           "G3b: changing ONLY pay_cycle_inputs's return (41->42) changes the reason text id (41->42) "
           "identically -- proves the return value is reused verbatim, not a fixed/derived id");
    }

    // =================================================================================================
    // G4 -- PAYMENT SUCCEEDED, OTHER PLAYER (cur_player != PlayerSide, 0x004726f9 JNZ taken): the
    // message does NOT fire, but the shared tail still runs (mode=1, completion_dispatch, state write).
    // =================================================================================================
    {
        Seed s;
        s.player               = 1;
        s.index                = 2;
        s.player_side          = 5; // != player -> OTHER
        s.bldg_energy          = 1.0;
        s.cap_energy           = 10.0;
        s.pay_cycle_inputs_ret = 9;
        seed_and_run(fx, s);

        ck(trace_eq({"pay_cycle_inputs", "ai_queue_release_order", "completion_dispatch", "notify_ui",
                     "refresh_building"}),
           "G4 @0x472700 JNZ 0x472764: other player -- message skipped entirely, straight to the shared tail");
        ck(g_sprintf_count == 0 && g_print_text_message_count == 0,
           "G4: no message on the other-player branch, even though payment succeeded");
        ck(g_release_calls.size() == 1 && g_release_calls[0].mode == 1,
           "G4: shared tail's ai_queue_release_order(mode=1) still fires for the other-player branch");
        ck(g_dispatch_calls.size() == 1, "G4: completion_dispatch still fires for the other-player branch");
    }

    // =================================================================================================
    // G5 -- state_transition_ids[1] is masked to the LOW 16 BITS: seed a value whose upper half is a
    // nonzero sentinel and confirm it is NEVER visible in the (uint16_t) state field.
    // =================================================================================================
    {
        Seed s;
        s.bldg_energy           = 10.0;
        s.cap_energy            = 10.0; // energy>=cap arm -- simplest path to the state write
        s.state_transition_lo   = 0x00CD;
        s.state_transition_hi16 = 0xFFFF; // every upper bit set
        seed_and_run(fx, s);

        ck_eq((uint32_t)fx.b(s.player, s.index).state, 0x00CDu,
              "G5 @0x4727b0: state_transition_ids[1] LOW 16 BITS only -- upper 16 bits (0xFFFF here) "
              "must not leak into the uint16_t state field");
    }

    // =================================================================================================
    // G6 -- PROD_PICK_NEXT override: AND gate, not OR. Six sub-cases -- {A_PRODUCTION, H_PRODUCTION,
    // neither} x {queued_count[0]==0, !=0} -- to prove BOTH the roster-type check and the queued_count
    // check must hold, and that the override OVERWRITES the state_transition_ids-derived write rather
    // than being skipped alongside it.
    // =================================================================================================
    {
        // G6a: A_PRODUCTION, queued_count[0] != 0 -- OVERRIDE fires.
        {
            Seed s;
            s.roster_type         = BUILDING_TYPE_A_PRODUCTION;
            s.queued_count0       = 1;
            s.bldg_energy         = 10.0;
            s.cap_energy          = 10.0;
            s.state_transition_lo = 0x1111; // distinct from PROD_PICK_NEXT(0x6c) -- proves OVERWRITE
            seed_and_run(fx, s);
            ck_eq((uint32_t)fx.b(s.player, s.index).state, (uint32_t)ST_PROD_PICK_NEXT,
                  "G6a @0x47284e: A_PRODUCTION + queued_count[0]!=0 -- state OVERRIDDEN to PROD_PICK_NEXT(0x6c), "
                  "overwriting the state_transition_ids write (0x1111) from step 3");
        }
        // G6b: A_PRODUCTION, queued_count[0] == 0 -- override does NOT fire (queued_count fails).
        {
            Seed s;
            s.roster_type         = BUILDING_TYPE_A_PRODUCTION;
            s.queued_count0       = 0;
            s.bldg_energy         = 10.0;
            s.cap_energy          = 10.0;
            s.state_transition_lo = 0x1111;
            seed_and_run(fx, s);
            ck_eq((uint32_t)fx.b(s.player, s.index).state, 0x1111u,
                  "G6b @0x472847 JZ 0x472854: A_PRODUCTION but queued_count[0]==0 -- override does NOT fire, "
                  "state stays the state_transition_ids value (proves the AND, type alone is not enough)");
        }
        // G6c: H_PRODUCTION, queued_count[0] != 0 -- OVERRIDE fires (the OTHER production type id).
        {
            Seed s;
            s.roster_type         = BUILDING_TYPE_H_PRODUCTION;
            s.queued_count0       = 1;
            s.bldg_energy         = 10.0;
            s.cap_energy          = 10.0;
            s.state_transition_lo = 0x2222;
            seed_and_run(fx, s);
            ck_eq((uint32_t)fx.b(s.player, s.index).state, (uint32_t)ST_PROD_PICK_NEXT,
                  "G6c @0x4727e4-0x47281f: H_PRODUCTION(0x15) takes the same override arm as A_PRODUCTION(0x01)");
        }
        // G6d: H_PRODUCTION, queued_count[0] == 0 -- override does NOT fire.
        {
            Seed s;
            s.roster_type         = BUILDING_TYPE_H_PRODUCTION;
            s.queued_count0       = 0;
            s.bldg_energy         = 10.0;
            s.cap_energy          = 10.0;
            s.state_transition_lo = 0x2222;
            seed_and_run(fx, s);
            ck_eq((uint32_t)fx.b(s.player, s.index).state, 0x2222u,
                  "G6d: H_PRODUCTION but queued_count[0]==0 -- override does NOT fire");
        }
        // G6e: neither production type, queued_count[0] != 0 -- override does NOT fire (type alone,
        // even with a nonzero queue, is not enough -- proves the AND from the OTHER side of G6b/d).
        {
            Seed s;
            s.roster_type         = 0x7F; // neither A_PRODUCTION(0x01) nor H_PRODUCTION(0x15)
            s.queued_count0       = 1;
            s.bldg_energy         = 10.0;
            s.cap_energy          = 10.0;
            s.state_transition_lo = 0x3333;
            seed_and_run(fx, s);
            ck_eq((uint32_t)fx.b(s.player, s.index).state, 0x3333u,
                  "G6e @0x4727e4/0x472816 JNZ 0x472854: non-production type -- the type check fails before "
                  "queued_count is even relevant, override does NOT fire despite queued_count[0]!=0 -- "
                  "proves the AND, queued_count alone is not enough");
        }
        // G6f: neither production type, queued_count[0] == 0 -- override does NOT fire (both conditions
        // false, the "nothing" case).
        {
            Seed s;
            s.roster_type         = 0x7F;
            s.queued_count0       = 0;
            s.bldg_energy         = 10.0;
            s.cap_energy          = 10.0;
            s.state_transition_lo = 0x4444;
            seed_and_run(fx, s);
            ck_eq((uint32_t)fx.b(s.player, s.index).state, 0x4444u,
                  "G6f: neither condition holds -- override does NOT fire (the negative/all-false case)");
        }
    }

    // =================================================================================================
    // G7 -- the PROD_PICK_NEXT override applies on the PAYMENT-SUCCEEDED arm too, not just the
    // energy>=cap arm G6 exercised (both call the same charge_gate_complete_cycle helper).
    // =================================================================================================
    {
        Seed s;
        s.bldg_energy          = 1.0; // below cap -- goes through pay_cycle_inputs
        s.cap_energy           = 10.0;
        s.pay_cycle_inputs_ret = 3;
        s.player_side          = 5; // other player -- isolates the override from the message path
        s.roster_type          = BUILDING_TYPE_A_PRODUCTION;
        s.queued_count0        = 1;
        s.state_transition_lo  = 0x5555;
        seed_and_run(fx, s);
        ck_eq((uint32_t)fx.b(s.player, s.index).state, (uint32_t)ST_PROD_PICK_NEXT,
              "G7: PROD_PICK_NEXT override also fires on the payment-succeeded arm's shared tail, "
              "not only the energy>=cap arm");
    }

    // =================================================================================================
    // G8 -- completion_dispatch's param_3/param_4 are ALWAYS 0, cross-checked with production-type
    // fixture state and a nonzero queued_count active at the same time (nothing in the fixture can
    // make them nonzero -- there is no live register for them to come from per the header's RESOLVED
    // note).
    // =================================================================================================
    {
        Seed s;
        s.bldg_energy   = 10.0;
        s.cap_energy    = 10.0;
        s.roster_type   = BUILDING_TYPE_H_PRODUCTION;
        s.queued_count0 = 1;
        s.game_clock    = 4242.5;
        seed_and_run(fx, s);
        ck(g_dispatch_calls.size() == 1 && g_dispatch_calls[0].param_3 == 0 && g_dispatch_calls[0].param_4 == 0,
           "G8: completion_dispatch param_3=0/param_4=0 regardless of the production/queued_count fixture state");
        ck_eq_d(g_dispatch_calls[0].clock, s.game_clock,
                "G8: completion_dispatch param_5 tracks *game_clock exactly, independent of param_3/param_4");
    }
}

} // namespace mh::sim::test
