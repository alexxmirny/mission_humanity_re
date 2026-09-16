//
// sim/sim_unit_fire_weapon.cpp -- see sim_unit_fire_weapon.h for the full derivation (preserved bug,
// switch-fallthrough structure, own_class-vs-target_class distinction, and the declared needs on the
// two sibling headers this file calls into).
//
#include "sim/sim_unit_fire_weapon.h"

#include "addr/mh_calls.gen.h" // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"       // ai_say / trace_budget -- the shared trace sink, not AI state

// Reimplemented siblings. bldg_get_coords is still called DIRECTLY (pure, building-target range-check
// path only); mount_pos / soldier_screen_pos are now ROUTED THROUGH unit_fire_weapon_calls, bound to
// these same public wrappers in the live path (2026-08-19 T3->T1 seam -- see the header struct comment).
#include "sim/sim_bldg_get_coords.h"
#include "sim/sim_unit_mount_pos.h"
#include "sim/sim_unit_soldier_screen_pos.h"

// projectile_spawn / fx_anim_* / weapon_scatter_offset / apply_area_damage: also routed through
// unit_fire_weapon_calls (bound to these headers' public wrappers), same 2026-08-19 seam.
#include "sim/sim_combat_kill_credit.h"
#include "sim/sim_weapon_projectile_spawn.h"
#include "state/host_api.h"
#include "state/host_events.h"
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"
#include "fp/x87.h" // CRT-X87: the shared x87 truncation helpers

