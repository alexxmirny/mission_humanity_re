//
// sim/sim_order_issue_0xf_adjacent.cpp -- see sim_order_issue_0xf_adjacent.h. Translated from the
// DISASSEMBLY (tmp/decomp_sim/llm_strat_order_issue_0xf_adjacent_{by_offset,enqueue}_*.asm), not from
// the Ghidra .c drafts (whose plate comments carry a stale "unaff_EBX artifact" note -- see the header
// banner for the correction).
//
#include "sim/sim_order_issue_0xf_adjacent.h"

#include "addr/mh_calls.gen.h"     // typed callables for the original functions we still call OUT to
#include "sim/sim_order_enqueue.h" // ORDER_KIND_UNIT (shared there; same 0x80 owner-tag idiom every
#include "addr/mh_rebind.gen.h"    // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"
// order-issue site in this codebase uses)

namespace mh::sim {

const order_issue_0xf_adjacent_calls &live_order_issue_0xf_adjacent_calls() {
    static const order_issue_0xf_adjacent_calls c = {
        MH_LIBMH_BIND(llm_strat_tiles_adjacent),
        MH_LIBMH_BIND(llm_strat_order_scratch_reset),
        MH_LIBMH_BIND(llm_strat_order_scratch_set_field),
        MH_LIBMH_BIND(llm_strat_order_enqueue),
        MH_LIBMH_BIND(llm_strat_unit_notify_status),
    };
    return c;
}

namespace detail {

void order_issue_0xf_adjacent_enqueue(const sim_view &v, const order_issue_0xf_adjacent_calls &c,
                                      uint16_t player, int32_t unit_idx, int32_t target_x,
                                      int32_t target_y) {
    const unit &u = unit_of(v, static_cast<uint32_t>(player), unit_idx);

    // 0x0046a346-0x0046a36d: gate -- Unit[unit.unit_proto_id].move_op_code == 0xf (the ground-move
    // class tag; see the field's own comment in addr/mh_structs.gen.h). No-op if this unit's type is
    // not a ground mover.
    if (v.cfg_units[u.unit_proto_id].move_op_code != 0xf) {
        return;
    }

    // 0x0046a373-0x0046a3b4: gate -- is (target_x,target_y) adjacent to the unit's own current tile?
    // No-op if not.
    if (c.tiles_adjacent(target_x, target_y, static_cast<int32_t>(u.x), static_cast<int32_t>(u.y)) ==
        0) {
        return;
    }

    // 0x0046a3b6-0x0046a3f6: stage the target tile into order scratch fields 0/1, enqueue order
    // 0xf/0x36 tagged unit-owned (owner_and_kind = player | 0x80), then notify the unit.
    c.order_scratch_reset();
    c.order_scratch_set_field(0, target_x);
    c.order_scratch_set_field(1, target_y);
    c.order_enqueue(static_cast<uint16_t>(unit_idx),
                    static_cast<uint16_t>(static_cast<uint32_t>(player) | ORDER_KIND_UNIT), 0xf, 0x36);
    c.unit_notify_status(player, unit_idx, 0);
}

void order_issue_0xf_adjacent_by_offset(const sim_view &v, const order_issue_0xf_adjacent_calls &c,
                                        uint32_t player, int32_t unit_idx, int32_t dx, int32_t dy) {
    const unit &u = unit_of(v, player, unit_idx);

    // 0x0046a1be-0x0046a205: new_x = width_mask & (unit.x + dx); new_y = height_mask & (unit.y + dy).
    // Both additions are done at full (unsigned) register width before the mask, matching the .asm's
    // ADD-then-AND order -- a negative dx/dy wraps via two's complement exactly like the original.
    const uint32_t new_x = map_width_mask(v) & (static_cast<uint32_t>(u.x) + static_cast<uint32_t>(dx));
    const uint32_t new_y =
        map_height_mask(v) & (static_cast<uint32_t>(u.y) + static_cast<uint32_t>(dy));

    // 0x0046a208-0x0046a215: falls straight through into order_issue_0xf_adjacent_enqueue's own entry
    // -- a direct call to this TU's own detail:: function, not through `c`/`mh::call::` (see header
    // banner).
    order_issue_0xf_adjacent_enqueue(v, c, static_cast<uint16_t>(player), unit_idx,
                                     static_cast<int32_t>(new_x), static_cast<int32_t>(new_y));
}

} // namespace detail

// ---- the public wrappers ---------------------------------------------------------------------------

void order_issue_0xf_adjacent_enqueue(uint16_t player, int32_t unit_idx, int32_t target_x,
                                      int32_t target_y) {
    const sim_view v = state().read;
    detail::order_issue_0xf_adjacent_enqueue(v, live_order_issue_0xf_adjacent_calls(), player, unit_idx,
                                             target_x, target_y);
}

void order_issue_0xf_adjacent_by_offset(uint32_t param_1, int32_t param_2, int32_t a2,
                                        int32_t param_4) {
    const sim_view v = state().read;
    detail::order_issue_0xf_adjacent_by_offset(v, live_order_issue_0xf_adjacent_calls(), param_1,
                                               param_2, a2, param_4);
}


} // namespace mh::sim
