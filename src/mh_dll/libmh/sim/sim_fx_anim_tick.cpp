//
// sim/sim_fx_anim_tick.cpp -- see sim_fx_anim_tick.h. Translated from the DISASSEMBLY
// (tmp/decomp/llm_strat_fx_anim_tick_004418a5.asm), cross-checked against the Ghidra .c draft, which
// matched field-for-field -- see the header banner for the full derivation.
//
#include "sim/sim_fx_anim_tick.h"

#include "ai/ai_state.h" // ai_say / trace_budget -- the shared trace sink, not AI state

namespace mh::sim {

namespace detail {

void fx_anim_tick(const sim_view &v, sim_store &own) {
    fx_anim &f = own.cur_fx_anim(); // _G_LLM_STRAT_CUR_FX_ANIM dereferenced, read+write

    // 0x004418bd-0x004418e8: time_step = Anim[f.anim_frame + 1].time, read ONCE. A permanent frame
    // (time == 0.0) makes the whole tick a no-op -- see header banner on why the raw-dword bit test
    // the asm uses is value-identical to this comparison.
    const double time_step = v.anim_frames[f.anim_frame + 1].time;
    if (time_step == 0.0) return;

    // 0x004418ee-0x00441914: elapsed = GAME_CLOCK - f.timestamp; if positive, rebase the stored
    // timestamp to GAME_CLOCK now -- the loop below tracks the backlog only in the local `elapsed`
    // from this point on, never re-reading f.timestamp as a moving baseline (except the partial-tick
    // absorb branch, which writes it once more).
    double elapsed = *v.game_clock - f.timestamp;
    if (elapsed > 0.0) f.timestamp = *v.game_clock;

    // 0x00441917-0x00441999: drain the backlog one Anim[] frame at a time.
    while (elapsed > 0.0) {
        if (time_step <= elapsed) {
            const int32_t next = v.anim_frames[f.anim_frame + 1].next;
            if (next == 0) {
                // 0x00441960-0x00441974: CHAIN END -- retire the effect and give back its pool slot.
                f.live = 0;
                own.fx_anim_pool_at(0).live -= 1; // slot 0's `.live` doubles as the pool live-count
                return;
            }
            // 0x00441976-0x00441996: advance to the next frame, consume one time_step of backlog.
            f.anim_frame += next;
            elapsed -= time_step;
        } else {
            // 0x00441930-0x0044194c: the remaining backlog doesn't cover one more frame -- absorb it
            // into the stored timestamp instead of the frame index, and stop.
            f.timestamp -= elapsed;
            elapsed = 0.0;
        }
    }
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

void fx_anim_tick() {
    sim_state st = state();
    detail::fx_anim_tick(st.read, st.own);
}


} // namespace mh::sim
