//
// sim_unit_estimate_weapon_damage_selftest.cpp -- `simtest` cases for
//   llm_strat_unit_estimate_weapon_damage @0x004d32aa (sim/sim_weapon_damage_calc.cpp)
//
// EVERY EXPECTED VALUE BELOW WAS DERIVED FROM THE RAW DISASSEMBLY,
// tmp/decomp_sim/llm_strat_unit_estimate_weapon_damage_004d32aa.asm, NOT from the .cpp under test or
// its header banner. Instruction addresses are cited inline next to the assertion or in the case's
// comment.
//
// =====================================================================================================
// THE THREE INPUT REGIONS -- READ BEFORE ADDING A CASE
// =====================================================================================================
// This block used to be headed "OUT OF SCOPE (G26)" and declared region (C) below untestable. That is
// no longer true: the divergence was CLOSED on 2026-08-16 and all three regions are in scope. The
// history is worth keeping, because the same claim was wrong three times in three different ways.
//
// THE MECHANISM. The stack slot [EBP-0x10] (target_owner) is written at EXACTLY ONE site
// (0x004d32d3-0x004d32d8: MOV EAX,EBX / AND EAX,0xf / MOV [EBP-0x10],EAX) gated on
// `TEST byte ptr [EBP-0x14],0xa0` (0x004d32cd). The armor-scaling branch does NOT recompute the
// nibble -- it RE-READS that slot twice (0x004d33d8, 0x004d33fa) under the INDEPENDENT gate
// `TEST byte ptr [EBP-0x14],0x40` (0x004d33d2/0x004d33d6), whose JNZ SKIPS the block, so the block
// runs when 0x40 is CLEAR. When (target_ref & 0xe0) == 0 the slot was never written this call.
//
// WHAT IT READS THEN, settled 2026-08-16: not residue -- the Watcom stack-probe imprint. `PUSH 0x38;
// CALL utils_assert_stack_capacity` runs AFTER `PUSH EBP`, so the probe's frame overlaps these locals
// and its `PUSH EAX` at 0x004cf47f (EAX = the 0x38 size arg) lands on exactly [EBP-0x10]; only ESI/EDI
// are pushed afterwards. 0x38 == 56, on every call, measured under cdb.
// The .cpp reproduces this (see STACK_PROBE_OWNER_IMPRINT and the slot model), so it is EQUIVALENT
// here and region (C) is testable. The region-(C) cases at the bottom of this file pin it, and both
// mutations -- imprint 0x38 -> 0, and reverting to `target_ref & 0xf` -- are CAUGHT.
//
// WHAT THE REGION IS NOT: a hot path. The earlier claim that llm_strat_ai_commit_attack_order/_alt
// "pass `target_ref & 0x0f` at their two live call sites" was wrong -- that nibble is the CALLER's own
// [EBP-0x10] local, and both sites pass the unmodified ref in EBX. 443 calls
// on a shape-valid all-AI soak produced 0 region-(C) entries. Reproduced because it is cheap and
// correct, not because it is frequent.
//
// THE THREE REGIONS. A case's target_ref lands in exactly one; state which in the case's comment.
//   (A) (target_ref & 0xa0) != 0  -- the slot IS validly written this call, so BOTH sub-cases are
//       safe regardless of the 0x40 bit: elevation classification runs off the fresh value, and if
//       0x40 is also clear the armor branch reads the SAME fresh value it just wrote.
//   (B) (target_ref & 0xa0) == 0 AND (target_ref & 0x40) != 0 -- the slot is never written, but the
//       armor branch is also never entered (0x40 gate shuts it), so the slot is never read either.
//       Elevation classification stays at its initial false (0x004d32c6's `MOV [EBP-0xc],0`).
//   (C) (target_ref & 0xe0) == 0 -- the slot is never written AND the armor branch IS entered, so both
//       of its reads see the imprint, 56. Needs sim_fixture::wide_u() to make roster row 56 real;
//       offline that index is past the fixture's roster, and in the live image it is simply more .bss.
//
#include "sim/sim_weapon_damage_calc.h"

#include "sim_test_support.h"

#include <cstddef> // offsetof -- armor_slot() addresses armor_prob[owner] for owner >= 9

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// ---- the cast --------------------------------------------------------------------------------------
// Distinct, non-zero, non-symmetric -- so an index swap (attacker vs. target, player vs. owner) does
// not accidentally read the same slot on both sides.
constexpr uint32_t ATTACKER_P   = 3;
constexpr int32_t  ATTACKER_I   = 5;
constexpr uint32_t TARGET_OWNER = 6; // must fit the low nibble (target_ref & 0xf)
constexpr int32_t  TARGET_I     = 9;
constexpr uint16_t TARGET_PROTO = 44; // cfg_units row id for the TARGET's unit type

// target_ref shapes, one per region named in the OUT-OF-SCOPE block above. All carry TARGET_OWNER
// (6 = 0b0110) in the low nibble.
constexpr uint32_t REF_A1 = 0x86u; // 1000_0110: 0xa0 bit SET (0x80), 0x40 bit CLEAR -- region (A),
                                   // armor branch runs off the freshly-written slot.
constexpr uint32_t REF_A2 = 0xE6u; // 1110_0110: 0xa0 bit SET (0xa0), 0x40 bit SET -- region (A),
                                   // armor branch skipped (0x004d33d2 TEST/JNZ fires).
