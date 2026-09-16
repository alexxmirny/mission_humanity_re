//
// sim/sim_unit_state_idle_scatter.cpp -- see sim_unit_state_idle_scatter.h. Translated from the
// DISASSEMBLY (tmp/decomp_sim/llm_strat_unit_state_idle_scatter_00485ebb.asm), not from the Ghidra .c
// draft (see the header banner for the storage-approach-tile out-param mix-up in the draft).
//
#include "sim/sim_unit_state_idle_scatter.h"

#include "addr/mh_calls.gen.h"     // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"           // ai_say / trace_budget -- the shared trace sink, not AI state
#include "sim/sim_order_enqueue.h" // UNIT_TYPE_A_HELI/H_HELI/_CARGO, UNIT_STATE_HOVER_ENGAGE, _PATROL_SWAP
#include "addr/mh_rebind.gen.h"    // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const unit_state_idle_scatter_calls &live_unit_state_idle_scatter_calls() {
    static const unit_state_idle_scatter_calls c = {
        MH_LIBMH_BIND(llm_strat_unit_set_state),
        MH_LIBMH_BIND(llm_rand_below),
        MH_LIBMH_BIND(llm_strat_storage_get_approach_tile),
        MH_LIBMH_BIND(llm_strat_unit_set_state_order),
    };
    return c;
}

namespace detail {

void unit_state_idle_scatter(const sim_view &v, sim_store &own, const unit_state_idle_scatter_calls &c) {
    unit          &u          = own.cur_unit(); // _G_LLM_STRAT_CUR_UNIT dereferenced -- see sim_view::cur_unit's comment.
    const uint16_t player     = *v.cur_player;
    const uint16_t unit_index = *v.cur_index;

    // 0x00485ed3-0x00485ef2: a queued order is about to replace this state -- do nothing this tick.
    if (u.order_queued != 0) {
        own.tick_budget() = 0.0;
        return;
    }

    // 0x00485ef7-0x00485fc3: heli-class units never scatter -- they hover in place instead. FOUR
    // near-identical CMP/JZ arms in the assembly (each re-deriving Unit[unit_proto_id] fresh rather
    // than caching it -- 0x00485f13/0x00485f45/0x00485f79/0x00485fad), collapsed to one OR test here,
    // matching sim_order_enqueue.cpp's already-established identical four-value membership test on this
    // same field (UNIT_TYPE_A_HELI/H_HELI/A_HELI_CARGO/H_HELI_CARGO from sim/sim_order_enqueue.h).
    const uint32_t proto_type = v.cfg_units[u.unit_proto_id].type;
    if (proto_type == UNIT_TYPE_A_HELI || proto_type == UNIT_TYPE_H_HELI ||
        proto_type == UNIT_TYPE_A_HELI_CARGO || proto_type == UNIT_TYPE_H_HELI_CARGO) {
        c.unit_set_state(UNIT_STATE_HOVER_ENGAGE); // 0x00485fc3-0x00485fcd
        return;
    }

    // 0x00485fd2-0x00485ffa: draw a random (dx,dy) offset, EACH AXIS its own independent
    // `llm_rand_below(5) - 2` draw (range [-2,2]) -- dx FIRST (stored at the loop's own EBP-0x1c slot),
    // then dy (EBP-0x18) -- re-rolling the WHOLE PAIR only while BOTH are exactly zero (`OR EAX,EDX;
    // TEST;JZ` on dx|dy, so a single nonzero axis is enough to exit the loop). Draw order is
    // load-bearing for the shared PRNG stream, same as every other rand_below site in this codebase.
    int32_t dx, dy;
    do {
        dx = c.rand_below(5) - 2;
        dy = c.rand_below(5) - 2;
    } while (dx == 0 && dy == 0);

    // 0x00485ffc-0x0048606c: goal_x/goal_y <- current tile + (dx,dy), each axis independently
    // torus-wrapped. The asm does the add as an 8-bit register ADD (byte wraparound) BEFORE the mask
    // AND; doing the add at full width and masking/truncating afterward is provably the same low byte
    // (AND is bitwise, so the low-8-bit result of `(x+dx) & mask` depends only on the low 8 bits of
    // each operand, regardless of what happens above bit 7) -- same idiom
    // sim_unit_state_hover_engage.cpp's own goal_x/goal_y sidestep write already uses. home_x/home_y <-
    // current x/y UNCONDITIONALLY (order matters: stamped from the CURRENT tile here, before the
    // possible goal override below, which never touches home).
    u.goal_x = static_cast<uint8_t>(map_width_mask(v) & static_cast<uint32_t>(u.x + dx));
    u.goal_y = static_cast<uint8_t>(map_height_mask(v) & static_cast<uint32_t>(u.y + dy));
    u.home_x = u.x;
    u.home_y = u.y;

    if (u.order == IDLE_SCATTER_ORDER_LANDING_REQUEST && u.home_storage_slot != 0) {
        // 0x00486085-0x004860da: mid-landing-request with a real home dock slot -- override the
        // scatter goal with the slot's actual approach tile (storage_idx=0 => "use the unit's own
        // home_storage_slot", see sim_storage_get_approach_tile.h) and re-enter the move-op state
        // directly (order stays LANDING_REQUEST). See the header banner for why the two out-param
        // locals below carry no literal stack offset despite the .c draft mislabeling its own.
        uint32_t out_fine_x = 0, out_fine_y = 0;
        c.storage_get_approach_tile(player, unit_index, &out_fine_x, &out_fine_y, 0u);
        u.goal_x = static_cast<uint8_t>(out_fine_x);                                       // 0x004860a0-0x004860a9
        u.goal_y = static_cast<uint8_t>(out_fine_y);                                       // 0x004860af-0x004860b8
        c.unit_set_state(static_cast<uint16_t>(v.cfg_units[u.unit_proto_id].move_op_arg)); // 0x004860be-0x004860da
    } else {
        // 0x004860dc-0x004860fc: plain scatter -- issue the move-op order with a PATROL_SWAP state,
        // keeping the scatter goal computed above.
        c.unit_set_state_order(static_cast<uint16_t>(v.cfg_units[u.unit_proto_id].move_op_arg),
                               UNIT_STATE_PATROL_SWAP);
    }
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

void unit_state_idle_scatter() {
    sim_state st = state();
    detail::unit_state_idle_scatter(st.read, st.own, live_unit_state_idle_scatter_calls());
}


} // namespace mh::sim
