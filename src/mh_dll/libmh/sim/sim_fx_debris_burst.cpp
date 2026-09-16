//
// sim/sim_fx_debris_burst.cpp -- see sim_fx_debris_burst.h. Translated from the DISASSEMBLY
// (tmp/decomp_sim1e2/llm_strat_spawn_debris_burst_0044dbd2.asm), not from the Ghidra .c draft -- see
// the header banner for the region-layout derivation, the mirrored-pair loop shape, the x87-pop tail,
// and the declared-need scale-divisor constant this file references.
//
#include "sim/sim_fx_debris_burst.h"

#include "addr/mh_calls.gen.h" // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"       // ai_say / trace_budget -- the shared trace sink, not AI state
#include "state/host_api.h"
#include "state/host_events.h"
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const debris_burst_calls &live_debris_burst_calls() {
    static const debris_burst_calls c = {
        MH_LIBMH_BIND(llm_rand_below_fx),
        MH_LIBMH_BIND(llm_map_zoom_scale_x_get),
    };
    return c;
}

namespace detail {

void spawn_debris_burst(int32_t intensity, const sim_view &v, sim_store &own, const debris_burst_calls &c) {
    // 0x0044dbed: llm_cam_jump_queue_clear(). LIBMH DRAINS IT ITSELF since 2026-09-09 (LIFT-R3B)
    // instead of emitting the record, and the reason is a hazard R3b does not name.
    //
    // R3b is "libmh reads back what the scope wrote". This was the mirror image. The record asks
    // the host to drain the queue, but the drain is PARAMETERISED BY STATE LIBMH THEN OVERWRITES:
    // the loop below refills cam_jump_offset_at/scale_at and sets cam_jump_queue_count at :77.
    // Hosted, the sink drains synchronously here and sees the OLD queue, which is correct. A
    // NULL-callback host drains at frame edge -- after the refill -- and would consume the burst
    // libmh had just queued: a different set of camera jumps applied, and the shake never played.
    // Not a stale read; a different action.
    //
    // Draining in libmh removes the dependency entirely. This is llm_cam_jump_queue_pop
    // @0x0044dd43 transcribed (COUNT--, then col/row/zoom/dirty in that order) under
    // llm_cam_jump_queue_clear @0x0044dd11's `while (COUNT > 0)`. Each step still crosses the
    // channel through the existing kinds, so the hosted arm performs the same original calls in
    // the same order at the same instant (R5) -- what changed is that the queue reads are ours.
    // evt::cam_set_col/_row write MAP_CAM_COL/_ROW themselves (the R3b camera hoist), so the next
    // iteration's *v.cam_col sees this one's update exactly as the original's re-read does.
    //
    // The OTHER two emit sites keep the record: llm_strat_session_state_reset and llm_game_load
    // do not refill the queue afterwards, so deferral cannot change what they drain, and neither
    // is reachable from llm_strat_frame -- measured, so they cannot share a frame with
    // llm_strat_spawn_enemy_landing's camera readback either.
    while (own.cam_jump_queue_count() > 0) {
        const int32_t slot         = own.cam_jump_queue_count() - 1;
        own.cam_jump_queue_count() = slot; // 0x0044dd4e: the decrement precedes every read
        const cam_jump_offset &off = own.cam_jump_offset_at(slot);
        mh::state::evt::cam_set_col(static_cast<int32_t>(
            v.geom->width_mask & static_cast<uint32_t>(*v.cam_col + off.dx)));
        mh::state::evt::cam_set_row(static_cast<int32_t>(
            static_cast<uint32_t>(*v.cam_row + off.dy) & v.geom->height_mask));
        const double zoom = own.cam_jump_scale_at(slot);
        mh::state::evt::view_zoom_scale(zoom, zoom); // 0x0044dda6: both axes, the same slot
        mh::state::evt::inv_viewport();              // 0x0044ddc9
    }

    // 0x0044dbf2-0x0044dc10: particle_budget = ((intensity * 30) / 3) / 100 -- TWO CHAINED IDIVs,
    // reproduced literally rather than algebraically reduced to `intensity / 10` (see header banner;
    // the first IDIV never truncates in practice since 30 is a multiple of 3, but the C++ still
    // performs both divisions the assembly performs).
    const int32_t scaled_by_10x3  = (intensity * 30) / 3;
    const int32_t particle_budget = scaled_by_10x3 / 100;

    // 0x0044dc1a-0x0044dc31: while (i < particle_budget && i < CAM_JUMP_QUEUE_SLOTS) { body; i += 2; }
    // -- the .c draft's for-loop condition, reproduced with the same two-term && the asm's two
    // compare/branch pairs implement (CMP-vs-budget, then CMP-vs-0x1e), not a min()/clamp.
    int32_t i = 0;
    while (i < particle_budget && i < CAM_JUMP_QUEUE_SLOTS) {
        // 0x0044dc33-0x0044dc4c: one (dx,dy) jitter pair, each axis its own llm_rand_below_fx(3) draw,
        // each minus 1 (range [-1, 1]).
        const int32_t dx = static_cast<int32_t>(c.rand_below_fx(3)) - 1;
        const int32_t dy = static_cast<int32_t>(c.rand_below_fx(3)) - 1;

        // 0x0044dc4f-0x0044dc67: entry i's offset = (dx, dy).
        own.cam_jump_offset_at(i).dx = dx;
        own.cam_jump_offset_at(i).dy = dy;

        // 0x0044dc6d-0x0044dc8d: entry i's scale = llm_rand_below_fx(0x32) / *debris_scale_divisor +
        // 1.0, computed entirely in extended/double precision -- NOT the .c draft's `(float)` cast (see
        // header banner's FP derivation; DECLARED NEED: v.debris_scale_divisor does not exist yet).
        const int32_t scale_draw_i = static_cast<int32_t>(c.rand_below_fx(0x32));
        own.cam_jump_scale_at(i)   = static_cast<double>(scale_draw_i) / *v.debris_scale_divisor + 1.0;

        // 0x0044dc93-0x0044dcaf: entry i+1's offset is the MIRRORED negation of entry i's (dx, dy) --
        // NOT a fresh draw.
        own.cam_jump_offset_at(i + 1).dx = -dx;
        own.cam_jump_offset_at(i + 1).dy = -dy;

        // 0x0044dcb5-0x0044dcd5: entry i+1's scale is its OWN independent llm_rand_below_fx(0x32) draw
        // -- NOT mirrored/shared with entry i's scale, unlike the offset above.
        const int32_t scale_draw_i1  = static_cast<int32_t>(c.rand_below_fx(0x32));
        own.cam_jump_scale_at(i + 1) = static_cast<double>(scale_draw_i1) / *v.debris_scale_divisor + 1.0;

        i += 2;
    }

    // 0x0044dce0-0x0044dce3: the survivor count is however far the loop actually advanced (0, 2, 4, ...
    // up to 30), not necessarily particle_budget.
    own.cam_jump_queue_count() = i;

    // 0x0044dce8-0x0044dcfd: call the original for its state-class effect, discard ST0 (an x87 stack
    // POP into entry 0's scale, immediately clobbered below) -- see header banner "THE TAIL IS AN x87
    // POP...". Dropping this call would drop whatever llm_map_zoom_scale_x_get's internal state-read
    // does, even though its return value is never used here.
    (void)c.zoom_scale_x_get();

    // 0x0044dcf3-0x0044dcfd: entry 0's scale is unconditionally overwritten to the IEEE-754 bit pattern
    // of 1.0 (low dword 0, high dword 0x3ff00000), regardless of what the loop above wrote there.
    own.cam_jump_scale_at(0) = 1.0;
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

void spawn_debris_burst(int32_t intensity) {
    sim_state st = state();
    detail::spawn_debris_burst(intensity, st.read, st.own, live_debris_burst_calls());
}


} // namespace mh::sim