constexpr uint32_t REF_B = 0x46u;  // 0100_0110: 0xa0 bits CLEAR, 0x40 bit SET -- region (B), gate1
                                   // never fires (classification defaults false) AND the armor branch
                                   // is shut, so the never-written slot is never read either.

// ---- seeding helpers --------------------------------------------------------------------------------

// unit::weapons[slot].weapon_id (0x004d3396/0x004d332e's byte read) and cfg_weapons[weapon_id].target
// (0x004d334e's TEST, weapon base +1) / .power[player] (0x004d33b7's FADD, weapon base +0x92).
void seed_attacker_weapon(sim_fixture &f, uint32_t player, int32_t unit_index, int32_t slot,
                          uint8_t weapon_id, uint8_t target_mask, double power_for_player) {
    f.u((int32_t)player, unit_index).weapons[slot].weapon_id = weapon_id;
    f.cfg_weapons[weapon_id].target                          = target_mask;
    f.cfg_weapons[weapon_id].power[player]                   = power_for_player;
}

// unit::unit_proto_id (the target roster slot's proto, read at 0x004d32e7 for classification and
// again -- via the SAME re-read stack slot, see the G26 block above -- for the armor lookup) and
// cfg_units[proto].type (0x004d32fb's CMP).
void seed_target_unit(sim_fixture &f, uint32_t owner, int32_t index, uint16_t proto, uint32_t type) {
    f.u((int32_t)owner, index).unit_proto_id = proto;
    f.cfg_units[proto].type                  = type;
}

// cfg_units[proto].armor_prob[9] (0x004d33fd's PUSH, cfg_units_base + 0xb5 + owner*4), seeded with a
// DISTINCT value per index (100 + 11*i) so indexing by the wrong owner/player is visible rather than
// accidentally matching.
void seed_armor_row(sim_fixture &f, uint16_t proto) {
    for (int32_t i = 0; i < 9; ++i) f.cfg_units[proto].armor_prob[i] = 100 + 11 * i;
}

// ---- the scale_pct recorder/knob -------------------------------------------------------------------

struct ev_scale {
    double  value;
    int32_t pct;
};
struct ewd_recorder {
    std::vector<ev_scale> scale_pct;
    double                scale_pct_return = 0.0; // what the stub hands back on ST0
    void                  reset() { *this = ewd_recorder{}; }
};
ewd_recorder g_ewd;

ev_scale scale_at(size_t i) {
    static const ev_scale none = {-999.0, -999};
    return i < g_ewd.scale_pct.size() ? g_ewd.scale_pct[i] : none;
}

const unit_estimate_weapon_damage_calls &rec_ewd_calls() {
    static const unit_estimate_weapon_damage_calls c = {
        [](double value, int32_t pct) -> double {
            g_ewd.scale_pct.push_back({value, pct});
            return g_ewd.scale_pct_return;
        },
    };
    return c;
}

// Identity-return variant: the real llm_math_scale_pct is value*pct/100, but for a case that only
// cares about WHICH row was indexed / whether the running total flowed through, an identity keeps the
// expectation exact binary arithmetic (same trick sim_weapon_damage_selftest.cpp's rec_kc_calls uses).
const unit_estimate_weapon_damage_calls &rec_ewd_calls_identity() {
    static const unit_estimate_weapon_damage_calls c = {
        [](double value, int32_t pct) -> double {
            g_ewd.scale_pct.push_back({value, pct});
            return value;
        },
    };
    return c;
}

} // namespace

// ==== elevation classification -- the target-ref-gated SETG, 0x004d32cd-0x004d3308 ==================
//
// 0x004d32cd  TEST byte ptr [EBP-0x14],0xa0 ; JZ 0x004d330b        -- gate1: skip entirely if clear
// 0x004d32d3  MOV EAX,EBX / AND EAX,0xf / MOV [EBP-0x10],EAX       -- target_owner = target_ref & 0xf
// 0x004d32db-0x004d32ef  MOVZX word [target_owner*0x5b04 + target_index*0xe9 + 0xdd8c4a]
//                        -- target_unit.unit_proto_id
// 0x004d32ef-0x004d32fb  proto_id * 0x23f (cfg_unit record stride, sizeof(cfg_unit)==0x23f)
// 0x004d32fb  CMP dword ptr [EAX+0xe4a176],0xe ; SETG AL           -- is_elevated = type > 0xe, SIGNED
// 0x004d3308  MOV [EBP-0xc],EAX                                    -- stash the flag

void test_type_exactly_0xe_is_not_elevated_boundary() {
    sim_fixture f;
    g_ewd.reset();
    seed_target_unit(f, TARGET_OWNER, TARGET_I, TARGET_PROTO, 0xe); // == boundary, NOT >
    seed_armor_row(f, TARGET_PROTO);
    // is_elevated==false -> required_bit = WEAPON_TARGET_GROUND (0x004d3356-0x004d337a's arm).
    seed_attacker_weapon(f, ATTACKER_P, ATTACKER_I, 0, 7, WEAPON_TARGET_GROUND, 12.0);

    sim_view v = f.view();
    // REF_A1: region (A), armor branch runs off the freshly-written slot (see file banner).
    const uint32_t r =
        detail::unit_estimate_weapon_damage(v, (int32_t)ATTACKER_P, ATTACKER_I, REF_A1, TARGET_I,
                                            rec_ewd_calls_identity());

    ck_eq(r, 12u,
          "type==0xe: SETG(0xe > 0xe) is false -> not elevated -> the GROUND weapon (power 12) is kept "
          "(0x004d32fb CMP/SETG, exact boundary, not >=)");
}

