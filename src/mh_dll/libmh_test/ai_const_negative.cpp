//
// ai_const_negative.cpp -- THE NEGATIVE COMPILE TEST for the AI const state view (RI-AI / AI0).
//
// AI0's acceptance asks that "a write attempt through the view FAILS TO COMPILE, proven by a
// negative test that is checked in and demonstrated to fail (not merely asserted)". This is that
// test, and `python tools/check_const_view.py` is what demonstrates it: the driver compiles this
// one translation unit once per case with the case's macro defined, requires each of those to
// FAIL, and then compiles it with NO macro defined and requires that to SUCCEED.
//
// THE LAST HALF IS THE ONE WITH TEETH. A file that fails to compile for any reason at all would
// pass a naive "did it fail?" check -- a typo, a missing header, a renamed field would all look
// like the architecture rule holding. So the driver also greps each failure for the SPECIFIC
// diagnostic that names a const violation, and the no-macro build proves the surrounding code is
// otherwise sound. That is the mutation check for this test, and it lives in the driver rather than
// here because it is a property of the pair, not of either arm.
//
// WHAT EACH CASE IS ABOUT:
//   1 -- the roster. `units` exists ONLY in ai_view, so R2 ("the AI writes orders, not sim state")
//        is a type error rather than a lint finding. This is the load-bearing case.
//   2 -- the other roster, same argument, because `buildings` is reached by a different accessor
//        and a hole in one is not a hole in the other.
//   3 -- player_data THROUGH THE READ VIEW. The AI may write its own store, but only through
//        ai_store, so a write must name `own` and is therefore greppable and countable.
//   4 -- the shared transient. Same argument as 3.
//   5 -- a NESTED record array reached through the view. Const has to survive
//        `players[i].ai_target_list[j].field`, which is where most AI read traffic lands; a scalar
//        field holding does not by itself prove an array of structs does.
//   6-8 -- AI1D's NEGATIVE ARM (2026-08-28), and a DIFFERENT rule from 1-5. Batch D writes seven
//        named fields inside the rosters, so the store gained `ai_roster_window` -- a field-scoped
//        hole. What has to be proven is that the hole is field-scoped and not record-scoped: an AI
//        write to a roster field NOT on the list must fail to compile. It does, but by ACCESS
//        CONTROL (the window's roster bases are private), not by const -- so these three carry
//        `// EXPECT access` and a failure with a const diagnostic is a broken test, not a pass.
//
// Cases are mutually exclusive on purpose (one macro per compile) so a failure can only be
// attributed to the construct it names.
//
#include <cstdint>

#include "ai/ai_state.h"

namespace {

// A view and a store bound to nothing: this file is never linked or run, only compiled. Using
// real buffers would add failure modes that have nothing to do with what is under test.
mh::ai::ai_view  g_view{};
mh::ai::ai_store g_store{};

void body() {
#if defined(MH_CONST_NEGATIVE_CASE_1)
    // R2: the AI must not write the unit roster.
    mh::ai::unit_of(g_view, 0, 0).energy = 0.0;
#elif defined(MH_CONST_NEGATIVE_CASE_2)
    // R2: nor the building roster.
    mh::ai::building_of(g_view, 0, 0).energy = 0.0;
#elif defined(MH_CONST_NEGATIVE_CASE_3)
    // The AI's own store is writable -- but not through the READ view.
    g_view.players[0].ai_turret_candidate = 7;
#elif defined(MH_CONST_NEGATIVE_CASE_4)
    // Same for the shared engage-candidate transient.
    g_view.engage_scratch[0].target_ref = 0;
#elif defined(MH_CONST_NEGATIVE_CASE_5)
    // Const has to survive a NESTED record array, not just a scalar field -- ai_target_list is
    // where most of the AI's read traffic actually lands.
    g_view.players[0].ai_target_list[0].victim_index = 0;
#elif defined(MH_CONST_NEGATIVE_CASE_6) // EXPECT access
    // ---- AI1D's NEGATIVE ARM (2026-08-28) ----
    // The batch-D roster window is FIELD-SCOPED, and this is the case that says so rather than
    // asserting it: `energy` is a roster field that is NOT on the window's list, and the only way to
    // reach it through the window is its private roster base. Access control, so C2248 -- reported
    // separately from the const family on purpose, because a case that fails with the OTHER rule's
    // diagnostic is a broken test.
    g_store.roster.units_[0].energy = 0.0;
#elif defined(MH_CONST_NEGATIVE_CASE_7) // EXPECT access
    // The same for the BUILDING roster: a hole in one is not a hole in the other (case 2's argument,
    // applied to the window).
    g_store.roster.buildings_[0].energy = 0.0;
#elif defined(MH_CONST_NEGATIVE_CASE_8) // EXPECT access
    // And the field the window deliberately refuses to publish as a reference even though it DOES
    // write it: order_status_flags (+0xe3) is the ORDER subsystem's byte, reachable only through the
    // two named word operations. Naming the record to get at it is the same private base again.
    g_store.roster.units_[0].order_status_flags = 0x40u;
#else
    // THE POSITIVE ARM. Every read below is legal and every write goes through `own`; if this does
    // not compile, the negative cases above prove nothing.
    const mh::ai::unit     &u              = mh::ai::unit_of(g_view, 0, 0);
    const mh::ai::building &b              = mh::ai::building_of(g_view, 0, 0);
    g_store.players[0].ai_turret_candidate = (int32_t)(u.energy + b.energy);
    g_store.engage_scratch[0].target_ref   = (uint32_t)g_view.players[0].is_alien_race;
    *g_store.engage_scratch_count          = *g_view.engage_scratch_count;
    // AI1D: the seven fields the window DOES publish must all be writable, or cases 6-8 above would
    // "pass" merely because the window is broken.
    g_store.roster.ai_group_next(0, 1)               = 2;
    g_store.roster.ai_group_prev(0, 1)               = 0;
    g_store.roster.ai_group_index(0, 1)              = 0xffffu;
    g_store.roster.incoming_threat_damage(0, 1)      = 3;
    g_store.roster.committed_weapon_damage_est(0, 1) = 4;
    g_store.roster.bldg_incoming_damage_tally(0, 1)  = 5;
    g_store.roster.engage_commit_set(0, 1);
    g_store.roster.engage_status_word_clear(0, 1);
    *g_store.map_width_mask  = 0xff;
    *g_store.map_height_mask = 0xff;
#endif
}

} // namespace

// Referenced so the TU is not empty under /W4; never called.
void mh_ai_const_negative_anchor() { body(); }
