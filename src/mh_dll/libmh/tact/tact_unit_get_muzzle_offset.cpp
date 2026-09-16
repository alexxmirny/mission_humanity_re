#include "tact/tact_unit_get_muzzle_offset.h"

namespace mh::tact {

namespace detail {

void unit_get_muzzle_offset(const tact_view &v, int32_t unit_id, int32_t *out_x, int32_t *out_y,
                            uint32_t weapon_slot) {
    const tact_unit &u = v.units[unit_id];

    // @0x0043212d-0x00432134
    const int32_t      sprite_id = u.sprite_id;
    const sprite_meta &sm        = v.sprite_meta_table[sprite_id];

    // @0x00432137-0x0043214a: 0 => the requested slot IS the active gun -> mount1; nonzero -> mount2.
    if ((u.active_gun ^ weapon_slot) == 0) {
        // @0x00432150-0x004321d3
        *out_x = ((int32_t)u.pos_col << 5) + 0x10 - sm.origin_x + sm.mount1_x;
        *out_y = (int32_t)u.pos_row * 0x18 + 0xc - sm.origin_y + sm.mount1_y +
                 v.character_types[u.type].height_gun1;
    } else {
        // @0x004321da-0x0043225d
        *out_x = ((int32_t)u.pos_col << 5) + 0x10 - sm.origin_x + sm.mount2_x;
        *out_y = (int32_t)u.pos_row * 0x18 + 0xc - sm.origin_y + sm.mount2_y +
                 v.character_types[u.type].height_gun2;
    }
}

} // namespace detail

void unit_get_muzzle_offset(int32_t unit_id, int32_t *out_x, int32_t *out_y, uint32_t weapon_slot) {
    tact_state st = state();
    detail::unit_get_muzzle_offset(st.read, unit_id, out_x, out_y, weapon_slot);
}

} // namespace mh::tact