void test_type_exactly_0xf_is_elevated_boundary() {
    sim_fixture f;
    g_ewd.reset();
    seed_target_unit(f, TARGET_OWNER, TARGET_I, TARGET_PROTO, 0xf); // one past the boundary
    seed_armor_row(f, TARGET_PROTO);
    // is_elevated==true -> required_bit = WEAPON_TARGET_AIR (0x004d333a-0x004d334e's arm). A ground
    // weapon in the SAME slot would be rejected; only the air one contributes.
    seed_attacker_weapon(f, ATTACKER_P, ATTACKER_I, 0, 7, WEAPON_TARGET_AIR, 9.0);

    sim_view       v = f.view();
    const uint32_t r =
        detail::unit_estimate_weapon_damage(v, (int32_t)ATTACKER_P, ATTACKER_I, REF_A1, TARGET_I,
                                            rec_ewd_calls_identity());

    ck_eq(r, 9u,
          "type==0xf: SETG(0xf > 0xe) is true -> elevated -> the AIR weapon (power 9) is kept "
          "(0x004d333a weapon_id read / 0x004d3347 TEST ...,0x2)");
}

// The compare at 0x004d32fb is `CMP dword ptr [...],0xe; SETG AL` -- SETG is SIGNED. `cfg_unit::type`
// is committed `uint32_t`, so a naive C++ `type > 0xeu` is an UNSIGNED compare that agrees with the
// instruction on every real value and disagrees on any value with the high bit set. That mismatch was
// live in sim_weapon_damage_calc.cpp until 2026-08-16 (its sibling llm_strat_unit_state_die_explode
// tests the SAME field against the SAME 0xe with a signed JG and had been fixed on 2026-08-14; this
// site had not). This case is what keeps the fix from being silently reverted: it drives `type` to
// 0x80000005, which is NEGATIVE as int32 (so SETG is FALSE -> not elevated -> GROUND weapon kept) and
// huge as uint32 (so the unsigned spelling would say elevated -> AIR weapon kept). The two answers are
// different weapons with different powers, so the assertion separates them.
//
// Unreachable on shipped cfg data -- every cfg_enum_E_UNIT_TYPE member is 0x00..0x18 (see the .h's
// enum table, resolved from the live DTM the same session). Tested anyway because "matching the
// instruction" is the project's standing choice, and an untested fix to an unreachable path is
// exactly the kind that gets refactored away by someone who reads the field's unsigned type.
void test_type_high_bit_set_is_a_SIGNED_compare_not_unsigned() {
    sim_fixture f;
    g_ewd.reset();
    seed_target_unit(f, TARGET_OWNER, TARGET_I, TARGET_PROTO, 0x80000005u);
    seed_armor_row(f, TARGET_PROTO);
    // Two slots, one of each class. Whichever survives the eligibility gate names the branch taken.
    seed_attacker_weapon(f, ATTACKER_P, ATTACKER_I, 0, 7, WEAPON_TARGET_GROUND, 12.0);
    seed_attacker_weapon(f, ATTACKER_P, ATTACKER_I, 1, 8, WEAPON_TARGET_AIR, 9.0);

    sim_view       v = f.view();
    const uint32_t r =
        detail::unit_estimate_weapon_damage(v, (int32_t)ATTACKER_P, ATTACKER_I, REF_A1, TARGET_I,
                                            rec_ewd_calls_identity());

    ck_eq(r, 12u,
          "type==0x80000005: SETG is SIGNED, so (negative > 0xe) is FALSE -> NOT elevated -> the "
          "GROUND weapon (power 12) is kept. An unsigned `> 0xeu` would say elevated and keep the AIR "
          "weapon (power 9) instead (0x004d32fb CMP / 0x004d3302 SETG)");
}

void test_gate1_not_fired_keeps_classification_false_even_if_target_would_be_elevated() {
    sim_fixture f;
    g_ewd.reset();
    // type=0xf would classify elevated=TRUE if the gate ran -- but REF_B's 0xa0 bits are clear, so
    // 0x004d32d1's JZ skips the whole classification block and [EBP-0xc] stays at its 0x004d32c6
    // init (0), never overwritten. Proves the flag is GATED, not "always computed then maybe ignored".
    seed_target_unit(f, TARGET_OWNER, TARGET_I, TARGET_PROTO, 0xf);
    seed_armor_row(f, TARGET_PROTO);
    // If (wrongly) elevated, the AIR weapon below would be picked and GROUND rejected -> result 20.
    // If (correctly) not elevated, the GROUND weapon is picked -> result 5.
    seed_attacker_weapon(f, ATTACKER_P, ATTACKER_I, 0, 7, WEAPON_TARGET_GROUND, 5.0);
    seed_attacker_weapon(f, ATTACKER_P, ATTACKER_I, 1, 8, WEAPON_TARGET_AIR, 20.0);

    sim_view v = f.view();
    // REF_B: region (B) -- gate1 never fires, armor branch also shut (0x40 set), so the never-written
    // slot is never read.
    const uint32_t r =
        detail::unit_estimate_weapon_damage(v, (int32_t)ATTACKER_P, ATTACKER_I, REF_B, TARGET_I,
                                            rec_ewd_calls_identity());

    ck_eq(r, 5u,
          "gate1 (target_ref & 0xa0) clear -> is_elevated stays the 0x004d32c6 init value (false) "
          "regardless of the target's real cfg type -> GROUND weapon (5) kept, AIR (20) rejected");
}

