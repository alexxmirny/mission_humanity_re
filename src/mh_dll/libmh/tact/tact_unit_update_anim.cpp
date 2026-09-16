//
// tact/tact_unit_update_anim.cpp -- see tact_unit_update_anim.h. Translated from the DISASSEMBLY,
// not from Ghidra's C.
//
#include "tact/tact_unit_update_anim.h"

#include "addr/mh_calls.gen.h"  // frontier callees (Law 4) -- bound into the calls struct only
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>        // GetPrivateProfileIntA -- the [promote] gate, same as mh::sim's sites
#include "crt/crt_select.h" // LIB-CRT: MH_CRT() picks the vendored CRT in the standalone build

namespace mh::tact {

const unit_update_anim_calls &live_unit_update_anim_calls() {
    static const unit_update_anim_calls c = {
        MH_LIBMH_BIND(time_GetCurrentTime),
        MH_CRT(llm_rand),
        MH_LIBMH_BIND(llm_tact_quantize_facing_dir),
    };
    return c;
}

namespace detail {
namespace {

// The "direct" half of the two-part frame-lookup pattern used throughout this function: `value` is
// still within [0, divisor) of this half's own sub-range, so the frame index counts UP from 0.
// Identical shape at every "ascending" sprite_id assignment in the .asm (state 0/1 frames[0]/[1],
// state 0 reset frames[2], state 2 frames[3], state 4/0x1f below-threshold frames[5]/frames[4]) --
// factored here because duplicating this exact arithmetic ~6 times over is where a transcription
// slip would hide, not because it is shared outside this one function's translation.
uint16_t frame_sprite_direct(int32_t start, int32_t count, int32_t facing_offset, int32_t divisor,
                             int32_t value) {
    return (uint16_t)(start + facing_offset * count + value / (divisor / count));
}

// The "descending" half: `value` has crossed `threshold`, so the frame index counts DOWN from
// count-1. `threshold` is 0 for state 3's frames[3] case (the .asm subtracts nothing --
// `(count-1) - value/(divisor/count)` literally), `divisor` itself for the frame_index-based pairs
// (state 0/1 frames[1], threshold==divisor==0x10), and 0x20 for state 4's progress-based frames[5]
// case. (State 0x1f's progress>=0x20 case is a DIFFERENT, flat, no-division shape -- see its own
// call site, not this helper.)
uint16_t frame_sprite_descending(int32_t start, int32_t count, int32_t facing_offset,
                                 int32_t divisor, int32_t threshold, int32_t value) {
    return (uint16_t)(start + facing_offset * count + (count - 1) -
                      (value - threshold) / (divisor / count));
}

// The continuing-animation (continuing_anim == true) advance shared by anim_state 0 and 1 --
// identical shape at @0x0042c696-0x0042c86c (state 0, tuned by STEP_SEC_1/JITTER_SCALE_1) and
// @0x0042ca4f-0x0042cd82 (state 1, tuned by STEP_SEC_2/JITTER_SCALE_2); both read/write the SAME
// frames[0]/frames[1] slots. Always ends by setting sprite_id; the just-wrapped case returns EARLY
// using frames[0] alone, matching the .asm's own early `JMP` back to the function epilogue.
void advance_running_anim(const tact_view &tv, tact_unit &u, const unit_update_anim_calls &c,
                          int32_t type_idx, double step_sec, double jitter_scale) {
    const character_type &ct = tv.character_types[type_idx];

    // @0x0042c696-0x0042c6be (state 0) / @0x0042ca4f-0x0042ca77 (state 1): still inside the current
    // animation cycle window?
    const double cycle_deadline = u.anim_cycle_time + u.frame_interval;
    if (c.time_now() <= cycle_deadline) {
        // @0x0042c871-0x0042c89d / @0x0042cc2a-0x0042cc56: a jittered per-frame deadline, re-drawn
        // from llm_rand on every call -- CALL ORDER is rand() then time_now(), preserved literally.
        const double jittered_deadline = (double)c.rand() * jitter_scale + u.anim_frame_time;
        if (jittered_deadline < c.time_now()) {
            u.anim_frame_time = c.time_now();            // @0x0042c89f / @0x0042cc58
            u.frame_index++;                             // @0x0042c8b1 / @0x0042cc6a
            if (u.frame_index > 0x1f) u.frame_index = 0; // @0x0042c8be-0x0042c8d5 / @0x0042cc77-cc8e
        }
        const int32_t count  = ct.frames[0].count;
        const int32_t facing = c.quantize_facing_dir(3, u.facing_dir);
        u.sprite_id          = (u.frame_index < 0x10)
                                   ? frame_sprite_direct(ct.frames[0].start, count, facing, 0x10,
                                                         u.frame_index)
                                   : frame_sprite_descending(ct.frames[0].start, count, facing,
                                                             0x10, 0x10, u.frame_index);
        return; // @0x0042c95c / @0x0042c9c9 (state 0), mirrored in state 1
    }

    // @0x0042c6c4-0x0042c6e5 (state 0) / @0x0042ca7d-0x0042ca9e (state 1): past the cycle window --
    // a plain per-frame step instead of a jitter re-draw.
    const double step_deadline = u.anim_frame_time + step_sec;
    if (step_deadline < c.time_now()) {
        u.anim_frame_time = c.time_now(); // @0x0042c6eb / @0x0042caa4
        u.frame_index++;
        if (u.frame_index > 0x1f) {
            // @0x0042c721-0x0042c77a / @0x0042cad3-0x0042cb33: a full cycle just completed -- reset
            // frame_index AND stamp a fresh anim_cycle_time, then compute the pose from frames[0]
            // ALONE (no threshold split, no division) and return EARLY, skipping the frames[1] logic
            // below entirely.
            u.frame_index        = 0;
            u.anim_cycle_time    = c.time_now();
            const int32_t count  = ct.frames[0].count;
            const int32_t facing = c.quantize_facing_dir(3, u.facing_dir);
            u.sprite_id          = (uint16_t)(facing * count + ct.frames[0].start);
            return;
        }
    }

    // @0x0042c77f-0x0042c86c / @0x0042cb38-0x0042cc25: no wrap this tick (either the step deadline
    // wasn't due yet, or it advanced without wrapping) -- pose from frames[1], split on frame_index
    // vs 0x10.
    const int32_t count  = ct.frames[1].count;
    const int32_t facing = c.quantize_facing_dir(3, u.facing_dir);
    u.sprite_id          = (u.frame_index < 0x10)
                               ? frame_sprite_direct(ct.frames[1].start, count, facing, 0x10,
                                                     u.frame_index)
                               : frame_sprite_descending(ct.frames[1].start, count, facing, 0x10,
                                                         0x10, u.frame_index);
}

} // namespace

void unit_update_anim(const tact_view &tv, tact_store &own, const unit_update_anim_calls &c,
                      int32_t unit_idx) {
    tact_unit &u = own.unit_at(unit_idx);

    // @0x0042c562-0x0042c57a: units whose `type` is beyond the 0..0x80 slot-occupancy range are
    // skipped entirely (no animation).
    if (u.type > 0x80) return;
    const int32_t type_idx = u.type; // reused below as the character_types index -- see the header
                                     // banner's UNCERTAINTY on this.

    // @0x0042c580-0x0042c5fb: decide whether to (re)start the animation THIS call.
    const bool cmd_active =
        (u.cmd_queue[u.cmd_index].op != 0) || (u.attack_cmd_op != 0);
    bool do_reset = cmd_active;
    if (!cmd_active) {
        const bool shift_held = (*tv.key_rshift_held & 1) != 0 || (*tv.key_lshift_held & 1) != 0;
        do_reset              = shift_held && ((u.status & 1) != 0);
    }

    // @0x0042c5ff-0x0042c638: the reset itself -- TWO SEPARATE time_now() calls (order preserved for
    // the offline oracle), not one value reused for both stamps.
    bool continuing_anim = true;
    if (do_reset) {
        u.anim_frame_time = c.time_now();
        u.frame_index     = 0;
        u.anim_cycle_time = c.time_now();
        continuing_anim   = false;
    }

    const int32_t anim_state = u.anim_state; // @0x0042c63f-0x0042c645

    if (anim_state == 0) {
        if (continuing_anim) {
            // @0x0042c696-0x0042c86c
            advance_running_anim(tv, u, c, type_idx, *tv.unit_anim_step_sec_1,
                                 *tv.unit_anim_jitter_scale_1);
        } else {
            // @0x0042c9ce-0x0042ca40: frames[2], `progress`, divisor 0x40, no extra offset.
            const character_type &ct     = tv.character_types[type_idx];
            const int32_t         count  = ct.frames[2].count;
            const int32_t         facing = c.quantize_facing_dir(3, u.facing_dir);
            u.sprite_id                  = frame_sprite_direct(ct.frames[2].start, count, facing, 0x40, u.progress);
        }
    } else if (anim_state == 1) {
        if (continuing_anim) {
            // @0x0042ca4f-0x0042cd82
            advance_running_anim(tv, u, c, type_idx, *tv.unit_anim_step_sec_2,
                                 *tv.unit_anim_jitter_scale_2);
        } else {
            // @0x0042cd87-0x0042ce09: SAME frames[2]/divisor-0x40 shape as state 0's reset pose,
            // PLUS an extra +(count/2) half-frame offset state 0's own reset pose does not have.
            const character_type &ct     = tv.character_types[type_idx];
            const int32_t         count  = ct.frames[2].count;
            const int32_t         facing = c.quantize_facing_dir(3, u.facing_dir);
            u.sprite_id                  = (uint16_t)(ct.frames[2].start + facing * count + count / 2 +
                                     u.progress / (0x40 / count));
        }
    } else if (anim_state == 2) {
        // @0x0042ce0e-0x0042d0c5: unconditional, frames[3], `progress`, divisor 0x10, ascending.
        const character_type &ct     = tv.character_types[type_idx];
        const int32_t         count  = ct.frames[3].count;
        const int32_t         facing = c.quantize_facing_dir(3, u.facing_dir);
        u.sprite_id                  = frame_sprite_direct(ct.frames[3].start, count, facing, 0x10, u.progress);
    } else if (anim_state == 3) {
        // @0x0042ce85-0x0042ceff: unconditional, frames[3] (same slot as state 2), `progress`,
        // divisor 0x10, descending with threshold 0 (no subtraction in the .asm at all).
        const character_type &ct     = tv.character_types[type_idx];
        const int32_t         count  = ct.frames[3].count;
        const int32_t         facing = c.quantize_facing_dir(3, u.facing_dir);
        u.sprite_id =
            frame_sprite_descending(ct.frames[3].start, count, facing, 0x10, /*threshold=*/0,
                                    u.progress);
    } else if (anim_state == 4) {
        // @0x0042cf04-0x0042d003: frames[5], threshold on `progress` vs 0x20, divisor 0x20. Each
        // branch makes its OWN quantize_facing_dir call (@0x0042cf40 / @0x0042cfe6), preserved
        // literally rather than hoisted.
        const character_type &ct    = tv.character_types[type_idx];
        const int32_t         count = ct.frames[5].count;
        if (u.progress < 0x20) {
            const int32_t facing = c.quantize_facing_dir(3, u.facing_dir);
            u.sprite_id          = frame_sprite_direct(ct.frames[5].start, count, facing, 0x20, u.progress);
        } else {
            const int32_t facing = c.quantize_facing_dir(3, u.facing_dir);
            u.sprite_id          = frame_sprite_descending(ct.frames[5].start, count, facing, 0x20, 0x20,
                                                           u.progress);
        }
    } else if (anim_state == 0x1f) {
        // @0x0042d008-0x0042d0c5: frames[4], threshold on `progress` vs 0x20. Below: same
        // divisor-0x20 ascending shape as state 4. At/above: a FLAT "last frame" pose -- no division
        // at all (@0x0042d0a6-0x0042d0be), NOT the descending-with-threshold shape state 4 uses.
        const character_type &ct    = tv.character_types[type_idx];
        const int32_t         count = ct.frames[4].count;
        if (u.progress < 0x20) {
            const int32_t facing = c.quantize_facing_dir(3, u.facing_dir);
            u.sprite_id          = frame_sprite_direct(ct.frames[4].start, count, facing, 0x20, u.progress);
        } else {
            const int32_t facing = c.quantize_facing_dir(3, u.facing_dir);
            u.sprite_id          = (uint16_t)(facing * count + ct.frames[4].start + count - 1);
        }
    }
    // else: anim_state is 5..0x1e, or >0x1f and !=0x1f -- no-op, matches @0x0042c672's default JMP
    // straight to the epilogue.
}

} // namespace detail

void unit_update_anim(int32_t unit_idx) {
    tact_state st = state();
    detail::unit_update_anim(st.read, st.own, live_unit_update_anim_calls(), unit_idx);
}


// ---- THE PROMOTED ARM IS GONE (fork F2E: tactical mode is demoted permanently) ------------------
//
// This TU used to carry counter-wrapped `promoted_arm::` adapters and an MH_EXPORT_REPLACE install
// for each, so our bodies could take the game's entry points. The fork's config selector has two
// hosted answers, `original` and `brokered`, and TACTICAL MODE IS ORIGINAL IN BOTH: the reimplemented
// spine the fork ships is the strategic one. So the install surface has no configuration left to be
// armed in, and an installer nothing can arm is not a dormant feature, it is a claim about what runs
// that is false in every run.
//
// THE BODIES ABOVE ARE UNTOUCHED and stay reachable two ways: the offline oracle (net_selftest
// tacttest) drives them directly, and their rebind rows survive (fork ruling Q2 -- the BIND survives,
// only the per-row runtime gate died), so a standalone host binds them unconditionally. What is gone
// is only the route that overwrote the game's own entry inside a hosted process.

} // namespace mh::tact
