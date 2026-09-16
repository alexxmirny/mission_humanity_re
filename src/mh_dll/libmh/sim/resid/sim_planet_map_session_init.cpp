//
// sim/resid/sim_planet_map_session_init.cpp -- see sim_planet_map_session_init.h. Translated from
// the DISASSEMBLY
// (tmp/decomp_sim_resid/llm_strat_planet_map_session_init_004dc65a.asm), the Ghidra .c being a
// draft.
//
#include "sim/resid/sim_planet_map_session_init.h"

#include "addr/mh_calls.gen.h" // typed callables for the effectful/frontier originals we still call OUT to
#include "state/host_api.h"
#include "state/host_events.h"  // LIFT-TABLE S4: the palette block crosses as ONE invalidate record
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const planet_map_session_init_calls &live_planet_map_session_init_calls() {
    static const planet_map_session_init_calls c = {
        MH_LIBMH_BIND(llm_strat_rng_seed_channel),
        MH_LIBMH_BIND(llm_strat_bldg_recompute_cell_grid),
        MH_LIBMH_BIND(llm_strat_ai_spiral_table_init),
    };
    return c;
}

namespace detail {

// ---- llm_strat_planet_map_session_init @0x004dc65a ------------------------------------------------
void planet_map_session_init(const sim_view &v, sim_store &own, const planet_map_session_init_calls &c) {
    // 0x004dc666-0x004dc677: width_m/height_m = map_width/map_height - 1. See the header's
    // declared_needs note -- own.width_m_mut()/height_m_mut() are new accessors this translation
    // needs added to sim_store (RID_WIDTH_M / RID_HEIGHT_M's write side).
    own.width_m_mut()  = static_cast<uint32_t>(*v.map_width - 1);
    own.height_m_mut() = static_cast<uint32_t>(*v.map_height - 1);

    // 0x004dc67c: frontier init helper, no args, no return used.
    c.bldg_recompute_cell_grid();

    // 0x004dc681-0x004dc68a: reseed RNG channel 2 with value 0.
    c.rng_seed_channel(2, 0);

    // 0x004dc68d: reset the AI active-player high-water mark.
    own.ai_active_player_count() = 0;

    // 0x004dc697: frontier init helper, no args, no return used.
    c.ai_spiral_table_init();

    // ---- THE PALETTE BLOCK LEAVES (LIFT-TABLE S4, 2026-09-09) -----------------------------------
    //
    // 0x004dc69c-0x004dc744 was nine llm_gfx_pack_rgb16 calls filling nine MF_VIEW/OWN_ISLAND cells
    // -- presentation state sitting in the sim store because migration put it there, and
    // pack_rgb16's ONLY libmh caller. R1, not R3's internalize branch (docs/libmh-abi.md sec 0a):
    // the cut goes above the leaf, the block leaves with its cells, and colour packing never enters
    // the deterministic core. The nine calls, their nine literal triples and their nine stores are
    // now in the hosted sink (seams/host_event_sink.cpp, LIBMH_EVK_INV_PLANET_MAP_PALETTE), where
    // the (red, blue, green) argument order is spelled out.
    //
    // MEASURED, and it is why the record carries no payload and why sec 6's UI-capture oracle for
    // this stage does not exist: all nine cells are WRITE-ONLY IMAGE-WIDE. One WRITE xref each
    // (checked against the reference manager, not just src/mh_dll), no reader in libmh, in the
    // seams, or in the original. Nothing renders from them, so no capture could go red for a wrong
    // colour; the oracle is that census plus the R5 hosted arm.
    mh::state::evt::inv_planet_map_palette();
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

void planet_map_session_init() {
    sim_state st = state();
    detail::planet_map_session_init(st.read, st.own, live_planet_map_session_init_calls());
}

} // namespace mh::sim