// ==== the weapon loop, 0x004d330b-0x004d33c9 =========================================================

void test_weapon_id_zero_slot_is_skipped() {
    sim_fixture f;
    g_ewd.reset();
    seed_target_unit(f, TARGET_OWNER, TARGET_I, TARGET_PROTO, 5); // <=0xe -> not elevated -> GROUND
    seed_armor_row(f, TARGET_PROTO);
    // slot 0 left at its fixture-reset weapon_id==0 (never seeded); slot 1 carries a real weapon.
    // If slot 0 were NOT skipped, cfg_weapons[0]'s zeroed .target (0) would fail the bit test anyway,
    // so seed cfg_weapons[0].power[ATTACKER_P] with a poison value that would show up in the sum if
    // the id==0 check (0x004d332e) were bypassed and this slot's power got added regardless.
    f.cfg_weapons[0].power[ATTACKER_P] = 999.0;
    seed_attacker_weapon(f, ATTACKER_P, ATTACKER_I, 1, 7, WEAPON_TARGET_GROUND, 6.0);

    sim_view       v = f.view();
    const uint32_t r =
        detail::unit_estimate_weapon_damage(v, (int32_t)ATTACKER_P, ATTACKER_I, REF_A2, TARGET_I,
                                            rec_ewd_calls_identity());

    ck_eq(r, 6u,
          "weapon_id==0 in slot 0 -> skipped (0x004d332e CMP/JZ), only slot 1's 6.0 contributes, not "
          "999.0 + 6.0");
    ck_eq((uint32_t)g_ewd.scale_pct.size(), 0u,
          "REF_A2 (0x40 bit set) -> armor branch never entered, scale_pct not called");
}

void test_ground_weapon_rejected_when_target_is_elevated() {
    sim_fixture f;
    g_ewd.reset();
    seed_target_unit(f, TARGET_OWNER, TARGET_I, TARGET_PROTO, 0x10); // > 0xe -> elevated -> AIR needed
    seed_armor_row(f, TARGET_PROTO);
    seed_attacker_weapon(f, ATTACKER_P, ATTACKER_I, 0, 7, WEAPON_TARGET_GROUND, 40.0);

    sim_view       v = f.view();
    const uint32_t r =
        detail::unit_estimate_weapon_damage(v, (int32_t)ATTACKER_P, ATTACKER_I, REF_A2, TARGET_I,
                                            rec_ewd_calls_identity());

    ck_eq(r, 1u,
          "elevated target + GROUND-only weapon -> the target-bit test at 0x004d334e/0x004d3350 "
          "rejects it (elevated arm falls to LAB_004d3350 then JNZ 0x004d33c8, skip) -> running_total "
          "stays 0 -> floor-at-1 (0x004d342b) forces the return to 1, not 40");
}

void test_air_weapon_rejected_when_target_is_not_elevated() {
    sim_fixture f;
    g_ewd.reset();
    seed_target_unit(f, TARGET_OWNER, TARGET_I, TARGET_PROTO, 3); // <=0xe -> not elevated -> GROUND needed
    seed_armor_row(f, TARGET_PROTO);
    seed_attacker_weapon(f, ATTACKER_P, ATTACKER_I, 0, 7, WEAPON_TARGET_AIR, 40.0);

    sim_view       v = f.view();
    const uint32_t r =
        detail::unit_estimate_weapon_damage(v, (int32_t)ATTACKER_P, ATTACKER_I, REF_A2, TARGET_I,
                                            rec_ewd_calls_identity());

    ck_eq(r, 1u,
          "not-elevated target + AIR-only weapon -> 0x004d337a TEST ...,0x1 fails -> JZ 0x004d33c8, "
          "skip -> floor-at-1 forces the return to 1");
}

