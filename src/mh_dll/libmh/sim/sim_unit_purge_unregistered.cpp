//
// sim/sim_unit_purge_unregistered.cpp -- see sim_unit_purge_unregistered.h. Translated from the
// DISASSEMBLY (tmp/decomp/llm_strat_unit_purge_unregistered_00499bd0.asm), not from Ghidra's C draft:
// the draft's do-while exit condition re-derives the just-fetched chain link's owner/unit_id from a
// SECOND record fetch that the assembly never performs (see the header's derivation and
// uncertainties), and its energy gate reads as `0.0 < energy` where the raw FCOMP/SAHF/JC idiom is
// NaN-inclusive on the "keep processing" side -- both re-walked against the raw CMP/Jcc/FCOMP targets
// rather than trusted.
//
#include "sim/sim_unit_purge_unregistered.h"

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"        // ai_say / trace_budget -- the shared trace sink, not AI state
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

namespace {

// tile_object::unit and unit::unit_above are both `uint8_t[2]` in the generated header (the struct
// generator renders map_t_unit_full_id as a raw byte pair, not a scalar) -- this reassembles the
// little-endian word the original addresses with a single `MOV AX, word ptr [...]`. High nibble (bits
// 12-15) = owning player, low 12 bits = roster index. Same idiom as ai_spiral_scan.cpp's and
// ai_holding_pen.cpp's own local helpers of the same name; not shared across translation units (each
// TU that needs it defines its own, per the "no new shared helpers" rule -- this one is a trivial
// byte-pair reassembly, not new logic, and libmh/sim/ does not depend on libmh/ai/ regardless).
inline uint16_t unit_full_id_word(const uint8_t (&packed)[2]) {
    return (uint16_t)(packed[0] | (packed[1] << 8));
}

} // namespace

const purge_unregistered_calls &live_purge_unregistered_calls() {
    static const purge_unregistered_calls gc = {
        MH_LIBMH_BIND(llm_unit_state_is_boarding),
    };
    return gc;
}

namespace detail {

void unit_purge_unregistered(const sim_view &v, sim_store &own, const purge_unregistered_calls &gc,
                             uint32_t player) {
    // 0x00499bf2-0x00499c03: increment-then-recheck against 0x64 (100), starting at 1 -- a plain
    // `for (i = 1; i < 100; ++i)`, nothing to re-derive about the loop SHAPE (unlike the inner do-while
    // below).
    for (int32_t i = 1; i < 100; ++i) {
        // `own`, not `unit_of(v, ...)`: this slot is the one WRITTEN below (pending_damage).
        unit &u = own.unit_at(player, i);

        // 0x00499c15/0x1d: unsigned word compare against 0 (JBE on an unsigned value can only fire on
        // equality) -- unit_proto_id == 0 skips the slot.
        if (u.unit_proto_id == 0) continue;

        // 0x00499c2f-0x3a: FLDZ / FCOMP energy / FNSTSW / SAHF / JC-continue. See the header's
        // derivation -- `energy <= 0.0` (false for NaN) is the correct reproduction of the NaN-keeps-
        // processing behaviour this idiom has, NOT `!(energy > 0.0)` (true for NaN, which would skip).
        if (u.energy <= 0.0) continue;

        bool registered = false;

        // 0x00499c58-0x6c: Unit[unit_proto_id].type, SIGNED JLE against UNIT_TYPE_A_HELI-1 (0xe).
        if ((int32_t)v.cfg_units[u.unit_proto_id].type < (int32_t)UNIT_TYPE_A_HELI) {
            // 0x00499d1c-0x00499d66: building-slot types -- registered iff this tile's `building`
            // field names this exact roster slot.
            if (tile_at(v, u.x, u.y).building == (uint32_t)i) registered = true;
        } else {
            // 0x00499c72-0x00499d1a: mobile types -- walk the tile's packed unit-stack chain looking
            // for a self-reference (owner == player (the function's own parameter, NOT the chain
            // link's own owner from a prior step) && link_id == i). See the header's derivation for
            // why this is a plain `link == 0 || registered` do-while and not the .c draft's
            // two-condition reconstruction.
            uint16_t link = unit_full_id_word(tile_at(v, u.x, u.y).unit);
            do {
                const uint32_t owner   = (uint32_t)((link & 0xf000u) >> 12);
                const uint32_t link_id = (uint32_t)(link & 0x0fffu);
                if (owner == player && link_id == (uint32_t)i) registered = true;
                link = unit_full_id_word(unit_of(v, owner, (int32_t)link_id).unit_above);
            } while (link != 0 && !registered);
        }

        // 0x00499d67-0x00499d8b: not registered -> always purge; registered -> purge only if the unit
        // is currently boarding. `state` zero-extended to 32 bits for the call, matching the asm's
        // `MOVZX EAX, word ptr state` before the CALL (same cast sim_order_dispatch.cpp uses for the
        // identical callee).
        if (!registered || gc.unit_state_is_boarding((int32_t)(uint32_t)u.state) != 0) {
            // 0x00499d8d-0x00499db3: FLD energy / FSTP pending_damage -- a plain double copy.
            u.pending_damage = u.energy;
        }
    }
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

void unit_purge_unregistered(uint32_t player) {
    sim_state st = state();
    detail::unit_purge_unregistered(st.read, st.own, live_purge_unregistered_calls(), player);
}


} // namespace mh::sim
