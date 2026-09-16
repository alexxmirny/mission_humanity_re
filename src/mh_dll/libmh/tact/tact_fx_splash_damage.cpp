//
// tact/tact_fx_splash_damage.cpp -- see tact_fx_splash_damage.h. Translated from the DISASSEMBLY,
// not from Ghidra's C.
//
#include "tact/tact_fx_splash_damage.h"

#include "addr/mh_calls.gen.h" // frontier/sibling callees (Law 4)
#include "state/host_api.h"
#include "state/host_events.h"
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "tact/tact_facing_to_delta.h"
#include "tact/tact_unit_set_anim_state.h"
#include "state/rebind_targets.gen.h"

namespace mh::tact {

const fx_splash_damage_calls &live_fx_splash_damage_calls() {
    static const fx_splash_damage_calls c = {
        // KEPT ORIGINAL: was pinned here because the committed shape disagreed with our
        // `int32_t *` out-pointers (sig_<fn> said `void *`, a generator artifact). TACT1-P C6
        // (2026-09-04) fixed the generator so the boundary now carries the committed pointee and
        // the shapes agree -- see libmh_rebind_targets.json. The binding itself is left mh::call::
        // here (a rebind-arming decision, not a type fix).
        // Bound since TACT1-P C6 (2026-09-04): the row was pinned to the original while sig_<fn>
        // spelled its out-params `void *` -- the generator blunting the committed `int *`, not the
        // ABI. See libmh_rebind_targets.json.
        MH_LIBMH_BIND(llm_tact_facing_to_delta),
        MH_LIBMH_BIND(llm_tact_unit_set_anim_state),
        mh::state::evt::inv_tact_unit_slot,
    };
    return c;
}

namespace detail {

void fx_splash_damage(const tact_view &v, tact_store &own, const fx_splash_damage_calls &c,
                      int32_t col, int32_t row, int32_t radius_tiles, int32_t damage) {
    const int32_t width  = *v.grid_width;
    const int32_t height = *v.grid_height;

    // @0x004314cb-0x00431653: outer row loop, inner col loop, both inclusive both ends.
    for (int32_t rr = row - radius_tiles; rr <= row + radius_tiles; ++rr) {
        for (int32_t cc = col - radius_tiles; cc <= col + radius_tiles; ++cc) {
            // @0x0043150d-0x00431530: toroidal wrap, then the roster index off `.building`.
            const int32_t col_w  = (width - 1) & cc;
            const int32_t row_w  = (height - 1) & rr;
            const int32_t target = own.planes().tile_object_at(col_w, row_w).building;
            if (target == 0) continue; // @0x00431533-0x00431537

            tact_unit &tu = own.unit_at(target);

            // @0x0043153d-0x0043155d
            if (tu.anim_state == 2 || tu.anim_state == 3) continue;

            if (tu.hp > damage) {
                // @0x00431611-0x0043163b: NOT lethal. `spare_nonzero_owner` has no writer anywhere
                // in the binary (see the .h banner) -- this owner-gated skip is dead in practice,
                // preserved as a real branch.
                if (*v.fx_splash_spare_nonzero_owner == 0 || tu.owner == 0) {
                    tu.hp = static_cast<uint16_t>(tu.hp - damage); // @0x0043162a
                }
                c.unit_refresh_ui_slot(target); // @0x0043163b
            } else {
                // @0x00431579-0x004315d5: LETHAL. The kill stamp runs only when BOTH conditions
                // hold (two separate original tests, ANDed).
                if (tu.anim_state <= 1 && tu.progress > 0) {
                    int32_t dx = 0, dy = 0;
                    c.facing_to_delta(tu.facing_dir, &dx, &dy);
                    own.planes().passable_at(col + dx, row + dy) = mh::state::PASSABLE_DEFAULT;
                    own.planes().passable_at(col, row)           = mh::state::PASSABLE_BLOCKED;
                }
                // @0x004315dc-0x004315f3
                tu.hp       = 0;
                tu.progress = 0;
                c.unit_set_anim_state(target, 0x1f); // @0x004315fa
                c.unit_refresh_ui_slot(target);      // @0x00431607
            }
        }
    }
}

} // namespace detail

void fx_splash_damage(int32_t col, int32_t row, int32_t radius_tiles, int32_t damage) {
    tact_state st = state();
    detail::fx_splash_damage(st.read, st.own, live_fx_splash_damage_calls(), col, row, radius_tiles,
                             damage);
}

} // namespace mh::tact


namespace mh::tact {


} // namespace mh::tact