void test_all_four_weapon_slots_are_visited_loop_bound() {
    sim_fixture f;
    g_ewd.reset();
    seed_target_unit(f, TARGET_OWNER, TARGET_I, TARGET_PROTO, 5); // not elevated -> GROUND
    seed_armor_row(f, TARGET_PROTO);
    // Four DISTINCT weapon ids/powers, one per slot 0..UNIT_WEAPON_SLOTS-1 (4). A translation that
    // stopped the loop one slot early (a JLE/JL-bound mistake at 0x004d33cc's `CMP EDX,4 / JC`, which
    // is "continue while slot < 4", i.e. slots 0,1,2,3) would miss slot 3's contribution and total 60
    // instead of 100.
    static_assert(UNIT_WEAPON_SLOTS == 4, "this case assumes exactly 4 slots");
    seed_attacker_weapon(f, ATTACKER_P, ATTACKER_I, 0, 10, WEAPON_TARGET_GROUND, 10.0);
    seed_attacker_weapon(f, ATTACKER_P, ATTACKER_I, 1, 11, WEAPON_TARGET_GROUND, 20.0);
    seed_attacker_weapon(f, ATTACKER_P, ATTACKER_I, 2, 12, WEAPON_TARGET_GROUND, 30.0);
    seed_attacker_weapon(f, ATTACKER_P, ATTACKER_I, 3, 13, WEAPON_TARGET_GROUND, 40.0);

    sim_view       v = f.view();
    const uint32_t r =
        detail::unit_estimate_weapon_damage(v, (int32_t)ATTACKER_P, ATTACKER_I, REF_A2, TARGET_I,
                                            rec_ewd_calls_identity());

    ck_eq(r, 100u,
          "all 4 slots visited (0x004d33c9 CMP EDX,4 / JC, EXCLUSIVE bound i.e. slots 0..3): "
          "10+20+30+40 = 100, not 60 (a 3-slot bound) or missing any one slot");
}

// ==== per-step truncation, 0x004d33b4-0x004d33c5 (Site A, weapon_damage_add_and_trunc) ==============

void test_truncation_happens_per_weapon_not_after_summing() {
    sim_fixture f;
    g_ewd.reset();
    seed_target_unit(f, TARGET_OWNER, TARGET_I, TARGET_PROTO, 5); // not elevated -> GROUND
    seed_armor_row(f, TARGET_PROTO);
    // Two weapons, each power 2.9. Per-step: trunc(0+2.9)=2, then trunc(2+2.9)=trunc(4.9)=4. A
    // translation that instead summed the raw doubles first and truncated once would get
    // trunc(2.9+2.9)=trunc(5.8)=5. 0x004d33bd's CALL utils_math_trunc sits INSIDE the loop body
    // (LAB_004d3383), once per surviving weapon -- not once after LAB_004d33c9's loop exit.
    seed_attacker_weapon(f, ATTACKER_P, ATTACKER_I, 0, 7, WEAPON_TARGET_GROUND, 2.9);
    seed_attacker_weapon(f, ATTACKER_P, ATTACKER_I, 1, 8, WEAPON_TARGET_GROUND, 2.9);

    sim_view       v = f.view();
    const uint32_t r =
        detail::unit_estimate_weapon_damage(v, (int32_t)ATTACKER_P, ATTACKER_I, REF_A2, TARGET_I,
                                            rec_ewd_calls_identity());

    ck_eq(r, 4u, "per-step trunc(trunc(0+2.9)=2, then trunc(2+2.9)=4) = 4, not batched trunc(5.8)=5");
}

void test_truncation_rounds_toward_zero_not_floor() {
    sim_fixture f;
    g_ewd.reset();
    seed_target_unit(f, TARGET_OWNER, TARGET_I, TARGET_PROTO, 5); // not elevated -> GROUND
    seed_armor_row(f, TARGET_PROTO);
    // trunc(0 + -1.5): utils_math_trunc's inlined body (0x004d059f MOV AH,0x1f -> x87 RC=11,
    // truncate-toward-zero) gives -1, NOT floor's -2. -1 as the function's uint32_t return is
    // 0xFFFFFFFF (running_total is nonzero, so the 0x004d342b floor-at-1 fixup does not touch it).
    seed_attacker_weapon(f, ATTACKER_P, ATTACKER_I, 0, 7, WEAPON_TARGET_GROUND, -1.5);

    sim_view       v = f.view();
    const uint32_t r =
        detail::unit_estimate_weapon_damage(v, (int32_t)ATTACKER_P, ATTACKER_I, REF_A2, TARGET_I,
                                            rec_ewd_calls_identity());

    ck_eq(r, 0xFFFFFFFFu,
          "trunc(-1.5) toward zero = -1, reinterpreted as uint32_t = 0xFFFFFFFF -- a floor "
          "implementation would give -2 = 0xFFFFFFFE instead");
}

void test_weapon_power_indexed_by_attacker_player_not_target_owner() {
    sim_fixture f;
    g_ewd.reset();
    seed_target_unit(f, TARGET_OWNER, TARGET_I, TARGET_PROTO, 5); // not elevated -> GROUND
    seed_armor_row(f, TARGET_PROTO);
    f.u((int32_t)ATTACKER_P, ATTACKER_I).weapons[0].weapon_id = 7;
    f.cfg_weapons[7].target                                   = WEAPON_TARGET_GROUND;
    // 0x004d33a3-0x004d33a8: MOV ECX,ESI(player=ATTACKER_P) / SHL ECX,3 / ADD EAX,ECX -- the row index
    // into Weapon[weapon_id].power[9] is the ATTACKER's player id, not TARGET_OWNER. Poison the
    // target-owner row so a swapped index is visible.
    f.cfg_weapons[7].power[ATTACKER_P]   = 15.0;
    f.cfg_weapons[7].power[TARGET_OWNER] = 9999.0;

    sim_view       v = f.view();
    const uint32_t r =
        detail::unit_estimate_weapon_damage(v, (int32_t)ATTACKER_P, ATTACKER_I, REF_A2, TARGET_I,
                                            rec_ewd_calls_identity());

    ck_eq(r, 15u, "power row indexed by ATTACKER's player (15.0), not TARGET_OWNER's poisoned row (9999.0)");
}

