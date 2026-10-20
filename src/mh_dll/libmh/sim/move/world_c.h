//
// sim/move/world_c.h -- mods:M1H lane C: the libmh-side WORLD for the shared movement-prototype headers (nudge.h, near_enough.h,
// group_speed.h): sim_view reads + sim_store writes + the libmh call binders, so the twin runs the splice's code over the same
// memory (the sim regions ARE the game's regions) and calls the same game functions (through the libmh binders, i.e. our body when
// the rebind is armed, the original otherwise -- exactly what the neighbouring libmh code does).
//
// `Calls` is unit_state_move_walker_calls or unit_state_group_marshal_calls; both carry the members used here under the same names.
//
#pragma once

#include <cstdint>

#include "sim/move/twin_hit_c.h"
#include "sim/sim_state.h"

namespace mh::sim::mvc {

using unit_rec_t    = mh::sim::unit; // aliases: the world's member functions below are NAMED unit / soldier
using soldier_rec_t = mh::sim::soldier;
using tile_rec_t    = mh::sim::tile_object;
using cfg_rec_t     = mh::sim::cfg_unit;
using wp_rec_t      = mh::sim::path_waypoint;
using order_rec_t   = mh::sim::order;

template <class Calls>
struct libmh_world {
    const sim_view &v;
    sim_store      &own;
    const Calls    &c;

    // ---- records ----
    unit_rec_t *unit(uint32_t player, int32_t idx) {
        if (player >= 8 || idx < 0 || idx >= v.caps.units) return nullptr;
        return &own.unit_at(player, idx);
    }
    const cfg_rec_t *cfg(const unit_rec_t &u) { return &v.cfg_units[u.unit_proto_id]; }
    tile_rec_t      &tile(uint32_t x, uint32_t y) { return own.tile_object_at(static_cast<int32_t>(x), static_cast<int32_t>(y)); }
    uint8_t         &pass(uint32_t x, uint32_t y) { return own.passable_at(static_cast<int32_t>(x), static_cast<int32_t>(y)); }
    wp_rec_t        *path(uint32_t player, uint32_t slot) { return &own.path_buffer_at(player, static_cast<int32_t>(slot), 0); }
    void             dir_delta(int h, int *dx, int *dy) {
        *dx = static_cast<int8_t>(v.map_dir_step_deltas[h * 2]);
        *dy = static_cast<int8_t>(v.map_dir_step_deltas[h * 2 + 1]);
    }
    uint32_t       mask_x() { return v.geom->width_mask; }
    uint32_t       mask_y() { return v.geom->height_mask; }
    const void    *game_clock() { return v.game_clock; }
    soldier_rec_t *soldier(uint32_t player, uint32_t si) {
        if (si >= static_cast<uint32_t>(v.caps.soldiers)) return nullptr;
        return &own.soldier_at(player, static_cast<int32_t>(si));
    }
    uint16_t cur_index() { return *v.cur_index; }
    int32_t  map_w() { return *v.map_width; }
    int32_t  map_h() { return *v.map_height; }

    // ---- the game's own functions ----
    void    fow_remove_sight(uint32_t player, int32_t x, int32_t y, uint8_t sight) { c.fow_remove_sight(player, x, y, sight); }
    void    fow_update(uint32_t player, uint32_t x, uint32_t y, uint8_t sight) { c.map_fow_UpdateFoWPlus(player, x, y, sight); }
    void    soldiers_set_heading(uint16_t player, int32_t idx, uint8_t h) { c.unit_soldiers_set_heading(player, idx, h); }
    int32_t make_single_step(uint32_t player, int32_t idx) { return c.path_make_single_step(player, idx); }
    void    free_slot(uint16_t player, int32_t idx) { c.path_free_slot(player, idx); }
    void    set_order_param(int32_t player, int32_t idx, int16_t param) { c.unit_set_order_param(player, idx, param); }
    void    set_state_of(int32_t player, int32_t idx, int16_t state) { c.unit_set_state_of(player, idx, state); }
    void    notify_status(uint32_t player, int32_t idx, uint32_t code) { c.unit_notify_status(player, idx, code); }

    // ---- evidence ----
    void note(const char *what, uint32_t player, int32_t ai, int32_t bi, uint32_t sx, uint32_t sy, uint32_t dx, uint32_t dy, int n) {
        mh::ai::ai_say("; M1N %s p%u unit %d at %u,%u / unit %d at %u,%u (%d) (libmh twin)\n", what, player, static_cast<int>(ai), sx, sy,
                       static_cast<int>(bi), dx, dy, n);
    }
    void note_reverse(uint32_t player, int32_t idx, uint32_t fx, uint32_t fy, uint32_t tx, uint32_t ty, int ms) {
        if (twin_hit_counts()[TS_REVERSE] < 200) // mods:M1G 4c; the hit itself is counted by the caller, after this note
            mh::ai::ai_say("; M1G reverse p%u unit %d %u,%u -> %u,%u ms %d (libmh twin)\n", player, static_cast<int>(idx), fx, fy, tx, ty, ms);
    }
    void hit_nd() { twin_hit(TS_ND); }
    void hit_mk() { twin_hit(TS_MK); }
    void hit_mk2() { twin_hit(TS_MK2); }
    void hit_eq() { twin_hit(TS_EQ); }
};

// The group-speed world: ONE player's roster, the wave's scratch and the due-now order queue.
struct libmh_speed_world {
    const sim_view    &v;
    sim_store         &own;
    uint32_t           player;
    int32_t            units() { return v.caps.units; }
    unit_rec_t        &unit(int32_t idx) { return own.unit_at(player, idx); }
    const cfg_rec_t   &cfg(uint32_t proto) { return v.cfg_units[proto]; }
    int32_t            scratch_unit(int32_t w) { return own.group_move_scratch_at(w).unit_idx; }
    int32_t            queue_count() { return own.order_count(); }
    const order_rec_t &queue(int32_t k) { return own.order_at(k); }
    void               log_wave(int32_t legs, int32_t ground, double fastest, double slowest) {
        mh::ai::ai_say("; group speed (mods:M1X) player %u wave of %d (%d ground): step_speed fastest %d/1000 -> slowest %d/1000 (libmh twin)\n", player,
                                     legs, ground, static_cast<int>(fastest * 1000.0), static_cast<int>(slowest * 1000.0));
    }
};

} // namespace mh::sim::mvc
