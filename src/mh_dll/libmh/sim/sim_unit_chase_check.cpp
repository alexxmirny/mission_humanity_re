//
// sim/sim_unit_chase_check.cpp -- see sim_unit_chase_check.h. Translated from the DISASSEMBLY
// (tmp/decomp_sim/llm_strat_unit_chase_check_004866fb.asm), not from the Ghidra .c draft.
//
#include "sim/sim_unit_chase_check.h"

#include "addr/mh_calls.gen.h"            // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"                  // ai_say / trace_budget -- the shared trace sink, not AI state
#include "sim/sim_target_class.h"         // mh::sim::target_class -- reimplemented sibling
#include "sim/sim_unit_fire_at_target.h"  // mh::sim::unit_fire_at_target -- reimplemented sibling, THIS batch
#include "sim/sim_unit_in_weapon_range.h" // mh::sim::unit_in_weapon_range -- reimplemented sibling
#include "sim/sim_unit_set_state.h"       // mh::sim::unit_set_state -- reimplemented sibling
#include "sim/sim_unit_set_state_order.h" // mh::sim::unit_set_state_order -- reimplemented sibling
#include "addr/mh_rebind.gen.h"           // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const unit_chase_check_calls &live_unit_chase_check_calls() {
    static const unit_chase_check_calls c = {
        MH_LIBMH_BIND(llm_strat_unit_get_coords),
        MH_LIBMH_BIND(llm_strat_target_release_ref),
        mh::sim::target_class,
        mh::sim::unit_in_weapon_range,
        mh::sim::unit_fire_at_target,
        mh::sim::unit_set_state_order,
        mh::sim::unit_set_state,
    };
    return c;
}

namespace {
// Same truncating-toward-zero signed-divide-by-32 idiom as every other mh/sim TU's file-local
// fine_to_tile() (SAR/SHL/SBB/SAR in the asm, `/32` in C++ -- see sim_bldg_state_charge.cpp's own
// derivation comment for the equivalence proof).
inline int32_t fine_to_tile(int32_t fine) { return fine / 32; }
} // namespace

namespace detail {

int32_t unit_chase_check(const sim_view &v, sim_store &own, const unit_chase_check_calls &c) {
    const unit &u = *v.cur_unit;

    // ---- stage 1, 0x00486713-0x0048675d: if the target is a live UNIT (target_ref&0xa0), refresh
    // target_fine_x/y in place from the target's own current position. A direct write into the CURRENT
    // unit's own fields, performed through get_coords' out-pointers.
    if ((u.target_ref & 0xa0) != 0) {
        int32_t fine_x = 0, fine_y = 0;
        c.unit_get_coords(static_cast<uint16_t>(u.target_ref & 0xf),
                          static_cast<int32_t>(static_cast<uint16_t>(u.target_index)), &fine_x, &fine_y);
        unit &self         = own.unit_at(*v.cur_player, *v.cur_index);
        self.target_fine_x = fine_x;
        self.target_fine_y = fine_y;
    }

    // ---- stage 2, 0x0048675d-0x004867ce: classify the target and test weapon range against its
    // (freshly refreshed, if stage 1 ran) tile position. target_index (int16_t) is ZERO-extended at
    // every one of its three uses in this function (0x00486762/0x00486803/0x0048684c, all MOVZX) --
    // CORRECTED (reimpl-verify, 2026-08-22): stages 2/3 originally passed the bare (sign-extending)
    // int16_t->int32_t conversion here and below; only stage 1's get_coords call had the uint16_t
    // round-trip. Applied uniformly now, matching every other target_index/target2_index consumer in
    // this codebase (sim_unit_target_tracking.cpp).
    const int32_t target_index32 = static_cast<int32_t>(static_cast<uint16_t>(u.target_index));
    const int32_t target_class_result =
        c.target_class(static_cast<uint32_t>(u.target_ref), target_index32);
    const int32_t  tile_x = fine_to_tile(u.target_fine_x);
    const int32_t  tile_y = fine_to_tile(u.target_fine_y);
    const uint32_t in_range =
        c.unit_in_weapon_range(*v.cur_player, *v.cur_index, tile_x, tile_y, target_class_result);

    if (in_range == 0) {
        // ---- "still tracking, out of range", 0x004868e9-0x00486906: the ONLY path that returns 0.
        if (u.state == 0x2e) c.unit_set_state(0x2f);
        return 0;
    }

    // ---- stage 3, 0x004867d6-0x0048681f: a live (energy>0.0, NaN-is-alive) BUILDING or UNIT target
    // fires immediately, skipping the state-transition logic below entirely. The two bit tests are
    // mutually exclusive in target_ref's own encoding, so at most one of these fires.
    bool target_alive_now = false;
    if ((u.target_ref & 0x40) != 0) {
        const building &tb = building_of(v, u.target_ref & 0xf, target_index32);
        target_alive_now   = !(tb.energy <= 0.0); // ORDERED 0.0<energy; NaN counts as alive.
    } else if ((u.target_ref & 0xa0) != 0) {
        const unit &tu   = unit_of(v, u.target_ref & 0xf, target_index32);
        target_alive_now = !(tu.energy <= 0.0);
    }

    if (target_alive_now) {
        // ---- stage 4, 0x0048686a-0x0048686f: fire via the sibling, return 1.
        c.unit_fire_at_target();
        return 1;
    }

    // ---- stage 5, "target expired", 0x00486871-0x004868e0: release a still-set ref, clear it, then
    // transition state/order.
    if (u.target_ref != 0) {
        c.target_release_ref(*v.cur_player, *v.cur_index, 1);
        unit &self        = own.unit_at(*v.cur_player, *v.cur_index);
        self.target_ref   = 0;
        self.target_index = 0;
    }
    if (u.state == 0x2e) {
        c.unit_set_state_order(0x2f, 0x13);
    } else {
        c.unit_set_state_order(0x13, 0x13);
    }
    return 1;
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

int32_t unit_chase_check() {
    sim_state st = state();
    return detail::unit_chase_check(st.read, st.own, live_unit_chase_check_calls());
}


} // namespace mh::sim
