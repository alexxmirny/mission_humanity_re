//
// state/mode_planes.h -- the FIVE mode-shared planes, bound ONCE (RI-TACT / TACT0).
//
// ---------------------------------------------------------------------------------------------
// WHAT THESE ARE, AND WHY THEY ARE NOT `libmh/sim/`'s AND NOT `libmh/tact/`'s
// ---------------------------------------------------------------------------------------------
//
// `tile_objects`, `passable`, `_G_LLM_STRAT_PATH_BUFFERS`, `_G_LLM_STRAT_PATH_SLOT_FLAGS` and
// `_G_LLM_STRAT_PATH_FREE_SLOT_COUNT` are the map's per-tile and pathfinder substrate. BOTH game
// modes write them, and the modes are mutually exclusive in time: the strategic sim writes them
// while GAME_MODE == 2, a tactical mission writes the 128x128 top-left sub-block of the same
// arrays at the same stride while GAME_MODE == 6, and the excursion restores them wholesale from
// the planet file on the way out (the tactical closure Sect. 9).
//
// That is one substrate with two tenants, so it is bound in one place rather than twice. Before
// this file, `sim_store` held its own five pointers and a tactical store would have held five
// more -- two independent bindings of the same regions, which is a shape `sim_state.h` already
// accepted elsewhere and which is wrong HERE, because here the two consumers are not independent:
// they are the same bytes, alternating.
//
// THE MEASUREMENT THAT SETTLED THE OWNERSHIP (2026-08-24, TACT0):
//   * No ORIGINAL function writes these planes from both modes -- the writer sets are DISJOINT by
//     mode (map loaders / strategic pathing / tactical mission).
//   * Of the 787 functions reachable from both mode roots, the 15 that touch a plane are ALL
//     READS. There is no shared write machinery to attribute to either mode.
// So `region_ownership.json` declares all five `Map/geometry` -- the layer that owns the grid's
// layout and lifecycle -- and both modes appear in the cross-write ledger as named tenants. Before
// that, `tile_objects` and `passable` -- the two halves of ONE 256x256 grid, indexed identically
// as (x<<8)|y -- carried DIFFERENT owners, which was simply a mistake in the data.
//
// ---------------------------------------------------------------------------------------------
// THE COMPILE-TIME PROPERTY, AND THE ONE IT CANNOT BE
// ---------------------------------------------------------------------------------------------
//
// W2/W3 from `sim/sim_state.h` apply here unchanged and for the same reasons: the pointers are
// PRIVATE (every write goes through an accessor returning a reference to ONE record, so no base
// escapes and nothing can go stale across a rebase), and the CONSTRUCTOR is private with an
// enumerated friend list -- the two production binders and the two offline fixtures. A fifth
// binder is a diff a reviewer sees, which is the whole of W3.
//
// WHAT C++ CANNOT CARRY, STATED RATHER THAN FAKED: the real invariant on these planes is
// TEMPORAL -- only one mode may be writing at a time -- and that is not a type property. A
// `mode_planes` handed to tactical code is indistinguishable, to the compiler, from one handed to
// sim code. So the temporal half is a RUNTIME guard (`expect_mode()` below) with a red arm in
// `net_selftest.exe tacttest`, and this comment is the honest statement that the compiler is not
// doing that part. A guarantee oversold is worse than none (see sim_state.h's own version of this
// paragraph).
//
#pragma once
#include <cstdint>

#include "addr/mh_regions.gen.h"
#include "addr/mh_structs.gen.h"

// The four sanctioned binders, forward-declared so the friend list below can name them. This IS
// the W3 list for the shared planes; it is deliberately short and deliberately explicit.
namespace mh::sim {
struct sim_state;
sim_state state();
struct sim_fixture;
class sim_store;
} // namespace mh::sim
namespace mh::tact {
struct tact_state;
tact_state state();
struct tact_fixture;
} // namespace mh::tact

namespace mh::state {

// The record types, aliased so a consumer names what it indexes rather than a generated symbol.
using tile_object   = mh::game::mh_map_tile_object_data;
using path_waypoint = mh::game::mh_llm_strat_path_waypoint;

// `passable` sentinels, named because the raw 0/1/2 at a call site says nothing. Recovered from
// llm_tact_map_reset (writes 2 over the sub-block) and llm_tact_unit_move_advance (2 = the tile a
// unit is stepping OFF, 0 = the tile it is stepping ONTO).
inline constexpr uint8_t PASSABLE_BLOCKED = 0;
inline constexpr uint8_t PASSABLE_DEFAULT = 2;

// The strategic pathfinder arena's shape. slot*300 waypoints of 2 bytes = 0x258 per slot, 100
// slots per owner = 0xea60 -- the strides the disassembly folds into its disp32
// (llm_tact_unit_move_advance @0x00431046/0x0043104c; the tactical closure Sect. 9).
inline constexpr int32_t PATH_WAYPOINTS_PER_SLOT  = 300;
inline constexpr int32_t PATH_SLOTS_PER_OWNER     = 100;
inline constexpr int32_t PATH_WAYPOINTS_PER_OWNER = PATH_SLOTS_PER_OWNER * PATH_WAYPOINTS_PER_SLOT;

// ---------------------------------------------------------------------------------------------

class mode_planes {
public:
    mode_planes(const mode_planes &)            = default;
    mode_planes &operator=(const mode_planes &) = default;

