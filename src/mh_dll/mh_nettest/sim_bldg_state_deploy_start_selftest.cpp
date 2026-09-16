//
// sim_bldg_state_deploy_start_selftest.cpp -- `simtest` cases for
// mh::sim::detail::bldg_state_deploy_start (sim/sim_bldg_state_deploy.h/.cpp), the
// llm_strat_bldg_state_deploy_start @0x00471377 building-state handler.
//
// EXPECTED BEHAVIOUR HAND-DERIVED FROM THE DISASSEMBLY
// (tmp/decomp_sim/llm_strat_bldg_state_deploy_start_00471377.asm), NOT read off the .cpp body:
//
//   0x0047138f-0x004713a4: type = Building[cur_building->building_id].type, read ONCE.
//   0x004713a7-0x004713c1: four-way dispatch on `type`:
//     type <  0xd (0x004713bb), == 0x6  (JZ @0x004713bf taken) -> A_MOTHER liftoff arm.
//     type <  0xd (0x004713bb), != 0x6  (JMP @0x004713c1)      -> default (TO_UNIT) arm.
//     type == 0xd (JBE @0x004713b1 taken, -> 0x004713c3)       -> A_SHUTTLE liftoff arm.
//     type >  0xd (0x004713b3), == 0x1a (JZ @0x004713b7 taken) -> H_MOTHER liftoff arm (same
//                                                                  target, 0x004713ef, as A_MOTHER).
//     type >  0xd (0x004713b3), != 0x1a (JMP @0x004713b9)      -> default (TO_UNIT) arm.
//   Shuttle arm (0x004713c3-0x004713ed): PUSH GAME_CLOCK+4 (high dword, @0x004713c3), PUSH
//     GAME_CLOCK (low dword, @0x004713c9), MOVZX EDX=CUR_INDEX (@0x004713cf), MOVZX EAX=CUR_PLAYER
//     (@0x004713d6) -- FRESH globals, NOT this function's own param_1/param_2 -- CALL
//     llm_strat_bldg_start_liftoff_anim_shuttle (@0x004713dd, EBX/ECX = this function's own
//     param_3/param_4, untouched since entry); state = DEPLOY_ANIM_WAIT(0x84) (@0x004713e7).
//   Mother arm (0x004713ef-0x00471419): identical shape -- PUSH high (@0x004713ef), PUSH low
//     (@0x004713f5), MOVZX EDX/EAX (@0x004713fb/0x00471402), CALL
//     llm_strat_bldg_start_liftoff_anim_mother (@0x00471409); state = DEPLOY_ANIM_WAIT(0x84)
//     (@0x00471413).
//   Default arm (0x0047141b-0x00471420): state = TO_UNIT(0x7c), NO liftoff call at all.
//   0x00471426-0x00471434: llm_strat_bldg_notify_ui(cur_player, cur_index), UNCONDITIONAL on every
//     path (MOVZX EDX=CUR_INDEX @0x00471426, MOVZX EAX=CUR_PLAYER @0x0047142d, CALL @0x00471434) --
//     this is the header's correction: a prior hazard note wrongly claimed neither land_activate NOR
//     deploy_start calls notify_ui; deploy_start plainly does, on EVERY arm including the plain-type
//     default. Covered explicitly below (T4a/T4b) so a regression that guards the call on the
//     liftoff branch would be caught.
//   GAME_CLOCK split (see sim_bldg_state_deploy.h's own derivation): the two PUSHes read the SAME
//     8 raw bytes of GAME_CLOCK; the value pushed LAST (GAME_CLOCK's own low dword, offset +0) is the
//     5th thunk arg (param_5), the value pushed FIRST (GAME_CLOCK+4, high dword) is the 6th
//     (param_6). This file re-derives the expected lo/hi from the fixture's raw double via its OWN
//     memcpy split (split_clock_for_check below), independent of sim_bldg_state_deploy.cpp's
//     split_game_clock, and compares the two uint32_t's directly -- never reconstructing a double
//     from param_5/param_6 and comparing doubles (a bug that swapped lo/hi would still show as two
//     equal doubles if compared that way for a palindromic bit pattern; comparing the raw dwords
//     against an independently-seeded, non-symmetric lo/hi rules that out).
//
#include "sim/sim_bldg_state_deploy.h"

#include <cstdint>
#include <cstring>
#include <vector>

#include "sim/sim_order_enqueue.h" // BUILDING_TYPE_A_MOTHER/H_MOTHER/A_SHUTTLE
#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// ---- shared trace: proves CALL ORDER (liftoff call, if any, THEN notify_ui) --------------------
std::vector<const char *> g_trace;
void                      tr(const char *tag) { g_trace.push_back(tag); }

