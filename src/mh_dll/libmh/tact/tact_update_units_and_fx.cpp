//
// tact/tact_update_units_and_fx.cpp -- see tact_update_units_and_fx.h. Translated from the
// DISASSEMBLY (tmp/decomp_tact/llm_tact_update_units_and_fx_0042aff2.asm), not from Ghidra's .c.
//
#include "tact/tact_update_units_and_fx.h"

#include "tact/tact_fx_update_projectile.h" // mh::tact::fx_update_projectile's public wrapper
#include "tact/tact_unit_weapons_tick.h"    // mh::tact::unit_weapons_tick's public wrapper

namespace mh::tact {

namespace detail {

void update_units_and_fx(const tact_view &v, tact_store &own) {
    // @0x0042b00a: first of two redundant resets -- see step 2 below and the header banner.
    own.move_path_cache_valid() = 0;

    // @0x0042b014-0x0042b064: SCAN 1 -- unit slots [1, 0x80] inclusive.
    for (int32_t i = TACT_UNIT_FIRST_SLOT; i <= TACT_UNIT_LAST_SLOT; ++i) {
        // @0x0042b02e: SECOND reset, inside the loop body, on every iteration regardless of the
        // type gate below -- PRESERVED literally (redundant in the original, do not dedupe).
        own.move_path_cache_valid() = 0;

        // @0x0042b03f-0x0042b056: call gate is the CONJUNCTION of both threshold checks --
        // type == 0 is excluded by the first (JBE), type >= 0x80 is excluded by the second (JC) --
        // and both gate the SAME single call site (0x0042b05d). See the header banner for the
        // opcode-level derivation.
        const uint8_t type = v.units[i].type;
        if (type > 0 && type < 0x80) {
            // @0x0042b05a-0x0042b05d: already-translated TACT1C sibling, via its public wrapper.
            mh::tact::unit_weapons_tick(i);
        }
    }

    // @0x0042b064-0x0042b095: SCAN 2 -- fx-pool slots [0, TACT_FX_POOL_SLOTS).
    for (int32_t i = 0; i < TACT_FX_POOL_SLOTS; ++i) {
        // @0x0042b082-0x0042b089: fx_type == 0 means a free slot (see mh_llm_tact_fx::fx_type's own
        // Ghidra comment) -- skip; otherwise update the slot.
        if (v.fx_pool[i].fx_type != 0) {
            // @0x0042b08b-0x0042b08e: already-translated TACT1C sibling, via its public wrapper.
            mh::tact::fx_update_projectile(i);
        }
    }
}

} // namespace detail

void update_units_and_fx() {
    tact_state st = state();
    detail::update_units_and_fx(st.read, st.own);
}


} // namespace mh::tact