namespace mh::sim {

const unit_fire_weapon_calls &live_unit_fire_weapon_calls() {
    static const unit_fire_weapon_calls c = {
        // (a) ORIGINAL callees via mh::call::
        MH_LIBMH_BIND(llm_strat_dist_out_of_range),
        MH_LIBMH_BIND(llm_strat_dir_from_to),
        MH_LIBMH_BIND(llm_rand_below),
        mh::state::evt::snd_play_at,
        // (b) reimplemented siblings via their own public wrappers -- indirect call to the SAME
        // function the arms used to call directly (production behaviour-identical; see the header
        // struct comment for why they are seamed rather than called directly).
        mh::sim::calc_mount_fine_pos,
        mh::sim::calc_mount_render_pos,
        mh::sim::unit_soldier_get_sprite_screen_pos,
        mh::sim::projectile_spawn,
        mh::sim::fx_anim_dir_frame_stride,
        mh::sim::fx_anim_spawn,
        mh::sim::weapon_scatter_offset,
        mh::sim::apply_area_damage,
    };
    return c;
}

namespace {

// The tile<-fine conversion (`SAR EDX,0x1f / SHL EDX,0x5 / SBB EAX,EDX / SAR EAX,0x5`), reproduced per
// this project's per-TU convention (see sim_projectile_tick.cpp / sim_bldg_state_destroyed.cpp's
// identical helper) -- value-for-value C's truncating `/ 32`.
inline int32_t fine_to_tile(int32_t fine) { return fine / 32; }

// ---- utils_math_trunc @0x004d0596, reproduced as inline asm (ST0 in/out, x87-only) -----------------
// ONE call site here (case 3's ring_count, 0x0048c19b), same FSTCW/mov ah,0x1f/FLDCW/FRNDINT/FLDCW-
// restore/FISTP body as sim_projectile_tick.cpp's `trunc_only()` reproduces for this identical callee
// -- copied verbatim rather than a `static_cast<int32_t>` of a C++ double, which would round at 64-bit
// precision instead of the assembly's 80-bit x87 stack (see uncertainties[]).
int32_t trunc_only(double x) {
    return ::mh::fp::trunc_i32(x);
}

} // namespace

namespace detail {

void unit_fire_weapon(const sim_view &v, sim_store &own, const unit_fire_weapon_calls &c,
                      uint32_t player_raw, uint32_t unit_index_raw, uint8_t weapon_slot_select,
                      uint32_t target_ref, uint16_t target_index, int32_t target_fine_x,
                      int32_t target_fine_y) {
    // Every use of `player` in the original reads only the low 16 bits (`word ptr [EBP-0x1c]`); the
    // low 32 bits of `unit_index` are used in full (dword IMULs).
    const uint16_t player     = static_cast<uint16_t>(player_raw);
    const int32_t  unit_index = static_cast<int32_t>(unit_index_raw);

    unit &u = own.unit_at(player, unit_index);

    // 0x0048bb8d-0x0048bbaa: loop bound. See the header banner's PRESERVED BUG note -- the loop's
    // STARTING slot is unconditionally 0 (below), regardless of weapon_slot_select's value when it is
    // not the ==4 sentinel; that value's only observable effect is this bound.
    const int32_t loop_bound = (weapon_slot_select == 4) ? 4 : 1;

    // 0x0048bbb1-0x0048bc22: resolve the RANGE-CHECK-ONLY tile coordinates. See the header banner:
    // this does NOT override target_fine_x/target_fine_y -- those stay the caller's raw incoming fine
    // coordinates and are what every other use in this function reads directly.
    int32_t range_check_fine_x = target_fine_x;
    int32_t range_check_fine_y = target_fine_y;
    if ((target_ref & 0x40u) != 0) {
        mh::sim::bldg_get_coords(static_cast<uint16_t>(target_ref & 0xfu), target_index,
                                 &range_check_fine_x, &range_check_fine_y);
    }
    const int32_t target_tile_x = fine_to_tile(range_check_fine_x);
    const int32_t target_tile_y = fine_to_tile(range_check_fine_y);

    // 0x0048bc25-0x0048bc2c: PRESERVED BUG -- slot always starts at 0 (see the header banner).
    int32_t matched_count = 0;
    for (int32_t slot = 0; slot < loop_bound; ++slot) {
        unit_weapon &ws = u.weapons[slot];
        if (!ws.enabled_2) continue;
        ++matched_count;
        if (!ws.enabled) continue;

        const uint8_t     wid = ws.weapon_id;
        const cfg_weapon &w   = v.cfg_weapons[wid];

        // 0x0048bcb4-0x0048bcda: own_class, read ONLY by the two muzzle-flash fx_anim_spawn calls
        // below (case 4 / case 3) -- see the header banner's own_class-vs-target_class note.
        const int32_t own_elevation = u.elevation;
        const int32_t own_class     = (own_elevation != 0) ? 2 : 1;

        // 0x0048bcde-0x0048bd21: target_class, from the TARGET's elevation when target_ref names a
        // live unit target (bits 0x80|0x20, tested as &0xa0).
        int32_t target_elevation = 0;
        int32_t target_class     = 1;
        if ((target_ref & 0xa0u) != 0) {
            target_elevation = unit_of(v, target_ref & 0xfu, target_index).elevation;
            if (target_elevation != 0) target_class = 2;
        }

        const uint8_t target_mask = (target_class == 1) ? WEAPON_TARGET_GROUND : WEAPON_TARGET_AIR;
        if ((w.target & target_mask) == 0) continue;

        // 0x0048bd57-0x0048bdde: range check, ONLY in "auto" (weapon_slot_select==4) mode -- see the
        // header banner.
        if (loop_bound > 1) {
            const uint32_t out_of_range =
                c.dist_out_of_range(w.range_min[player] - 1, w.range_max[player] + 1, u.x, u.y,
                                    target_tile_x, target_tile_y);
            if (out_of_range != 0) continue;
        }

        // ---- fire: mount/soldier screen position, recomputed fresh at each point the original does
        // (never cached across the switch's fallthrough -- see the header banner on the double-draw
        // this implies for llm_rand_below when the block runs twice).
        int32_t    mount_x = 0, mount_y = 0, render_y = 0;
        const auto recompute_mount_or_soldier_pos = [&](uint32_t mount_idx) {
            const int32_t soldier_count = v.cfg_units[u.unit_proto_id].soldier_count;
            if (soldier_count == 0) {
                mount_x  = static_cast<int32_t>(c.calc_mount_fine_pos(player, unit_index, mount_idx, '\x01'));
                mount_y  = static_cast<int32_t>(c.calc_mount_fine_pos(player, unit_index, mount_idx, '\0'));
                render_y = c.calc_mount_render_pos(player, unit_index, mount_idx, '\0');
            } else {
                const int32_t soldier_hop = c.rand_below(soldier_count);
                uint32_t      sx = 0, sy = 0;
                c.soldier_screen_pos(player, unit_index, soldier_hop, &sx, &sy);
                mount_x  = static_cast<int32_t>(sx);
                mount_y  = static_cast<int32_t>(sy);
                render_y = mount_y;
            }
        };

        // Shared by the case-2/6 and case-1/5/8 bodies (0x0048c408-0x0048c536): identical argument
        // shape, only mount_x/mount_y/own_elevation/target_elevation differ by which recompute ran.
        const auto fire_projectile = [&]() {
            c.projectile_spawn(
                mount_x, mount_y, own_elevation, static_cast<uint32_t>(target_fine_x),
                static_cast<uint32_t>(target_fine_y), target_elevation, static_cast<int32_t>(wid),
                static_cast<uint8_t>(target_class), static_cast<uint16_t>(target_ref), target_index,
                static_cast<uint32_t>(player) | 0x80u, unit_index);
        };

        // Shared ammo/pocket/reload_timer/enabled bookkeeping, identical in case 1/5/8 (0x0048c53b-
        // 0x0048c67d) and case 3 (0x0048c1f4-0x0048c33d).
        const auto consume_ammo_and_reload = [&]() {
            ws.ammo -= 1;
            if (ws.ammo == 0) {
                if (ws.pocket != -1) ws.pocket -= 1;
                if (ws.pocket == 0) {
                    ws.enabled_2 = 0;
                } else {
                    ws.reload_timer = w.long_time;
                }
            } else {
                ws.reload_timer = w.short_time;
            }
            ws.enabled = 0;
        };

        switch (w.type) {
            case 2:
            case 6: {
                // case 2/6 body (0x0048c342-0x0048c414): a first, sound-less, ammo-less shot.
                recompute_mount_or_soldier_pos(1);
                fire_projectile();
                [[fallthrough]];
            }
            case 1:
            case 5:
            case 8: {
                // case 1/5/8 body (0x0048c419-0x0048c67d): full shot -- fresh recompute, sound, spawn,
                // ammo/reload bookkeeping.
                recompute_mount_or_soldier_pos(1);
                if (*v.sim_active != 0) {
                    // The original offscreen_snd_volume(0x0048c451)+snd_play(0x0048c46a) pair,
                    // fused into ONE position-carrying record (the LIFT-NOTIFY offscreen conversion): the hosted
                    // sink re-runs that exact pair synchronously at emit.
                    c.snd_play_at(w.sound_fire, fine_to_tile(mount_x), fine_to_tile(render_y));
                }
                fire_projectile();
                consume_ammo_and_reload();
                break;
            }
            case 4: {
                // case 4 body (0x0048be2f-0x0048bf25): muzzle-flash-only fx_anim_spawn (own_class), then
                // falls through into case 3's body.
                recompute_mount_or_soldier_pos(2);
                const int32_t facing = c.dir_from_to(mount_x, mount_y, target_fine_x, target_fine_y);
                const int32_t stride = c.fx_anim_dir_frame_stride(w.fite_explo);
                c.fx_anim_spawn(static_cast<uint32_t>(mount_x), static_cast<uint32_t>(render_y),
                                static_cast<uint32_t>(w.fite_explo + stride * (facing - 1)),
                                *v.game_clock, static_cast<uint32_t>(own_class));
                [[fallthrough]];
            }
            case 3: {
                // case 3 body (0x0048bf2a-0x0048c33d): scatter offset, its OWN muzzle-flash fx_anim_spawn
                // (own_class again -- see the header banner: recomputed fresh, not shared with case 4's),
                // the scatter-adjusted impact fx_anim_spawn (target_class), dual SIM_ACTIVE-gated sound
                // (fire at the mount, target at the impact tile), apply_area_damage, then the same
                // ammo/reload bookkeeping as case 1/5/8.
                recompute_mount_or_soldier_pos(1);

                int32_t scatter_dx = 0, scatter_dy = 0;
                c.weapon_scatter_offset(u.experience, w.missing[player], static_cast<uint32_t>(mount_x),
                                        static_cast<uint32_t>(mount_y), static_cast<uint32_t>(target_fine_x),
                                        static_cast<uint32_t>(target_fine_y), &scatter_dx, &scatter_dy);

                const int32_t facing = c.dir_from_to(mount_x, mount_y, target_fine_x, target_fine_y);
                const int32_t stride = c.fx_anim_dir_frame_stride(w.fite_explo);
                c.fx_anim_spawn(static_cast<uint32_t>(mount_x), static_cast<uint32_t>(render_y),
                                static_cast<uint32_t>(w.fite_explo + stride * (facing - 1)),
                                *v.game_clock, static_cast<uint32_t>(own_class));

                // 0x0048c070-0x0048c0a3: pixel-space wrap masks (v.geom->bw_mask/bh_mask), NOT the
                // tile-space width_mask/height_mask pair -- see sim_bldg_get_coords.h's identical note.
                const uint32_t imp_x        = v.geom->bw_mask & static_cast<uint32_t>(target_fine_x + scatter_dx);
                const uint32_t imp_y_ground = v.geom->bh_mask & static_cast<uint32_t>(target_fine_y + scatter_dy);
                const uint32_t imp_y_vis =
                    v.geom->bh_mask & static_cast<uint32_t>(target_fine_y + scatter_dy - target_elevation);

                c.fx_anim_spawn(imp_x, imp_y_vis, static_cast<uint32_t>(w.target_explo), *v.game_clock,
                                static_cast<uint32_t>(target_class));

                if (*v.sim_active != 0) {
                    // The original's TWO offscreen_snd_volume+snd_play pairs (fire at the mount,
                    // target at the impact tile), each fused into ONE position-carrying record
                    // (the LIFT-NOTIFY offscreen conversion): the hosted sink re-runs each exact pair synchronously
                    // at emit -- still two volume calls at the binary level, original order.
                    c.snd_play_at(w.sound_fire, fine_to_tile(mount_x), fine_to_tile(render_y));
                    c.snd_play_at(w.sound_target, fine_to_tile(static_cast<int32_t>(imp_x)),
                                  fine_to_tile(static_cast<int32_t>(imp_y_vis)));
                }

                const uint32_t killer_info = static_cast<uint32_t>(player) | 0x80u;
                const int32_t  ring_count  = trunc_only(w.fire_range[player]);
                c.apply_area_damage(fine_to_tile(static_cast<int32_t>(imp_x)),
                                    fine_to_tile(static_cast<int32_t>(imp_y_ground)), target_class,
                                    w.power[player], ring_count,
                                    static_cast<uint32_t>(w.area_damage_owner_filter), killer_info,
                                    unit_index);

                consume_ammo_and_reload();
                break;
            }
            default:
                // type == 7 (a genuine original no-op weapon type) and anything outside [1,8]: nothing
                // fires; just advance to the next slot.
                break;
        }
    }

    // 0x0048c689-0x0048c6a9: no weapon slot matched (enabled_2 never set) -> deselect.
    if (matched_count == 0) {
        u.selected_weapon = 100;
    }
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

void unit_fire_weapon(uint32_t player, uint32_t unit_index, uint8_t weapon_slot_select,
                      uint32_t target_ref, uint16_t target_index, int32_t target_fine_x,
                      int32_t target_fine_y) {
    sim_state st = state();
    detail::unit_fire_weapon(st.read, st.own, live_unit_fire_weapon_calls(), player, unit_index,
                             weapon_slot_select, target_ref, target_index, target_fine_x, target_fine_y);
}


} // namespace mh::sim
