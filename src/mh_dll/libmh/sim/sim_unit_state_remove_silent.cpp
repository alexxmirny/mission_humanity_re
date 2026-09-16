//
// sim/sim_unit_state_remove_silent.cpp -- see sim_unit_state_remove_silent.h. Translated from the
// DISASSEMBLY (tmp/decomp_sim/llm_strat_unit_state_remove_silent_0048592d.asm) -- see the header
// banner for the units[player][0]-energy-write derivation and the field-offset cross-checks.
//
#include "sim/sim_unit_state_remove_silent.h"

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"        // ai_say / trace_budget -- the shared trace sink, not AI state
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const unit_state_remove_silent_calls &live_unit_state_remove_silent_calls() {
    static const unit_state_remove_silent_calls c = {
        MH_LIBMH_BIND(llm_strat_ai_notify_object_removed),
        MH_LIBMH_BIND(llm_strat_unit_remove_from_map),
        MH_LIBMH_BIND(llm_strat_unit_on_destroyed),
        MH_LIBMH_BIND(llm_strat_target_release_ref),
        MH_LIBMH_BIND(llm_strat_unit_housing_count_remove),
        MH_LIBMH_BIND(llm_strat_storage_release_door_held_by_unit),
        MH_LIBMH_BIND(llm_strat_path_free_slot),
        MH_LIBMH_BIND(llm_strat_player_presence_lost),
        MH_LIBMH_BIND(llm_strat_unit_set_state),
        MH_LIBMH_BIND(map_fow_UpdateFoWPlus),
        MH_LIBMH_BIND(game_SetEvent),
        MH_LIBMH_BIND(llm_strat_unit_notify_ui),
    };
    return c;
}

namespace {

// DAT_0050146a, read-memory-confirmed by the conductor (batch SIM1-G1 context.md). A DIFFERENT
// compiler-emitted literal from sim_unit_teardown.cpp's DAT_00501482 (same bit pattern -1.0, different
// address -- see the header banner).
inline constexpr double UNIT_STATE_REMOVE_SILENT_UNIT0_ENERGY_DELTA = -1.0; // DAT_0050146a

} // namespace