// ==== the armor-scaling branch, 0x004d33d2-0x004d3427 ================================================

void test_armor_branch_skipped_when_bldg_bit_set() {
    sim_fixture f;
    g_ewd.reset();
    seed_target_unit(f, TARGET_OWNER, TARGET_I, TARGET_PROTO, 5); // not elevated -> GROUND
    seed_armor_row(f, TARGET_PROTO);
    seed_attacker_weapon(f, ATTACKER_P, ATTACKER_I, 0, 7, WEAPON_TARGET_GROUND, 8.0);

    sim_view v = f.view();
    // REF_A2: 0x40 bit set -> 0x004d33d6 JNZ 0x004d3427 skips the whole armor block.
    const uint32_t r =
        detail::unit_estimate_weapon_damage(v, (int32_t)ATTACKER_P, ATTACKER_I, REF_A2, TARGET_I,
                                            rec_ewd_calls_identity());

    ck_eq(r, 8u, "armor branch skipped -> the raw weapon-loop sum (8) passes through unscaled");
    ck_eq((uint32_t)g_ewd.scale_pct.size(), 0u, "target_ref & 0x40 set -> llm_math_scale_pct never called");
}

void test_armor_branch_scales_by_the_target_owners_armor_row() {
    sim_fixture f;
    g_ewd.reset();
    seed_target_unit(f, TARGET_OWNER, TARGET_I, TARGET_PROTO, 5); // not elevated -> GROUND
    seed_armor_row(f, TARGET_PROTO);                              // armor_prob[TARGET_OWNER=6] = 100 + 11*6 = 166
    seed_attacker_weapon(f, ATTACKER_P, ATTACKER_I, 0, 7, WEAPON_TARGET_GROUND, 7.0);

    sim_view v = f.view();
    // REF_A1: 0x40 clear -> armor branch runs, re-reading the slot 0x004d32d3-0x004d32d8 wrote to
    // TARGET_OWNER (safe -- region (A), see file banner).
    const uint32_t r = detail::unit_estimate_weapon_damage(v, (int32_t)ATTACKER_P, ATTACKER_I, REF_A1,
                                                           TARGET_I, rec_ewd_calls());

    ck_eq((uint32_t)g_ewd.scale_pct.size(), 1u, "llm_math_scale_pct called exactly once");
    ck_eq_d(scale_at(0).value, 7.0,
            "scale_pct's value arg = the pre-scaling running_total (7.0), exactly representable "
            "(0x004d3404-0x004d3417 FILD/FSTP)");
    ck_eq((uint32_t)scale_at(0).pct, 166u,
          "scale_pct's pct arg = armor_prob[target_owner=6] = 166, not armor_prob[ATTACKER_P=3]=133 "
          "or armor_prob[0]=100 (0x004d33fd PUSH [proto*0x23f + target_owner*4 + 0xe4a14d])");
}

void test_armor_scaled_result_replaces_running_total_truncated_toward_zero() {
    {
        sim_fixture f;
        g_ewd.reset();
        seed_target_unit(f, TARGET_OWNER, TARGET_I, TARGET_PROTO, 5);
        seed_armor_row(f, TARGET_PROTO);
        seed_attacker_weapon(f, ATTACKER_P, ATTACKER_I, 0, 7, WEAPON_TARGET_GROUND, 3.0);
        g_ewd.scale_pct_return = 2.9; // Site B: trunc(2.9) via 0x004d340e-0x004d3424, QWORD FISTP

        sim_view       v = f.view();
        const uint32_t r = detail::unit_estimate_weapon_damage(v, (int32_t)ATTACKER_P, ATTACKER_I,
                                                               REF_A1, TARGET_I, rec_ewd_calls());
        ck_eq(r, 2u, "trunc(scale_pct's return 2.9) toward zero = 2, replacing the pre-scale total (3)");
    }
    {
        sim_fixture f;
        g_ewd.reset();
        seed_target_unit(f, TARGET_OWNER, TARGET_I, TARGET_PROTO, 5);
        seed_armor_row(f, TARGET_PROTO);
        seed_attacker_weapon(f, ATTACKER_P, ATTACKER_I, 0, 7, WEAPON_TARGET_GROUND, 3.0);
        g_ewd.scale_pct_return = -1.9; // negative: toward-zero trunc gives -1, not floor's -2

        sim_view       v = f.view();
        const uint32_t r = detail::unit_estimate_weapon_damage(v, (int32_t)ATTACKER_P, ATTACKER_I,
                                                               REF_A1, TARGET_I, rec_ewd_calls());
        ck_eq(r, 0xFFFFFFFFu,
              "trunc(-1.9) toward zero = -1 = 0xFFFFFFFF as uint32_t, and it is NONZERO so the "
              "0x004d3427 floor-at-1 fixup does not touch it -- a floor implementation would give -2");
    }
}

// ==== floor-at-1, 0x004d3427-0x004d342b ===============================================================

