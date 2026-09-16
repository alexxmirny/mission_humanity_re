//
// sim/sim_unit_path_queue_count.cpp -- see sim_unit_path_queue_count.h. Translated from the
// DISASSEMBLY (tmp/decomp_sim/llm_strat_unit_path_queue_count_0041fea7.asm), not from any Ghidra `.c`
// draft.
//
#include "sim/sim_unit_path_queue_count.h"

namespace mh::sim {

namespace detail {

int32_t unit_path_queue_count(const sim_view &v, int32_t unit_index, int32_t max_len) {
    // 0x0041feb6-0x0041fec5: EBX/ECX are saved into locals here but are NOT parameters of this
    // function (the committed prototype takes only unit_index/EAX and max_len/EDX) -- they are
    // whatever the caller last left in those registers. The locals they seed (`x_accum`/`y_accum`
    // below) are written every iteration but never read back by anything outside themselves, never
    // stored to memory, and never returned (see the header banner) -- so their seed value is provably
    // unobservable. Initialized to 0 here rather than reproducing an incoming-register value that has
    // no equivalent in this call's C++ signature.
    int32_t x_accum = 0;
    int32_t y_accum = 0;

    // 0x0041fec8: prev_heading = 0.
    int32_t prev_heading = 0;

    // 0x0041fec2/-0x1c and 0x0041fec5/-0x18 above are x_accum/y_accum; -0x14 is `i`, initialized to 0
    // at 0x0041fecf.
    int32_t i = 0;

    // 0x0041feeb: owner is re-read from the global on every iteration in the original (each of the
    // repeated `IMUL EAX,[_G_LLM_STRAT_GROUP_ORDER_OWNER],0xea60` blocks below) -- hoisting the read
    // is safe ONLY because nothing in this function's body (nor anything it calls -- it calls nothing
    // but the inert stack-capacity probe) writes _G_LLM_STRAT_GROUP_ORDER_OWNER, so re-reading it
    // every time is value-for-value identical to reading it once. (This function makes no outward
    // calls that could reenter the sim between reads, unlike the roster-writing functions the const
    // view's W1 discipline is about.)
    const int32_t owner = *v.group_order_owner;

    // 0x0041fed6-0x0041fede: outer loop, condition checked BEFORE each slot. `i` reaching `max_len`
    // without an early exit falls straight through to `return max_len` (EXIT A) -- see the header
    // banner: EAX is reloaded with `i` right here, immediately before the JMP to the shared epilogue.
    for (; i < max_len; ++i) {
        // 0x0041feeb-0x0041ff03: flat_index = owner*PATH_WAYPOINTS_PER_PLAYER +
        // unit_index*PATH_WAYPOINTS_PER_SLOT + i -- same indexing every other SIM1-G1/G2 TU uses
        // against v.path_buffers directly (read-only context: sim_unit_state_move_walker.cpp /
        // sim_unit_path_detour.cpp precedent). 0x0041ff05: `.run_length != 0`.
        const int32_t flat_index =
            owner * PATH_WAYPOINTS_PER_PLAYER + unit_index * PATH_WAYPOINTS_PER_SLOT + i;
        if (v.path_buffers[flat_index].run_length == 0) {
            // 0x0041ff0c: EXIT B. EAX is NOT reloaded with `i` on this path -- the last value written
            // to it is the byte offset that fed the CMP above (`2 * flat_index`, the CMP's own
            // `+0xaee8c1` displacement is never added into EAX itself). Preserved exactly per house
            // rule 10 -- this is NOT `i`, even though the function's name says "count". See the header
            // banner and the translation report's uncertainties.
            return 2 * flat_index;
        }

        // ---- everything below this point in the loop body is DEAD from an external-behavior
        // standpoint (never returned, never stored to memory, never affects a branch) -- see the
        // header banner. Transcribed anyway per house rule 1.

        // 0x0041ff12-0x0041ff36: delta = path_buffers[unit_index][max_len].heading - prev_heading.
        // NOTE the FIXED index `max_len` here, not the loop variable `i` -- read exactly as the
        // assembly computes it, not "corrected" to `i`.
        const int32_t fixed_slot_index =
            owner * PATH_WAYPOINTS_PER_PLAYER + unit_index * PATH_WAYPOINTS_PER_SLOT + max_len;
        int32_t delta = static_cast<int32_t>(v.path_buffers[fixed_slot_index].heading) - prev_heading;
        // 0x0041ff39-0x0041ff3f: delta < 0 ? delta += 24 : delta (wrap into a 24-way heading range).
        if (delta < 0) delta += 24;
        // 0x0041ff43-0x0041ff49: `delta == 15` short-circuits the second compare; either way nothing
        // downstream reads either comparison's flags -- both are dead, transcribed for fidelity only.
        if (delta == 15) {
            // falls straight to LAB_0041ff4d, same as the untaken branch below.
        } else {
            (void)(delta == 9); // 0x0041ff49: CMP with no consuming branch -- inert.
        }

        // 0x0041ff4d-0x0041ff6e: prev_heading := path_buffers[unit_index][i].heading (index `i` this
        // time -- the current slot, distinct from the fixed-`max_len` read above).
        prev_heading = v.path_buffers[flat_index].heading;

        // 0x0041ff71-0x0041ffa1: x_accum += dir_step_deltas[prev_heading][0] * run_length[i].
        const int32_t run_length_i = v.path_buffers[flat_index].run_length;
        x_accum += static_cast<int32_t>(v.map_dir_step_deltas[prev_heading * 2 + 0]) * run_length_i;

        // 0x0041ffa4-0x0041ffd4: y_accum += dir_step_deltas[prev_heading][1] * run_length[i].
        y_accum += static_cast<int32_t>(v.map_dir_step_deltas[prev_heading * 2 + 1]) * run_length_i;

        // 0x0041ffd7-0x0041ffe6: wrap both accumulators through (map_width|map_height) - 1. These are
        // the plain map-DIMENSION globals (0x00825084/0x00825064, sim_view::map_width/map_height),
        // NOT a pre-computed mask -- the assembly's own `DEC EAX` before each AND is what makes that
        // explicit (a pre-computed mask would not need the decrement).
        x_accum &= (*v.map_width - 1);
        y_accum &= (*v.map_height - 1);
        // 0x0041ffe9: JMP back to the loop-condition check (the `for` header above).
    }

    // 0x0041fed6-0x0041fede: EXIT A. `i` == `max_len` here.
    return i;
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

int32_t unit_path_queue_count(int32_t unit_index, int32_t max_len) {
    const sim_view v = state().read;
    return detail::unit_path_queue_count(v, unit_index, max_len);
}


} // namespace mh::sim
