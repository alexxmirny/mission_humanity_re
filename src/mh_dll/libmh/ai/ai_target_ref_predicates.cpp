//
// ai/ai_target_ref_predicates.cpp -- see ai_target_ref_predicates.h. Translated from the
// DISASSEMBLY (tmp/decomp_a4/llm_strat_ai_target_ref_is_alive_004d4082.asm,
// tmp/decomp_a4/llm_strat_ai_target_ref_has_ground_weapon_004d73dd.asm and
// tmp/decomp_a4/llm_strat_ai_target_ref_has_aa_weapon_004d7457.asm), not from Ghidra's C:
//
//   * has_ground_weapon's decompile shows a single self-contained function with an inline loop
//     over `units[...].weapons[0..3]` for the unit case -- that loop is Ghidra silently inlining
//     the body of llm_strat_unit_has_ground_weapon, the function this one actually TAIL-JUMPS to
//     (0x004d73e9 `JNZ 0x004d7377`). The real unit case is one call, routed through
//     calls.unit_has_ground_weapon with the ref UNMASKED, matching the push-order evidence in
//     ai_state.h's ai_calls comment.
//   * has_aa_weapon's decompile does the same thing twice over: the unit case inlines
//     llm_strat_unit_has_aa_weapon's body, and the building case inlines llm_strat_bldg_has_aa_weapon's
//     body -- neither call is real; the assembly is 0x15 (21) bytes total, a JNZ tail-jump for the
//     unit case and a bare fall-through (no jump at all) for the building case, into
//     llm_strat_bldg_has_aa_weapon, simply the next function in the image.
//
#include "ai/ai_target_ref_predicates.h"


namespace mh::ai {
namespace detail {

int32_t target_ref_is_alive(const ai_view &v, uint32_t target_ref_packed, int32_t target_index) {
    const uint32_t owner = ref_owner(target_ref_packed);

    // ref_is_building_by_a0 -- (ref & 0xa0) == 0 -- is the selector this function's own assembly
    // uses (`TEST AL,0xa0` at 0x004d4097); the 0x40 selector is a different function's convention
    // and is not live here.
    if (ref_is_building_by_a0(target_ref_packed)) {
        const building &b = building_of(v, owner, target_index);
        if (b.building_id == 0) return 0;
        // See the header comment: `<=` (not `!(... > 0.0)`) is what makes a NaN energy read as
        // alive, matching the original's FCOMPP/SAHF/JC dance.
        if (b.energy - b.pending_damage <= 0.0) return 0;
        return 1;
    }

    const unit &u = unit_of(v, owner, target_index);
    if (u.unit_proto_id == 0) return 0;
    if (u.energy - u.pending_damage <= 0.0) return 0;
    return 1;
}

int32_t target_ref_has_ground_weapon(const ai_view &v, const ai_calls &gc, uint32_t target_ref_packed,
                                     int32_t target_index) {
    if (!ref_is_building_by_a0(target_ref_packed)) {
        // Tail-jump to llm_strat_unit_has_ground_weapon (0x004d7377) -- UNMASKED ref, unchanged
        // index.
        return gc.unit_has_ground_weapon(target_ref_packed, target_index) != 0 ? 1u : 0u;
    }

    // The building case is this function's OWN body (0x004d73eb..0x004d7455) -- there is no
    // separate bldg_has_ground_weapon to call. Mask =1 confirmed from THIS function's own
    // `TEST byte ptr [EAX + 0xc3a521],0x1` at 0x004d744e.
    const uint32_t  owner     = ref_owner(target_ref_packed);
    const building &b         = building_of(v, owner, target_index);
    const int32_t   weapon_id = v.cfg_buildings[b.building_id].weapon_id;
    if (weapon_id == 0) return 0;
    return (v.cfg_weapons[weapon_id].target & 1) != 0 ? 1u : 0u;
}

int32_t target_ref_has_aa_weapon(const ai_calls &gc, uint32_t target_ref_packed, int32_t target_index) {
    if (!ref_is_building_by_a0(target_ref_packed)) {
        // Tail-jump to llm_strat_unit_has_aa_weapon (0x004d7311) -- UNMASKED ref, unchanged index.
        return gc.unit_has_aa_weapon(target_ref_packed, target_index) != 0 ? 1u : 0u;
    }

    // No jump at all here: the assembly masks the owner nibble into EAX (`AND EAX,0xf`) and simply
    // FALLS OFF THE END of this function's 0x15 bytes into llm_strat_bldg_has_aa_weapon
    // (0x004d746c), the next function in the image -- a tail call spelled without a jump. Its
    // first argument is therefore the ALREADY-MASKED owner nibble, not the packed ref; its second
    // argument is target_index, untouched by anything in this function.
    const uint32_t owner = ref_owner(target_ref_packed);
    return gc.bldg_has_aa_weapon(owner, target_index) != 0 ? 1u : 0u;
}

} // namespace detail

// ---- the public wrappers ------------------------------------------------------------------------

int32_t target_ref_is_alive(uint32_t target_ref_packed, int32_t target_index) {
    const ai_state st = state();
    return detail::target_ref_is_alive(st.read, target_ref_packed, target_index);
}

int32_t target_ref_has_ground_weapon(uint32_t target_ref_packed, int32_t target_index) {
    const ai_state st = state();
    return detail::target_ref_has_ground_weapon(st.read, live_calls(), target_ref_packed, target_index);
}

int32_t target_ref_has_aa_weapon(uint32_t target_ref_packed, int32_t target_index) {
    return detail::target_ref_has_aa_weapon(live_calls(), target_ref_packed, target_index);
}


} // namespace mh::ai
