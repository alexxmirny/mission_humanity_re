//
// sim/sim_projectile_tick.cpp -- see sim_projectile_tick.h. Translated from the DISASSEMBLY
// (tmp/decomp/llm_strat_projectile_tick_00440e1c.asm), not from the Ghidra .c draft -- see the
// header's banner for the two provably-dead branches, the utils_math_trunc reproduction, and the
// declared-need padding field this file references.
//
#include "sim/sim_projectile_tick.h"

#include "addr/mh_calls.gen.h" // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"       // ai_say / trace_budget -- the shared trace sink, not AI state
#include "state/host_api.h"
#include "state/host_events.h"
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"
#include "fp/x87.h"         // CRT-X87: the shared x87 truncation helpers
#include "fp/x87_shapes.h"  // CRT-X87: the hoisted x87 blocks (the asm moved, it did not change)
#include "crt/crt_select.h" // LIB-CRT: MH_CRT() picks the vendored CRT in the standalone build

namespace mh::sim {

const projectile_tick_calls &live_projectile_tick_calls() {
    static const projectile_tick_calls c = {
        MH_LIBMH_BIND(llm_strat_map_wrapped_delta),
        MH_LIBMH_BIND(llm_map_wrap_delta_row),
        MH_CRT(llm_sqrt),
        MH_LIBMH_BIND(llm_strat_unit_get_coords),
        MH_LIBMH_BIND(llm_strat_fx_anim_spawn),
        mh::state::evt::snd_play_at,
        MH_LIBMH_BIND(llm_strat_dir_from_to),
        MH_LIBMH_BIND(llm_fx_anim_dir_frame_stride),
        MH_LIBMH_BIND(llm_strat_apply_area_damage),
    };
    return c;
}

namespace {

// The tile<-fine conversion (`SAR EDX,0x1f / SHL EDX,0x5 / SBB EAX,EDX / SAR EAX,0x5`), 4 occurrences
// in this function (impact damage x2, explo-pulse damage x2). Value-for-value C's truncating `/ 32` --
// see sim_bldg_state_destroyed.cpp's / sim_order_enqueue.cpp's fine_to_tile() for the verification;
// re-derived locally per this project's per-TU convention.
inline int32_t fine_to_tile(int32_t fine) { return fine / 32; }

// See the header banner's derivation: `weapon_id*0x16c + 0xc3a520 + 0x5a == 0` is the raw address of
// `&Weapon[weapon_id].missing` wrapping to a null VA, which is PROVEN unreachable for any int32
// weapon_id (0x16c is divisible by 4, 0xc3a57a is not ≡0 mod 4, so no weapon_id solves the
// congruence). Reproduced as a literal, always-false runtime check rather than deleted.
inline bool weapon_missing_addr_wraps_to_null(int32_t weapon_id) {
    return static_cast<uint32_t>(weapon_id) * 0x16cu + 0xc3a520u + 0x5au == 0u;
}

// ---- utils_math_trunc @0x004d0596, reproduced as inline asm (ST0 in/out, x87-only) -----------------
// Same body (FSTCW/mov ah,0x1f/FLDCW/FRNDINT/FLDCW-restore, addresses 0x004d0597-0x004d05a9) as
// sim_bldg_state_destroyed.cpp's debris_intensity() / sim_unit_refund.cpp's refund_amount() reproduce
// for this identical callee -- three shape-specific helpers so each call site's whole FPU chain (from
// the first FLD/FILD through the final FISTP) stays on the 80-bit x87 stack with no round-trip through
// a 64-bit `double` local, exactly like the assembly. See the header banner for which of the function's
// nine call sites uses which shape.

// Shape A (0x00440e93 scatter_x, 0x00440eb0 scatter_y): trunc(value_int * (ratio_num / ratio_den)).
// The divide happens BEFORE the multiply -- order matters for the 80-bit intermediate.
int32_t trunc_scaled_int(int32_t value_int, double ratio_num, double ratio_den) {
    return ::mh::fp::trunc_scaled_int(value_int, ratio_num, ratio_den);
}

// Shape B (0x004412e5, fire_range[idx] alone): trunc(x).
int32_t trunc_only(double x) {
    return ::mh::fp::trunc_i32(x);
}

// Shape C (six sites: flight interp x2, smoke puff x2, explo pulse x2): trunc(a * b / c). The
// multiply happens BEFORE the divide -- order matters for the 80-bit intermediate.
int32_t trunc_mul_div(double a, double b, double c) {
    return ::mh::fp::trunc_mul_div(a, b, c);
}

} // namespace

namespace detail {

void projectile_tick(const sim_view &v, sim_store &own, const projectile_tick_calls &c) {
    projectile       &p = own.cur_projectile(); // _G_LLM_STRAT_CUR_PROJECTILE dereferenced, read+write
    const cfg_weapon &w = v.cfg_weapons[p.weapon_id];

    // 0x00440e3b-0x00440e49: elapsed = GAME_CLOCK - launch_time, read ONCE and reused throughout
    // (the flight-vs-impact decision, both duration recomputes, and both scatter trunc calls).
    const double elapsed_since_launch = *v.game_clock - p.launch_time;

    // 0x00440e4c-0x00440e61: cur->x/cur->y cached into stack locals that are WRITTEN but never READ
    // again anywhere in this 0xa89-byte function (verified across the whole body) -- a genuine dead
    // store in the original, not reproduced. Same posture as sim_prod_shuttle_depart.cpp's dead
    // accumulator.

    // 0x00440e69-0x00440ec1: per-tick scatter offset. The "reset to 0" arm (LAB_00440eba) is
    // UNREACHABLE -- see the header banner's proof -- kept literal via
    // weapon_missing_addr_wraps_to_null() rather than deleted.
    int32_t scatter_dx_frac, scatter_dy_frac;
    if (weapon_missing_addr_wraps_to_null(p.weapon_id)) {
        scatter_dx_frac = 0;
        scatter_dy_frac = 0;
    } else {
        scatter_dx_frac = trunc_scaled_int(p.scatter_x, elapsed_since_launch, p.duration);
        scatter_dy_frac = trunc_scaled_int(p.scatter_y, elapsed_since_launch, p.duration);
    }

    // 0x00440ec8-0x00440f19: is the homing target still valid? (homing_unit set, Weapon.homing==1,
    // target energy > 0). unit_of()'s bounds are unchecked, matching the original.
    const bool homing_valid = p.homing_unit != 0 && w.homing == 1 &&
                              unit_of(v, p.homing_player, p.homing_unit).energy > 0.0;

    // Impact-tile locals, populated by EITHER branch below (ballistic reads existing dst_*; homing
    // retargets and writes dst_*), consumed by the impact/flight decision and the facing/anim update
    // further down regardless of which branch ran. `recompute_facing` is the .c draft's `local_38`,
    // tracked here by what it actually gates rather than by its Ghidra stack-slot name.
    uint32_t imp_x = 0, imp_y_ground = 0, imp_y_vis = 0;
    bool     recompute_facing = false;

    if (!homing_valid) {
        // 0x004410a3-0x004410bb: SECOND occurrence of the identical dead sentinel test -- also always
        // false, so this whole block always executes (see header banner).
        if (!weapon_missing_addr_wraps_to_null(p.weapon_id)) {
            // 0x004410c1-0x00441106: local impact tile = existing dst_* + this tick's scatter, masked.
            // Does NOT write dst_x/dst_y_ground/dst_y_vis back (unlike the homing branch below) --
            // matches the field comment "dst_x: retargeted each tick if homing".
            imp_x = v.geom->bw_mask &
                    (static_cast<uint32_t>(p.dst_x) + static_cast<uint32_t>(scatter_dx_frac));
            imp_y_ground = v.geom->bh_mask & (static_cast<uint32_t>(p.dst_y_ground) +
                                              static_cast<uint32_t>(scatter_dy_frac));
            imp_y_vis    = v.geom->bh_mask &
                        (static_cast<uint32_t>(p.dst_y_vis) + static_cast<uint32_t>(scatter_dy_frac));

            // 0x0044110e-0x00441130: writes p.delta_x/p.delta_y (PERMANENT struct fields).
            c.map_wrapped_delta(p.src_x, p.src_y_vis, static_cast<int32_t>(imp_x),
                                static_cast<int32_t>(imp_y_vis), &p.delta_x, &p.delta_y);
            // 0x00441135-0x00441155: ephemeral ground-based delta, NOT stored into p -- used only for
            // the sqrt distance below.
            double ground_dx = 0.0, ground_dy = 0.0;
            c.map_wrapped_delta(p.src_x, p.src_y_ground, static_cast<int32_t>(imp_x),
                                static_cast<int32_t>(imp_y_ground), &ground_dx, &ground_dy);
            const int32_t row_delta   = c.map_wrap_delta_row(0, p.src_y_vis, 0, p.src_y_ground);
            const double  row_delta_d = static_cast<double>(row_delta);

            // 0x00441054-0x00441075: sum grouping is LEFT-TO-RIGHT as the asm computes it -- see the
            // header banner's note on why this is NOT the same grouping as the .c draft's rendering.
            const double flight_dist = c.sqrt_fn(ground_dx * ground_dx + ground_dy * ground_dy +
                                                 row_delta_d * row_delta_d);
            p.duration               = (flight_dist * w.speed) / static_cast<double>(w.length);

            if (w.type != 8) recompute_facing = true;
        }
    } else {
        // 0x00440f19-0x0044109e: homing retarget, unconditional facing recompute (no type check,
        // unlike the ballistic branch above).
        int32_t coord_x = 0, coord_y = 0;
        c.unit_get_coords(p.homing_player, p.homing_unit, &coord_x, &coord_y);

        imp_x   = v.geom->bw_mask & (static_cast<uint32_t>(coord_x) + static_cast<uint32_t>(scatter_dx_frac));
        p.dst_x = static_cast<uint16_t>(imp_x);
        imp_y_ground =
            v.geom->bh_mask & (static_cast<uint32_t>(coord_y) + static_cast<uint32_t>(scatter_dy_frac));
        p.dst_y_ground = static_cast<uint16_t>(imp_y_ground);

        // dst_y_vis = bh_mask & (dst_y_ground - (int16)target.elevation) -- matches the Ghidra .c
        // draft's rendering of the 16-bit SUB/AND sequence at 0x00440f8c-0x00440fae; the exact
        // partial-register reuse across that idiom was not independently re-derived byte-by-byte (see
        // uncertainties[]).
        const unit &target = unit_of(v, p.homing_player, p.homing_unit);
        p.dst_y_vis        = static_cast<uint16_t>(
            v.geom->bh_mask & (static_cast<uint32_t>(p.dst_y_ground) -
                               static_cast<uint32_t>(static_cast<int16_t>(target.elevation))));
        imp_y_vis = p.dst_y_vis;

        c.map_wrapped_delta(p.src_x, p.src_y_vis, p.dst_x, p.dst_y_vis, &p.delta_x, &p.delta_y);
        double ground_dx = 0.0, ground_dy = 0.0;
        c.map_wrapped_delta(p.src_x, p.src_y_ground, p.dst_x, p.dst_y_ground, &ground_dx, &ground_dy);
        const int32_t row_delta   = c.map_wrap_delta_row(0, p.src_y_vis, 0, p.src_y_ground);
        const double  row_delta_d = static_cast<double>(row_delta);

        const double flight_dist = c.sqrt_fn(ground_dx * ground_dx + ground_dy * ground_dy +
                                             row_delta_d * row_delta_d);
        p.duration               = (flight_dist * w.speed) / static_cast<double>(w.length);
        recompute_facing         = true;
    }

    // 0x004411ee-0x004411f7: still in flight, or impact?
    if (elapsed_since_launch < p.duration) {
        // 0x00441368-0x004413f6: flight interpolation. `%` matches the asm's IDIV-based remainder
        // (truncating, sign-of-dividend) exactly -- big_width/big_height are added first specifically
        // so the numerator stays non-negative.
        const int32_t dx_frac = trunc_mul_div(p.delta_x, elapsed_since_launch, p.duration);
        p.x                   = static_cast<uint16_t>(
            (static_cast<int32_t>(p.src_x) + dx_frac + static_cast<int32_t>(v.geom->big_width)) %
            static_cast<int32_t>(v.geom->big_width));
        const int32_t dy_frac = trunc_mul_div(p.delta_y, elapsed_since_launch, p.duration);
        p.y                   = static_cast<uint16_t>(
            (static_cast<int32_t>(p.src_y_vis) + dy_frac + static_cast<int32_t>(v.geom->big_height)) %
            static_cast<int32_t>(v.geom->big_height));
    } else {
        // 0x00441202-0x00441363: impact.
        c.fx_anim_spawn(imp_x, imp_y_vis, static_cast<uint32_t>(w.target_explo), *v.game_clock,
                        static_cast<uint32_t>(p.owner_player));
        p.x = static_cast<uint16_t>(imp_x);
        p.y = static_cast<uint16_t>(imp_y_vis);

        if (*v.sim_active != 0) {
            // The original offscreen_snd_volume(0x00441279)+snd_play(0x00441292) pair, fused into
            // ONE position-carrying record (the LIFT-NOTIFY offscreen conversion): the hosted sink re-runs that
            // exact pair synchronously at emit.
            c.snd_play_at(w.sound_target, fine_to_tile(static_cast<int32_t>(imp_x)),
                          fine_to_tile(static_cast<int32_t>(imp_y_ground)));
        }

        // `power`/`fire_range` are double[9] but indexed by a 4-bit mask (0..15) -- an ORIGINAL
        // out-of-bounds read for indices 9..15, reproduced exactly, not bounds-clamped (see header).
        const int32_t weapon_slot_idx = static_cast<int32_t>(p.shooter_ref) & 0xf;
        const int32_t ring_count      = trunc_only(w.fire_range[weapon_slot_idx]);
        c.apply_area_damage(fine_to_tile(static_cast<int32_t>(imp_x)),
                            fine_to_tile(static_cast<int32_t>(imp_y_ground)),
                            static_cast<int32_t>(p.owner_player), w.power[weapon_slot_idx], ring_count,
                            static_cast<uint32_t>(w.area_damage_owner_filter), // DECLARED NEED, see header
                            static_cast<uint32_t>(p.shooter_ref), p.shooter_unit);

        p.active = 0;
        own.projectile_pool_at(0).active -= 1; // slot 0's `.active` doubles as the pool's live count
    }

    // 0x004413f6-0x00441468: facing/anim-stride advance, only when a duration recompute happened
    // above (i.e. every tick except one whose weapon_id trips the provably-dead sentinel).
    if (recompute_facing) {
        const int32_t new_facing   = c.dir_from_to(p.x, p.y, static_cast<int32_t>(imp_x),
                                                   static_cast<int32_t>(imp_y_vis));
        const int32_t delta_facing = new_facing - p.facing;
        const int32_t stride       = c.fx_anim_dir_frame_stride(w.bullet_anim);
        p.anim_loop_frame += delta_facing * stride;
        p.anim_frame += delta_facing * stride;
        p.facing = new_facing;
    }

    // 0x00441468-0x00441539: bullet-anim advance loop. `time_step` is cached ONCE before the loop
    // (the asm's dVar19/dVar2, both the same value read once) -- `.next` is re-read fresh each
    // iteration because `p.anim_frame` moves under it.
    {
        const int32_t frame0    = p.anim_frame;
        const double  time_step = v.anim_frames[frame0 + 1].time;
        if (time_step != 0.0) { // asm's 64-bit-halves nonzero test is just `time_step != 0.0`
            double time_left = *v.game_clock - p.anim_clock;
            p.anim_clock     = *v.game_clock;
            while (0.0 < time_left) {
                if (time_step <= time_left) {
                    const int32_t next = v.anim_frames[p.anim_frame + 1].next;
                    if (next == 0) {
                        p.anim_frame = p.anim_loop_frame;
                    } else {
                        p.anim_frame += next;
                    }
                    time_left -= time_step;
                } else {
                    p.anim_clock -= time_left;
                    time_left = 0.0;
                }
            }
        }
    }

    // 0x0044153e-0x004416ce: smoke-puff emission loop, gated on Weapon.smoke_sprite != 0.
    if (w.smoke_sprite != 0) {
        const double impact_time  = p.launch_time + p.duration; // cached ONCE before the loop
        const double smoke_period = w.smoke_time;               // cached ONCE (asm's dVar8/dVar7/dVar6)
        if (smoke_period != 0.0) {
            double time_left = *v.game_clock - p.smoke_clock;
            while (0.0 < time_left) {
                if (time_left < smoke_period) {
                    time_left = 0.0;
                } else {
                    p.smoke_clock += smoke_period;
                    const double puff_dt = p.smoke_clock - p.launch_time;
                    if (impact_time <= p.smoke_clock) {
                        time_left = 0.0;
                    } else {
                        const int32_t dy_frac = trunc_mul_div(p.delta_y, puff_dt, p.duration);
                        const int32_t tile_y =
                            (static_cast<int32_t>(v.geom->big_height) + static_cast<int32_t>(p.src_y_vis) +
                             dy_frac) %
                            static_cast<int32_t>(v.geom->big_height);
                        const int32_t dx_frac = trunc_mul_div(p.delta_x, puff_dt, p.duration);
                        const int32_t tile_x =
                            (static_cast<int32_t>(v.geom->big_width) + static_cast<int32_t>(p.src_x) +
                             dx_frac) %
                            static_cast<int32_t>(v.geom->big_width);
                        c.fx_anim_spawn(static_cast<uint32_t>(tile_x), static_cast<uint32_t>(tile_y),
                                        static_cast<uint32_t>(w.smoke_sprite), p.smoke_clock, 1u);
                        time_left -= smoke_period;
                    }
                }
            }
        }
    }

    // 0x004416ce-0x0044189b: in-flight explosion-pulse damage loop, gated on explo_pulse_flag != 0.
    if (p.explo_pulse_flag != 0) {
        const double explo_period = w.explo_time;               // cached ONCE
        const double impact_time  = p.launch_time + p.duration; // cached ONCE, own stack slot
        double       time_left    = *v.game_clock - p.explo_pulse_clock;
        while (0.0 < time_left) {
            if (time_left < explo_period) {
                time_left = 0.0;
            } else {
                p.explo_pulse_clock += explo_period;
                const double pulse_dt = p.explo_pulse_clock - p.launch_time;
                if (impact_time <= p.explo_pulse_clock) {
                    time_left = 0.0;
                } else {
                    const int32_t dy_frac = trunc_mul_div(p.delta_y, pulse_dt, p.duration);
                    const int32_t tile_y  = (static_cast<int32_t>(v.geom->big_height) +
                                            static_cast<int32_t>(p.src_y_ground) + dy_frac) %
                                           static_cast<int32_t>(v.geom->big_height);
                    const int32_t dx_frac = trunc_mul_div(p.delta_x, pulse_dt, p.duration);
                    const int32_t tile_x  = (static_cast<int32_t>(v.geom->big_width) +
                                            static_cast<int32_t>(p.src_x) + dx_frac) %
                                           static_cast<int32_t>(v.geom->big_width);
                    c.apply_area_damage(fine_to_tile(tile_x), fine_to_tile(tile_y),
                                        static_cast<int32_t>(p.owner_player), static_cast<double>(w.power_),
                                        w.explo_time_,
                                        static_cast<uint32_t>(w.area_damage_owner_filter), // DECLARED NEED
                                        static_cast<uint32_t>(p.shooter_ref), p.shooter_unit);
                    time_left -= explo_period;
                }
            }
        }
    }
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

void projectile_tick() {
    sim_state st = state();
    detail::projectile_tick(st.read, st.own, live_projectile_tick_calls());
}


} // namespace mh::sim
