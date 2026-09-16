//
// tact/tact_map_reset.cpp -- see tact_map_reset.h. Translated from the DISASSEMBLY, not from
// Ghidra's C.
//
#include "tact/tact_map_reset.h"

#include "addr/mh_calls.gen.h"  // frontier callee (Law 4): llm_tact_mission_load
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "tact/tact_mission_load.h"

namespace mh::tact {

const map_reset_calls &live_map_reset_calls() {
    static const map_reset_calls c = {
        MH_LIBMH_BIND(llm_tact_mission_load),
    };
    return c;
}

namespace detail {

void map_reset(tact_store &own, const map_reset_calls &c, char *mission_name) {
    // @0x0042e72e-0x0042e765: reserve owner-0 path slot 0, free the other 99.
    own.planes().path_slot_flag_at(0, 0) = 1;
    for (int32_t j = 1; j < mh::state::PATH_SLOTS_PER_OWNER; ++j) {
        own.planes().path_slot_flag_at(0, j) = 0;
    }

    // @0x0042e767-0x0042e86d: wipe the top-left 128x128 sub-block of the shared grid.
    for (int32_t col = 0; col < TACT_MAP_DIM; ++col) {
        for (int32_t row = 0; row < TACT_MAP_DIM; ++row) {
            mh::state::tile_object &t = own.planes().tile_object_at(col, row);
            t.flags[0]                = 2;
            t.flags[1]                = 0x40;
            t.building                = 0;
            t.unit[0]                 = 0;
            t.unit[1]                 = 0;
            t.class_owner             = 0;
            t.visibility              = 0;

            own.planes().passable_at(col, row) = mh::state::PASSABLE_DEFAULT;

            uint16_t *height_sprites = own.map_tile_height_sprites();
            for (int32_t k = 0; k < 8; ++k) {
                height_sprites[col * 1024 + row * 8 + k] = 0;
            }
        }
    }

    // @0x0042e877-0x0042e87a: hand off to the mission loader; return value discarded (the original
    // does not save EAX after this call either).
    c.mission_load(mission_name);
}

} // namespace detail

void map_reset(char *mission_name) {
    tact_state st = state();
    detail::map_reset(st.own, live_map_reset_calls(), mission_name);
}

} // namespace mh::tact


namespace mh::tact {


} // namespace mh::tact
