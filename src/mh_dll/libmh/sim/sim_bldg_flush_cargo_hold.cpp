#include "sim/sim_bldg_flush_cargo_hold.h"

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"        // ai_say / trace_budget -- the shared trace sink, not AI state
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const bldg_flush_cargo_hold_calls &live_bldg_flush_cargo_hold_calls() {
    static const bldg_flush_cargo_hold_calls c = {
        MH_LIBMH_BIND(llm_prod_shuttle_bay_unload_all),
        MH_LIBMH_BIND(llm_strat_prod_shuttle_slot_release),
        MH_LIBMH_BIND(game_SetEvent),
    };
    return c;
}

namespace detail {

void bldg_flush_cargo_hold(const sim_view &v, sim_store &own, const bldg_flush_cargo_hold_calls &c,
                           uint32_t player, int32_t building_index) {
    // The original stores the full EAX to [EBP-0x14] at 0x0048e05d but re-narrows it to the LOW 16
    // BITS at every subsequent use -- MOVZX EAX/EDX, word ptr [EBP-0x14] at 0x0048e066/06f/089/092/
    // 0c7/0fc/131 before every record-address computation and the prod_shuttle_slot_release call, and
    // CMP AX at 0x0048e14e for the PlayerSide compare. The dword is never read whole again. Mask ONCE
    // here (a real truncation, not the reinterpret static_cast<int32_t> would give) so a caller with
    // garbage in bits 16-31 cannot drive an out-of-bounds record index -- same defect and fix the
    // SIM1D found in llm_strat_production_complete.
    const uint16_t p = static_cast<uint16_t>(player);

    // 0x0048e063-0x0048e06a: llm_prod_shuttle_bay_unload_all(player, building_index) -- SIBLING in
    // this SCC (a ring member per tmp/ring_context.md), called via `c` to the ORIGINAL, never through
    // mh::sim::. Runs BEFORE the shuttle_slot read below, matching the asm's order.
    c.bay_unload_all(p, building_index);

    // 0x0048e082: buildings[player][building_index].shuttle_slot, read ONCE and reused for the
    // remaining three re-reads the asm performs (0x0048e0a5/0x0048e0da/0x0048e10f) -- see the
    // header's THE SHUTTLE_SLOT RE-READ note for why a single read is behaviourally identical (the
    // only writer of this field in the whole sim closure is THIS function, at step 5 below, and
    // nothing runs between the first read and the last that could touch it).
    const uint32_t slot = building_of(v, p, building_index).shuttle_slot;

    // 0x0048e089-0x0048e08d: llm_strat_prod_shuttle_slot_release(player, slot) -- SIBLING in this
    // SCC (the already-migrated llm_strat_prod_shuttle_slot_release), called via `c` to the
    // ORIGINAL, never through mh::sim::.
    c.prod_shuttle_slot_release(static_cast<int32_t>(p), static_cast<int32_t>(slot));

    // 0x0048e0be/0x0048e0f3/0x0048e128: clear the slot record's leading three fields to 0, in the
    // asm's own write order (type_ref_id, then src_building_type, then src_building_index -- back-
    // derived from the three literal write addresses [0xbd2172, 0xbd2174, 0xbd2170] against
    // mh_structs.gen.h's static_assert'd offsets 0x16/0x18/0x14, all relative to the same record
    // base 0xbd215c).
    prod_shuttle_slot &rec = own.prod_shuttle_slot_at(p, static_cast<int32_t>(slot));
    rec.type_ref_id        = 0;
    rec.src_building_type  = 0;
    rec.src_building_index = 0;

    // 0x0048e144: clear buildings[player][building_index].shuttle_slot LAST -- after every read of
    // the old value above has already happened.
    own.building_at(p, building_index).shuttle_slot = 0;

    // 0x0048e14e-0x0048e15c: refresh the build-projects UI only if `player` is the local human
    // player (PlayerSide, 16-bit equality -- established idiom across libmh/sim/, e.g.
    // sim_bldg_notify_ui.cpp/sim_unit_on_destroyed.cpp).
    if (static_cast<int16_t>(p) == *v.player_side) {
        c.set_event(BLDG_FLUSH_CARGO_HOLD_BUILD_PROJECTS_REFRESH);
    }
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

void bldg_flush_cargo_hold(uint32_t player, int32_t building_index) {
    sim_state st = state();
    detail::bldg_flush_cargo_hold(st.read, st.own, live_bldg_flush_cargo_hold_calls(), player,
                                  building_index);
}


} // namespace mh::sim
