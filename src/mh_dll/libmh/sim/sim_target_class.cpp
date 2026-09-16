//
// sim/sim_target_class.cpp -- see sim_target_class.h. Translated from the DISASSEMBLY
// (tmp/decomp/llm_strat_target_class_004495f9.asm), which agrees with the Ghidra .c draft here --
// the draft's body was used as a map and the address arithmetic re-derived independently from the
// listing per house rules.
//
#include "sim/sim_target_class.h"


namespace mh::sim {

namespace detail {

int32_t target_class(const sim_view &v, uint32_t owner_and_kind_flag, int32_t roster_slot) {
    // 0x0044961d-0x00449624: TEST ...,0xa0; JZ -- if neither unit-kind bit is set (including every
    // BUILDING ref, which never sets 0x80/0x20), return the default GROUND without reading `units`.
    // 0x00449626-0x00449642: else AND the low nibble for the owning player (ref_owner(), same
    // REF_OWNER_MASK sim_state.h already names) and test that unit's elevation. Both guards must
    // hold for AIR -- a single `&&`, not two independent early-outs (both fall-through paths land on
    // the same GROUND default).
    if (((owner_and_kind_flag & 0xa0u) != 0) &&
        (unit_of(v, ref_owner(owner_and_kind_flag), roster_slot).elevation != 0)) {
        return WEAPON_TARGET_AIR;
    }
    return WEAPON_TARGET_GROUND;
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

int32_t target_class(uint32_t owner_and_kind_flag, int32_t roster_slot) {
    const sim_view v = state().read;
    return detail::target_class(v, owner_and_kind_flag, roster_slot);
}


} // namespace mh::sim
