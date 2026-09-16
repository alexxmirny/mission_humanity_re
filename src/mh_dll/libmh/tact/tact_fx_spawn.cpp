//
// tact/tact_fx_spawn.cpp -- see tact_fx_spawn.h. Translated from the DISASSEMBLY, not from Ghidra's
// C.
//
#include "tact/tact_fx_spawn.h"

#include <cmath>
#include <cstdlib> // std::abs

#include "addr/mh_calls.gen.h" // frontier callees (Law 4)
#include "state/host_api.h"
#include "state/host_events.h"
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "tact/tact_calc_dir24.h"
#include "state/rebind_targets.gen.h"
#include "crt/crt_select.h" // LIB-CRT: MH_CRT() picks the vendored CRT in the standalone build

namespace mh::tact {

const fx_spawn_calls &live_fx_spawn_calls() {
    static const fx_spawn_calls c = {
        MH_LIBMH_BIND(time_GetCurrentTime),
        mh::state::evt::snd_fx_play,
        MH_LIBMH_BIND(llm_tact_calc_dir24),
        MH_CRT(llm_sqrt),
    };
    return c;
}

namespace detail {

int32_t fx_spawn(const tact_view &tv, tact_store &own, const fx_spawn_calls &c, int32_t fx_type,
                 uint8_t owner, int32_t x, int32_t y, int32_t x2, int32_t y2, uint8_t altitude) {
    // @0x0042bdfd-0x0042be34: first free slot in 1..0x3ff (slot 0 never allocated).
    int32_t slot = -1;
    for (int32_t i = 1; i < 0x400; ++i) {
        if (own.fx_at(i).fx_type == 0) {
            slot = i;
            break;
        }
    }
    if (slot == -1) return 1; // @0x0042be2d-0x0042be34

    fx_entry &fx = own.fx_at(slot);

    // @0x0042be39-0x0042bee3
    fx.fx_type    = static_cast<uint8_t>(fx_type);
    fx.owner      = owner;
    fx.pos_x      = (double)x;
    fx.pos_y      = (double)y;
    fx.travel_dx  = 0.0;
    fx.travel_dy  = 0.0;
    fx.tile_col   = static_cast<uint8_t>(x / 32); // @0x0042be9d-0x0042beae, signed div by 32 (matches C `/`)
    fx.tile_row   = static_cast<uint8_t>(y / 24); // @0x0042bebf-0x0042becd, real IDIV
    fx.altitude   = altitude;
    fx.move_clock = c.time_now(); // @0x0042bee3-0x0042beec

    // ---- @0x0042bef2-0x0042c0f5: view-relative sound gate --------------------------------------
    const int32_t half_w     = own.view_tiles_w() / 2;
    const int32_t half_h     = own.view_tiles_h() / 2;
    const int32_t center_col = own.map_cam_col() + half_w;
    const int32_t center_row = own.map_cam_row() + half_h;
    const int32_t tile_col_x = x / 32;
    const int32_t tile_row_y = y / 24;
    const int32_t dist_col   = std::abs(tile_col_x - center_col);
    const int32_t dist_row   = std::abs(tile_row_y - center_row);

    bool played = false;
    if (dist_col <= half_w && dist_row <= half_h) {
        // @0x0042bf8e-0x0042bfff: fully in view -- fixed volume 100, signed pan.
        const int32_t dcol = tile_col_x - center_col;
        int32_t       pan  = dcol * 128 / own.view_tiles_w() + 128;
        if (pan < 0) pan = 0;
        if (pan > 0xff) pan = 0xff;
        c.fx_play_sound(fx_type, 100, pan);
        played = true;
    } else if (dist_col <= own.view_tiles_w() && dist_row <= own.view_tiles_h()) {
        // @0x0042c024-0x0042c0f5: distant-but-audible falloff.
        int32_t vol;
        if (dist_col > dist_row) {
            vol = 100 / (dist_col - half_w); // @0x0042c02c-0x0042c052
        } else if (dist_col < dist_row) {
            vol = 100 / (dist_row - half_h); // @0x0042c05d-0x0042c083
        } else {
            // @0x0042c055-0x0042c05b, dist_col == dist_row: the ORIGINAL reads an uninitialised
            // stack slot here -- neither the col- nor row-vol assignment executes on this exact
            // tie (traced both CMP/JGE pairs). C++ has no defined stand-in for that read; 0 is a
            // documented substitute, not a derived value.
            vol = 0;
        }
        const int32_t dcol = tile_col_x - center_col;
        int32_t       pan  = dcol * 128 / own.view_tiles_w() + 128;
        if (pan < 0) pan = 0;
        if (pan > 0xff) pan = 0xff;
        c.fx_play_sound(fx_type, vol, pan);
        played = true;
    }
    (void)played; // @0x0042c0fa is reached whether or not the sound played

    // ---- @0x0042c0fa-0x0042c136: dir24 + target stamp -------------------------------------------
    if (x == x2 && y == y2) {
        fx.dir24 = 1;
    } else {
        fx.dir24 = static_cast<uint8_t>(c.calc_dir24(x, y, x2, y2));
    }
    fx.target_x = (double)x2;
    fx.target_y = (double)y2;

    // ---- @0x0042c136-0x0042c1fa: velocity ---------------------------------------------------------
    // @0x0042c150-0x0042c172: INT arithmetic, exactly as the original's 32-bit IMUL/ADD (no
    // widening) -- a large spawn/target span can overflow this the same way the original does.
    const int32_t dxi     = x2 - x;
    const int32_t dyi     = y2 - y;
    const int32_t dist_sq = dxi * dxi + dyi * dyi;
    if ((double)dist_sq > 0.0) {
        const double dist = c.sqrt_fn((double)dist_sq);
        fx.vel_x          = (double)dxi / dist;
        fx.vel_y          = (double)dyi / dist;
    } else {
        fx.vel_x = 0.0;
        fx.vel_y = 0.0;
    }

    // ---- @0x0042c1fa-0x0042c25b -------------------------------------------------------------------
    fx.frame_counter = 0;
    // Qualified type name: `fx_type` (the alias) is shadowed by this function's `fx_type` parameter.
    const mh::tact::fx_type &kind = tv.fx_type_table[fx_type];
    if (kind.direct != 0) {
        fx.sprite_frame = static_cast<uint16_t>((fx.dir24 - 1) * kind.frames + fx.frame_counter);
    } else {
        fx.sprite_frame = static_cast<uint16_t>(fx.frame_counter);
    }

    ++own.fx_live_count(); // @0x0042c25b
    return 0;
}

} // namespace detail

int32_t fx_spawn(int32_t fx_type, uint8_t owner, int32_t x, int32_t y, int32_t x2, int32_t y2,
                 uint8_t altitude) {
    tact_state st = state();
    return detail::fx_spawn(st.read, st.own, live_fx_spawn_calls(), fx_type, owner, x, y, x2, y2,
                            altitude);
}

} // namespace mh::tact


namespace mh::tact {


} // namespace mh::tact
