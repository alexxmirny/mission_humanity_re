#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// ---- the outward-call tables, one per shadowable function that calls out ----------------------

struct map_fow_update_fow_plus_calls {
    void (*update_fow_plus_impl)(); // map_fow_UpdateFoWPlus_impl @0x004a6817
};
const map_fow_update_fow_plus_calls &live_map_fow_update_fow_plus_calls();

struct fow_remove_sight_calls {
    void (*remove_sight_apply)(); // llm_strat_fow_remove_sight_apply @0x004a6946
};
const fow_remove_sight_calls &live_fow_remove_sight_calls();

struct sight_add_circle_calls {
    void (*update_fow_plus)(uint32_t player, uint32_t x, uint32_t y,
                            uint8_t sight); // map_fow_UpdateFoWPlus @0x0049681a
};
const sight_add_circle_calls &live_sight_add_circle_calls();

struct sight_remove_circle_calls {
    void (*remove_sight)(uint32_t player, int32_t x, int32_t y,
                         uint8_t radius); // llm_strat_fow_remove_sight @0x00496868
};
const sight_remove_circle_calls &live_sight_remove_circle_calls();

namespace detail {

// map_fow_ConvertSightToArea @0x004a6792. Internal-only -- see the [EBP-RETURN] banner above. Reads
// G_TMP_SIGHT (own, since this same family writes it) and the ten table pointers (v).
const map_t_tile_coord *convert_sight_to_area(const sim_view &v, sim_store &own);

// map_fow_UpdateFoWPlus @0x0049681a. Stages (player, x, y, sight) into the G_TMP_* globals, then
// calls out to the ORIGINAL _impl (see the CALLEES banner above).
void update_fow_plus(sim_store &own, const map_fow_update_fow_plus_calls &c, uint32_t player,
                     uint32_t x, uint32_t y, uint8_t sight);

// map_fow_UpdateFoWPlus_impl @0x004a6817. The real "add sight" walk over the RLE table
// convert_sight_to_area() returns -- see the .cpp for the full derivation of the packed torus-wrap
// accumulation and the is_human-gated tile_objects write.
void update_fow_plus_impl(const sim_view &v, sim_store &own);

// llm_strat_fow_remove_sight @0x00496868. Stages (player, x, y, radius), calls out to the ORIGINAL
// _apply.
void remove_sight(sim_store &own, const fow_remove_sight_calls &c, uint32_t player, int32_t x,
                  int32_t y, uint8_t radius);

// llm_strat_fow_remove_sight_apply @0x004a6946. The real "remove sight" walk: decrements
// visible_by_count, clears the player's visibility bit on reaching 0, and (human players only) marks
// the tile explored. See the .cpp for the full derivation.
void remove_sight_apply(const sim_view &v, sim_store &own);

// llm_strat_sight_add_circle @0x004968b6. Walks Building[building_id].area[10][10]; for every nonzero
// cell, calls out to the ORIGINAL map_fow_UpdateFoWPlus at the wrapped (origin+dx, origin+dy) tile.
// Returns void -- see the .cpp's uncertainty note on the committed uint32_t return type.
void sight_add_circle(const sim_view &v, const sight_add_circle_calls &c, uint32_t player,
                      int32_t origin_x, int32_t origin_y, int32_t building_id, uint8_t sight);

// llm_strat_sight_remove_circle @0x0049694f. Same footprint walk as sight_add_circle, calling
// llm_strat_fow_remove_sight per nonzero cell instead. Returns void -- same uncertainty as above.
void sight_remove_circle(const sim_view &v, const sight_remove_circle_calls &c, uint32_t player,
                         int32_t origin_x, int32_t origin_y, int32_t building_id, uint8_t radius);

} // namespace detail

// ---- the public wrappers, one per shadowable original --------------------------------------------
void update_fow_plus(uint32_t player, uint32_t x, uint32_t y, uint8_t sight);
void update_fow_plus_impl();
void remove_sight(uint32_t player, int32_t x, int32_t y, uint8_t radius);
void remove_sight_apply();
void sight_add_circle(uint32_t player, int32_t origin_x, int32_t origin_y, int32_t building_id,
                      uint8_t sight);

// DECLARED HERE so another TU's rebind can name them. The committed rows return uint32_t / int32_t
// where the public wrappers return void -- see finding (F) above: the original's return value is
// dead register-reuse noise and 9 is the always-observed value. The binder is pinned to the
// committed shape, so a MH_LIBMH_BIND site binds these. They lived beside the differential
// oracle until F2D retired it, and were never part of it.
namespace rebind_arm {
uint32_t sight_add_circle(uint32_t player, int32_t origin_x, int32_t origin_y, int32_t building_id,
                          uint8_t sight);
int32_t  sight_remove_circle(int32_t player, int32_t origin_x, int32_t origin_y,
                             int32_t building_id, uint8_t radius);
} // namespace rebind_arm
void sight_remove_circle(uint32_t player, int32_t origin_x, int32_t origin_y, int32_t building_id,
                         uint8_t radius);

namespace detail {
} // namespace detail

} // namespace mh::sim
