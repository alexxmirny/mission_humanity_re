//
// sim/sim_locate_active_port.cpp -- see sim_locate_active_port.h. Translated from the DISASSEMBLY
// (tmp/decomp/llm_strat_locate_active_port_0048fbfa.asm), not from the Ghidra .c draft (the draft's
// overall shape reads correctly and was independently re-walked byte-for-byte -- see the header).
//
#include "sim/sim_locate_active_port.h"

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"        // ai_say / trace_budget -- the shared trace sink, not AI state
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const locate_active_port_calls &live_locate_active_port_calls() {
    static const locate_active_port_calls c = {
        MH_LIBMH_BIND(llm_strat_tile_neighbor_reverse_dir),
    };
    return c;
}

namespace detail {

uint32_t locate_active_port(const sim_view &v, const locate_active_port_calls &c, uint32_t player,
                            int32_t *out_col, int32_t *out_row, uint32_t *out_port_slot) {
    // 0x0048fc1b: MOVZX word -- every roster index below reads only the low 16 bits of player.
    const uint32_t p = player & 0xffffu;

    // 0x0048fc1b-0x0048fc2c: buildings[player][0].index -- the roster's SENTINEL/COUNT slot. See the
    // header banner: this counter is INCREMENTED below, the reverse of every sibling roster-scan idiom
    // in this codebase -- reproduced exactly, not "fixed" to decrement.
    uint32_t remaining = static_cast<uint32_t>(static_cast<uint16_t>(building_of(v, p, 0).index));

    int32_t slot = 1;
    for (;;) {
        // 0x0048fc36-0x0048fc42: the loop-top OR exit. `remaining == 0` can only fire here on the very
        // first check (see header banner) since the loop never decrements it.
        if (slot > 99 || remaining == 0) {
            return 0;
        }

        const building &b = building_of(v, p, slot);

        // 0x0048fc62-0x0048fc6d: `!(energy <= 0.0)`, NOT `0.0 < energy` -- FLDZ/FCOMP/FNSTSW/SAHF/JNC.
        // Traced through the FPU condition codes (C0->CF via FNSTSW/SAHF): JNC (taken = skip to the
        // next slot) fires exactly when ST(0)>=Source, i.e. energy<=0; it does NOT fire (falls through
        // to this body) on the unordered/NaN case, matching `!(energy <= 0.0)` and NOT `0.0 < energy`
        // (which would be false for NaN and take the wrong branch) -- same
        // FLDZ/FCOMP/FNSTSW/SAHF/JNC-to-`!(x <= 0.0)` correction sim_bldg_find_mothership_position.cpp
        // and sim_bldg_mother_reelect_primary.cpp already document for the identical idiom. No
        // reachable state is known to put a NaN in this field; this is a zero-cost faithfulness match.
        if (!(b.energy <= 0.0)) {
            // 0x0048fc73-0x0048fc76: incremented UNCONDITIONALLY once energy>0, BEFORE the type gate
            // below -- a comma-operator side effect in the Ghidra draft, preserved in that order even
            // though it has no further bearing on this iteration once the type gate fails.
            ++remaining;

            // 0x0048fc7d-0x0048fcc9: cfg type gate, A_PORT checked first, H_PORT second.
            const uint8_t bldg_type = v.cfg_buildings[b.building_id].type;
            if ((bldg_type == BUILDING_TYPE_A_PORT || bldg_type == BUILDING_TYPE_H_PORT) &&
                // 0x0048fccb-0x0048fce5: built_flags == 3 (connected AND staffed).
                b.built_flags == 3 &&
                // 0x0048fce9-0x0048fd04: online_state != 0.
                b.online_state != 0) {
                // ---- FOUND (0x0048fd0b-0x0048fdac) -----------------------------------------------
                *out_port_slot = static_cast<uint32_t>(b.sub_id);

                const unit_storage &s      = storage_of(v, p, static_cast<int32_t>(*out_port_slot));
                const building     &owner  = building_of(v, p, s.b_index);
                const uint32_t      facing = v.cfg_buildings[owner.building_id].door_approach_route[0];

                // Register mapping at the call site: EAX=exit_tile_x, EDX=exit_tile_y, EBX=facing,
                // ECX=out_col, [stack]=out_row -- see the header banner for the byte-level derivation.
                // SIGNEDNESS MISMATCH (TACT1-P C6): out_col/out_row are this function's own committed
                // `int32_t *`, but tile_neighbor_reverse_dir's committed out_x/out_y are `uint32_t *` --
                // same memory, reinterpreted at the boundary rather than widened/narrowed in value.
                c.tile_neighbor_reverse_dir(s.exit_tile_x, s.exit_tile_y, static_cast<int32_t>(facing),
                                            reinterpret_cast<uint32_t *>(out_col),
                                            reinterpret_cast<uint32_t *>(out_row));
                // 0x0048fdb1: result is a LITERAL 1, unconditionally -- the callee's own return value
                // (if any) is discarded.
                return 1;
            }
        }

        // 0x0048fdba -> 0x0048fc47: the shared "next slot" tail (energy<=0, or any of the three gates
        // above missed).
        ++slot;
    }
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

uint32_t locate_active_port(uint32_t player, int32_t *out_col, int32_t *out_row, uint32_t *out_port_slot) {
    const sim_view v = state().read;
    return detail::locate_active_port(v, live_locate_active_port_calls(), player, out_col, out_row,
                                      out_port_slot);
}


} // namespace mh::sim
