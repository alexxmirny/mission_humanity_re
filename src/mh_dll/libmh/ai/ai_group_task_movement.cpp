//
// ai/ai_group_task_movement.cpp -- see ai_group_task_movement.h. Translated from the DISASSEMBLY
// (tmp/decomp/llm_strat_ai_group_task_{patrol_shuttle_004eabd3,loiter_wander_004ead22,
// nudge_stragglers_004eaecb}.asm), not from Ghidra's .c.
//
// EVERY ABSOLUTE RE-DERIVED against addr/mh_structs.gen.h's offsetof asserts (unit_group::
// pending_param/task_code/active_flag/active_sub_code/active_param_a..d @0x26/0x2a/0x2c/0x2d/
// 0x2f/0x33/0x37/0x3b, ::head_unit @0xa; unit::ai_group_next @0xd4) by walking the (player*0x288fc +
// group*0xa66) base every call computes and checking each literal displacement against
// ai_groups_offset + field_offset -- every one of them checks out exactly. See the two functions'
// own comments for what did NOT check out cleanly (the loiter_wander x87 sequence and two untracked
// literal constants it reads).
//
#include "ai/ai_group_task_movement.h"

#include <cmath>


namespace mh::ai {
namespace detail {

void group_task_patrol_shuttle(const ai_view &v, const ai_store &own, const ai_calls &gc,
                               int32_t player_id, int32_t group_index) {
    (void)own;
    const unit_group &grp     = v.players[player_id].ai_groups[group_index];
    const int32_t     pending = grp.pending_param;

    if (grp.active_sub_code != 0) { // CMP word,0 / JBE @0x004eac12 -- else, straight to the tail
        int32_t mid_x = 0, mid_y = 0;
        gc.tile_midpoint_wrapped(grp.active_param_a, grp.active_param_b, grp.active_param_c,
                                 grp.active_param_d, &mid_x, &mid_y); // 0x004eac38

        // Four MOVE legs, this exact order: midpoint, anchor-1, midpoint again, anchor-2
        // (0x004eac59/0x004eac80/0x004eaca1/0x004eacc8). Each carries pending_param and three
        // trailing zeros.
        gc.group_task_enqueue(player_id, group_index, 3, (uint32_t)pending, (uint32_t)mid_x,
                              (uint32_t)mid_y, 0, 0, 0);
        gc.group_task_enqueue(player_id, group_index, 3, (uint32_t)pending,
                              (uint32_t)grp.active_param_a, (uint32_t)grp.active_param_b, 0, 0, 0);
        gc.group_task_enqueue(player_id, group_index, 3, (uint32_t)pending, (uint32_t)mid_x,
                              (uint32_t)mid_y, 0, 0, 0);
        gc.group_task_enqueue(player_id, group_index, 3, (uint32_t)pending,
                              (uint32_t)grp.active_param_c, (uint32_t)grp.active_param_d, 0, 0, 0);

        // Re-enqueue self (task_code 5), carrying the four anchor params unchanged and the
        // decremented bounce counter (0x004eaccd-0x004ead13).
        const int16_t sub_code_next = (int16_t)(grp.active_sub_code - 1);
        gc.group_task_enqueue(player_id, group_index, 5, (uint32_t)pending,
                              (uint32_t)grp.active_param_a, (uint32_t)grp.active_param_b,
                              (uint32_t)grp.active_param_c, (uint32_t)grp.active_param_d,
                              (uint16_t)sub_code_next);
    } else {
        // First activation: hand off via task_code 8, pending_param only (0x004eacfe-0x004ead0e).
        gc.group_task_enqueue(player_id, group_index, 8, (uint32_t)pending, 0, 0, 0, 0, 0);
    }
    gc.group_task_dequeue(player_id, group_index); // 0x004ead15/0x004ead1a shared tail
}

void group_task_loiter_wander(const ai_view &v, const ai_store &own, const ai_calls &gc,
                              int32_t player_id, int32_t group_index) {
    (void)own;
    const unit_group &grp = v.players[player_id].ai_groups[group_index];

    if (grp.active_sub_code == 0) { // CMP word,0 / JBE @0x004ead5e -- first activation
        gc.group_task_enqueue(player_id, group_index, 8, (uint32_t)grp.pending_param, 0, 0, 0, 0, 0);
        gc.group_task_dequeue(player_id, group_index);
        return;
    }

    // ---- the sin/cos jitter draw, x87, 0x004ead64-0x004eadbe. SEE THE TRANSLATOR'S UNCERTAINTY ----
    // channel-2 RNG draw -> radians (scaled by loiter_angle_scale) -> sin/cos via the ORIGINAL's own
    // table-based primitives (never substitute std::sin/std::cos -- brief rule 9) -> each scaled by
    // loiter_jitter_radius and floored to an integer tile offset. `(int32_t)std::floor(x)` stands in
    // for the original's floor()-then-utils_math_trunc pair: trunc-toward-zero of an already-floored
    // (hence already-integral) finite double is a no-op, so the pair collapses to one floor + one
    // truncating cast with no behavioural difference for any in-range value.
    const double  angle    = gc.rand_state_advance(2) * (*v.loiter_angle_scale); // 0x004ead66/0x004ead6e
    const double  s        = gc.math_fsin_reduce_loop(angle);                    // 0x004ead80
    const double  c        = gc.math_cos_impl(angle);                            // 0x004ead88
    const double  radius   = (double)(*v.loiter_jitter_radius);                  // 0x004ead77/0x004eada5
    const int32_t jitter_x = (int32_t)std::floor(radius * s);                    // sin-derived; pairs with X, see below
    const int32_t jitter_y = (int32_t)std::floor(radius * c);                    // cos-derived; pairs with Y, see below

    // Anchor: the group's own stored point unless it is the centroid sentinel (-1), in which case
    // the live member centroid is used instead (0x004eadc1-0x004eae03).
    int32_t anchor_x, anchor_y;
    if (grp.active_param_a == -1) {
        uint32_t cx = 0, cy = 0;                                     // committed llm_strat_ai_group_compute_centroid out-params
        gc.group_compute_centroid(player_id, group_index, &cx, &cy); // 0x004eadd4
        anchor_x = cx;
        anchor_y = cy;
    } else {
        anchor_x = grp.active_param_a;
        anchor_y = grp.active_param_b;
    }

    // THE SIN LEG IS MASKED BY WIDTH AND THE COS LEG BY HEIGHT -- read verbatim off
    // 0x004eadd9-0x004eade7 (X) and 0x004eae09-0x004eae14 (Y); see the header/uncertainty note.
    const int32_t tgt_x = (int32_t)(((uint32_t)(anchor_x + jitter_x)) & *v.map_width_mask);
    const int32_t tgt_y = (int32_t)(((uint32_t)(anchor_y + jitter_y)) & *v.map_height_mask);

    const int32_t pending = grp.pending_param;
    // MOVE to the jittered tile (task_code 3, 0x004eae53).
    gc.group_task_enqueue(player_id, group_index, 3, (uint32_t)pending, (uint32_t)tgt_x,
                          (uint32_t)tgt_y, 0, 0, 0);
    // Re-enqueue self (task_code 0xe) to the SAME jittered tile, bounce counter decremented
    // (0x004eae58-0x004eae97).
    const int16_t sub_code_next = (int16_t)(grp.active_sub_code - 1);
    gc.group_task_enqueue(player_id, group_index, 0xe, (uint32_t)pending, (uint32_t)tgt_x,
                          (uint32_t)tgt_y, 0, 0, (uint16_t)sub_code_next);
    gc.group_task_dequeue(player_id, group_index); // shared tail, 0x004ea064
}

void group_task_nudge_stragglers(const ai_view &v, const ai_store &own, const ai_calls &gc,
                                 uint32_t player_id, int32_t group_index) {
    (void)own;
    const unit_group &grp     = v.players[player_id].ai_groups[group_index];
    uint32_t          unit_id = grp.head_unit; // 0x004eaeff

    while (unit_id != 0) {                                                // TEST/JNZ @0x004eaf60
        if (gc.unit_is_order_pending(player_id, (int32_t)unit_id) == 0) { // 0x004eaf0d/0x004eaf12
            gc.unit_flag_and_move(player_id, (int32_t)unit_id, (uint32_t)grp.active_param_a,
                                  (uint32_t)grp.active_param_b); // 0x004eaf45
        }
        unit_id = unit_of(v, player_id, (int32_t)unit_id).ai_group_next; // 0x004eaf56
    }
}

} // namespace detail

void group_task_patrol_shuttle(int32_t player_id, int32_t group_index) {
    const ai_state st = state();
    detail::group_task_patrol_shuttle(st.read, st.own, live_calls(), player_id, group_index);
}

void group_task_loiter_wander(int32_t player_id, int32_t group_index) {
    const ai_state st = state();
    detail::group_task_loiter_wander(st.read, st.own, live_calls(), player_id, group_index);
}

void group_task_nudge_stragglers(uint32_t player_id, int32_t group_index) {
    const ai_state st = state();
    detail::group_task_nudge_stragglers(st.read, st.own, live_calls(), player_id, group_index);
}


} // namespace mh::ai
