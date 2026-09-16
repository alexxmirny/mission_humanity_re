//
// sim/sim_cfg_anim_frame_at_progress.cpp -- see sim_cfg_anim_frame_at_progress.h. Translated from the
// DISASSEMBLY (tmp/decomp/llm_cfg_anim_frame_at_progress_0046190d.asm), cross-checked against the
// Ghidra .c draft, which matched the branch structure exactly -- see the header banner for the full
// derivation, including why `target` is computed once rather than re-multiplied per iteration.
//
#include "sim/sim_cfg_anim_frame_at_progress.h"

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"        // ai_say / trace_budget -- the shared trace sink, not AI state
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const cfg_anim_frame_at_progress_calls &live_cfg_anim_frame_at_progress_calls() {
    static const cfg_anim_frame_at_progress_calls c = {
        MH_LIBMH_BIND(cfg_GetAnimTime),
    };
    return c;
}

namespace detail {

int32_t cfg_anim_frame_at_progress(const sim_view &v, const cfg_anim_frame_at_progress_calls &c,
                                   int32_t start_frame, double progress_fraction) {
    // 0x0046193c-0x0046194d: total = cfg_GetAnimTime(start_frame); target = total * progress_fraction,
    // computed ONCE before the loop (the asm overwrites the same scratch slot in place -- see header).
    const double total  = c.get_anim_time(start_frame);
    const double target = total * progress_fraction;

    double  accumulated = 0.0;
    int32_t idx         = start_frame;

    // LAB_00461950-0x00461999: advance while the frame we're about to add still fits under `target`
    // AND the chain has a next frame to advance to; otherwise stop and return idx as-is. See header
    // banner for the FCOMP/JA direction derivation and the "no cycle guard" note.
    while (accumulated + v.anim_frames[idx + 1].time <= target && v.anim_frames[idx + 1].next != 0) {
        accumulated += v.anim_frames[idx + 1].time;
        idx += v.anim_frames[idx + 1].next; // delta, not an absolute index -- same convention as
                                            // every other Anim[]-chain walker in this codebase.
    }

    return idx;
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

int32_t cfg_anim_frame_at_progress(int32_t start_frame, double progress_fraction) {
    const sim_view v = state().read;
    return detail::cfg_anim_frame_at_progress(v, live_cfg_anim_frame_at_progress_calls(), start_frame,
                                              progress_fraction);
}


} // namespace mh::sim
