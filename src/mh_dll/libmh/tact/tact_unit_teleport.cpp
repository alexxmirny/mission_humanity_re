//
// tact/tact_unit_teleport.cpp -- see tact_unit_teleport.h. Translated from the DISASSEMBLY, not
// from Ghidra's C.
//
#include "tact/tact_unit_teleport.h"

#include "addr/mh_calls.gen.h"         // frontier callees (Law 4): unit_destroy/fx_spawn/vision/camera/rand/cmd_advance
#include "tact/tact_facing_to_delta.h" // the already-translated facing_to_delta (not a frontier callee)
#include "addr/mh_rebind.gen.h"        // LIB-REBIND: the config-selected binder
#include "tact/tact_unit_cmd_advance.h"
#include "tact/tact_unit_vision.h"
#include "state/rebind_targets.gen.h"
#include "crt/crt_select.h" // LIB-CRT: MH_CRT() picks the vendored CRT in the standalone build

namespace mh::tact {

const unit_teleport_calls &live_unit_teleport_calls() {
    static const unit_teleport_calls c = {
        MH_LIBMH_BIND(llm_tact_unit_destroy),
        MH_LIBMH_BIND(llm_tact_fx_spawn),
        MH_LIBMH_BIND(llm_tact_unit_vision_add),
        MH_LIBMH_BIND(llm_tact_unit_vision_remove),
        MH_LIBMH_BIND(llm_tact_camera_center_on_tile),
        MH_CRT(llm_rand),
        MH_LIBMH_BIND(llm_tact_unit_cmd_advance),
        mh::tact::facing_to_delta,
    };
    return c;
}

namespace detail {

void unit_teleport(tact_store &own, mh::state::mode_planes &planes, const unit_teleport_calls &c,
                   int32_t teleport_id, int32_t unit_id) {
    teleport_zone &tz = own.teleport_zone_at(teleport_id);
    tact_unit     &u  = own.unit_at(unit_id);

    // @0x00432e3a-0x00432e41: refuse entirely once this zone is field_28-flagged.
    if (tz.field_28 != 0) return;

    // @0x00432e4b-0x00432e66: refuse when this zone is NO_ENEMY-flagged and the caller IS owner 1
    // (this function's "the enemy" side -- see the header comment; this is the inverse of the
    // first-glance reading of the field name).
    if (tz.no_enemy == 1 && u.owner == 1) return;

    const int32_t cur_col = u.pos_col;
    const int32_t cur_row = u.pos_row;

    // @0x00432e6b-0x00432ec3: if the unit was mid-move, cancel the pending step: free the tile it
    // was stepping toward and re-occupy the tile it is still standing on.
    if (u.progress != 0) {
        int32_t dx = 0, dy = 0;
        c.facing_to_delta(u.facing_dir, &dx, &dy);
        planes.passable_at(cur_col + dx, cur_row + dy) = mh::state::PASSABLE_DEFAULT;
        planes.passable_at(cur_col, cur_row)           = mh::state::PASSABLE_BLOCKED;
        u.progress                                     = 0;
    }

    int32_t slot = 0;

    // @0x00432eca-0x00432ed5: association[0] != 0 selects the LINKED-ZONE path.
    if (tz.association[0] != 0) {
        // ---- LINKED: destination is another zone, looked up via association[] ------------------
        if (tz.mode == 2) {
            // @0x004331de-0x00433223: SEQUENCE -- advance the chain cursor. NOT bounded to 8 here;
            // see the header comment on out-of-declared-bounds `slot`.
            slot = tz.field_03 + 1;
            if (association_at(tz, slot) == 0) slot = 0;
            tz.field_03 = static_cast<uint8_t>(slot);
        } else if (tz.mode == 1) {
            // @0x00433223-0x0043328c: RANDOM -- burn a llm_rand()-derived budget scanning forward
            // through non-empty association slots, wrapping at 8.
            int32_t remaining = c.rand() / 4096;
            slot              = 0;
            while (remaining > 0) {
                if (association_at(tz, slot) != 0) {
                    --remaining;
                    ++slot;
                    if (slot > 7) slot = 0;
                } else {
                    slot = 0;
                }
            }
            if (association_at(tz, slot) == 0) slot = 0;
        }

        // @0x004332a3-0x00433418: find the zone whose `.id` matches association[slot].
        const uint8_t target_id = association_at(tz, slot);
        int32_t       target    = -1;
        for (int32_t scan2 = 1; scan2 < 0x40; ++scan2) {
            if (own.teleport_zone_at(scan2).id == target_id) {
                target = scan2;
                break;
            }
        }

        if (target < 0) {
            // @0x0043341d: no matching zone -- destroy the unit and spawn its death fx.
            const int32_t px = cur_col * 32 + 16;
            const int32_t py = cur_row * 24 + 12;
            c.unit_destroy(static_cast<uint32_t>(unit_id));
            c.fx_spawn(tz.death, 0, px, py, px, py, 0x28);
            return;
        }

        teleport_zone &dest = own.teleport_zone_at(target);
        // @0x004332de-0x004332e5: a destination zone already field_28-flagged refuses silently.
        if (dest.field_28 != 0) return;

        c.unit_vision_remove(unit_id);
        planes.tile_object_at(cur_col, cur_row).building = 0;
        planes.passable_at(cur_col, cur_row)             = mh::state::PASSABLE_DEFAULT;

        const int32_t new_col                            = dest.start_col;
        const int32_t new_row                            = dest.start_row;
        planes.passable_at(new_col, new_row)             = mh::state::PASSABLE_BLOCKED;
        planes.tile_object_at(new_col, new_row).building = static_cast<uint16_t>(unit_id);
        u.pos_col                                        = static_cast<uint8_t>(new_col);
        u.pos_row                                        = static_cast<uint8_t>(new_row);
        c.unit_vision_add(unit_id);
        c.unit_cmd_advance(unit_id, u.cmd_index);
        if (u.owner == 0) {
            c.camera_center_on_tile(new_col, new_row);
        }
        dest.field_28 = 1;
        return;
    }

    // ---- DIRECT: destination is this zone's own dest_col[]/dest_row[] table -------------------
    if (tz.mode == 2) {
        // @0x00432edb-0x00432f3c: SEQUENCE -- advance the chain cursor. NOT bounded to 8 here.
        slot = tz.field_03 + 1;
        if (dest_col_at(tz, slot) == 0 && dest_row_at(tz, slot) == 0) slot = 0;
        tz.field_03 = static_cast<uint8_t>(slot);
    } else if (tz.mode == 1) {
        // @0x00432f3c-0x00432ff4: RANDOM -- same shape as the linked-zone random scan, over
        // (dest_col,dest_row) pairs instead of a single association byte.
        int32_t remaining = c.rand() / 4096;
        slot              = 0;
        while (remaining > 0) {
            if (dest_col_at(tz, slot) != 0 && dest_row_at(tz, slot) != 0) {
                --remaining;
                ++slot;
                if (slot > 7) slot = 0;
            } else {
                slot = 0;
            }
        }
        if (dest_col_at(tz, slot) == 0 && dest_row_at(tz, slot) == 0) slot = 0;
    }

    int32_t dest_col = dest_col_at(tz, slot);
    int32_t dest_row = dest_row_at(tz, slot);

    // @0x0043301e-0x00433032: (0xff,0xff) means "no real destination" -- destroy + death fx.
    if (dest_col == 0xff && dest_row == 0xff) {
        const int32_t px = cur_col * 32 + 16;
        const int32_t py = cur_row * 24 + 12;
        c.unit_destroy(static_cast<uint32_t>(unit_id));
        c.fx_spawn(tz.death, 0, px, py, px, py, 0x28);
        return;
    }

    // @0x00433075-0x004330f7: probe the 2x2 block at (dest_col,dest_row) for a `.building`-free
    // corner, preferring (dest_col,dest_row) itself, then +col, then +row, then +col+row.
    if (planes.tile_object_at(dest_col, dest_row).building != 0) {
        if (planes.tile_object_at(dest_col + 1, dest_row).building != 0) {
            if (planes.tile_object_at(dest_col, dest_row + 1).building != 0) {
                if (planes.tile_object_at(dest_col + 1, dest_row + 1).building != 0) {
                    // @0x004330d5: all four corners occupied -- give up entirely, no move, no destroy.
                    return;
                }
                ++dest_col;
                ++dest_row;
            } else {
                ++dest_row;
            }
        } else {
            ++dest_col;
        }
    }

    // @0x004330f7-0x004331d9: perform the move.
    c.unit_vision_remove(unit_id);
    planes.tile_object_at(cur_col, cur_row).building   = 0;
    planes.passable_at(cur_col, cur_row)               = mh::state::PASSABLE_DEFAULT;
    planes.passable_at(dest_col, dest_row)             = mh::state::PASSABLE_BLOCKED;
    planes.tile_object_at(dest_col, dest_row).building = static_cast<uint16_t>(unit_id);
    u.pos_col                                          = static_cast<uint8_t>(dest_col);
    u.pos_row                                          = static_cast<uint8_t>(dest_row);
    c.unit_vision_add(unit_id);
    c.unit_cmd_advance(unit_id, u.cmd_index);
    if (u.owner == 0) {
        c.camera_center_on_tile(dest_col, dest_row);
    }
}

} // namespace detail

void unit_teleport(int32_t teleport_id, int32_t unit_id) {
    tact_state st = state();
    detail::unit_teleport(st.own, st.own.planes(), live_unit_teleport_calls(), teleport_id, unit_id);
}

} // namespace mh::tact


namespace mh::tact {


} // namespace mh::tact