namespace detail {

void unit_state_remove_silent(const sim_view &v, sim_store &own, const unit_state_remove_silent_calls &c) {
    unit &u = own.cur_unit(); // _G_LLM_STRAT_CUR_UNIT dereferenced -- see sim_view::cur_unit's comment:
                              // bound from the SAME resolved pointer as *v.cur_player/*v.cur_index, so
                              // u IS units[*v.cur_player][*v.cur_index]. The assembly re-derives that
                              // same address twice more below (via CUR_UNIT reloads, not roster row/col
                              // arithmetic this time -- unlike DIE_EXPLODE/move_walker's sibling
                              // functions) -- reused here as one reference, same simplification those
                              // TUs' own bodies document.
    const uint16_t player = *v.cur_player;
    const uint16_t index  = *v.cur_index;

    // 0x00485945-0x00485959: UNCONDITIONAL notify. The `0` third argument is the same EBX the asm
    // zeroes at function entry (XOR EBX,EBX) and never rewrites before this call -- see the header
    // banner: NOT dead code, this call's own hard_remove argument.
    c.ai_notify_object_removed(static_cast<uint32_t>(player) | 0x80u, static_cast<uint32_t>(index), 0);

    // 0x0048595e-0x004859a1: energy ALWAYS zeroed (both dwords of the double in the asm; equivalent to
    // one `= 0.0` store). ADDITIONALLY, only when the unit's energy was > 0.0 going in, FADD the
    // -1.0 constant into units[player][0].energy -- SLOT 0 of this player's roster, NOT
    // units[player][index] (the unit actually being removed). See the header banner: transcribed
    // literally per the translator brief's "preserve documented original bugs" rule.
    if (u.energy > 0.0) {
        u.energy = 0.0;
        own.unit_at(player, 0).energy += UNIT_STATE_REMOVE_SILENT_UNIT0_ENERGY_DELTA;
    } else {
        u.energy = 0.0;
    }

    // 0x004859b4-0x00485a0e: small-vs-big dispatch on cfg type, SIGNED compare against UNIT_TYPE_A_HELI
    // (0xf) exactly as sim_unit_state_die_explode.h's identical field/boundary documents (proto.type is
    // committed uint32_t, but the asm's own CMP+JG is signed -- the two only disagree when the high bit
    // of proto.type is set, never true for shipped cfg content).
    const uint16_t unit_proto_id = u.unit_proto_id;
    if (static_cast<int32_t>(v.cfg_units[unit_proto_id].type) < static_cast<int32_t>(UNIT_TYPE_A_HELI)) {
        c.unit_remove_from_map(player, static_cast<uint32_t>(index));
    } else {
        c.unit_on_destroyed(player, static_cast<uint32_t>(index));
    }

    // 0x00485a0e-0x00485a94: release target refs, ALWAYS checked, common to both arms. Independent
    // ifs (both can fire), not else-if -- matches DIE_EXPLODE's/teardown's identical pair. Mode
    // literals 1/3 are RELEASE_MODE_PRIMARY_CLEAR/_SECONDARY_CLEAR, not imported by name here for the
    // same ODR-collision reason sim_unit_state_die_explode.cpp's own comment documents.
    if (u.target_ref != 0) {
        c.target_release_ref(static_cast<uint32_t>(player), static_cast<int32_t>(index), 1u);
        u.target_ref   = 0;
        u.target_index = 0;
    }
    if (u.target2_ref != 0) {
        c.target_release_ref(static_cast<uint32_t>(player), static_cast<int32_t>(index), 3u);
        u.target2_ref   = 0;
        u.target2_index = 0;
    }

    // 0x00485a94-0x00485adb: housing + door reservations, then the path slot if one was assigned.
    c.unit_housing_count_remove(static_cast<int32_t>(player), unit_proto_id);
    c.storage_release_door_held_by_unit(static_cast<int32_t>(player), static_cast<int32_t>(index));
    if (u.path_slot_id != 0xffu) {
        c.path_free_slot(player, static_cast<int32_t>(index));
    }

    // 0x00485add-0x00485b29: per-player/per-planet alive counter ONLY -- no units_lost_total bump here
    // (unlike DIE_EXPLODE/teardown, which both also increment it; this handler is the "silent" removal
    // path). IF the decremented count == 0: player_presence_lost(player, 0).
    player_profile &profile = own.profile_at(player);
    profile.units_alive[*v.planet_index] -= 1;
    if (profile.units_alive[*v.planet_index] == 0) {
        c.player_presence_lost(player, 0u);
    }

    // 0x00485b29-0x00485b54: commit CORPSE_FOW_DECAY(4) via the ORIGINAL setter (this function does not
    // write unit.state itself), THEN overload move_microstep as the saved sight radius for FoW cleanup
    // (matches the field's own struct comment exactly, same as DIE_EXPLODE's identical tail).
    c.unit_set_state(UNIT_STATE_REMOVE_SILENT_CORPSE_FOW_DECAY);
    u.move_microstep = v.cfg_units[unit_proto_id].sight;

    // 0x00485b54-0x00485ba1: FoW refresh at the unit's tile x/y; sight = the low byte of move_microstep
    // just written (matches the asm's own MOVZX byte reload of the field).
    c.map_fow_UpdateFoWPlus(player, u.x, u.y, static_cast<uint8_t>(u.move_microstep));
    c.game_SetEvent(UNIT_STATE_REMOVE_SILENT_MAP_OBJECTS_REFRESH);
    c.unit_notify_ui(player, static_cast<uint32_t>(index));
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

void unit_state_remove_silent() {
    sim_state st = state();
    detail::unit_state_remove_silent(st.read, st.own, live_unit_state_remove_silent_calls());
}


} // namespace mh::sim
