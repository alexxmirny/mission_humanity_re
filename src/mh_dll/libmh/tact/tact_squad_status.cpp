//
// tact/tact_squad_status.cpp -- see tact_squad_status.h. Translated from the DISASSEMBLY, not from
// Ghidra's C.
//
#include "tact/tact_squad_status.h"


namespace mh::tact {
namespace detail {

void squad_sync_hp(const tact_view &v, tact_store &own) {
    for (int32_t slot = 0; slot < *v.squad_size; ++slot) {
        // @0x00438f40-0x00438f4d: skip entirely (no write) when the slot's OWN blackboard entry
        // already reads <= 0 -- the "unused slot" convention.
        auto &status = own.squad_status_at(slot);
        if (status.energy_pct <= 0) {
            continue;
        }

        // 1-based unit index, matching TACT_UNIT_FIRST_SLOT.
        const tact_unit &u = unit_of(v, slot + 1);

        if (u.hp == 0) {
            // @0x00438f5d-0x00438f6d.
            status.energy_pct = 0;
        } else if (u.owner != 0) {
            // @0x00438f86-0x00438fec.
            status.energy_pct = 0;
        } else {
            // @0x00438f9c-0x00438fd5: signed hp*100 / character_types[u.type].energy, floored at 1
            // as a SEPARATE branch (not the IDIV's own truncation direction).
            int32_t pct = (int32_t)((int32_t)u.hp * 100) / (int32_t)v.character_types[u.type].energy;
            if (pct == 0) {
                pct = 1;
            }
            status.energy_pct = pct;
        }
    }
}

} // namespace detail

void squad_sync_hp() {
    tact_state st = state();
    detail::squad_sync_hp(st.read, st.own);
}

} // namespace mh::tact


namespace mh::tact {


} // namespace mh::tact
