//
// tact/tact_unit_fire_weapon.h -- TACT1C: the turret-update entry point for a unit's weapon: turns
// to face the aim/target point if not already aligned, otherwise fires (consuming ammo/reloading/
// gun-switching on empty) and spawns the muzzle fx.
//
//   llm_tact_unit_fire_weapon @0x00430855 (0x422)
//   void __mh_watcall_ebx_volatile llm_tact_unit_fire_weapon(int building_id, int weapon_subindex,
//                                                              int fire_arg)
//   NOTE: `weapon_subindex` (EDX) is loaded into a local at @0x0043086e and never read again --
//   genuinely unused by this function, not merely by this translation.
//
// SHAPE, transcribed literally from @0x00430874-0x00430c6f:
//   1. @0x00430874-0x004308d5: cx/cy = the unit's own tile anchor screen position
//      ((pos_col<<5)+0x10, pos_row*24+0xc); target_dir = calc_dir24(cx, cy, aim_x, aim_y).
//   2. @0x004308eb-0x0043090f: if facing_dir != target_dir: if progress > 0, RETURN (already
//      turning/busy); else unit_rotate_step(building_id, target_dir) and RETURN -- fires nothing
//      this call.
//   3. @0x00430914-0x0043096c: (facing already aligned) gun_slot = fire_arg ^ active_gun;
//      weapon_type_id = (&character_types[type].number_gun1)[gun_slot] (adjacent-byte idiom, as
//      elsewhere in this batch). If (time_now() - weapon_timer) < fx_type[weapon_type_id].speed_fire
//      (cooldown not elapsed): attack_cmd_op = 0; RETURN.
//   4. @0x00430971-0x004309d6: weapon_timer = time_now() (a SECOND, independent clock read). If
//      (&gun1_bullets)[gun_slot] == 0 (out of ammo in the requested gun): if the OTHER gun
//      ((&gun1_bullets)[gun_slot^1]) also has none: attack_cmd_op = 0; RETURN. Otherwise: TOGGLE
//      active_gun and the local gun_slot, refresh the char panel row, and RETURN without firing.
//   5. @0x004309f0-0x00430a6a: (chosen gun has ammo) --(&gun1_bullets)[gun_slot]; if it just hit 0
//      AND (&gun1_magazines)[gun_slot] > 0: --magazines[gun_slot]; refill
//      bullets[gun_slot] = fx_type[weapon_type_id].bullets; and ADD fx_type[weapon_type_id].repeat
//      onto weapon_timer (reload delay stacked on top of the just-set timestamp).
//   6. @0x00430a6d-0x00430a99: redraw the sidebar slot; muzzle_x/muzzle_y =
//      unit_get_muzzle_offset(building_id, weapon_slot=fire_arg) -- OVERWRITES cx/cy from step 1.
//   7. @0x00430a86-0x00430b98: pick the scatter target point (tx,ty): the HOVERED unit's tile
//      screen position if a hovered unit exists, is NOT this unit, and has a DIFFERENT owner;
//      otherwise the unit's OWN current aim point (aim_x,aim_y). Then, for whichever pair is
//      chosen: weapon_calc_scatter(building_id, muzzle_x, muzzle_y, tx, ty, &out_dx, &out_dy,
//      fire_arg); altitude = kneeling ? (&kneel_gun1)[gun_slot] : (&height_gun1)[gun_slot]
//      (adjacent-byte idiom, gated on anim_state==2||3, same shape as tact_weapon_calc_scatter.h);
//      fx_spawn(weapon_type_id, character_types[type].who, muzzle_x, muzzle_y, tx+out_dx,
//      ty+out_dy, altitude); attack_cmd_op = 0. (This same sequence is duplicated verbatim in the
//      original for each of the two (tx,ty) sources -- @0x00430af3-0x00430b93 and
//      @0x00430bba-0x00430c54 -- unified here into one helper since the logic is identical.)
//   8. @0x00430c5d-0x00430c6f: move_state_timer = time_now() (a THIRD, independent clock read).
//
// PROOF: OFFLINE, NOT by choice of cost. the measured write closure of llm_tact_unit_fire_weapon
// -> 35 functions reachable, 45 regions written -- almost entirely UI/font presentation scratch
// reached through the char-panel/sidebar refresh calls (the SAME "structurally expensive to
// rig-arm for no verification gain" shape as llm_tact_unit_despawn's and llm_tact_unit_destroy's
// manifest entries; the migration driver's advisory RIG label is a call-graph heuristic, not this
// region measurement, so the two disagree here). Proof is an offline
// oracle over the turn-vs-fire gate, the cooldown gate, the ammo/reload/gun-switch state machine,
// and both scatter-target arms, with all seven outward calls mocked via the calls struct.
//
#pragma once
#include <cstdint>

#include "tact/tact_state.h"

namespace mh::tact {

struct unit_fire_weapon_calls {
    double (*time_now)();                                                           // time_GetCurrentTime @0x00427616
    int32_t (*calc_dir24)(int32_t x1, int32_t y1, int32_t x2, int32_t y2);          // llm_tact_calc_dir24 @0x0042e0e2
    void (*unit_rotate_step)(int32_t building_id, int32_t target_dir);              // llm_tact_unit_rotate_step @0x00430df1
    void (*ui_char_panel_row_refresh)(int32_t unit_id);                             // llm_tact_ui_char_panel_row_refresh @0x00434f28
    void (*ui_sidebar_redraw_unit_slot)(int32_t unit_id);                           // llm_tact_ui_sidebar_redraw_unit_slot @0x00435008
    void (*unit_get_muzzle_offset)(int32_t unit_id, int32_t *out_x, int32_t *out_y, // llm_tact_unit_get_muzzle_offset @0x00432105
                                   uint32_t weapon_slot);
    void (*weapon_calc_scatter)(int32_t building_id, int32_t x, int32_t y, int32_t tx, // llm_tact_weapon_calc_scatter @0x00430c77
                                int32_t ty, int32_t *out_dx, int32_t *out_dy,
                                int32_t weapon_slot);
    int32_t (*fx_spawn)(int32_t fx_type, uint8_t owner, int32_t x, int32_t y, int32_t x2, // llm_tact_fx_spawn @0x0042bdce
                        int32_t y2, uint8_t altitude);
};

const unit_fire_weapon_calls &live_unit_fire_weapon_calls();

namespace detail {

// llm_tact_unit_fire_weapon @0x00430855.
void unit_fire_weapon(const tact_view &v, tact_store &own, const unit_fire_weapon_calls &c,
                      int32_t building_id, int32_t fire_arg);

} // namespace detail

void unit_fire_weapon(int32_t building_id, int32_t weapon_subindex, int32_t fire_arg);


} // namespace mh::tact
