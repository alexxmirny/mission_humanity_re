//
// sim/sim_unit_path_step_blocked.cpp -- see sim_unit_path_step_blocked.h. Translated from the
// DISASSEMBLY (tmp/decomp_sim/llm_strat_unit_path_step_blocked_00495406.asm), not from any Ghidra .c
// draft (none was exported for this batch's addressed slice) -- every field access below was
// address-derived against mh_structs.gen.h's static_assert'd offsets and the `Unit` cfg array's
// registered base (0x00e4a098), not inferred from a decompiler rendering.
//
#include "sim/sim_unit_path_step_blocked.h"

#include "addr/mh_calls.gen.h"  // typed callable for the one original function we still call OUT to
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const unit_path_step_blocked_calls &live_unit_path_step_blocked_calls() {
    static const unit_path_step_blocked_calls c = {
        MH_LIBMH_BIND(llm_strat_facing24_to_delta),
    };
    return c;
}

namespace detail {

int32_t unit_path_step_blocked(const sim_view &v, const unit_path_step_blocked_calls &c, uint32_t player,
                               int32_t unit_idx) {
    const unit &u = unit_of(v, player, unit_idx);

    // 0x00495417-0x00495456: own.path_buffer_at(player, unit.path_slot_id, 0).heading -- entry index 0
    // (the buffer's FIRST waypoint), player-stride PATH_WAYPOINTS_PER_PLAYER (0xea60/2 = 30000),
    // slot-stride PATH_WAYPOINTS_PER_SLOT (0x258/2 = 300). No sim_store::path_buffer_at() call here:
    // this function only READS the buffer, so it indexes the const view directly.
    const uint32_t path_index =
        player * PATH_WAYPOINTS_PER_PLAYER + static_cast<uint32_t>(u.path_slot_id) * PATH_WAYPOINTS_PER_SLOT;
    const int32_t cur_heading = v.path_buffers[path_index].heading;

    // 0x00495459-0x00495465/0x00495471: valid facing domain is [1, 24] (0x18) inclusive; outside it
    // (JL on <1, falling through past JLE on >24) there is no direction to test -- blocked.
    if (cur_heading < 1 || cur_heading > 0x18) return 1;

    // 0x00495471-0x0049547e: direction delta for the current path heading. Out-params start zeroed
    // like every other sibling call site (sim_unit_predict_coords.cpp, sim_unit_state_move_walker.cpp)
    // even though the callee is expected to always fill both.
    int32_t out_dx = 0, out_dy = 0;
    c.facing24_to_delta(static_cast<uint32_t>(cur_heading), &out_dx, &out_dy);

    // 0x0049547f-0x004954cc: candidate tile = own tile + delta, each axis independently wrapped through
    // the map's TILE-space masks (map_width_mask/map_height_mask), matching
    // sim_pathfind_grid_geometry.cpp's tile_neighbor_in_dir's identical `(coord + delta) & mask` idiom.
    // u.x/u.y are uint8_t (unsigned {0..255}); the add promotes to (signed) int before the cast, exactly
    // reproducing the original's 32-bit ADD-then-AND.
    const int32_t cand_x = static_cast<int32_t>(static_cast<uint32_t>(u.x + out_dx) & map_width_mask(v));
    const int32_t cand_y = static_cast<int32_t>(static_cast<uint32_t>(u.y + out_dy) & map_height_mask(v));

    const tile_object &tile = tile_at(v, cand_x, cand_y);

    // 0x004954cf-0x004954fb: a PASSABLE candidate whose tile_objects `.building` word is 0 (no
    // occupant) is free -- not blocked. When the candidate is NOT passable, this whole check is
    // skipped (the original's JZ jumps straight past it to the occupant-bit test below) --
    // passability alone never decides "blocked" on its own, only "not blocked" together with
    // "unoccupied". CORRECTED 2026-08-20 (reimpl-verify divergence): the original reads `.building`
    // (offset +0x2, `CMP word ptr [...+0xd1ec82],0x0` @0x004954ef) and `.class_owner` (offset +0x6,
    // `TEST byte ptr [...+0xd1ec86],0x80` @0x00495515 / `MOV AL,[...+0xd1ec86]` @0x00495530), NOT
    // `.flags` (offset +0x0) or `.unit[0]` (offset +0x4) as an earlier draft of this file used --
    // both wrong fields happened to compile, but read the wrong bytes of the same struct. See
    // tools/data/ghidra_findings.json for the byte-offset cross-check against mh_structs.gen.h and
    // sim_path_step_check_and_request_detour.cpp's already-correct decode of these same two
    // addresses.
    if (v.passable[(cand_x << 8) | cand_y] != 0) {
        if (tile.building == 0) return 0;
    }

    // 0x00495507-0x0049551c: bit 0x80 of `.class_owner` marks a valid occupant record. Clear -> no
    // occupant -> blocked.
    if ((tile.class_owner & 0x80u) == 0) return 1;

    // 0x00495522-0x00495553: owner = low nibble of `.class_owner` (0-15; the original does not
    // bounds-check this against MAX_PLAYERS and neither does this translation -- a faithful
    // transcription, not a new gap). occupant_index = `.building`, re-read a SECOND time (the
    // original re-loads it rather than reusing the value from the passable arm above, which may not
    // even have run).
    const uint32_t  owner        = tile.class_owner & 0x0fu;
    const int32_t   occupant_idx = static_cast<int32_t>(tile.building);
    const unit     &occupant     = unit_of(v, owner, occupant_idx);
    const cfg_unit &occ_proto    = v.cfg_units[occupant.unit_proto_id];

    // 0x00495556-0x0049560a: not blocked (return 0) if the occupant's `state` equals its OWN class's
    // move_op_arg, OR its class's move_op_code, OR the literal 0xc (unbacked by any enum found in this
    // tree -- see declared_needs); otherwise blocked. Order matters: move_op_arg is checked first,
    // move_op_code second, exactly as the assembly's two branch blocks are laid out.
    if (occupant.state == occ_proto.move_op_arg) return 0;
    if (occupant.state == occ_proto.move_op_code) return 0;
    if (occupant.state == 0xc) return 0;

    return 1; // 0x0049560c
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

int32_t unit_path_step_blocked(uint32_t player, int32_t unit_idx) {
    const sim_view v = state().read;
    return detail::unit_path_step_blocked(v, live_unit_path_step_blocked_calls(), player, unit_idx);
}


} // namespace mh::sim
