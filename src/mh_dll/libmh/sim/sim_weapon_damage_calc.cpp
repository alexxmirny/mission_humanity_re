//
// sim/sim_weapon_damage_calc.cpp -- see sim_weapon_damage_calc.h. Translated from the DISASSEMBLY
// (tmp/decomp_sim1e2/llm_strat_weapon_pixel_distance_ratio_00448cc3.asm,
// tmp/decomp_sim1e2/llm_strat_unit_estimate_weapon_damage_004d32aa.asm), not from the Ghidra .c
// drafts -- see the header banner for the full field/address derivation, the target-ref bit
// provenance, the armor_prob extent decision, and the two utils_math_trunc shapes.
//
#include "sim/sim_weapon_damage_calc.h"

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"        // ai_say / trace_budget -- the shared trace sink, not AI state
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"
#include "fp/x87_shapes.h"  // CRT-X87: the hoisted x87 blocks (the asm moved, it did not change)
#include "crt/crt_select.h" // LIB-CRT: MH_CRT() picks the vendored CRT in the standalone build

namespace mh::sim {

const weapon_pixel_distance_ratio_calls &live_weapon_pixel_distance_ratio_calls() {
    static const weapon_pixel_distance_ratio_calls c = {
        MH_LIBMH_BIND(llm_strat_pixel_delta_wrapped),
        MH_CRT(llm_sqrt),
    };
    return c;
}

const unit_estimate_weapon_damage_calls &live_unit_estimate_weapon_damage_calls() {
    static const unit_estimate_weapon_damage_calls c = {
        MH_LIBMH_BIND(llm_math_scale_pct),
    };
    return c;
}

namespace {

// ---- the packed target-ref bits, duplicated from ai/ai_state.h's REF_UNIT_BITS/REF_BLDG_BIT per
// the mh/sim-does-not-depend-on-mh/ai rule (see the header banner). Kept file-private -- nothing
// outside this TU needs them.
inline constexpr uint32_t TARGET_REF_OWNER_MASK = 0x0fu;
inline constexpr uint32_t TARGET_REF_UNIT_BITS  = 0xa0u; // (ref & 0xa0) != 0 -> this ref is a UNIT
inline constexpr uint32_t TARGET_REF_BLDG_BIT   = 0x40u; // (ref & 0x40) != 0 -> this ref is a BUILDING

// cfg_enum_E_UNIT_TYPE::A_GROUND. The elevation test at 0x004d32fb is `type > A_GROUND`, i.e.
// "is the target one of the FLYING types" -- the enum's members run UNDEFINED(0), the ten infantry
// rows, A_WALKER(0xb), H_WALKER(0xc), H_GROUND(0xd), A_GROUND(0xe), then A_HELI(0xf) .. H_HELI_CARGO
// (0x18). 0xe is therefore the LAST GROUND TYPE and the compare is an exact ground/air split, not an
// arbitrary threshold. (Resolved 2026-08-16 from the live DTM, closing this file's own DECLARED NEED
// -- the member list is in neither addr/mh_structs.gen.h nor docs/structs.md, both of which carry
// only the type tag.)
inline constexpr int32_t E_UNIT_TYPE_A_GROUND = 0xe;

// ---- the [EBP-0x10] stack-probe imprint ------------------------------------------------------------
// 0x004d32aa's prologue is `PUSH EBP; MOV EBP,ESP; PUSH 0x38; CALL utils_assert_stack_capacity`, i.e.
// the Watcom stack probe is called AFTER the frame pointer is set and BEFORE `SUB ESP,0x1c` -- so the
// probe's own frame overlaps the locals this function is about to be handed. With R = ESP at entry
// (EBP = R-4), the probe's `PUSH EAX` at 0x004cf47f pushes EAX = the 0x38 size argument (loaded by its
// opening `XCHG [ESP+4],EAX`) onto R-0x14, which IS [EBP-0x10]; this function then pushes only ESI/EDI
// (R-8, R-0xc), leaving it intact. The probe's own `MOV EAX,[ESP+0xc]` at 0x004cf4b0 reads that slot
// back as the size, so the probe's code self-verifies the value.
//
// Confirmed live under cdb (breakpoint at 0x004d32cd, all-AI soak): [EBP-0x10] == 0x00000038 on every
// hit, alongside [EBP-0x20] == 0xfffffffc (the probe's page-touch literal) and [EBP-0x1c] == the
// caller's EBX -- three imprint slots agreeing with the derivation, so this is the probe's frame and
// not a coincidence.
//
// NUMERICALLY IT IS THIS FUNCTION'S OWN FRAME SIZE. Spelled as the frame size rather than a bare 56 so
// the derivation survives: if the frame ever changed, so would this.
inline constexpr uint32_t STACK_PROBE_FRAME_SIZE    = 0x38;
inline constexpr uint32_t STACK_PROBE_OWNER_IMPRINT = STACK_PROBE_FRAME_SIZE;

// ---- utils_math_trunc @0x004d0596, reproduced as inline asm (ST0 in/out, x87-only) -----------------
// Same body (FSTCW/mov ah,0x1f/FLDCW/FRNDINT/FLDCW-restore, addresses 0x004d0597-0x004d05a9) as
// ai_group_muster_pick.cpp's weapon_power_add_and_trunc / sim_projectile_tick.cpp's trio reproduce
// for this identical callee -- re-derived locally per this project's per-TU convention.

// Site A (0x004d33b4-0x004d33c5): trunc((int64_t)running_total + *addend), QWORD FISTP destination
// with only the low dword kept -- the EXACT shape ai_group_muster_pick.cpp's
// weapon_power_add_and_trunc already uses for this same callee; re-declared privately here.
int32_t weapon_damage_add_and_trunc(uint32_t running_total, const double *addend) {
    return ::mh::fp::trunc_add_u32_double_low(running_total, addend);
}

// Site B (0x004d340e-0x004d3424): trunc(x) alone, applied to llm_math_scale_pct's ST0 return with NO
// intervening FLD (the CALL leaves the value already on the FPU stack, matching the assembly's
// back-to-back CALL scale_pct / CALL utils_math_trunc at 0x004d3417-0x004d341c). QWORD FISTP
// destination with only the low dword kept -- see the header banner's note on why this is NOT
// sim_projectile_tick.cpp's dword-FISTP trunc_only.
int32_t trunc_qword_low(double x) {
    return ::mh::fp::trunc_qword_low(x);
}

} // namespace

namespace detail {

double weapon_pixel_distance_ratio(const sim_view &v, int32_t weapon_id, int32_t x1, int32_t y1,
                                   int32_t x2, int32_t y2, const weapon_pixel_distance_ratio_calls &c) {
    // 0x00448ce4-0x00448cf8: two int32 OUT-pointers, not doubles -- see the header banner's hazard #4
    // note (matches addr/mh_calls.gen.h's `void *param_5, void *param_6` marshalling).
    int32_t dx = 0, dy = 0;
    c.pixel_delta_wrapped(x1, y1, x2, y2, &dx, &dy);

    // 0x00448d04-0x00448d14: 32-bit wrapping IMUL/ADD, stored to a 32-bit slot BEFORE the FILD widens
    // it to double -- see the header banner's note on why this is not the same as summing two
    // already-widened doubles.
    const int32_t dist_sq = dx * dx + dy * dy;
    const double  dist    = c.sqrt_fn(static_cast<double>(dist_sq)); // 0x00448d17-0x00448d20

    // 0x00448d25-0x00448d3a: (dist * Weapon[weapon_id].speed) / Weapon[weapon_id].length -- see the
    // header banner's derivation of the +0x48/+0x24 offsets as `speed`/`length`.
    const cfg_weapon &w = v.cfg_weapons[weapon_id];
    return (dist * w.speed) / static_cast<double>(w.length);
}

uint32_t unit_estimate_weapon_damage(const sim_view &v, int32_t player, int32_t unit_index,
                                     uint32_t target_ref, int32_t target_index,
                                     const unit_estimate_weapon_damage_calls &c) {
    // 0x004d32cd-0x004d3308: elevation classification, gated on (target_ref & 0xa0) != 0 (i.e. the
    // ref names a UNIT target, not a building -- ai_state.h's ref_is_building_by_a0 convention,
    // duplicated locally, see the header banner). If the gate is false, `is_elevated_target` stays
    // false for the whole function, exactly matching [EBP-0xc]'s initial 0 never being overwritten.
    //
    // [EBP-0x10] IS MODELLED AS A SLOT, NOT AS A C++ LOCAL (2026-08-16). The armor branch further
    // down re-reads this slot rather than recomputing the nibble, and reaches it through an
    // INDEPENDENT gate -- so its value on the `(target_ref & 0xe0) == 0` path is whatever the slot
    // held BEFORE this function's only write to it. That is not previous-frame residue: it is the
    // Watcom stack-probe imprint, and it is a constant. `target_owner_slot`
    // therefore starts at that constant instead of being declared inside the gate.
    uint32_t target_owner_slot = STACK_PROBE_OWNER_IMPRINT;

    bool is_elevated_target = false;
    if ((target_ref & TARGET_REF_UNIT_BITS) != 0) {
        target_owner_slot           = target_ref & TARGET_REF_OWNER_MASK; // 0x004d32d3-0x004d32d8
        const uint32_t target_owner = target_owner_slot;
        const unit    &target_unit  = unit_of(v, target_owner, target_index);
        // `CMP dword ptr [...],0xe; SETG` -- SETG is SIGNED, and `type` is committed uint32_t, so the
        // cast is what makes the C++ compare the same one the instruction does. Unreachable on
        // shipped cfg data (see the header banner's enum table: every member is 0x00..0x18), but the
        // sibling llm_strat_unit_state_die_explode tests the SAME field against the SAME 0xe with a
        // signed JG and was fixed the same way -- matching the instruction is the standing choice
        // here, not a judgement about reachability.
        is_elevated_target =
            static_cast<int32_t>(v.cfg_units[target_unit.unit_proto_id].type) > E_UNIT_TYPE_A_GROUND;
    }

    // 0x0044330b-0x004d33c9: sum power[player] across the ATTACKER's up to 4 weapon slots, keeping
    // only weapons whose target-class bit matches is_elevated_target.
    uint32_t    running_total = 0;
    const unit &attacker      = unit_of(v, static_cast<uint32_t>(player), unit_index);
    for (int32_t slot = 0; slot < UNIT_WEAPON_SLOTS; ++slot) {
        const uint8_t weapon_id = attacker.weapons[slot].weapon_id;
        if (weapon_id == 0) continue; // 0x004d332e

        const uint8_t required_bit = is_elevated_target ? WEAPON_TARGET_AIR : WEAPON_TARGET_GROUND;
        if ((v.cfg_weapons[weapon_id].target & required_bit) == 0) continue; // 0x004d334e/0x004d3381

        running_total = static_cast<uint32_t>(weapon_damage_add_and_trunc(
            running_total, &v.cfg_weapons[weapon_id].power[player])); // 0x004d3383-0x004d33c5
    }

    // 0x004d33d2-0x004d3427: scale through llm_math_scale_pct's per-(target proto, target owner)
    // armor_prob lookup, but ONLY when target_ref does NOT carry the building bit (ai_state.h:
    // ref_is_building_by_40, duplicated locally) -- an INDEPENDENT gate from the elevation
    // classification above (see the header banner's note on the two disagreeing on nibble 0).
    if ((target_ref & TARGET_REF_BLDG_BIT) == 0) {
        // ============================================================================================
        // THE FORMER DECLARED DIVERGENCE -- CLOSED 2026-08-16. Kept in full because it took three
        // corrections to get right and the wrong versions are instructive.
        // Found by reimpl-verify 2026-08-15; the first draft of this block (and the header banner)
        // claimed the original performs "a real, independent SECOND computation" here. It does not.
        //
        // WHAT THE ORIGINAL ACTUALLY DOES. The stack slot [EBP-0x10] is written at exactly ONE site,
        // 0x004d32d3-0x004d32d8 (`MOV EAX,EBX; AND EAX,0xf; MOV [EBP-0x10],EAX`), and that site is
        // gated on `TEST byte ptr [EBP-0x14],0xa0; JZ` (0x004d32cd). This armor branch is reached
        // through an INDEPENDENT gate, `TEST byte ptr [EBP-0x14],0x40; JNZ` (0x004d33d2), and it does
        // NOT recompute the nibble -- it re-reads that same slot twice, at 0x004d33d8 (the proto
        // lookup) and 0x004d33fa (the armor_prob index). Nothing between 0x004d32d8 and 0x004d33fa
        // writes it, so when the 0xa0 gate was skipped this branch indexes with a value this function
        // never computed.
        //
        // AND THAT VALUE IS 0x38 == 56, DETERMINISTICALLY (corrected 2026-08-16 -- this block used to
        // say "whatever the previous frame left there"). It is not a previous frame's: `PUSH 0x38;
        // CALL utils_assert_stack_capacity` (0x004d32ad) runs AFTER `PUSH EBP`, so the Watcom stack
        // probe's own frame overlaps these locals, and its `PUSH EAX` at 0x004cf47f -- EAX being the
        // 0x38 size argument -- lands on exactly [EBP-0x10]. The probe even reads the slot back as the
        // size at 0x004cf4b0, so its code self-verifies the claim. Measured live under cdb (6 hits,
        // all-AI soak): 0x00000038 every time. It is the frame size, not call-history garbage.
        //
        // THE PATH IS LATENT, NOT LIVE -- REFUTED 2026-08-16. This block used to claim
        // `llm_strat_ai_commit_attack_order` passes `target_ref & 0x0f` (built at 0x004d562b-
        // 0x004d5632) so "the uninitialised read happens every time that arm runs, two of the five
        // call sites". It does not. That `AND DL,0xf` writes commit_attack_order's OWN [EBP-0x10] --
        // the SAME DISPLACEMENT IN A DIFFERENT FRAME -- and that local is passed to a different
        // function entirely (0x004d568c `MOVZX EBX,word ptr [EBP-0x10]` -> CALL 0x0046c21c).
        // target_ref reaches us in EBX, and EBX is untouched from commit_attack_order's entry to
        // either call site, so both pass the UNMODIFIED ref; _alt is structurally identical. One of
        // the two is provably safe besides: 0x004d5645 sits on the `BL & 0x40 != 0` arm, so our own
        // 0x40 gate shuts the armor branch.
        //
        // Divergence therefore requires a caller to pass a ref with 0x80, 0x20 AND 0x40 all clear --
        // the "no target" encoding per mh_map_object_unit::target_ref's field comment. Measured on an
        // all-AI soak (cdb at 0x004d33d8): every observed ref carried the 0x80 bit, so the slot was
        // always freshly written and this branch always agreed with `target_ref & 0xf`. Latent, not
        // proven unreachable -- llm_strat_target_release_ref (0x004daca5) passes a STORED ref field
        // whose range is unenumerated.
        //
        // NOW REPRODUCED, SO THIS IS NO LONGER A DIVERGENCE (2026-08-16). The slot is modelled at the
        // top of the function (`target_owner_slot`, initialised to STACK_PROBE_OWNER_IMPRINT), and
        // this branch RE-READS it exactly as 0x004d33d8/0x004d33fa do -- it does not recompute
        // `target_ref & 0xf`. On the (target_ref & 0xe0) == 0 path that yields 56, which is what the
        // original indexes with.
        //
        // NO RAW ADDRESS IS NEEDED, and that is not a loophole -- the arithmetic is identical. The
        // original computes `0xdd8c4a + owner*0x5b04 + target_index*0xe9`; `unit_of` computes
        // `v.units + (owner*UNITS_PER_PLAYER + target_index)`, and UNITS_PER_PLAYER*sizeof(unit) ==
        // 100*0xe9 == 0x5b04 (both static_assert'd in addr/mh_structs.gen.h). Likewise
        // `&cfg_units[proto].armor_prob[0] + 56` is `0xe4a098 + proto*0x23f + 0xb5 + 56*4` ==
        // `0xe4a14d + proto*0x23f + 56*4`, the original's second address exactly. So the generated
        // code matches even though the C++ index is out of the DECLARED array bounds.
        //
        // WHAT IS TRUE AND WORTH KNOWING ANYWAY: with owner == 56 both reads leave their arrays. In
        // the live image that is harmless in the sense that it is what the original does -- units[56]
        // lands ~1.3 MB past the roster inside player_data (0x00f1752a), armor_prob[56] lands 224
        // bytes past a 9-element array but still INSIDE the 575-byte cfg_unit record. Offline it is a
        // genuine out-of-bounds index on a std::vector, which is why
        // sim_unit_estimate_weapon_damage_selftest.cpp widens the fixture's roster (see its
        // `wide_u`) for the cases that walk this path, rather than letting ASan take the run down.
        //
        // EXTENT, unchanged: the struct declares armor_prob[9] and the original applies no bound at
        // all -- not the 0..15 the nibble would give, and certainly not 9. Indexed directly with the
        // slot, no clamp, matching translator-brief rule 14.
        // ============================================================================================
        const uint32_t target_owner = target_owner_slot; // 0x004d33d8 / 0x004d33fa -- a RE-READ
        const unit    &target_unit  = unit_of(v, target_owner, target_index);
        const uint16_t proto_id     = target_unit.unit_proto_id;

        const int32_t pct    = v.cfg_units[proto_id].armor_prob[target_owner];
        const double  scaled = c.scale_pct(static_cast<double>(running_total), pct);
        running_total        = static_cast<uint32_t>(trunc_qword_low(scaled));
    }

    // 0x004d3427-0x004d342b: floor at 1, never return 0.
    if (running_total == 0) running_total = 1;
    return running_total;
}

} // namespace detail

// ---- the public wrappers --------------------------------------------------------------------------

double weapon_pixel_distance_ratio(int32_t weapon_id, int32_t x1, int32_t y1, int32_t x2, int32_t y2) {
    const sim_view v = state().read;
    return detail::weapon_pixel_distance_ratio(v, weapon_id, x1, y1, x2, y2,
                                               live_weapon_pixel_distance_ratio_calls());
}

uint32_t unit_estimate_weapon_damage(int32_t player, int32_t unit_index, uint32_t target_ref,
                                     int32_t target_index) {
    const sim_view v = state().read;
    return detail::unit_estimate_weapon_damage(v, player, unit_index, target_ref, target_index,
                                               live_unit_estimate_weapon_damage_calls());
}


} // namespace mh::sim
