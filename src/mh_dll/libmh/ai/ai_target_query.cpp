//
// ai/ai_target_query.cpp -- see ai_target_query.h. Translated from the DISASSEMBLY
// (tmp/decomp_a3/llm_strat_ai_target_ref_has_engageable_weapon_004d3cf0.asm and
// tmp/decomp_a3/llm_strat_ai_target_dist_sq_004d81a6.asm), not from Ghidra's C:
//
//   * has_engageable_weapon's decompile folds the two CALLs' real 1-byte bool returns into
//     `CONCAT31(extraout_var, bVar1) != 0` and reports a phantom 4th "extraout_ECX" argument -- there
//     is no 4th parameter; ECX is simply the mode selector this function computes for ITSELF at
//     0x004d3d00-0x004d3d16, entirely before either call, and never leaves the function otherwise.
//   * target_dist_sq's decompile shows a `void` return, which is wrong -- the original's tail is
//     `CALL llm_strat_toroidal_dist_sq` immediately followed by `JMP 0x004d7e65`, a shared Watcom
//     epilogue (not a call), so the callee's EAX return IS this function's return, same pattern as
//     ai_turret_threat.cpp's tail. The four resolve-then-call branches (unit/unit, unit/building,
//     building/unit, building/building) the .c derives ARE each individually correct against the
//     assembly (traced instruction-by-instruction, including the shared tails at LAB_004d8245) --
//     only the "returns nothing" claim is wrong.
//
#include "ai/ai_target_query.h"


namespace mh::ai {
namespace detail {

int32_t target_ref_has_engageable_weapon(const ai_calls &gc, int32_t target_ref_kind,
                                         int32_t target_ref_index, uint32_t attacker_weapon_flags) {
    // NOT a symmetric priority test: mode 2 only when 0x20 is set AND 0x40 is clear. Every other
    // combination -- including NEITHER bit set -- falls through to mode 1.
    const uint32_t mode =
        ((attacker_weapon_flags & 0x40u) == 0 && (attacker_weapon_flags & 0x20u) != 0) ? 2u : 1u;

    // Both predicates are called UNCONDITIONALLY and in this order (ground first, then aa) -- the
    // call COUNT is observable to the shadow oracle, so this must not short-circuit on `mode`.
    uint32_t bits = 0;
    if (gc.target_ref_has_ground_weapon((uint32_t)target_ref_kind, target_ref_index) != 0) bits |= 1u;
    if (gc.target_ref_has_aa_weapon((uint32_t)target_ref_kind, target_ref_index) != 0) bits |= 2u;

    return (mode & bits) != 0 ? 1u : 0u;
}

uint32_t target_dist_sq(const ai_view &v, const ai_calls &gc, uint32_t ref_a, int32_t idx_a,
                        uint32_t ref_b, int32_t idx_b) {
    const uint32_t owner_a = ref_owner(ref_a);
    const uint32_t owner_b = ref_owner(ref_b);

    int32_t ax, ay, bx, by;
    if (ref_is_building_by_40(ref_a)) {
        const building &a = building_of(v, owner_a, idx_a);
        ax                = a.x;
        ay                = a.y;
    } else {
        const unit &a = unit_of(v, owner_a, idx_a);
        ax            = a.x;
        ay            = a.y;
    }
    if (ref_is_building_by_40(ref_b)) {
        const building &b = building_of(v, owner_b, idx_b);
        bx                = b.x;
        by                = b.y;
    } else {
        const unit &b = unit_of(v, owner_b, idx_b);
        bx            = b.x;
        by            = b.y;
    }

    // Argument order confirmed from the cdecl push order at the shared call site: a.x, a.y, b.x, b.y
    // in every one of the four unit/building combinations.
    return gc.toroidal_dist_sq(ax, ay, bx, by);
}

} // namespace detail

// ---- the public wrappers ------------------------------------------------------------------------

int32_t target_ref_has_engageable_weapon(int32_t target_ref_kind, int32_t target_ref_index,
                                         uint32_t attacker_weapon_flags) {
    return detail::target_ref_has_engageable_weapon(live_calls(), target_ref_kind, target_ref_index,
                                                    attacker_weapon_flags);
}

uint32_t target_dist_sq(uint32_t ref_a, int32_t idx_a, uint32_t ref_b, int32_t idx_b) {
    const ai_state st = state();
    return detail::target_dist_sq(st.read, live_calls(), ref_a, idx_a, ref_b, idx_b);
}


} // namespace mh::ai