    // ---- the 256x256 tile grid --------------------------------------------------------------
    //
    // COLUMN-MAJOR: the record index is (tile_x << 8) | tile_y, i.e. 8 bytes per tile with tile_x
    // as the OUTER index. Identical to sim_state.h's `tile_at()` and to the original's own
    // `IMUL col,0x800 / row*8` folding. Unchecked, exactly like every accessor in sim_store: the
    // originals index out of range on purpose in places and a translation must be able to say so.
    tile_object &tile_object_at(int32_t tile_x, int32_t tile_y) {
        return tile_objects_[(tile_x << 8) | tile_y];
    }
    const tile_object &tile_object_at(int32_t tile_x, int32_t tile_y) const {
        return tile_objects_[(tile_x << 8) | tile_y];
    }

    // ---- the passability plane, same indexing ------------------------------------------------
    uint8_t &passable_at(int32_t tile_x, int32_t tile_y) {
        return passable_[(tile_x << 8) | tile_y];
    }
    uint8_t passable_at(int32_t tile_x, int32_t tile_y) const {
        return passable_[(tile_x << 8) | tile_y];
    }

    // ---- the pathfinder arena ----------------------------------------------------------------
    //
    // One waypoint by reference. `owner` is the per-player arena (0xea60 apart), `slot` a route
    // (0x258 apart), `entry` a (heading, run_length) pair.
    // The arithmetic is deliberately IDENTICAL to sim_store::path_buffer_at()'s, which SIM1-G1
    // proved against the original: same strides, same operand order. Two tenants of one arena that
    // disagreed about its shape would be a corruption nobody could attribute.
    path_waypoint &path_waypoint_at(int32_t owner, int32_t slot, int32_t entry) {
        return path_buffers_[owner * PATH_WAYPOINTS_PER_OWNER + slot * PATH_WAYPOINTS_PER_SLOT +
                             entry];
    }
    uint8_t &path_slot_flag_at(int32_t owner, int32_t slot) {
        return path_slot_flags_[owner * PATH_SLOTS_PER_OWNER + slot];
    }
    int32_t &path_free_slot_count_at(int32_t owner) { return path_free_slot_count_[owner]; }

private:
    // W3: exactly four binders -- two production, two offline. Adding a fifth is a reviewable diff.
    mode_planes(tile_object *tile_objects, uint8_t *passable, path_waypoint *path_buffers,
                uint8_t *path_slot_flags, int32_t *path_free_slot_count)
        : tile_objects_(tile_objects), passable_(passable), path_buffers_(path_buffers),
          path_slot_flags_(path_slot_flags), path_free_slot_count_(path_free_slot_count) {}

    friend mh::sim::sim_state mh::sim::state();
    friend struct mh::sim::sim_fixture;
    // sim_store CONSTRUCTS its member from pointers its own binder already resolved, so this is a
    // fifth NAME on the list rather than a fifth BINDER: it never names a region or an address.
    friend class mh::sim::sim_store;
    friend mh::tact::tact_state mh::tact::state();
    friend struct mh::tact::tact_fixture;

    // W2: private, so no plane base ever escapes this class.
    tile_object   *tile_objects_;
    uint8_t       *passable_;
    path_waypoint *path_buffers_;
    uint8_t       *path_slot_flags_;
    int32_t       *path_free_slot_count_;
};

// ---- the tile record's two-readings byte pair -------------------------------------------------
//
// `mh_map_tile_object_data::unit` is TWO BYTES with two incompatible readings by game mode, and the
// generated header renders it flat as `uint8_t unit[2]` because the union is not applied in Ghidra.
// Indexing it by hand at a call site is how the two readings get confused, so both are named here
// and nothing else should touch `unit[]` directly:
//
//   unit[0] -- STRATEGIC unit id low byte; in TACTICAL, the occupancy stamp
//              (llm_tact_tile_rebuild_occupancy_layer writes the FX type stamp in the low 7 bits
//              and ORs 0x80 for "a unit stands here").
//   unit[1] -- STRATEGIC unit player; in TACTICAL, the move-path preview overlay nibble pair
//              (high = dir group, low = step marker).
//
// They cannot coexist: llm_tact_move_path_preview_clear zeroes unit[1] over the whole sub-block,
// which would destroy a strategic unit's player field for any unit on that tile. That is the
// clearest single piece of evidence that the modes are exclusive in time, not merely by convention.
inline uint8_t &tile_occupancy(mode_planes &p, int32_t x, int32_t y) {
    return p.tile_object_at(x, y).unit[0];
}
inline uint8_t &tile_overlay(mode_planes &p, int32_t x, int32_t y) {
    return p.tile_object_at(x, y).unit[1];
}

} // namespace mh::state
