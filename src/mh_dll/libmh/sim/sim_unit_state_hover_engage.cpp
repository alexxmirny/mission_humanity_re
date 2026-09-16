//
// sim/sim_unit_state_hover_engage.cpp -- see sim_unit_state_hover_engage.h. Translated from the
// DISASSEMBLY (tmp/decomp/llm_strat_unit_state_hover_engage_004807d9.asm), not from the Ghidra .c
// draft -- see the header's banner for the queued-order-record field re-derivation and the
// MOVE_PATH_12 value correction.
//
#include "sim/sim_unit_state_hover_engage.h"

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"        // ai_say / trace_budget -- the shared trace sink, not AI state
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const unit_state_hover_engage_calls &live_unit_state_hover_engage_calls() {
    static const unit_state_hover_engage_calls c = {
        MH_LIBMH_BIND(llm_strat_order_queue_find_index),
        MH_LIBMH_BIND(llm_strat_order_queue_apply_and_dequeue),
        MH_LIBMH_BIND(llm_strat_unit_set_state),
        MH_LIBMH_BIND(llm_strat_unit_set_state_order),
        MH_LIBMH_BIND(llm_strat_unit_hover_tile_crowded),
        MH_LIBMH_BIND(llm_strat_unit_chase_check),
    };
    return c;
}

namespace {

// ---- llm_strat_unit_state members this file compares against -----------------------------------
// No backing Ghidra enum exists for `llm_strat_unit_state` (checked, per sim_unit_state_die_explode.h/
// sim_order_dispatch.cpp/sim_unit_update_rotation.cpp's identical findings) -- these are this TU's own
// local copies (anonymous namespace, internal linkage, matching sim_order_dispatch.cpp's own pattern),
// NOT imported from either sibling header (avoids the ODR/shared-struct collision risk those files'
// comments document), but value-identical to and cross-checked against them:
//   UNIT_STATE_HOVER_ENGAGE/_HOVER_ENGAGE_2F/_MOVE_PATH_12  <- sim_order_dispatch.cpp
//   STATE_ATTACK_UNIT/_ATTACK_UNIT_RETURN/_ATTACK_BUILDING  <- sim_unit_update_rotation.cpp
//   STATE_GROUP_STEP                                        <- sim_unit_update_rotation.cpp
//
// UNIT_STATE_MOVE_PATH_12 = 0x12, confirmed at 0x004808c9 (`MOV EAX,0x12`) -- NOT 0x0c as the SIM1-G1
// batch context's prose incorrectly stated; sim_order_dispatch.cpp's already-committed constant agrees
// with the asm, so 0x12 is used here.
inline constexpr uint16_t UNIT_STATE_HOVER_ENGAGE    = 0x2e; // 0x004807f6/0x00480958/0x00480a54
inline constexpr uint16_t UNIT_STATE_HOVER_ENGAGE_2F = 0x2f; // 0x004808d8/0x00480958
inline constexpr uint16_t UNIT_STATE_HOVER_DISENGAGE = 0x37; // 0x00480964/0x004809b3
inline constexpr uint16_t UNIT_STATE_MOVE_PATH_12    = 0x12; // 0x004808c9/0x00480a23(as EDX)/0x004809ba
inline constexpr uint16_t STATE_GROUP_STEP           = 0x0b; // 0x00480a23(EAX)/0x00480a34/0x00480b05
inline constexpr uint16_t STATE_ATTACK_UNIT          = 0x1a; // 0x0048083f-.../0x004809ec/0x00480b23
inline constexpr uint16_t STATE_ATTACK_UNIT_RETURN   = 0x1b; // 0x0048084f-.../0x004809f5/0x00480b2f
inline constexpr uint16_t STATE_ATTACK_BUILDING      = 0x1c; // 0x0048083f/0x004809e3/0x00480b3d

// The order-queue "kind" filter llm_strat_order_queue_find_index's 3rd argument uses -- 0x80 = "unit
// target" per mh_llm_strat_order::owner_and_kind's own field comment ("0x20/0x80 = unit target").
// SAME value as sim_order_enqueue.h's ORDER_KIND_UNIT / sim_order_dispatch.h's ORDER_KIND_UNIT_EX;
// local copy per this file's own established anonymous-namespace convention above.
inline constexpr int32_t ORDER_KIND_UNIT = 0x80; // 0x0048080d

// Hover step-cost multiplier applied to cfg step_speed, an image double constant (SIM1-G1 batch
// context section 5, read-memory-confirmed by the conductor): _DAT_00501410 = 3.0.
inline constexpr double HOVER_STEP_COST_MULT = 3.0; // DAT_00501410

} // namespace