bool trace_eq(const std::vector<const char *> &want) {
    if (g_trace.size() != want.size()) return false;
    for (size_t i = 0; i < want.size(); ++i)
        if (std::strcmp(g_trace[i], want[i]) != 0) return false;
    return true;
}

// ---- per-callee recorders (3, one per bldg_state_deploy_start_calls member) --------------------
struct LiftoffCall {
    uint32_t param_1; // expected: FRESH cur_player, NOT this function's own dead param_1
    int32_t  param_2; // expected: FRESH cur_index, NOT this function's own dead param_2
    uint32_t param_3; // expected: forwarded verbatim
    uint32_t param_4; // expected: forwarded verbatim
    uint32_t param_5; // expected: GAME_CLOCK low dword
    uint32_t param_6; // expected: GAME_CLOCK high dword
};
std::vector<LiftoffCall> g_mother_calls;
std::vector<LiftoffCall> g_shuttle_calls;

void rec_liftoff_mother(uint32_t p1, int32_t p2, uint32_t p3, uint32_t p4, uint32_t p5, uint32_t p6) {
    tr("liftoff_mother");
    g_mother_calls.push_back({p1, p2, p3, p4, p5, p6});
}
void rec_liftoff_shuttle(uint32_t p1, int32_t p2, uint32_t p3, uint32_t p4, uint32_t p5, uint32_t p6) {
    tr("liftoff_shuttle");
    g_shuttle_calls.push_back({p1, p2, p3, p4, p5, p6});
}

struct NotifyCall {
    uint16_t player;
    uint32_t index;
};
std::vector<NotifyCall> g_notify_calls;
void                    rec_notify_ui(uint16_t player, uint32_t index) {
    tr("notify_ui");
    g_notify_calls.push_back({player, index});
}

const bldg_state_deploy_start_calls g_calls = {
    &rec_liftoff_mother,
    &rec_liftoff_shuttle,
    &rec_notify_ui,
};

void reset_observations() {
    g_trace.clear();
    g_mother_calls.clear();
    g_shuttle_calls.clear();
    g_notify_calls.clear();
}

// Independent test-side re-derivation of the GAME_CLOCK raw-dword split (see the file banner) --
// deliberately NOT a call into sim_bldg_state_deploy.cpp's own split_game_clock, so a bug in that
// production helper cannot cancel itself out against this check.
void split_clock_for_check(double clock, uint32_t &lo, uint32_t &hi) {
    uint64_t bits;
    std::memcpy(&bits, &clock, sizeof(bits));
    lo = static_cast<uint32_t>(bits);
    hi = static_cast<uint32_t>(bits >> 32);
}

// Builds a double whose raw low/high dwords are exactly `lo`/`hi` -- the inverse of the split
// above, used only to SEED the fixture (never to reconstruct-and-compare a double).
double make_clock(uint32_t lo, uint32_t hi) {
    uint64_t bits = (static_cast<uint64_t>(hi) << 32) | lo;
    double   d;
    std::memcpy(&d, &bits, sizeof(d));
    return d;
}

// ---- fixture seeding -----------------------------------------------------------------------------
// All fields DISTINCT and non-symmetric (per sim_test_support.h's own banner) so a swapped-arg or
// swapped-half translation disagrees with the fixture instead of accidentally matching it.
struct Seed {
    uint16_t player  = 3;
    uint16_t index   = 9;
    uint16_t cfg_row = 42; // building_id
    uint8_t  type    = 0;  // set per case

    // Dead register carriers (EAX/EDX, clobbered before ever being read -- see the header's hazard
    // note) -- distinct from player/index AND from each other so a mistaken forward would be caught.
    uint32_t param_1 = 0xDEAD0001u;
    uint32_t param_2 = 0xDEAD0002u;

    // EBX/ECX -- untouched since entry, forwarded verbatim into whichever liftoff callee fires.
    uint32_t param_3 = 0x9111A222u;
    uint32_t param_4 = 0x9333B444u;

    // GAME_CLOCK's raw low/high dwords -- individually distinguishable (neither zero, neither equal
    // to the other, neither a byte-rotation of the other) so a lo/hi swap is observable.
    uint32_t clock_lo = 0x11223344u;
    uint32_t clock_hi = 0x55667788u;
};

void seed_and_run(sim_fixture &fx, const Seed &s) {
    fx.reset();

    building &b   = fx.b(static_cast<int32_t>(s.player), static_cast<int32_t>(s.index));
    b.building_id = s.cfg_row;
    b.state       = static_cast<uint16_t>(0xBEEF); // sentinel, distinct from TO_UNIT(0x7c)/DEPLOY_ANIM_WAIT(0x84)

    fx.cur_building_ptr = &b;
    fx.view_cur_player  = s.player;
    fx.view_cur_index   = s.index;

    fx.cfg_buildings[s.cfg_row].type = s.type;
    fx.game_clock                    = make_clock(s.clock_lo, s.clock_hi);

    reset_observations();

    sim_view  v   = fx.view();
    sim_store own = fx.store();
    detail::bldg_state_deploy_start(v, own, s.param_1, s.param_2, s.param_3, s.param_4, g_calls);
}

} // namespace

