//
// sim/sim_path_make_single_step.cpp -- see sim_path_make_single_step.h. Translated from the
// DISASSEMBLY (tmp/decomp_sim/llm_strat_path_make_single_step_0049561f.asm), not from any Ghidra .c
// draft -- see the header banner for the full field/register derivation.
//
#include "sim/sim_path_make_single_step.h"

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"        // ai_say / trace_budget -- the shared trace sink, not AI state
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const path_make_single_step_calls &live_path_make_single_step_calls() {
    static const path_make_single_step_calls c = {
        MH_LIBMH_BIND(llm_strat_path_free_slot),
        MH_LIBMH_BIND(llm_strat_tile_delta_wrapped),
        MH_LIBMH_BIND(llm_strat_path_attach_slot),
    };
    return c;
}

namespace detail {

int32_t path_make_single_step(const sim_view &v, sim_store &own, const path_make_single_step_calls &c,
                              uint32_t player, int32_t unit_index) {
    unit &u = own.unit_at(player, unit_index);

    // 0x0049564f-0x00495663: release any path slot the unit currently holds before recomputing one.
    if (u.path_slot_id != 0xff) {
        c.path_free_slot((uint16_t)player, unit_index);
    }

    // 0x00495664-0x004956d4: signed torus delta from the unit's GOAL to its CURRENT tile position
    // (x1=goal_x, y1=goal_y, x2=x, y2=y -- see the header's register/offset derivation).
    int32_t delta_x = 0;
    int32_t delta_y = 0;
    c.tile_delta_wrapped(u.goal_x, u.goal_y, u.x, u.y, &delta_x, &delta_y);

    // 0x004956d9-0x00495723: scan the 8 principal facing_step_offset entries (i = 1,4,7,...,22, one
    // per 24-way-heading octant) for the one whose (dx,dy) exactly equals (delta_x,delta_y); first
    // match wins.
    int32_t found_heading = 0;
    for (int32_t i = 1; i <= 0x18; i += 3) {
        const facing_step_offset_pair &step = v.facing_step_offset[i];
        if (step.dx == delta_x && step.dy == delta_y) {
            found_heading = i;
            break;
        }
    }
    if (found_heading == 0) return 0; // 0x00495723-0x00495730: no exact-direction match

    // 0x00495735-0x0049580a: find the player's first free path slot.
    for (int32_t slot = 0; slot < PATH_SLOTS_PER_PLAYER; ++slot) {
        if (own.path_slot_flag_at((int32_t)player, slot) == 1) continue; // 0x00495760: busy, try next

        // 0x00495766-0x004957af: seed the path buffer with a single one-tile waypoint. entry1's
        // run_length is deliberately left untouched -- the asm writes only these three bytes.
        own.path_buffer_at(player, slot, 0).heading    = (uint8_t)found_heading;
        own.path_buffer_at(player, slot, 0).run_length = 1;
        own.path_buffer_at(player, slot, 1).heading    = 0;

        // 0x004957b6-0x004957c4: claim the slot (PATH_SLOT_FLAGS[player][slot]=1,
        // unit.path_slot_id=slot, PATH_FREE_SLOT_COUNT[player]--), all inside the ORIGINAL callee.
        c.path_attach_slot((int32_t)player, unit_index, slot);

        u.path_cursor              = 0; // 0x004957d8 (int32 field)
        u.path_blocked_retry_count = 0; // 0x004957f5 (byte field)
        return 1;                       // 0x004957fc
    }
    return 0; // 0x0049580a: all 100 slots busy
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

int32_t path_make_single_step(uint32_t player, int32_t unit_index) {
    sim_state st = state();
    return detail::path_make_single_step(st.read, st.own, live_path_make_single_step_calls(), player,
                                         unit_index);
}


} // namespace mh::sim
