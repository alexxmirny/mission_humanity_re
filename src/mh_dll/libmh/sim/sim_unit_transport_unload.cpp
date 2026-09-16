//
// sim/sim_unit_transport_unload.cpp -- see sim_unit_transport_unload.h. Translated from the
// DISASSEMBLY (tmp/decomp/llm_unit_transport_unload_field_00488a8b.asm,
// tmp/decomp/llm_unit_transport_unload_docked_004891d8.asm), not from Ghidra's C: both drafts read
// correctly for the overall shape and were used as a map, but every field offset, register-to-arg
// mapping, and branch target below was independently re-walked against the raw CMP/JZ/JNZ/IMUL
// opcodes per house rules -- including the one place (docked's dead read, see the header FINDING)
// where the draft silently dropped a real instruction.
//
#include "sim/sim_unit_transport_unload.h"

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"        // ai_say / trace_budget -- the shared trace sink, not AI state
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"
#include "state/promoted_select.h" // LIB-REF-SPLIT: MH_PROMOTED

namespace mh::sim {

const unit_transport_unload_calls &live_unit_transport_unload_calls() {
    static const unit_transport_unload_calls c = {
        MH_LIBMH_BIND(llm_strat_unit_spawn_on_tile),
        MH_LIBMH_BIND(llm_strat_unit_spawn_docked),
        MH_LIBMH_BIND(llm_strat_dir_from_to),
        MH_LIBMH_BIND(llm_strat_unit_squad_pick_lead_soldier_in_direction),
        MH_LIBMH_BIND(llm_strat_unit_soldier_unlink),
        MH_PROMOTED_ROW(llm_strat_facing_step_apply) /* renamed from llm_ui_cursor_apply_anim_frame_offset 2026-09-02 */,
        MH_LIBMH_BIND(llm_strat_unit_soldiers_start_walk_anim),
        MH_LIBMH_BIND(llm_strat_ctrl_group_contains_unit),
        MH_LIBMH_BIND(llm_strat_unit_ctrl_group_assign),
        MH_PROMOTED_ROW(llm_strat_squad_placement_offset_lookup) /* renamed from llm_ui_cursor_lookup_offset_pair 2026-09-02 */,
    };
    return c;
}

namespace detail {

// `unit.unit_above` is `uint8_t[2]` (`map_t_unit_full_id`) -- see header HAZARD 2. Local copy of the
// idiom sim_unit_put_on_map.cpp / sim_unit_soldier_screen_pos.cpp / ai_holding_pen.cpp / others each
// carry their own copy of, per that idiom's own established per-TU duplication.
inline uint16_t unit_full_id_word(const uint8_t (&packed)[2]) {
    return static_cast<uint16_t>(packed[0]) | (static_cast<uint16_t>(packed[1]) << 8);
}

// ---------------------------------------------------------------------------------------------
// llm_unit_transport_unload_field @0x00488a8b
// ---------------------------------------------------------------------------------------------
void unload_field(const sim_view &v, sim_store &own, const unit_transport_unload_calls &c,
                  uint32_t transport_player, uint32_t transport_unit_index) {
    const uint16_t player = static_cast<uint16_t>(transport_player);
    const int32_t  tidx   = static_cast<int32_t>(transport_unit_index);

    // 0x00488aa8-0x00488ace: cfg soldier capacity of the transport's CURRENT type -- the loop's
    // remaining-count seed. Read BEFORE any decrement (see header HAZARD 1).
    int32_t remaining = v.cfg_units[own.unit_at(player, tidx).unit_proto_id].soldier_count;

    // 0x00488ad1-0x00488aef: soldier_proto_id = transport.unit_proto_id (re-read, still
    // pre-decrement) - remaining_seed + 1. Full 32-bit unsigned arithmetic in the original (no sign
    // extension of either operand) -- truncation to 16 bits happens only where it is CONSUMED
    // (spawn_on_tile's uint16_t param), so computing it that way here is bit-identical.
    const uint16_t soldier_proto_id = static_cast<uint16_t>(
        static_cast<uint32_t>(own.unit_at(player, tidx).unit_proto_id) -
        static_cast<uint32_t>(remaining) + 1u);

    // 0x00488af2-0x00488b29: snapshot x/y (byte fields, zero-extended) -- read ONCE, reused for
    // every direction probe below (the original never re-reads them mid-loop either).
    const int32_t origin_x = own.unit_at(player, tidx).x;
    const int32_t origin_y = own.unit_at(player, tidx).y;

    // 0x00488b3f-0x00488b70: per-soldier energy/experience SHARE, computed ONCE off the INITIAL
    // capacity and reused unchanged every iteration (field-only behavior -- docked recomputes its
    // energy share every iteration instead, see that function below).
    const double  energy_share     = own.unit_at(player, tidx).energy / static_cast<double>(remaining);
    const int32_t experience_share = own.unit_at(player, tidx).experience / remaining;

    // 0x00488b7a-0x00488f31: `while (dir < 8 && remaining > 1)`.
    for (int32_t dir = 0; dir < 8 && remaining > 1; ++dir) {
        // 0x00488b93-0x00488bc8: candidate neighbor tile, wrap-masked -- same idiom
        // sim_bldg_footprint_is_clear.cpp's own tile derivation uses.
        const int32_t tile_x =
            (origin_x + v.dir8_offsets[dir].dx) & static_cast<int32_t>(map_width_mask(v));
        const int32_t tile_y =
            (origin_y + v.dir8_offsets[dir].dy) & static_cast<int32_t>(map_height_mask(v));

        // 0x00488bcb-0x00488bf3: `passable != 0 && tile_objects.building == 0`; anything else ->
        // skip to the next direction. `tile_at()`'s `.building` field (offset 0x2) matches the
        // original's own CMP width (word).
        if (v.passable[(tile_x << 8) | tile_y] == 0 || tile_at(v, tile_x, tile_y).building != 0) {
            continue;
        }

        // 0x00488bf7-0x00488c0d: spawn onto the candidate tile.
        const int32_t spawned = c.spawn_on_tile(static_cast<uint32_t>(tile_x), static_cast<uint32_t>(tile_y),
                                                soldier_proto_id, player);
        if (spawned == 0) {
            // 0x00488f25: abort the remaining attempts, same as docked's spawn-failure arm.
            remaining = 0;
            continue;
        }

        // 0x00488c17-0x00488c34: heading from the transport to the tile it just spawned onto.
        const int32_t dir_to_tile =
            c.dir_from_to(origin_x << 5, origin_y << 5, tile_x << 5, tile_y << 5);

        // 0x00488c37-0x00488c51: the NEW unit's own (freshly-initialized) soldier-chain head.
        const uint16_t new_chain_head = unit_full_id_word(own.unit_at(player, spawned).unit_above);

        // 0x00488c54-0x00488c7d: pick a lead soldier off the TRANSPORT's own chain, in that
        // direction -- this becomes the position source for the new unit's chain-head record.
        const uint32_t lead_soldier = c.squad_pick_lead_soldier_in_direction(
            player, unit_full_id_word(own.unit_at(player, tidx).unit_above),
            static_cast<uint32_t>(dir_to_tile));

        // 0x00488c7d-0x00488cfc: seed the new unit's chain-head soldier record from the picked lead
        // (start_x/y copied, end_x/y hardcoded to 0x10 -- read directly off the two `MOV byte ..,
        // 0x10` immediates, not derived).
        own.soldier_at(player, new_chain_head).start_x = own.soldier_at(player, lead_soldier).start_x;
        own.soldier_at(player, new_chain_head).start_y = own.soldier_at(player, lead_soldier).start_y;
        own.soldier_at(player, new_chain_head).end_x   = 0x10;
        own.soldier_at(player, new_chain_head).end_y   = 0x10;

        // 0x00488d03-0x00488d12: detach the picked lead soldier from the transport's own chain.
        c.soldier_unlink(player, tidx, lead_soldier);

        // 0x00488d15-0x00488d49: apply the cursor animation frame offset in-place onto the
        // chain-head's own start_x/y (out-params -- the original writes back through them).
        c.cursor_apply_anim_frame_offset(
            reinterpret_cast<char *>(&own.soldier_at(player, new_chain_head).start_x),
            reinterpret_cast<char *>(&own.soldier_at(player, new_chain_head).start_y), dir_to_tile);

        // 0x00488d4e-0x00488da6: cur_x/y = the (possibly just-adjusted) start_x/y.
        own.soldier_at(player, new_chain_head).cur_x = own.soldier_at(player, new_chain_head).start_x;
        own.soldier_at(player, new_chain_head).cur_y = own.soldier_at(player, new_chain_head).start_y;

        // 0x00488da6-0x00488db2: kick off the new unit's walk animation.
        c.soldiers_start_walk_anim(player, spawned);

        // 0x00488db2-0x00488e01: transport's own proto_id/energy/experience decrement (see header
        // HAZARD 1 for why unit_proto_id specifically is decremented here, not just used).
        own.unit_at(player, tidx).unit_proto_id =
            static_cast<uint16_t>(own.unit_at(player, tidx).unit_proto_id - 1);
        own.unit_at(player, tidx).energy -= energy_share;
        own.unit_at(player, tidx).experience -= experience_share;

        // 0x00488e0a-0x00488e99: the new unit's own fields -- energy/experience from the shares
        // above, move_microstep copied from the transport, facing_target/current from the
        // direction just computed.
        own.unit_at(player, spawned).energy         = energy_share;
        own.unit_at(player, spawned).experience     = experience_share;
        own.unit_at(player, spawned).move_microstep = own.unit_at(player, tidx).move_microstep;
        const uint8_t facing                        = static_cast<uint8_t>(dir_to_tile);
        own.unit_at(player, spawned).facing_target  = facing;
        own.unit_at(player, spawned).facing_current = facing;

        // 0x00488eb5-0x00488ebb: `remaining -= 1` (post-decrement read, same value used for the
        // loop's own bound check next iteration).
        remaining -= 1;

        // 0x00488ebb-0x00488f23: local-player-only control-group propagation, same shape as
        // docked's own tail below.
        if (player == static_cast<uint16_t>(*v.player_side)) {
            if (c.ctrl_group_contains_unit(static_cast<uint32_t>(tidx), v.ctrl_groups[0].count, 0) != 0) {
                c.ctrl_group_assign(spawned, 0);
            }
            if (own.unit_at(player, tidx).ctrl_group_id != 0) {
                c.ctrl_group_assign(spawned, own.unit_at(player, tidx).ctrl_group_id);
            }
        }
    }

    // ---- tail (0x00488f31-0x00489055): re-lay-out the remaining chain's render end-positions ----
    const int32_t remaining_after = v.cfg_units[own.unit_at(player, tidx).unit_proto_id].soldier_count;
    uint16_t      chain           = unit_full_id_word(own.unit_at(player, tidx).unit_above);
    for (int32_t i = 1; i <= remaining_after; ++i) {
        const uint16_t node = chain;
        c.cursor_lookup_offset_pair(remaining_after, i,
                                    reinterpret_cast<char *>(&own.soldier_at(player, node).end_x),
                                    reinterpret_cast<char *>(&own.soldier_at(player, node).end_y));
        // field-only: refresh the TRANSPORT's own soldier-slot start_x/y from its OWN cur_x/y --
        // cur_x/y itself is left untouched (see header "WHERE THEY DIFFER").
        own.soldier_at(player, tidx).start_x = own.soldier_at(player, tidx).cur_x;
        own.soldier_at(player, tidx).start_y = own.soldier_at(player, tidx).cur_y;
        chain                                = own.soldier_at(player, node).next_soldier;
    }
    c.soldiers_start_walk_anim(player, tidx);
}

// ---------------------------------------------------------------------------------------------
// llm_unit_transport_unload_docked @0x004891d8
// ---------------------------------------------------------------------------------------------
void unload_docked(const sim_view &v, sim_store &own, const unit_transport_unload_calls &c,
                   uint32_t player_in, uint32_t unit_index_in) {
    const uint16_t player = static_cast<uint16_t>(player_in);
    const int32_t  tidx   = static_cast<int32_t>(unit_index_in);

    // 0x00489208-0x0048921b: cfg soldier capacity of the transport's CURRENT type (loop seed).
    int32_t remaining = v.cfg_units[own.unit_at(player, tidx).unit_proto_id].soldier_count;

    // 0x0048921e-0x0048923c: soldier_proto_id, same derivation/arithmetic-width as field's own --
    // see that function's comment.
    const uint16_t soldier_proto_id = static_cast<uint16_t>(
        static_cast<uint32_t>(own.unit_at(player, tidx).unit_proto_id) -
        static_cast<uint32_t>(remaining) + 1u);

    // 0x0048923f-0x00489259: the transport's own dock/storage slot -- the `probe_slot` arg every
    // spawn_docked call in this loop reuses unchanged.
    const uint32_t probe_slot = own.unit_at(player, tidx).home_storage_slot;

    // 0x0048925c-0x004893c8: `while (remaining > 1)` -- no direction/terrain search at all.
    while (remaining > 1) {
        // 0x00489266-0x0048928d: detach the head of the transport's OWN chain (always the head --
        // unlike field, there is no "pick a lead in a direction" step).
        c.soldier_unlink(player, tidx, unit_full_id_word(own.unit_at(player, tidx).unit_above));

        // 0x00489292-0x004892a5: spawn into the transport's own storage slot.
        const int32_t spawned = c.spawn_docked(soldier_proto_id, player, probe_slot);
        if (spawned == 0) {
            // 0x004893bc: abort the remaining attempts.
            remaining = 0;
            continue;
        }

        // 0x004892af-0x004892c9: DEAD READ in the original (`units[player][spawned].unit_above`,
        // result never consumed again) -- see header FINDING. Deliberately not reproduced; a MOV
        // into an otherwise-dead stack local has no observable effect.

        // 0x004892cc-0x004892e8: per-soldier energy share, recomputed EVERY iteration off the
        // CURRENT (already-decremented) `remaining` -- NOT hoisted before the loop like field's own
        // (re-verified against the FILD/FDIVR address inside the loop body).
        const double energy_share =
            own.unit_at(player, tidx).energy / static_cast<double>(remaining);

        // 0x004892eb-0x00489321: transport's own proto_id/energy decrement (experience is NOT
        // touched anywhere in this function -- field-only field).
        own.unit_at(player, tidx).unit_proto_id =
            static_cast<uint16_t>(own.unit_at(player, tidx).unit_proto_id - 1);
        own.unit_at(player, tidx).energy -= energy_share;

        // 0x00489327-0x00489343: the new unit's own energy.
        own.unit_at(player, spawned).energy = energy_share;

        // 0x0048934c-0x004893b4: local-player-only control-group propagation, same shape as
        // field's own tail above.
        if (player == static_cast<uint16_t>(*v.player_side)) {
            if (c.ctrl_group_contains_unit(static_cast<uint32_t>(tidx), v.ctrl_groups[0].count, 0) != 0) {
                c.ctrl_group_assign(spawned, 0);
            }
            if (own.unit_at(player, tidx).ctrl_group_id != 0) {
                c.ctrl_group_assign(spawned, own.unit_at(player, tidx).ctrl_group_id);
            }
        }

        // 0x004893b4-0x004893ba: `remaining -= 1`.
        remaining -= 1;
    }

    // ---- tail (0x004893c8-0x00489518): re-lay-out the remaining chain's render positions --------
    const int32_t remaining_after = v.cfg_units[own.unit_at(player, tidx).unit_proto_id].soldier_count;
    uint16_t      chain           = unit_full_id_word(own.unit_at(player, tidx).unit_above);
    for (int32_t i = 1; i <= remaining_after; ++i) {
        const uint16_t node = chain;
        c.cursor_lookup_offset_pair(remaining_after, i,
                                    reinterpret_cast<char *>(&own.soldier_at(player, node).end_x),
                                    reinterpret_cast<char *>(&own.soldier_at(player, node).end_y));
        // docked-only: the TRANSPORT's own soldier-slot cur_x/y is set from the WALKED node's
        // end_x/y, and THEN start_x/y is set from that cur_x/y -- see header "WHERE THEY DIFFER".
        own.soldier_at(player, tidx).cur_x   = own.soldier_at(player, node).end_x;
        own.soldier_at(player, tidx).start_x = own.soldier_at(player, tidx).cur_x;
        own.soldier_at(player, tidx).cur_y   = own.soldier_at(player, node).end_y;
        own.soldier_at(player, tidx).start_y = own.soldier_at(player, tidx).cur_y;
        chain                                = own.soldier_at(player, node).next_soldier;
    }
    // NOTE: unlike field, docked's tail does NOT call soldiers_start_walk_anim -- confirmed absent
    // from the disassembly (the function returns straight from the tail loop's exit, 0x00489518).
}

} // namespace detail

// ---- the public wrappers -----------------------------------------------------------------------

void unload_field(uint32_t transport_player, uint32_t transport_unit_index) {
    sim_state st = state();
    detail::unload_field(st.read, st.own, live_unit_transport_unload_calls(), transport_player,
                         transport_unit_index);
}

void unload_docked(uint32_t player, uint32_t unit_index) {
    sim_state st = state();
    detail::unload_docked(st.read, st.own, live_unit_transport_unload_calls(), player, unit_index);
}


} // namespace mh::sim
