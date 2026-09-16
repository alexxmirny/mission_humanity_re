//
// tact/tact_unit_fire_weapon.cpp -- see tact_unit_fire_weapon.h. Translated from the DISASSEMBLY,
// not from Ghidra's C.
//
#include "tact/tact_unit_fire_weapon.h"

#include "addr/mh_calls.gen.h" // frontier/sibling callees (Law 4)
#include "state/host_api.h"
#include "state/host_events.h"
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "tact/tact_calc_dir24.h"
#include "tact/tact_unit_rotate_step.h"
#include "state/rebind_targets.gen.h"

namespace mh::tact {

const unit_fire_weapon_calls &live_unit_fire_weapon_calls() {
    static const unit_fire_weapon_calls c = {
        MH_LIBMH_BIND(time_GetCurrentTime),
        MH_LIBMH_BIND(llm_tact_calc_dir24),
        MH_LIBMH_BIND(llm_tact_unit_rotate_step),
        mh::state::evt::inv_tact_char_panel_row, // LIFT-TACT slice 1: the sole caller, so the fine
                                                 // entry leaves the libmh-facing table with it
        mh::state::evt::inv_tact_sidebar_slot,
        // Both were KEPT ORIGINAL until TACT1-P C6 (2026-09-04): the committed shapes appeared
        // to disagree with our `int32_t *` out-pointers because the generator degraded every
        // pointee to `void *`. With that fixed the rows bind like their siblings.
        MH_LIBMH_BIND(llm_tact_unit_get_muzzle_offset),
        MH_LIBMH_BIND(llm_tact_weapon_calc_scatter),
        MH_LIBMH_BIND(llm_tact_fx_spawn),
    };
    return c;
}

namespace detail {

void unit_fire_weapon(const tact_view &v, tact_store &own, const unit_fire_weapon_calls &c,
                      int32_t building_id, int32_t fire_arg) {
    tact_unit &u = own.unit_at(building_id);

    // @0x00430874-0x004308d5: own tile-anchor screen position, and the direction from it to the
    // current aim point.
    const int32_t cx         = ((int32_t)u.pos_col << 5) + 0x10;
    const int32_t cy         = (int32_t)u.pos_row * 0x18 + 0xc;
    const int32_t target_dir = c.calc_dir24(cx, cy, u.aim_x, u.aim_y);

    // @0x004308eb-0x0043090f: turn toward the target before firing.
    if (u.facing_dir != target_dir) {
        if (u.progress > 0) return;                  // @0x004308f0-0x004308fe: already turning/busy
        c.unit_rotate_step(building_id, target_dir); // @0x00430904
        return;
    }

    // @0x00430914-0x0043093b: adjacent-byte idiom, same shape as tact_weapon_calc_scatter.h.
    const int32_t            gun_slot       = fire_arg ^ u.active_gun;
    const character_type    &ct             = v.character_types[u.type];
    const int32_t            weapon_type_id = (&ct.number_gun1)[gun_slot];
    const mh::tact::fx_type &wt             = v.fx_type_table[weapon_type_id];

    // @0x0043093b-0x0043096c: cooldown gate (first, read-only clock read).
    if (c.time_now() - u.weapon_timer < wt.speed_fire) {
        u.attack_cmd_op = 0;
        return;
    }

    // @0x00430971-0x0043097d: SECOND, independent clock read -- stamps weapon_timer now.
    u.weapon_timer = c.time_now();

    // @0x00430983-0x004309d6: ammo / gun-switch state machine.
    uint8_t &bullets_this = (&u.gun1_bullets)[gun_slot];
    if (bullets_this == 0) {
        uint8_t &bullets_other = (&u.gun1_bullets)[gun_slot ^ 1];
        if (bullets_other == 0) {
            u.attack_cmd_op = 0; // @0x004309db
            return;
        }
        // @0x004309ad-0x004309d1: switch to the other gun; does NOT fire this call.
        u.active_gun ^= 1;
        c.ui_char_panel_row_refresh(building_id);
        return;
    }

    // @0x004309f0-0x00430a6a: consume a round; reload from the magazine if this emptied it.
    --bullets_this;
    if (bullets_this == 0 && (&u.gun1_magazines)[gun_slot] > 0) {
        --(&u.gun1_magazines)[gun_slot];
        bullets_this = wt.bullets;
        u.weapon_timer += wt.repeat;
    }

    // @0x00430a6d-0x00430a99: muzzle screen position (OVERWRITES cx/cy's role), then pick the
    // scatter target: the hovered unit's tile if it is a real, different, enemy-owned unit;
    // otherwise this unit's own current aim point.
    c.ui_sidebar_redraw_unit_slot(building_id);
    int32_t muzzle_x = 0, muzzle_y = 0;
    c.unit_get_muzzle_offset(building_id, &muzzle_x, &muzzle_y, (uint32_t)fire_arg);

    const int32_t hovered = *v.hovered_unit_id;
    int32_t       tx, ty;
    const bool    at_hovered_enemy =
        hovered > 0 && hovered != building_id && u.owner != own.unit_at(hovered).owner;
    if (at_hovered_enemy) {
        // @0x00430abf-0x00430af0: the hovered enemy's tile screen position.
        const tact_unit &hu = own.unit_at(hovered);
        tx                  = ((int32_t)hu.pos_col << 5) + 0x10;
        ty                  = (int32_t)hu.pos_row * 0x18 + 0xc;
    } else {
        // @0x00430b98-0x00430bb7: this unit's own aim point.
        tx = u.aim_x;
        ty = u.aim_y;
    }

    // @0x00430af3-0x00430b93 / @0x00430bba-0x00430c54: the two branches are ALMOST the same call,
    // and this file said "identical in both branches -- unified here" until 2026-09-04 (TACT1-P C5).
    // THEY DIFFER BY ONE INSTRUCTION, and it is a coordinate: the own-aim branch adds the ALTITUDE
    // into the y argument as well --
    //   hovered:  EAX = [EBP-0x1c] + [EBP-0x34]                  (ty + out_dy)      @0x00430b5c
    //   own aim:  EAX = [EBP-0x1c] + [EBP-0x34] + [EBP-0x2c]     (ty + out_dy + alt) @0x00430c23-0x00430c29
    // -- so a unit firing at its own aim point spawns its projectile one altitude higher than the
    // unified form did. Found by the POZ3 (combat) arm of C5's baseline-vs-promoted comparison,
    // which diverged on `tact_fx_pool` at frame 2882; the quiet mission never fires and never saw it.
    int32_t out_dx = 0, out_dy = 0;
    c.weapon_calc_scatter(building_id, muzzle_x, muzzle_y, tx, ty, &out_dx, &out_dy, fire_arg);
    const int32_t altitude = (u.anim_state == 2 || u.anim_state == 3) ? (&ct.kneel_gun1)[gun_slot]
                                                                      : (&ct.height_gun1)[gun_slot];
    c.fx_spawn(weapon_type_id, ct.who, muzzle_x, muzzle_y, tx + out_dx,
               ty + out_dy + (at_hovered_enemy ? 0 : altitude), static_cast<uint8_t>(altitude));
    u.attack_cmd_op = 0;

    // @0x00430c5d-0x00430c6f: THIRD, independent clock read.
    u.move_state_timer = c.time_now();
}

} // namespace detail

void unit_fire_weapon(int32_t building_id, int32_t weapon_subindex, int32_t fire_arg) {
    (void)weapon_subindex; // @0x0043086e: loaded, never read again -- unused by the original itself
    tact_state st = state();
    detail::unit_fire_weapon(st.read, st.own, live_unit_fire_weapon_calls(), building_id, fire_arg);
}

} // namespace mh::tact


namespace mh::tact {


} // namespace mh::tact