void test_floor_at_one_when_no_weapon_matches() {
    sim_fixture f;
    g_ewd.reset();
    seed_target_unit(f, TARGET_OWNER, TARGET_I, TARGET_PROTO, 5); // not elevated -> GROUND needed
    seed_armor_row(f, TARGET_PROTO);
    // AIR-only weapon against a GROUND requirement -> rejected -> running_total stays 0 the whole loop.
    seed_attacker_weapon(f, ATTACKER_P, ATTACKER_I, 0, 7, WEAPON_TARGET_AIR, 99.0);

    sim_view v = f.view();
    // REF_A2: armor branch skipped too, so this isolates the floor fixup from the scale step.
    const uint32_t r =
        detail::unit_estimate_weapon_damage(v, (int32_t)ATTACKER_P, ATTACKER_I, REF_A2, TARGET_I,
                                            rec_ewd_calls_identity());

    ck_eq(r, 1u,
          "running_total stays 0 through the whole loop -> 0x004d3429 TEST EBX,EBX / JNZ not taken -> "
          "0x004d342b MOV EBX,1 -> returns 1, never 0");
}

void test_floor_at_one_does_not_touch_a_nonzero_result() {
    sim_fixture f;
    g_ewd.reset();
    seed_target_unit(f, TARGET_OWNER, TARGET_I, TARGET_PROTO, 5);
    seed_armor_row(f, TARGET_PROTO);
    seed_attacker_weapon(f, ATTACKER_P, ATTACKER_I, 0, 7, WEAPON_TARGET_GROUND, 5.0);

    sim_view       v = f.view();
    const uint32_t r =
        detail::unit_estimate_weapon_damage(v, (int32_t)ATTACKER_P, ATTACKER_I, REF_A2, TARGET_I,
                                            rec_ewd_calls_identity());

    ck_eq(r, 5u, "running_total==5 (nonzero) -> 0x004d3429 JNZ taken, straight to return, not bumped to 1");
}

// ---- region (C): the stack-probe imprint path, (target_ref & 0xe0) == 0 ----------------------------
// This region was EXCLUDED by the banner above until 2026-08-16 as an unreproducible divergence. It is
// reproduced now, so it is testable, and these are the cases that pin it. On this path `[EBP-0x10]` is
// never written, so both reads (0x004d33d8, 0x004d33fa) see the Watcom stack-probe imprint: the
// function's own frame size, 0x38 == 56. Derived from the disassembly of the
// prologue + utils_assert_stack_capacity, and confirmed under cdb on a live all-AI soak.

constexpr uint32_t IMPRINT_OWNER = 56;    // 0x38 -- the frame size PUSHed at 0x004d32ad
constexpr uint32_t REF_C1        = 0x06u; // 0000_0110: 0xa0 clear AND 0x40 clear -> region (C)
constexpr uint32_t REF_C0        = 0x00u; // the "no target" encoding -- also region (C)
constexpr uint16_t IMPRINT_PROTO = 61;    // proto in units[56][TARGET_I]; != TARGET_PROTO so an
                                          // owner mix-up changes the answer instead of hiding in it

// armor_prob[owner] for owner >= 9. The original applies NO bound (see the .cpp's extent note), so the
// fixture has to be able to seed the very slot it reads. Addressed through the record's bytes rather
// than by subscripting past the declared [9]: the address is identical either way (cfg_unit + 0xb5 +
// owner*4, and 0xb5 + 56*4 == 405, still inside the 0x23f-byte record), and this spelling does not ask
// the test itself to depend on out-of-bounds subscripting behaving.
int32_t &armor_slot(sim_fixture &f, uint16_t proto, uint32_t owner) {
    auto *rec = reinterpret_cast<uint8_t *>(&f.cfg_units[proto]);
    return *reinterpret_cast<int32_t *>(rec + offsetof(cfg_unit, armor_prob) + owner * 4u);
}

// Seeds BOTH the row the imprint names and the decoy row the ref's nibble names, so every assertion
// below distinguishes them. Returns nothing; the constants are fixed per case.
void seed_region_C(sim_fixture &f, uint32_t decoy_owner, int32_t decoy_pct_seed_proto) {
    // wide_u FIRST: it may reallocate the roster, which would dangle any reference handed out earlier.
    f.wide_u(IMPRINT_OWNER, TARGET_I).unit_proto_id = IMPRINT_PROTO;
    // ELEVATED on purpose. Gate 1 does not fire in region (C), so the classification must never run --
    // if some future edit classified off the imprint row, this would flip the required weapon bit to
    // AIR and the GROUND weapon below would be rejected, turning the result into the floor value 1.
    f.cfg_units[IMPRINT_PROTO].type             = 0x10; // H_HELI, > A_GROUND
    armor_slot(f, IMPRINT_PROTO, IMPRINT_OWNER) = 77;
    // The decoy: fully seeded so a translation that recomputes `target_ref & 0xf` here -- which is what
    // this file's .cpp did until 2026-08-16 -- reads a valid, different, WRONG value rather than junk.
    seed_target_unit(f, decoy_owner, TARGET_I, (uint16_t)decoy_pct_seed_proto, 5);
    seed_armor_row(f, (uint16_t)decoy_pct_seed_proto);                 // armor_prob[decoy_owner] = 100 + 11*owner
    armor_slot(f, (uint16_t)decoy_pct_seed_proto, IMPRINT_OWNER) = 88; // right owner, wrong proto
    seed_attacker_weapon(f, ATTACKER_P, ATTACKER_I, 0, 7, WEAPON_TARGET_GROUND, 9.0);
}