void run_bldg_state_deploy_start_tests() {
    sim_fixture fx;

    // =================================================================================================
    // T1 -- A_MOTHER (0x6): dispatch decided @0x004713bb/0x004713bf (JZ taken). liftoff_mother fires
    // with FRESH cur_player/cur_index + forwarded param_3/param_4 + the GAME_CLOCK dword split;
    // liftoff_shuttle does NOT fire; state = DEPLOY_ANIM_WAIT; notify_ui fires last.
    // =================================================================================================
    {
        Seed s;
        s.type = BUILDING_TYPE_A_MOTHER; // 0x06
        seed_and_run(fx, s);

        ck(trace_eq({"liftoff_mother", "notify_ui"}),
           "T1: A_MOTHER -- liftoff_mother (@0x00471409) then notify_ui (@0x00471434), nothing else");
        ck(g_shuttle_calls.empty(), "T1: liftoff_shuttle is NEVER called on the mother arm");
        ck(g_mother_calls.size() == 1, "T1: liftoff_mother called exactly once");
        if (g_mother_calls.size() == 1) {
            const auto &c = g_mother_calls[0];
            ck(c.param_1 == s.player && c.param_2 == static_cast<int32_t>(s.index),
               "T1 @0x004713fb/0x00471402: liftoff_mother gets FRESH cur_player/cur_index, not this "
               "function's dead param_1/param_2");
            ck(c.param_3 == s.param_3 && c.param_4 == s.param_4,
               "T1 @0x00471409: liftoff_mother's param_3/param_4 forwarded verbatim (EBX/ECX untouched "
               "since entry)");
            uint32_t want_lo, want_hi;
            split_clock_for_check(fx.game_clock, want_lo, want_hi);
            ck_eq(c.param_5, want_lo,
                  "T1 @0x004713f5 (PUSH GAME_CLOCK, pushed last): liftoff_mother param_5 = GAME_CLOCK "
                  "LOW dword");
            ck_eq(c.param_6, want_hi,
                  "T1 @0x004713ef (PUSH GAME_CLOCK+4, pushed first): liftoff_mother param_6 = "
                  "GAME_CLOCK HIGH dword");
        }
        ck_eq(static_cast<uint32_t>(fx.b(s.player, s.index).state),
              static_cast<uint32_t>(BLDG_STATE_DEPLOY_ANIM_WAIT),
              "T1 @0x00471413: state = DEPLOY_ANIM_WAIT(0x84) on the mother arm");
        ck(g_notify_calls.size() == 1 && g_notify_calls[0].player == s.player &&
               g_notify_calls[0].index == s.index,
           "T1 @0x00471426-0x00471434: notify_ui(cur_player, cur_index) fires, mother arm");
    }

    // =================================================================================================
    // T2 -- H_MOTHER (0x1a): dispatch decided @0x004713b3/0x004713b7 (JZ taken), a DIFFERENT compare
    // than T1's but the SAME target arm (0x004713ef) -- must produce identical effects to T1.
    // =================================================================================================
    {
        Seed s;
        s.type = BUILDING_TYPE_H_MOTHER; // 0x1a
        seed_and_run(fx, s);

        ck(trace_eq({"liftoff_mother", "notify_ui"}),
           "T2: H_MOTHER -- takes the SAME mother-liftoff arm as A_MOTHER (0x004713ef)");
        ck(g_mother_calls.size() == 1 && g_shuttle_calls.empty(),
           "T2: liftoff_mother fires, liftoff_shuttle does not");
        if (g_mother_calls.size() == 1) {
            const auto &c = g_mother_calls[0];
            ck(c.param_3 == s.param_3 && c.param_4 == s.param_4,
               "T2 @0x00471409: H_MOTHER arm also forwards param_3/param_4 verbatim");
        }
        ck_eq(static_cast<uint32_t>(fx.b(s.player, s.index).state),
              static_cast<uint32_t>(BLDG_STATE_DEPLOY_ANIM_WAIT),
              "T2 @0x00471413: state = DEPLOY_ANIM_WAIT(0x84) on the H_MOTHER arm too");
        ck(g_notify_calls.size() == 1, "T2 @0x00471434: notify_ui fires, H_MOTHER arm");
    }

    // =================================================================================================
    // T3 -- A_SHUTTLE (0xd): dispatch decided @0x004713ad/0x004713b1 (JBE taken). liftoff_shuttle
    // fires (NOT liftoff_mother), same forwarding/state/notify contract as the mother arms.
    // =================================================================================================
    {
        Seed s;
        s.type = BUILDING_TYPE_A_SHUTTLE; // 0x0d
        seed_and_run(fx, s);

        ck(trace_eq({"liftoff_shuttle", "notify_ui"}),
           "T3: A_SHUTTLE -- liftoff_shuttle (@0x004713dd) then notify_ui (@0x00471434), nothing else");
        ck(g_mother_calls.empty(), "T3: liftoff_mother is NEVER called on the shuttle arm");
        ck(g_shuttle_calls.size() == 1, "T3: liftoff_shuttle called exactly once");
        if (g_shuttle_calls.size() == 1) {
            const auto &c = g_shuttle_calls[0];
            ck(c.param_1 == s.player && c.param_2 == static_cast<int32_t>(s.index),
               "T3 @0x004713cf/0x004713d6: liftoff_shuttle gets FRESH cur_player/cur_index");
            ck(c.param_3 == s.param_3 && c.param_4 == s.param_4,
               "T3 @0x004713dd: liftoff_shuttle's param_3/param_4 forwarded verbatim");
            uint32_t want_lo, want_hi;
            split_clock_for_check(fx.game_clock, want_lo, want_hi);
            ck_eq(c.param_5, want_lo,
                  "T3 @0x004713c9 (PUSH GAME_CLOCK, pushed last): liftoff_shuttle param_5 = GAME_CLOCK "
                  "LOW dword");
            ck_eq(c.param_6, want_hi,
                  "T3 @0x004713c3 (PUSH GAME_CLOCK+4, pushed first): liftoff_shuttle param_6 = "
                  "GAME_CLOCK HIGH dword");
        }
        ck_eq(static_cast<uint32_t>(fx.b(s.player, s.index).state),
              static_cast<uint32_t>(BLDG_STATE_DEPLOY_ANIM_WAIT),
              "T3 @0x004713e7: state = DEPLOY_ANIM_WAIT(0x84) on the shuttle arm");
        ck(g_notify_calls.size() == 1, "T3 @0x00471434: notify_ui fires, shuttle arm");
    }

    // =================================================================================================
    // T4a -- plain type, type < 0xd and != 0x6 (dispatch decided @0x004713bb/0x004713c1, JMP taken):
    // NEITHER liftoff function is called; state = TO_UNIT(0x7c); notify_ui STILL fires (the header's
    // correction -- a prior hazard note wrongly claimed neither land_activate nor deploy_start calls
    // notify_ui at all; this default arm is exactly the case that note would get wrong).
    // =================================================================================================
    {
        Seed s;
        s.type = 5; // < 0xd, != BUILDING_TYPE_A_MOTHER(0x6)
        seed_and_run(fx, s);

        ck(trace_eq({"notify_ui"}),
           "T4a: plain type (<0xd, !=6) -- ONLY notify_ui fires, no liftoff call at all");
        ck(g_mother_calls.empty() && g_shuttle_calls.empty(),
           "T4a: neither liftoff_mother nor liftoff_shuttle is called on the default arm");
        ck_eq(static_cast<uint32_t>(fx.b(s.player, s.index).state), static_cast<uint32_t>(BLDG_STATE_TO_UNIT),
              "T4a @0x00471420: state = TO_UNIT(0x7c) on the default arm");
        ck(g_notify_calls.size() == 1 && g_notify_calls[0].player == s.player &&
               g_notify_calls[0].index == s.index,
           "T4a @0x00471426-0x00471434: notify_ui(cur_player, cur_index) fires UNCONDITIONALLY even "
           "with no liftoff call -- the corrected hazard-note claim");
    }

    // =================================================================================================
    // T4b -- plain type, type > 0xd and != 0x1a (dispatch decided @0x004713b3/0x004713b9, JMP taken):
    // the OTHER false branch of the four-way dispatch -- same default-arm contract as T4a.
    // =================================================================================================
    {
        Seed s;
        s.type = 0x20; // > 0xd, != BUILDING_TYPE_H_MOTHER(0x1a)
        seed_and_run(fx, s);

        ck(trace_eq({"notify_ui"}),
           "T4b: plain type (>0xd, !=0x1a) -- ONLY notify_ui fires, no liftoff call at all");
        ck_eq(static_cast<uint32_t>(fx.b(s.player, s.index).state), static_cast<uint32_t>(BLDG_STATE_TO_UNIT),
              "T4b @0x00471420: state = TO_UNIT(0x7c) on this default arm too");
        ck(g_notify_calls.size() == 1, "T4b @0x00471434: notify_ui still fires");
    }
}

} // namespace mh::sim::test
