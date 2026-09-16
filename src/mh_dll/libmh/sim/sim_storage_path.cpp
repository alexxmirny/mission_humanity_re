//
// sim/sim_storage_path.cpp -- see sim_storage_path.h. Translated from the DISASSEMBLY
// (tmp/decomp_sim/llm_strat_storage_setup_{exit,approach}_path_*.asm), not from any exported Ghidra
// `.c` draft -- every address cited below was independently walked against the raw
// IMUL/MOVZX/CMP/JZ/JL/IDIV opcodes per house rules.
//
#include "sim/sim_storage_path.h"

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"        // ai_say / trace_budget -- the shared trace sink, not AI state
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const storage_path_attach_calls &live_storage_path_attach_calls() {
    static const storage_path_attach_calls c = {
        MH_LIBMH_BIND(llm_strat_path_attach_slot),
    };
    return c;
}

namespace detail {

void storage_setup_exit_path(const sim_view &v, sim_store &own, const storage_path_attach_calls &c,
                             uint32_t player, int32_t unit_index, uint8_t exit_x, uint8_t exit_y,
                             int32_t path_slot, int32_t storage_slot) {
    // reimpl-verify note (matching sim_storage_can.cpp's own trio): the ORIGINAL reloads `player` via
    // a 16-bit MOVZX at every one of its many uses (never a plain 32-bit reload) -- narrow ONCE here.
    const uint16_t p = static_cast<uint16_t>(player);

    const unit_storage &st          = storage_of(v, p, storage_slot); // 0x00495cfb-0x00495d24
    const building     &b           = building_of(v, p, st.b_index);  // 0x00495d24-0x00495d2b
    const int32_t       building_id = b.building_id;

    unit &u = own.unit_at(p, unit_index);

    // 0x00495d41: move_microstep = 0x1f (31, the LAST of 32 entries per heading in
    // _G_LLM_STRAT_MOVE_MICROSTEPS) -- set BEFORE the table lookup below, and read back unchanged
    // (nothing between here and the lookup writes it again).
    u.move_microstep = 0x1f;

    // 0x00495d4b-0x00495d68: heading = Building[building_id].door_exit_route[0] % 24 (IDIV 0x18). The
    // dividend is a zero-extended BYTE (always in [0,255]), so despite the IDIV this is plain
    // unsigned modulo -- no signed-truncation subtlety (translator-brief rule 8 does not bite here).
    const uint8_t route0 = v.cfg_buildings[building_id].door_exit_route[0];
    u.move_heading       = static_cast<uint8_t>(route0 % 24); // 0x00495d6a-0x00495d7d

    // 0x00495d83-0x00495dd7: facing_target = move_microsteps[move_heading][move_microstep].facing
    // (move_microstep is still 0x1f, just set above). Row-major index, matching sim_view's own
    // documented [heading][MICROSTEPS_PER_HEADING] shape (already a bound accessor -- no declared
    // need here).
    u.facing_target =
        v.move_microsteps[static_cast<int32_t>(u.move_heading) * MICROSTEPS_PER_HEADING + u.move_microstep]
            .facing; // 0x00495dd1-0x00495dd7

    // 0x00495ddd-0x00495e31: RESOLVED (conductor, SIM1-G3, 2026-08-21) -- reimpl-verify
    // caught this as a real divergence. This is NOT the same store as facing_target re-targeted at
    // `y` (as the translator's own comment claimed): the write address is [EDX+0xdd8c75] = units_base
    // +0x2d = facing_current (per mh_structs.gen.h's static_assert), a DIFFERENT field from `y`
    // (+0x85, 0xdd8ccd) -- the two never alias, so the later exit_y write (0x00495e69) never
    // overwrites this one. facing_current is never touched again anywhere in the rest of this
    // function or in llm_strat_path_attach_slot's write closure, so it is a real, permanent write:
    // the same table value as facing_target (move_heading/move_microstep are unchanged between the
    // two lookups), applied to the sibling field.
    u.facing_current = u.facing_target; // 0x00495ddd-0x00495e31

    u.x = exit_x; // 0x00495e37-0x00495e4d
    u.y = exit_y; // 0x00495e53-0x00495e69

    // 0x00495e6f-0x00495ec3: copy Building[building_id].door_exit_route[0..) into this player's path
    // buffer slot -- a PRE-test while loop (checks the terminator/bound BEFORE copying; can copy zero
    // entries if route[0] is already 0xff). Only `.heading` is ever written -- `.run_length` is left
    // untouched, matching the original exactly (translator-brief rule 11).
    //
    // door_exit_route is declared byte[10], but the ORIGINAL's own read has no bound tied to that --
    // it is the same flat `Building[] + building_id*sizeof(record) + offset` addressing every other
    // cfg_buildings field uses, and door_exit_route/door_approach_route are laid out CONTIGUOUSLY in
    // the struct (offset 0x816, immediately followed by door_approach_route at 0x820, confirmed via
    // get-structure-info) -- an unterminated route past index 9 reads on into door_approach_route,
    // exactly as the original's single flat table does. A raw pointer into the field's own storage
    // reproduces that adjacency; PATH_WAYPOINTS_PER_SLOT(300) is the same bound the original enforces.
    const uint8_t *route  = v.cfg_buildings[building_id].door_exit_route;
    int32_t        cursor = 0;
    while (route[cursor] != 0xff && cursor < PATH_WAYPOINTS_PER_SLOT) {
        own.path_buffer_at(p, path_slot, cursor).heading = route[cursor];
        ++cursor;
    }
    own.path_buffer_at(p, path_slot, cursor).heading = 0xff; // 0x00495ec5-0x00495edf: the terminator

    c.path_attach_slot(static_cast<int32_t>(p), unit_index, path_slot); // 0x00495ee6-0x00495ef0

    u.path_cursor = 0; // 0x00495ef5-0x00495f08
}

void storage_setup_approach_path(const sim_view &v, sim_store &own, const storage_path_attach_calls &c,
                                 uint32_t player, int32_t unit_index, int32_t path_slot,
                                 int32_t storage_slot) {
    const uint16_t p = static_cast<uint16_t>(player); // same 16-bit-reload convention as the sibling

    // 0x00495b90-0x00495bc7: reads units[player][unit_index].x and .y into locals that are never
    // referenced again anywhere in this 0x164-byte body (confirmed by scanning the whole
    // disassembly). Provably dead pair of reads (plain register loads, no side effects, no later use)
    // -- omitted; see the translation report.

    const unit_storage &st          = storage_of(v, p, storage_slot); // 0x00495bd1-0x00495bfa
    const building     &b           = building_of(v, p, st.b_index);  // 0x00495bfa-0x00495c01
    const int32_t       building_id = b.building_id;

    // 0x00495c04-0x00495c27: move_heading = Building[building_id].door_approach_route[0], RAW (no
    // mod-24 -- unlike setup_exit_path's sibling read). door_approach_route[0] is byte-identical to
    // the field's former scalar name `facing` (still read that way by
    // llm_strat_locate_active_port/llm_strat_storage_get_approach_tile/llm_strat_unit_group_step_ground/
    // _plane, all updated to door_approach_route[0]).
    const uint8_t *route                    = v.cfg_buildings[building_id].door_approach_route;
    own.unit_at(p, unit_index).move_heading = route[0]; // 0x00495c21-0x00495c27

    // 0x00495c2d-0x00495c7d: copy Building[building_id].door_approach_route[0..) into the path buffer
    // -- a POST-test do-while (ALWAYS copies entry 0 first, THEN checks the NEXT entry for the 0xff
    // terminator / the 300-entry bound) -- a genuine structural difference from setup_exit_path's
    // pre-test loop, reproduced literally rather than unified. Only `.heading` is written, same as the
    // sibling. door_approach_route is declared byte[14]; an unterminated route past index 13 reads on
    // into `equivalent`/subsequent Building fields (and, past the whole 2114-byte record, into the
    // NEXT building_id's record) via this same raw-pointer adjacency -- see the sibling's comment
    // above. PATH_WAYPOINTS_PER_SLOT(300) is the same bound the original enforces.
    int32_t cursor = 0;
    do {
        own.path_buffer_at(p, path_slot, cursor).heading = route[cursor];
        ++cursor;
    } while (route[cursor] != 0xff && cursor < PATH_WAYPOINTS_PER_SLOT);
    own.path_buffer_at(p, path_slot, cursor).heading = 0xff; // 0x00495c7f-0x00495c99: the terminator

    c.path_attach_slot(static_cast<int32_t>(p), unit_index, path_slot); // 0x00495ca0-0x00495caa

    own.unit_at(p, unit_index).path_cursor = 0; // 0x00495caf-0x00495cc2
}

} // namespace detail

// ---- the public wrappers --------------------------------------------------------------------------

void storage_setup_exit_path(uint32_t player, int32_t unit_index, uint8_t exit_x, uint8_t exit_y,
                             int32_t path_slot, int32_t storage_slot) {
    sim_state st = state();
    detail::storage_setup_exit_path(st.read, st.own, live_storage_path_attach_calls(), player, unit_index,
                                    exit_x, exit_y, path_slot, storage_slot);
}

void storage_setup_approach_path(uint32_t player, int32_t unit_index, int32_t path_slot,
                                 int32_t storage_slot) {
    sim_state st = state();
    detail::storage_setup_approach_path(st.read, st.own, live_storage_path_attach_calls(), player,
                                        unit_index, path_slot, storage_slot);
}


} // namespace mh::sim