void test_region_C_armor_lookup_indexes_by_the_imprint_slot_not_the_ref_nibble() {
    sim_fixture f;
    g_ewd.reset();
    seed_region_C(f, /*decoy_owner*/ 6, /*decoy proto*/ TARGET_PROTO);

    sim_view       v = f.view();
    const uint32_t r = detail::unit_estimate_weapon_damage(v, (int32_t)ATTACKER_P, ATTACKER_I, REF_C1,
                                                           TARGET_I, rec_ewd_calls());
    (void)r;

    ck_eq((uint32_t)g_ewd.scale_pct.size(), 1u,
          "0x004d33d2 TEST ...,0x40 / JNZ not taken (ref 0x06 has 0x40 clear) -> the armor branch runs");
    ck_eq_d(scale_at(0).value, 9.0,
            "gate 1 never fired, so is_elevated stays false and the GROUND weapon counted: sum 9.0");
    ck_eq((uint32_t)scale_at(0).pct, 77u,
          "armor_prob indexed by the IMPRINT slot 56 on units[56]'s proto (77), NOT by target_ref&0xf=6 "
          "on that row's proto (166), NOT owner 56 of the decoy proto (88) -- the two reads at "
          "0x004d33d8/0x004d33fa are re-reads of [EBP-0x10], which this path never wrote");
}

void test_region_C_holds_for_ref_zero_the_no_target_encoding() {
    sim_fixture f;
    g_ewd.reset();
    // Decoy owner 0: target_ref == 0 means `target_ref & 0xf` == 0, so a nibble-recomputing
    // translation reads units[0][TARGET_I] -- seeded here with its own proto and armor row.
    seed_region_C(f, /*decoy_owner*/ 0, /*decoy proto*/ 44);

    sim_view       v = f.view();
    const uint32_t r = detail::unit_estimate_weapon_damage(v, (int32_t)ATTACKER_P, ATTACKER_I, REF_C0,
                                                           TARGET_I, rec_ewd_calls());
    (void)r;

    ck_eq((uint32_t)g_ewd.scale_pct.size(), 1u, "ref 0 has 0x40 clear -> armor branch runs");
    ck_eq((uint32_t)scale_at(0).pct, 77u,
          "ref==0 ('no target') still indexes with the imprint 56, not with the nibble 0 (which would "
          "give armor_prob[0]=100 on proto 44)");
}

void test_region_A_still_uses_the_freshly_written_nibble_not_the_imprint() {
    // The negative control for the two cases above: when gate 1 DOES fire, 0x004d32d8 overwrites the
    // slot, so the armor branch must read the nibble and the imprint row must be ignored entirely.
    sim_fixture f;
    g_ewd.reset();
    seed_region_C(f, /*decoy_owner*/ TARGET_OWNER, /*decoy proto*/ TARGET_PROTO);

    sim_view v = f.view();
    // REF_A1 = 0x86: 0xa0 bit set (gate 1 fires, slot := 6), 0x40 clear (armor branch runs).
    const uint32_t r = detail::unit_estimate_weapon_damage(v, (int32_t)ATTACKER_P, ATTACKER_I, REF_A1,
                                                           TARGET_I, rec_ewd_calls());
    (void)r;

    ck_eq((uint32_t)g_ewd.scale_pct.size(), 1u, "0x40 clear -> armor branch runs");
    ck_eq((uint32_t)scale_at(0).pct, 166u,
          "gate 1 fired, so the slot holds 6 and armor_prob[6]=166 is read -- the imprint 56 must NOT "
          "leak into region (A) (77) and neither must the decoy's owner-56 slot (88)");
}

void run_unit_estimate_weapon_damage_tests() {
    printf("-- llm_strat_unit_estimate_weapon_damage --\n");
    test_type_exactly_0xe_is_not_elevated_boundary();
    test_type_exactly_0xf_is_elevated_boundary();
    test_type_high_bit_set_is_a_SIGNED_compare_not_unsigned();
    test_gate1_not_fired_keeps_classification_false_even_if_target_would_be_elevated();
    test_weapon_id_zero_slot_is_skipped();
    test_ground_weapon_rejected_when_target_is_elevated();
    test_air_weapon_rejected_when_target_is_not_elevated();
    test_all_four_weapon_slots_are_visited_loop_bound();
    test_truncation_happens_per_weapon_not_after_summing();
    test_truncation_rounds_toward_zero_not_floor();
    test_weapon_power_indexed_by_attacker_player_not_target_owner();
    test_armor_branch_skipped_when_bldg_bit_set();
    test_armor_branch_scales_by_the_target_owners_armor_row();
    test_armor_scaled_result_replaces_running_total_truncated_toward_zero();
    test_floor_at_one_when_no_weapon_matches();
    test_floor_at_one_does_not_touch_a_nonzero_result();
    // region (C) -- the stack-probe imprint path, testable since the divergence was closed 2026-08-16
    test_region_C_armor_lookup_indexes_by_the_imprint_slot_not_the_ref_nibble();
    test_region_C_holds_for_ref_zero_the_no_target_encoding();
    test_region_A_still_uses_the_freshly_written_nibble_not_the_imprint();
}

} // namespace mh::sim::test