namespace detail {

void unit_state_hover_engage(const sim_view &v, sim_store &own, const unit_state_hover_engage_calls &c) {
    unit &u = own.cur_unit(); // _G_LLM_STRAT_CUR_UNIT dereferenced, read+write -- see sim_view::cur_unit's
                              // comment (bound from the same resolved pointer as *v.cur_player/*v.cur_index).
                              // The asm re-derives this same address repeatedly via the raw global rather
                              // than reusing a cached pointer; reused here as one local per the codebase's
                              // established simplification (matches sim_unit_state_die_explode.cpp's `u`).

    // ---- 0x004807f1-0x00480808: only when a fresh order is queued AND we're in the base HOVER_ENGAGE
    // state does the queued-order peek run at all; otherwise skip straight to the budget-gated tick.
    if (u.order_queued != 0 && u.state == UNIT_STATE_HOVER_ENGAGE) {
        // 0x0048080d-0x00480825: find_index(player, index, kind=UNIT_TARGET) -> queue index. The low 16
        // bits are what every downstream use reads (matches the .c draft's own `& 0xffff` masking of
        // this same value) -- NO bounds check here in the original; order_queued!=0 is what guarantees
        // find_index succeeds (translator-brief rule 14: don't add a check the original lacks).
        const uint16_t queue_idx = static_cast<uint16_t>(
            c.order_queue_find_index(*v.cur_player, static_cast<int32_t>(*v.cur_index), ORDER_KIND_UNIT));

        // 0x0048082f-0x0048083d: peek the queued order record. own.order_at is the only accessor
        // sim_state.h exposes for _G_LLM_STRAT_ORDER_QUEUE; binding it `const` here (never assigned
        // through) is a pure read, matching sim_order_dispatch_admin.cpp's own
        // `const order &rec = own.order_at(x.slot);` precedent for a reader of the same mutable-only
        // accessor. See the header banner for the param0-at-offset-0xC re-derivation (the .c draft's
        // `sStackY_5c` is a wrong stack-slot number for this same field).
        const order &q = own.order_at(queue_idx);

        // 0x0048083f-0x0048087e: BOTH the queued record's param0 AND the unit's own (pending) `order`
        // field must be one of {ATTACK_UNIT, ATTACK_UNIT_RETURN, ATTACK_BUILDING} -- a plain conjunction
        // of two independent 3-way membership tests, re-walked branch-by-branch against the raw
        // CMP/JZ/JNZ chain (not assumed from the .c draft, whose own compared local was wrong).
        if ((q.param0 == STATE_ATTACK_BUILDING || q.param0 == STATE_ATTACK_UNIT ||
             q.param0 == STATE_ATTACK_UNIT_RETURN) &&
            (u.order == STATE_ATTACK_BUILDING || u.order == STATE_ATTACK_UNIT ||
             u.order == STATE_ATTACK_UNIT_RETURN)) {
            // 0x00480880-0x00480897: apply the queued order now and dequeue it; falls through to the
            // budget-gated tick below (does NOT return early).
            c.order_queue_apply_and_dequeue(*v.cur_player, static_cast<int32_t>(*v.cur_index), queue_idx);
        } else {
            // 0x00480899-0x004808d3: not an attack-continuation order -- if the unit has already
            // finished turning to face its current move-microstep target, commit MOVE_PATH_12 and
            // return immediately (skips the budget-gated tick entirely this call).
            if (u.facing_target ==
                v.move_microsteps[u.move_heading * MICROSTEPS_PER_HEADING + u.move_microstep].facing) {
                c.unit_set_state(UNIT_STATE_MOVE_PATH_12);
                return;
            }
            // 0x004808d8-0x004808e2: still turning -- park in HOVER_ENGAGE_2F and fall through.
            c.unit_set_state(UNIT_STATE_HOVER_ENGAGE_2F);
        }
    }

    // ---- 0x004808e2-0x0048093f: the per-tick hover cost, budget-gated exactly like every other
    // unit-state handler's own tick-budget check.
    const double cost =
        v.cfg_units[u.unit_proto_id].step_speed[*v.cur_player] * HOVER_STEP_COST_MULT;
    if (*v.tick_budget < cost) {
        // 0x0048091a-0x0048093f: insufficient budget -- carry the shortfall into activity_clock and
        // drain the tick budget to zero (SAME idiom every other budget-gated sim state handler uses).
        u.activity_clock -= *v.tick_budget;
        own.tick_budget() = 0.0;
        return;
    }
    own.tick_budget() -= cost;

    // ---- 0x00480944-0x00480b49: spend committed -- dispatch on whether we're mid-transition
    // (HOVER_ENGAGE_2F/HOVER_DISENGAGE) or in the base HOVER_ENGAGE state.
    if (u.state != UNIT_STATE_HOVER_ENGAGE_2F && u.state != UNIT_STATE_HOVER_DISENGAGE) {
        // ---- 0x00480a4f-0x00480b1e: base-state branch (only touches elevation/crowded-tile sidestep
        // when state is EXACTLY HOVER_ENGAGE -- other values reaching here fall straight to the
        // chase_check gate below). ----
        if (u.state == UNIT_STATE_HOVER_ENGAGE) {
            // 0x00480a64-0x00480a6f: only the low BYTE is XORed in the asm (a byte-register alias on the
            // int32 field), which is bit-for-bit equivalent to a plain int32 `^= 1` since XOR-with-1
            // only ever touches bit 0 either way.
            u.elevation ^= 1;

            const int32_t crowded =
                c.unit_hover_tile_crowded(*v.cur_player, static_cast<uint32_t>(*v.cur_index));
            if (crowded != 0) {
                // 0x00480a8d-0x00480b14: sidestep one tile along DIR_REMAP_TABLE[move_heading]'s primary
                // step direction, torus-wrapped, then hand off to GROUP_STEP and clear move_group_id.
                const int32_t          step_primary = v.dir_remap_table[u.move_heading].step_primary;
                const dir_step_offset &step         = v.dir_step_offsets[step_primary];
                u.goal_x                            = static_cast<uint8_t>(map_width_mask(v) &
                                                                           static_cast<uint32_t>(u.x + step.dx)); // 0x00480ac3-0x00480ade
                u.goal_y                            = static_cast<uint8_t>(map_height_mask(v) &
                                                                           static_cast<uint32_t>(u.y + step.dy)); // 0x00480ae4-0x00480aff
                c.unit_set_state(STATE_GROUP_STEP);                                                               // 0x00480b05-0x00480b0a
                u.move_group_id = 0;                                                                              // 0x00480b0f-0x00480b1e
            }
        }

        // ---- 0x00480b1e-0x00480b49: common tail -- chase_check only for the three attack orders.
        if (u.order != STATE_ATTACK_UNIT && u.order != STATE_ATTACK_UNIT_RETURN &&
            u.order != STATE_ATTACK_BUILDING) {
            return;
        }
        c.unit_chase_check(); // return value unused by the original
        return;
    }

    // ---- 0x0048096f-0x00480a4a: mid-transition branch (state == HOVER_ENGAGE_2F or HOVER_DISENGAGE).
    if (u.facing_target !=
        v.move_microsteps[u.move_heading * MICROSTEPS_PER_HEADING + u.move_microstep].facing) {
        return; // 0x0048099d/0x00480a4a: still turning -- nothing to do this tick.
    }
    if (u.order_queued != 0 || u.state == UNIT_STATE_HOVER_DISENGAGE) {
        // 0x004809ba-0x004809c4: a fresh order is waiting, or we were actively disengaging -- either
        // way, commit the move and let the next tick's queued-order peek (above) sort out what's next.
        c.unit_set_state(UNIT_STATE_MOVE_PATH_12);
        return;
    }

    // 0x004809c9-0x00480a4a: no order queued and not disengaging -- dispatch on the unit's own
    // (pending) `order` field to decide what continuation state to enter.
    const uint16_t pending_order = u.order;
    if (pending_order == STATE_ATTACK_UNIT_RETURN) {
        // 0x004809f5-0x00480a1d: return-to-rally -- retarget goal at home, then hand off to GROUP_STEP
        // with MOVE_PATH_12 queued as the follow-on order.
        u.goal_x = u.home_x;
        u.goal_y = u.home_y;
        c.unit_set_state_order(STATE_GROUP_STEP, UNIT_STATE_MOVE_PATH_12);
        return;
    }
    if (pending_order == STATE_ATTACK_UNIT || pending_order == STATE_ATTACK_BUILDING) {
        c.unit_set_state(STATE_GROUP_STEP);
        return;
    }
    // default: none of the three attack-continuation orders -- stay in (or return to) HOVER_ENGAGE.
    c.unit_set_state(UNIT_STATE_HOVER_ENGAGE);
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

void unit_state_hover_engage() {
    sim_state st = state();
    detail::unit_state_hover_engage(st.read, st.own, live_unit_state_hover_engage_calls());
}


} // namespace mh::sim
