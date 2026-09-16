//
// sim/sim_unit_remove_from_map.cpp -- see sim_unit_remove_from_map.h. Translated from the DISASSEMBLY
// (tmp/decomp/llm_strat_unit_remove_from_map_00487252.asm), not from Ghidra's C: the draft's top-level
// guard renders as an OR whose second operand is decompiler noise (see the header banner) -- every
// branch below was re-walked against the raw CMP/JZ/JNZ targets and their fallthrough addresses.
//
#include "sim/sim_unit_remove_from_map.h"

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"        // ai_say / trace_budget -- the shared trace sink, not AI state
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const unit_remove_from_map_calls &live_unit_remove_from_map_calls() {
    static const unit_remove_from_map_calls c = {
        MH_LIBMH_BIND(llm_strat_population_remove),
        MH_LIBMH_BIND(llm_strat_fow_remove_sight),
        MH_LIBMH_BIND(llm_strat_unit_ctrlgroup_leave),
        MH_LIBMH_BIND(game_SetEvent),
    };
    return c;
}

namespace detail {

void unit_remove_from_map(const sim_view &v, sim_store &own, const unit_remove_from_map_calls &c,
                          uint16_t player, uint32_t unit_idx) {
    const unit &u = unit_of(v, (uint32_t)player, (int32_t)unit_idx);

    // ---- the two-way split (0x00487298-0x00487336) -- see the header derivation ------------------
    const int32_t soldier_count = v.cfg_units[u.unit_proto_id].soldier_count;
    // unit_above reassembled little-endian from its raw two bytes, same idiom
    // sim_unit_passive_engage.cpp's `remaining` derivation uses for the same field (there at slot 0 for
    // a different, roster-count, purpose; here at `unit_idx` for THIS unit's own crew-chain head, per
    // the struct's own field comment on `next_soldier`).
    const uint32_t chain_head = uint32_t(u.unit_above[0]) | (uint32_t(u.unit_above[1]) << 8);

    if (soldier_count != 0 && chain_head != 0) {
        // ---- soldier-unlink loop (0x004872c3-0x00487313): walk _G_LLM_STRAT_SOLDIERS[player]'s
        // singly-linked chain starting at chain_head, freeing every mounted soldier slot. -------------
        int32_t slot = (int32_t)chain_head;
        while (slot != 0) {
            own.soldier_at(player, slot).owner_unit = 0;
            // record [0]'s owner_unit doubles as the roster's used-slot count -- see
            // sim_unit_create_soldier.cpp / sim_unit_passive_engage.cpp for the same slot-0 dual-use.
            own.soldier_at(player, 0).owner_unit -= 1;
            slot = own.soldier_at(player, slot).next_soldier;
            // NOTE (0x0048730d-0x00487310): the original also decrements a STACK-LOCAL loop counter
            // here (seeded from `soldier_count` above), but the loaded value is never read again and
            // the counter never gates anything -- the only loop-exit test is `slot == 0`. A stack local
            // has no observable effect outside this function, so it is not modeled here; see
            // uncertainties[] in the translation report.
        }
        // Clear the chain head on the unit record itself (0x00487315-0x00487328), byte-wise -- the
        // field is `uint8_t unit_above[2]`, not a scalar, so no reinterpret_cast is used to zero it.
        own.unit_at(player, unit_idx).unit_above[0] = 0;
        own.unit_at(player, unit_idx).unit_above[1] = 0;
        // This path SKIPS the population-remove block entirely (LAB_00487315's own unconditional JMP
        // straight to the shared tail at 0x004873f8) -- see the header banner.
    } else {
        // ---- population-remove branch (0x00487336-0x004873f8): soldier_count == 0 OR chain_head == 0
        if (v.cfg_units[u.unit_proto_id].human != 0) {
            // Same cfg `human` field re-read three times in the asm, all identical (nothing writes it
            // in between) -- cached once here, same value-identical-caching precedent
            // sim_unit_ctrl_group.cpp's `old_group_id` documents.
            const int32_t human = v.cfg_units[u.unit_proto_id].human;
            own.population_at(player).human += human;
            own.population_at(player).human_in_field -= human;
            c.population_remove(player, human);
        }
    }

    // ---- shared tail (0x004873f8-0x00487535): always runs regardless of which branch above ran -----
    const int32_t tile_x = u.x;
    const int32_t tile_y = u.y;

    own.tile_object_at(tile_x, tile_y).building    = 0;
    own.tile_object_at(tile_x, tile_y).class_owner = 0;
    own.passable_at(tile_x, tile_y)                = u.origin_tile_was_passable;

    c.fow_remove_sight(player, tile_x, tile_y, v.cfg_units[u.unit_proto_id].sight);

    if (*v.session_mode == SESSION_MP_LOCKSTEP) { own.unit_at(player, unit_idx).ai_group_index = 0; }

    if (player == (uint16_t)*v.player_side) {
        // ---- local player: leave the control group + refresh the info panel -----------------------
        c.ctrlgroup_leave(unit_idx);
        c.set_event(EVENT_INFO_REFRESH);
    } else if (*v.click_select_target_flags == (uint16_t)(player | 0x80u) &&
               own.click_select_target_id() == unit_idx) {
        // ---- other player: only if the removed unit WAS the click-selected target -------------------
        own.click_select_target_id() = 0;
        c.set_event(EVENT_INFO_REFRESH);
    }
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

void unit_remove_from_map(uint16_t player, uint32_t unit_idx) {
    sim_state st = state();
    detail::unit_remove_from_map(st.read, st.own, live_unit_remove_from_map_calls(), player, unit_idx);
}


} // namespace mh::sim
