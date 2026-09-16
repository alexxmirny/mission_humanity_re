#include "sim/sim_unit_update_anim.h"

#include "ai/ai_state.h" // ai_say / trace_budget -- the shared trace sink, not AI state

namespace mh::sim {

namespace detail {

void unit_update_anim(const sim_view &v, sim_store &own) {
    unit &cur = own.cur_unit(); // *_G_LLM_STRAT_CUR_UNIT; both read and written throughout below.

    // GATE (0x0047dd6e-0x0047dd84): a plain boolean flag gating this function's entire body. Was an
    // auto-labeled `_unnamed_0x107` at translation time (translator-brief rule 17b); the conductor
    // renamed it in Ghidra to `dmg_smoke_enabled` (mh_cfg_final_struct_Unit offset 0x107, int32_t).
    if (v.cfg_units[cur.unit_proto_id].dmg_smoke_enabled == 0)
        return;

    // Anim[dmg_smoke_anim_id + 1].time, read ONCE here (0x0047dd99-0x0047dda8) and NEVER refreshed
    // inside the loop below, even on the iterations that change dmg_smoke_anim_id -- re-walked
    // against the raw addresses to be sure this is not a decompiler artifact (the exported .c's
    // dVar1/dVar2 split reads as two independent loads but both land on the identical stack slot,
    // ebp-0x24, and neither the loop body nor either branch ever re-stores to it). A genuine original
    // quirk (translator-brief rule 10: preserve it, don't "fix" it into a per-iteration re-read).
    const double anim_time = v.anim_frames[cur.dmg_smoke_anim_id + 1].time;

    double local_time        = *v.game_clock - cur.dmg_smoke_anim_timer;
    cur.dmg_smoke_anim_timer = *v.game_clock;

    // NaN-aware (0x0047ddd0-0x0047ddd8): FLDZ; FCOMP local_time; FNSTSW/SAHF; JNC exits ONLY when
    // CF==0, i.e. ST0 (0.0) >= local_time ORDERED -- an unordered result (local_time is NaN) ALSO
    // sets CF==1, so JNC does not fire and the loop keeps running on NaN. `!(local_time <= 0.0)`
    // reproduces that exactly (IEEE `<=` is false on NaN, so the negation is true -- loop continues),
    // unlike the exported .c's `0.0 < local_20`, which would falsely EXIT the loop on NaN -- the
    // opposite of the original. Same idiom sim_unit_passive_engage.cpp's header documents for its own
    // FCOMP/JNC shape.
    while (!(local_time <= 0.0)) {
        // NaN-aware (0x0047ddde-0x0047dde7): FLD local_time; FCOMP anim_time; FNSTSW/SAHF; JBE takes
        // this branch when CF==1 or ZF==1, i.e. local_time<=anim_time ORDERED, OR either operand is
        // NaN. `!(local_time > anim_time)` reproduces that (IEEE `>` is false on NaN, so the negation
        // is true), unlike the exported .c's plain `local_20 <= dVar1`, which would falsely SKIP this
        // branch on NaN.
        if (!(local_time > anim_time)) {
            // (a) the remaining budget fits inside the cached frame time: drain it and stop.
            cur.dmg_smoke_anim_timer -= local_time;
            local_time = 0.0;
        } else {
            // (b) doesn't fit: advance the anim chain and keep draining.
            const int32_t next = v.anim_frames[cur.dmg_smoke_anim_id + 1].next;
            if (next == 0) {
                // PRESERVE, do not "fix": the restart-base lookup reads
                // (&_G_LLM_STRAT_POP_STATS[7].layoff_cursor)[dmg_smoke_level] -- an out-of-declared-
                // bounds pointer walk off the POP_STATS array (POP_STATS has exactly MAX_PLAYERS==8
                // elements of sizeof(pop_stats)==0x34==52 bytes each -- 8*52==416, matching the
                // region's declared size; layoff_cursor sits at struct offset 0x30, so only index 0
                // lands inside element 7 itself, and every index>=1 reads whatever memory happens to
                // follow the 416-byte POP_STATS region in the image) that lands on an adjacent global
                // purely by memory layout. `v.population` (the existing sim_view member, already
                // pointing at the same 8-element array) exposes enough to reproduce this literally --
                // no additional declared need for this part, only #1/#2/#3 above. See uncertainties
                // for the risk this carries against an offline fixture whose POP_STATS buffer may not
                // have the same adjacent bytes as the live image.
                cur.dmg_smoke_anim_id = (&v.population[7].layoff_cursor)[cur.dmg_smoke_level];
            } else {
                cur.dmg_smoke_anim_id += next;
            }
            local_time -= anim_time;
        }
    }
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

void unit_update_anim() {
    sim_state st = state();
    detail::unit_update_anim(st.read, st.own);
}


} // namespace mh::sim
