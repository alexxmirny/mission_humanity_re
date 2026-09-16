//
// sim/sim_weapon_projectile_spawn.cpp -- see sim_weapon_projectile_spawn.h. Translated from the
// DISASSEMBLY (tmp/decomp_sim1e2/*.asm), not from the Ghidra .c drafts.
//
#include "sim/sim_weapon_projectile_spawn.h"

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"        // ai_say / trace_budget -- the shared trace sink, not AI state
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"
#include "fp/x87_shapes.h"  // CRT-X87: the hoisted x87 blocks (the asm moved, it did not change)
#include "crt/crt_select.h" // LIB-CRT: MH_CRT() picks the vendored CRT in the standalone build

namespace mh::sim {

const weapon_scatter_offset_calls &live_weapon_scatter_offset_calls() {
    static const weapon_scatter_offset_calls c = {
        MH_LIBMH_BIND(llm_strat_pixel_delta_wrapped),
        MH_LIBMH_BIND(llm_rand_below),
    };
    return c;
}

const projectile_spawn_calls &live_projectile_spawn_calls() {
    static const projectile_spawn_calls c = {
        MH_LIBMH_BIND(llm_strat_pixel_delta_wrapped),
        MH_LIBMH_BIND(llm_rand_below),
        MH_LIBMH_BIND(llm_strat_dir_from_to),
        MH_LIBMH_BIND(llm_strat_dir_sector_to),
        MH_LIBMH_BIND(llm_strat_map_wrapped_delta),
        MH_LIBMH_BIND(llm_map_wrap_delta_row),
        MH_CRT(llm_sqrt),
    };
    return c;
}

namespace {

// ---- llm_strat_weapon_scatter_offset's two shift-form idioms (0x0048c6b2), reproduced literally --
// per translator-brief rule 8. Both are PROVEN equivalent to the natural C++ operator (see the
// header banner), reproduced this way anyway for maximum fidelity to the bytes.

// `(x ^ (x>>31)) - (x>>31)` -- the CDQ/XOR/SUB abs() idiom (0x0048c703-0x0048c712 /
// 0x0048c70d-0x0048c712). Handles INT_MIN the same way the asm does (wraps to itself) without
// invoking C++'s signed-negation UB a ternary `-x` would.
inline int32_t abs_i32_asm(int32_t x) {
    const int32_t sign = x >> 31;
    return (x ^ sign) - sign;
}

// `(x - (x>>31)) >> 1` -- the signed-divide-by-2 idiom (0x0048c744-0x0048c751 /
// 0x0048c766-0x0048c773). Identical to C++'s truncating `x/2` for every input; kept in shift form
// per rule 8 rather than substituted.
inline int32_t half_trunc_asm(int32_t x) {
    const int32_t sign = x >> 31;
    return (x - sign) >> 1;
}

// ---- utils_math_trunc @0x004d0596, reproduced as inline asm (ST0 in/out, x87-only) ---------------
// Same body (FSTCW/mov ah,0x1f/FLDCW/FRNDINT/FLDCW-restore, 0x004d0597-0x004d05a9) as
// sim_projectile_tick.cpp/sim_bldg_state_destroyed.cpp/sim_unit_refund.cpp reproduce for this
// identical callee -- two more shape-specific helpers, one per call site in
// llm_strat_projectile_spawn's weapon-type-8 branch (see the header banner's (3)).

// 0x00464b2c (dx = trunc(cos * range_diff_scaled)): trunc(a*b).
int32_t trunc_mul(double a, double b) {
    return ::mh::fp::trunc_mul(a, b);
}

// 0x00464b5f (dy = trunc(sin * range_diff_scaled * scale_y)): trunc(a*b*c).
int32_t trunc_mul_mul(double a, double b, double c) {
    return ::mh::fp::trunc_mul_mul(a, b, c);
}

// See the header banner's derivation: `weapon_id*0x16c + 0xc3a520 + 0x5a == 0` is the raw address of
// `&Weapon[weapon_id].missing` wrapping to a null VA, PROVEN unreachable for any int32 weapon_id --
// same proof sim_projectile_tick.cpp's identically-named helper already establishes for the same
// callee; this TU keeps its own copy per house convention (no cross-TU helper sharing).
inline bool weapon_missing_addr_wraps_to_null(int32_t weapon_id) {
    return static_cast<uint32_t>(weapon_id) * 0x16cu + 0xc3a520u + 0x5au == 0u;
}

} // namespace

namespace detail {

// ---- llm_fx_anim_dir_frame_stride @0x0046177b -----------------------------------------------------

int32_t fx_anim_dir_frame_stride(const sim_view &v, int32_t start_frame) {
    int32_t idx   = start_frame;
    int32_t count = 0;
    for (;;) {
        ++count;
        // 0x004617b2/0x004617c1: Anim[idx+1].next, read once and reused for both the advance and the
        // loop test (the asm re-fetches the same address for each; nothing writes Anim between the
        // two reads -- see the header banner).
        const int32_t next = v.anim_frames[idx + 1].next;
        idx += next;
        if (next == 0) break;
    }
    return count;
}

// ---- llm_strat_fx_anim_spawn @0x00464855 -----------------------------------------------------------

uint32_t fx_anim_spawn(sim_store &own, uint32_t x, uint32_t y, uint32_t anim_frame_id, double elapsed,
                       uint32_t layer) {
    // 0x00464874-0x00464887: pool[0].live is the live-count header; if it has already hit the cap
    // (FX_ANIM_POOL_CAP-1 == 9999), the scan is skipped entirely.
    if (own.fx_anim_pool_at(0).live == FX_ANIM_POOL_CAP - 1) return 0;

    for (int32_t slot = 1; slot < FX_ANIM_POOL_CAP; ++slot) {
        fx_anim &e = own.fx_anim_pool_at(slot);
        if (e.live != 0) continue; // occupied, keep scanning

        // 0x004648b3-0x00464913: write order matches the asm exactly (layer, timestamp, anim_frame,
        // x, y, live, then the header bump) -- not that order is observable here, but faithful.
        e.layer      = static_cast<uint8_t>(layer);
        e.timestamp  = elapsed;
        e.anim_frame = static_cast<int32_t>(anim_frame_id);
        e.x          = static_cast<uint16_t>(x);
        e.y          = static_cast<uint16_t>(y);
        e.live       = 1;
        own.fx_anim_pool_at(0).live += 1;
        return 1;
    }
    return 0;
}

// ---- llm_strat_weapon_scatter_offset @0x0048c6b2 -----------------------------------------------------

void weapon_scatter_offset(int32_t shooter_experience_or_zero, int32_t weapon_missing_scale,
                           int32_t src_x, int32_t src_y, int32_t dst_x, int32_t dst_y,
                           int32_t *out_scatter_x, int32_t *out_scatter_y,
                           const weapon_scatter_offset_calls &c) {
    // 0x0048c6d3-0x0048c6ec: wrapped pixel delta between src and dst.
    int32_t dx = 0, dy = 0;
    c.pixel_delta_wrapped(src_x, src_y, dst_x, dst_y, &dx, &dy);

    // 0x0048c6ec-0x0048c6fd: accuracy_divisor = shooter_experience_or_zero/10 + 1 (ordinary signed
    // IDIV, truncating toward zero -- C++'s `/` matches exactly, no shift form used at this site).
    const int32_t accuracy_divisor = shooter_experience_or_zero / 10 + 1;

    // 0x0048c700-0x0048c712: sum of the two absolute deltas.
    const int32_t abs_sum = abs_i32_asm(dy) + abs_i32_asm(dx);

    // 0x0048c714-0x0048c730: two more ordinary signed IDIVs (both truncate toward zero, matching
    // C++'s `/`); the SAR-then-IDIV pairs in the asm are plain CDQ-equivalent sign extension ahead
    // of the divide, not an alternate rounding rule.
    const int32_t upper_bound = weapon_missing_scale * abs_sum / accuracy_divisor / 200;

    if (upper_bound == 0) {
        *out_scatter_x = 0;
        *out_scatter_y = 0;
        return;
    }

    // 0x0048c739-0x0048c77b: two INDEPENDENT rand_below draws, in this exact order (x then y) --
    // preserving the order is load-bearing for RNG-stream determinism.
    const int32_t draw_x = c.rand_below(upper_bound);
    *out_scatter_x       = half_trunc_asm(upper_bound) - draw_x;
    const int32_t draw_y = c.rand_below(upper_bound);
    *out_scatter_y       = half_trunc_asm(upper_bound) - draw_y;
}

// ---- llm_strat_projectile_spawn @0x00464932 -----------------------------------------------------

int32_t projectile_spawn(const sim_view &v, sim_store &own, const projectile_spawn_calls &c,
                         int32_t src_x, int32_t src_y_ground, int32_t src_y_vis_offset, uint32_t dst_x,
                         uint32_t dst_y_ground, int32_t dst_y_vis_offset, int32_t weapon_id,
                         uint8_t owner_player, uint16_t homing_player_and_flags,
                         int32_t homing_target_unit, uint32_t shooter_ref, int32_t shooter_unit_index) {
    // 0x00464953-0x00464966: pool full? (0x5db == PROJECTILE_POOL_CAP-1 live entries -- every usable
    // slot [1..PROJECTILE_POOL_CAP-1] occupied.)
    if (v.projectile_pool[0].active == PROJECTILE_POOL_CAP - 1) return 0;

    const cfg_weapon &w           = v.cfg_weapons[weapon_id];
    const uint8_t     weapon_type = w.type; // read ONCE, before the scan (0x0046496b-0x00464979),
                                            // matching the original's harmless redundant-if-full read

    for (int32_t slot = 1; slot < PROJECTILE_POOL_CAP; ++slot) {
        projectile &p = own.projectile_pool_at(slot);
        if (p.active != 0) continue; // occupied, keep scanning

        p.shooter_ref  = static_cast<uint16_t>(shooter_ref);
        p.shooter_unit = shooter_unit_index;
        p.weapon_id    = weapon_id;
        p.owner_player = owner_player;

        // (1) 0x004649dc-0x004649fb: both derived ONCE, before any branch. `dst_x`/`dst_y_ground` are
        // ordinary mutable locals from here (by-value parameters); the type-8 branch below may
        // overwrite them, matching the original's reuse of their stack-parameter slots as scratch.
        // THE MASK PAIR IS v.geom->bw_mask/bh_mask, NOT map_width_mask(v)/map_height_mask(v). All
        // four mask reads in this function are `general` @+0x0 / @+0x4 -- the PIXEL-space wrap pair
        // (0x00e15390 / 0x00e15394 at 0x004649e2, 0x004649f3, 0x00464b3a, 0x00464b6d) -- while the
        // two named helpers read width_mask @+0x8 / height_mask @+0x20, the TILE-space pair. Every
        // coordinate this function handles is in map pixels, so the tile masks truncate: with a
        // 256-tile map, height_mask is 0xff and a src_y of 0x1a26 becomes 0x26. Caught by the shadow
        // arm 2026-08-15 (6850 divergences in 6855 calls, first differing byte projectile+0x11 = the
        // HIGH byte of src_y_vis, original=1A ours=00 -- the low byte matched, which is the
        // signature of an 8-bit truncation rather than a wrong value). Exactly the trap
        // sim_bldg_get_coords.h's banner already warns about by name.
        uint32_t src_y_vis = v.geom->bh_mask &
                             (static_cast<uint32_t>(src_y_ground) - static_cast<uint32_t>(src_y_vis_offset));
        uint32_t dst_y_vis = v.geom->bh_mask & (dst_y_ground - static_cast<uint32_t>(dst_y_vis_offset));

        // (2) f is used to index BOTH units[f][...] (bound MAX_PLAYERS=8) and Weapon.missing/
        // range_min/range_max[f] (bound 9) -- an ORIGINAL unchecked out-of-bounds read for f in
        // [8,15]/[9,15] respectively, reproduced by indexing exactly as the asm does. See the header
        // banner's (2)/(3).
        const uint32_t f = shooter_ref & 0xfu;

        if (!weapon_missing_addr_wraps_to_null(weapon_id)) { // ALWAYS true -- see the header banner
            const int32_t shooter_experience_or_zero =
                (shooter_ref & 0x80u) == 0 ? 0 : unit_of(v, f, shooter_unit_index).experience;
            int32_t scatter_x = 0, scatter_y = 0;
            weapon_scatter_offset(shooter_experience_or_zero, w.missing[f], src_x, src_y_ground,
                                  static_cast<int32_t>(dst_x), static_cast<int32_t>(dst_y_ground),
                                  &scatter_x, &scatter_y,
                                  weapon_scatter_offset_calls{c.pixel_delta_wrapped, c.rand_below});
            p.scatter_x = scatter_x;
            p.scatter_y = scatter_y;
        }

        // (3) 0x00464a9c-0x00464b9d.
        if (weapon_type == 8) {
            p.facing = unit_of(v, f, shooter_unit_index).facing_target;

            const int32_t range_diff = w.range_max[f] - w.range_min[f]; // same f, same OOB pattern
            // 0x00464b0f. _G_LLM_STRAT_PROJECTILE_DIST_SCALE == 16.0. The translator named this
            // `..._offset_scale_x` and its partner `..._scale_y`, reading them as an (x,y) pair;
            // the conductor renamed both when registering the regions, because the assembly is not
            // symmetric: THIS one is applied once and feeds BOTH axes, and the other (-1.0) is a
            // sign flip on the sin component alone. The old names would have made an
            // apply-to-both-axes bug look correct.
            const double range_diff_scaled = static_cast<double>(range_diff) * (*v.projectile_dist_scale);

            const facing_trig &trig = v.facing_trig_table[p.facing];
            const int32_t      dx   = trunc_mul(trig.cos, range_diff_scaled);
            // 0x00464b3a reads `general` @+0x0 = bw_mask, the PIXEL-space X mask -- see the note at
            // the src_y_vis derivation above.
            dst_x = v.geom->bw_mask & (static_cast<uint32_t>(src_x) + static_cast<uint32_t>(dx));

            // 0x00464b59: FMUL by _G_LLM_STRAT_PROJECTILE_DIR_Y_SIGN (-1.0), sin component ONLY.
            const int32_t dy = trunc_mul_mul(trig.sin, range_diff_scaled, *v.projectile_dir_y_sign);
            // 0x00464b6d reads `general` @+0x4 = bh_mask, the PIXEL-space Y mask.
            dst_y_ground = v.geom->bh_mask & (static_cast<uint32_t>(src_y_ground) + static_cast<uint32_t>(dy));

            // 0x00464b78-0x00464b7b: dst_y_vis is set EQUAL to the recomputed dst_y_ground here, NOT
            // offset-adjusted like (1) above -- an original asymmetry, preserved rather than "fixed".
            dst_y_vis = dst_y_ground;
        } else {
            p.facing = c.dir_from_to(src_x, static_cast<int32_t>(src_y_vis), static_cast<int32_t>(dst_x),
                                     static_cast<int32_t>(dst_y_vis));
        }

        // (4) 0x00464b9d-0x00464c64. `fx_anim_dir_frame_stride` is MY sibling in this unit -- called
        // directly, not through `c`, exactly once per spawn.
        if (weapon_type == 5 || weapon_type == 6) {
            const int32_t dir_sector =
                c.dir_sector_to(src_x, static_cast<int32_t>(src_y_vis), static_cast<int32_t>(dst_x),
                                static_cast<int32_t>(dst_y_vis));
            const int32_t stride = fx_anim_dir_frame_stride(v, w.bullet_anim);
            p.anim_frame         = (dir_sector - 1) * stride + w.bullet_anim;
        } else {
            const int32_t stride = fx_anim_dir_frame_stride(v, w.bullet_anim);
            p.anim_frame         = w.bullet_anim + stride * (p.facing - 1);
        }
        p.anim_loop_frame = p.anim_frame;

        // (5) 0x00464c64-0x00464cce.
        p.smoke_sprite = (w.smoke_time == 0.0 || w.smoke_sprite == 0) ? 0 : w.smoke_sprite;

        // (6) 0x00464cce-0x00464d22.
        p.src_x        = static_cast<uint16_t>(src_x);
        p.src_y_ground = static_cast<uint16_t>(src_y_ground);
        p.src_y_vis    = static_cast<uint16_t>(src_y_vis);
        p.dst_x        = static_cast<uint16_t>(dst_x);
        p.dst_y_ground = static_cast<uint16_t>(dst_y_ground);
        p.dst_y_vis    = static_cast<uint16_t>(dst_y_vis);

        c.map_wrapped_delta(src_x, static_cast<int32_t>(src_y_vis), static_cast<int32_t>(dst_x),
                            static_cast<int32_t>(dst_y_vis), &p.delta_x, &p.delta_y);

        double ground_dx = 0.0, ground_dy = 0.0;
        c.map_wrapped_delta(src_x, src_y_ground, static_cast<int32_t>(dst_x),
                            static_cast<int32_t>(dst_y_ground), &ground_dx, &ground_dy);
        // Reads the just-written, already-truncated `p.src_y_vis`/`p.src_y_ground` back out, matching
        // the asm exactly (see the header banner's (6)), not the pre-truncation locals.
        const int32_t row_delta = c.map_wrap_delta_row(0, p.src_y_vis, 0, p.src_y_ground);

        // (7) 0x00464d92-0x00464dc8: one clock read, stamped into all four fields.
        const double now    = *v.game_clock;
        p.smoke_clock       = now;
        p.explo_pulse_clock = now;
        p.anim_clock        = now;
        p.launch_time       = now;

        // (8) 0x00464dd2-0x00464e34.
        if (w.homing == 1 && (homing_player_and_flags & 0x40u) == 0 && weapon_type != 8) {
            p.homing_player = homing_player_and_flags & 0xfu;
            p.homing_unit   = homing_target_unit;
        } else {
            p.homing_player = 0;
            p.homing_unit   = 0;
        }

        // (9) 0x00464e34-0x00464eb1.
        if (w.explo_time == 0.0) {
            p.explo_pulse_clock = 0.0;
            p.explo_pulse_flag  = 0;
        } else {
            p.explo_pulse_clock = *v.game_clock + w.explo_pulse_initial_delay; // DECLARED NEED
            p.explo_pulse_flag  = 1;
        }

        // (10) 0x00464ebf-0x00464f06: left-to-right sum grouping, matching the asm's FADDP pairing.
        const double row_delta_d = static_cast<double>(row_delta);
        const double flight_dist =
            c.sqrt_fn(ground_dx * ground_dx + ground_dy * ground_dy + row_delta_d * row_delta_d);
        p.duration = flight_dist * w.speed / static_cast<double>(w.length);

        // (11) 0x00464f06-0x00464f1a.
        own.projectile_pool_at(0).active += 1;
        p.active = 1;
        return 1;
    }
    return 0;
}

} // namespace detail

// ---- the public wrappers --------------------------------------------------------------------------

int32_t fx_anim_dir_frame_stride(int32_t start_frame) {
    const sim_view v = state().read;
    return detail::fx_anim_dir_frame_stride(v, start_frame);
}

uint32_t fx_anim_spawn(uint32_t x, uint32_t y, uint32_t anim_frame_id, double elapsed, uint32_t layer) {
    sim_state st = state();
    return detail::fx_anim_spawn(st.own, x, y, anim_frame_id, elapsed, layer);
}

void weapon_scatter_offset(int32_t param_1, int32_t param_2, uint32_t a2, uint32_t param_4,
                           uint32_t param_5, uint32_t param_6, int32_t *param_7, int32_t *param_8) {
    detail::weapon_scatter_offset(param_1, param_2, static_cast<int32_t>(a2), static_cast<int32_t>(param_4),
                                  static_cast<int32_t>(param_5), static_cast<int32_t>(param_6), param_7,
                                  param_8, live_weapon_scatter_offset_calls());
}

int32_t projectile_spawn(int32_t src_x, int32_t src_y_ground, int32_t src_y_vis_offset, uint32_t dst_x,
                         uint32_t dst_y_ground, int32_t dst_y_vis_offset, int32_t weapon_id,
                         uint8_t owner_player, uint16_t homing_player_and_flags,
                         int32_t homing_target_unit, uint32_t shooter_ref, int32_t shooter_unit_index) {
    sim_state st = state();
    return detail::projectile_spawn(st.read, st.own, live_projectile_spawn_calls(), src_x, src_y_ground,
                                    src_y_vis_offset, dst_x, dst_y_ground, dst_y_vis_offset, weapon_id,
                                    owner_player, homing_player_and_flags, homing_target_unit,
                                    shooter_ref, shooter_unit_index);
}


} // namespace mh::sim
