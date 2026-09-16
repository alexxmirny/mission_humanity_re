//
// sim/sim_unit_state_die_explode.cpp -- see sim_unit_state_die_explode.h. Translated from the
// DISASSEMBLY (tmp/decomp/llm_strat_unit_state_die_explode_004854f3.asm), not from the Ghidra .c
// draft -- see the header's banner for the fine_x/fine_y stack-slot re-derivation, the DECLARED NEED
// on the debris-row padding field, and the field-offset cross-checks.
//
#include "sim/sim_unit_state_die_explode.h"

#include "addr/mh_calls.gen.h" // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"       // ai_say / trace_budget -- the shared trace sink, not AI state
#include "state/host_api.h"
#include "state/host_events.h"
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const unit_state_die_explode_calls &live_unit_state_die_explode_calls() {
    static const unit_state_die_explode_calls c = {
        MH_LIBMH_BIND(llm_strat_unit_get_coords),
        mh::state::evt::snd_play_at,
        MH_LIBMH_BIND(llm_strat_fx_anim_spawn),
        MH_LIBMH_BIND(llm_rand_below),
        MH_LIBMH_BIND(llm_strat_unit_remove_from_map),
        MH_LIBMH_BIND(llm_strat_unit_calc_render_fine_y),
        MH_LIBMH_BIND(llm_strat_unit_on_destroyed),
        MH_LIBMH_BIND(llm_game_sp_outcome_announce),
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

namespace detail {

void unit_state_die_explode(const sim_view &v, sim_store &own, const unit_state_die_explode_calls &c) {
    unit &u = own.cur_unit(); // _G_LLM_STRAT_CUR_UNIT dereferenced, read+write -- see sim_view::cur_unit's
                              // comment: bound from the SAME resolved pointer as *v.cur_player/*v.cur_index,
                              // so u IS units[*v.cur_player][*v.cur_index]. The assembly re-derives that
                              // same address 3 more times below via roster row/col arithmetic instead of
                              // reusing this pointer (see the comment at the type-dispatch site) -- flagged
                              // in this translation's uncertainties as a reassembled-control-flow judgment
                              // call, not a settled fact.

    // ---- unit_proto_id/proto, read ONCE and reused -- see the header field-offset derivation --------
    const uint16_t  unit_proto_id = u.unit_proto_id;
    const cfg_unit &proto         = v.cfg_units[unit_proto_id];

    // ---- 0x0048550b-0x0048551f: get_coords out-param mapping is EBX=out_x=&[EBP-0x1c],
    // ECX=out_y=&[EBP-0x18] -- see the header banner's 3-way cross-check. Named fine_x/fine_y by what
    // they hold, not by Ghidra's stack-slot numbering.
    int32_t fine_x = 0, fine_y = 0;
    c.unit_get_coords(*v.cur_player, static_cast<int32_t>(*v.cur_index), &fine_x, &fine_y);

    // 0x00485524-0x00485574: death sound, gated on SIM_ACTIVE. The SAR/SHL/SBB/SAR sequence is the
    // same signed-truncating-divide-by-32 idiom every other sim TU calls fine_to_tile() for (see
    // sim_projectile_tick.cpp) -- reproduced inline via plain `/ 32` per this codebase's established
    // equivalence for this exact idiom.
    if (*v.sim_active != 0) {
        const int32_t tile_col = fine_x / 32; // 0x0048552d-0x0048553e
        const int32_t tile_row = fine_y / 32; // 0x00485540-0x0048554e
        // The original offscreen_snd_volume(0x00485555)+snd_play(0x0048556f) pair, fused into ONE
        // position-carrying record (the LIFT-NOTIFY offscreen conversion): the hosted sink re-runs that exact pair
        // synchronously at emit.
        c.snd_play_at(proto.sound_explo, tile_col, tile_row);
    }

    // 0x00485574-0x004855a4: small-vs-big dispatch. UNIT_TYPE_A_HELI == 0xf, so this is exactly the
    // boundary sim_unit_housing_count.h's own pair test (per sim_unit_teardown.h's cross-reference
    // note). The asm re-derives units[cur_player][cur_index].unit_proto_id fresh here (and twice more
    // below, for the two mothership-type checks) via roster row/col arithmetic rather than reading
    // through CUR_UNIT -- the IDENTICAL memory (see the comment on `u` above) -- so this reuses
    // `unit_proto_id`/`proto` read once at the top, matching sim_unit_on_destroyed.cpp's and
    // sim_unit_teardown.cpp's identical simplification of their own functions' repeated re-reads.
    const double elapsed = *v.game_clock - *v.tick_budget; // read fresh at 2-3 asm sites (0x005d0198 -
                                                           // 0x00ae3738), all the same value since
                                                           // neither global is written in this
                                                           // function; reused here as one local.

    // reimpl-verify (2026-08-14) found a real, if practically unreachable, divergence: the original's
    // CMP+JG at 0x004855a4 is a SIGNED comparison, but proto.type is committed uint32_t, so the
    // naive `<` here is unsigned -- the two only disagree when proto.type's high bit is set (never
    // true for shipped cfg content, which is why this was a `note`, not a `divergence`), but the
    // signed cast makes the translation match the original's actual instruction rather than merely
    // its observed behaviour on valid data.
    if (static_cast<int32_t>(proto.type) < static_cast<int32_t>(UNIT_TYPE_A_HELI)) {
        // ---- small/normal unit: explosion FX (+ optional debris burst), then remove ---------------
        // 0x004855aa-0x004855dd: explosion FX at the unit's own fine (x, y), owner_or_flag=1.
        c.fx_anim_spawn(static_cast<uint32_t>(fine_x), static_cast<uint32_t>(fine_y),
                        static_cast<uint32_t>(proto.anim_explo), elapsed, 1u);

        // 0x004855e2-0x0048567d: optional debris burst, gated on the debris-row field != 0 (DECLARED
        // NEED -- see the header banner).
        if (proto.debris_anim_row != 0) {
            // 0x004855fe-0x0048561b: two independent rand_below(16)-8 offsets, X-offset draw FIRST
            // then Y-offset draw -- the ORDER is load-bearing for llm_rand_below's shared PRNG stream
            // (translator-brief rule 13: never reorder RNG draws).
            const int32_t scatter_dx = c.rand_below(16) - 8; // 0x004855fe-0x0048560b
            const int32_t scatter_dy = c.rand_below(16) - 8; // 0x0048560e-0x0048561b

            // 0x0048563c-0x00485650: THIRD rand_below draw, range 4 -- selects the column within this
            // Unit's own 4-entry row of death_anim_table: death_anim_table[debris_row*4 + roll], same
            // `row*4 + rand_below(4)` indexing sim_bldg_state_destroyed.cpp documents for Building's
            // `trace` field.
            const int32_t  death_roll = c.rand_below(4);
            const uint32_t anim_id =
                static_cast<uint32_t>(v.death_anim_table[proto.debris_anim_row * 4 + death_roll]);

            // 0x00485660-0x00485678: scatter offset added, then masked by the map's torus-wrap masks
            // (general.bw_mask / general.bh_mask, i.e. v.geom->bw_mask / v.geom->bh_mask).
            const uint32_t burst_x = v.geom->bw_mask & static_cast<uint32_t>(fine_x + scatter_dx);
            const uint32_t burst_y = v.geom->bh_mask & static_cast<uint32_t>(fine_y + scatter_dy);
            c.fx_anim_spawn(burst_x, burst_y, anim_id, elapsed, 0u); // uVar6 = 0
        }

        // 0x0048567d-0x00485690: remove the unit; the small arm's own JMP routes straight to the
        // common tail below, skipping the big arm entirely.
        c.unit_remove_from_map(*v.cur_player, static_cast<uint32_t>(*v.cur_index));
    } else {
        // ---- big/mothership-class unit: on_destroyed (+ SP outcome announce) -----------------------
        // 0x00485695-0x004856a8: render-space Y REPLACES fine_y (fine_x is untouched).
        fine_y = static_cast<int32_t>(
            c.unit_calc_render_fine_y(*v.cur_player, static_cast<int32_t>(*v.cur_index)));

        // 0x004856ab-0x004856de: explosion FX at (fine_x, render_fine_y), owner_or_flag=2 (vs 1 in the
        // small-unit arm above).
        c.fx_anim_spawn(static_cast<uint32_t>(fine_x), static_cast<uint32_t>(fine_y),
                        static_cast<uint32_t>(proto.anim_explo), elapsed, 2u);

        c.unit_on_destroyed(*v.cur_player, static_cast<uint32_t>(*v.cur_index));

        // 0x004856f6-0x0048576d: SP mothership-lost announce. A PLAIN CONJUNCTION -- re-walked
        // branch-by-branch against the raw CMP/JZ/JNZ targets (a player mismatch JNZ's past BOTH type
        // checks straight to the merge point with no write in between; type==A_HELI_MOTHER JZ's
        // straight to the announce, short-circuiting the type==H_HELI_MOTHER check; otherwise falls
        // through to that second check) -- unlike sim_unit_on_destroyed.h's own mother-ship tail, there
        // is no unconditional write folded in ahead of this gate here, so `&&`/`||` is faithful as
        // written (not the comma-operator hazard that sibling function's header documents).
        if (static_cast<int16_t>(*v.cur_player) == *v.player_side &&
            (proto.type == UNIT_TYPE_A_HELI_MOTHER || proto.type == UNIT_TYPE_H_HELI_MOTHER)) {
            c.game_sp_outcome_announce();
        }
    }

    // ---- 0x00485772-0x004857f8: release target refs, ALWAYS checked, common to both arms -----------
    // Independent ifs (both can fire), not else-if -- matches sim_unit_teardown.cpp's identical pair.
    // Mode literals 1/3 match sim_order_dispatch.h's RELEASE_MODE_PRIMARY_CLEAR/_SECONDARY_CLEAR
    // values; not imported by name here for the same ODR-collision reason sim_unit_teardown.h's own
    // DECLARED NEED documents (this file already includes sim_unit_type_predicates.h, which itself
    // includes sim_order_enqueue.h -- pulling in a THIRD header just for two int literals used once
    // each is not worth the redefinition risk the moment a probe TU includes this file alongside
    // sim_order_dispatch.h).
    if (u.target_ref != 0) {
        c.target_release_ref(static_cast<uint32_t>(*v.cur_player), static_cast<int32_t>(*v.cur_index),
                             1u); // primary, RELEASE_MODE_PRIMARY_CLEAR
        u.target_ref   = 0;
        u.target_index = 0;
    }
    if (u.target2_ref != 0) {
        c.target_release_ref(static_cast<uint32_t>(*v.cur_player), static_cast<int32_t>(*v.cur_index),
                             3u); // secondary, RELEASE_MODE_SECONDARY_CLEAR
        u.target2_ref   = 0;
        u.target2_index = 0;
    }

    // 0x004857f8-0x0048580d: housing + door reservations.
    c.unit_housing_count_remove(static_cast<int32_t>(*v.cur_player), unit_proto_id);
    c.storage_release_door_held_by_unit(static_cast<int32_t>(*v.cur_player),
                                        static_cast<int32_t>(*v.cur_index));

    // 0x00485820-0x00485841: path slot, only if one was assigned.
    if (u.path_slot_id != 0xffu) {
        c.path_free_slot(*v.cur_player, static_cast<int32_t>(*v.cur_index));
    }

    // 0x00485841-0x004858ab: per-player/per-planet loss/alive counters + presence-lost. The asm's own
    // INC-then-DEC-then-compare order is reproduced verbatim (both fields live in the SAME
    // profile_at(player) record, so the order has no observable effect here, but nothing is gained by
    // reordering it).
    player_profile &profile = own.profile_at(*v.cur_player);
    profile.units_lost_total[*v.planet_index] += 1;  // 0x00485841-0x0048585f
    profile.units_alive[*v.planet_index] -= 1;       // 0x0048585f-0x0048587d
    if (profile.units_alive[*v.planet_index] == 0) { // 0x0048587d-0x004858ab
        c.player_presence_lost(*v.cur_player, 0u);
    }

    // 0x004858ab-0x004858d6: commit CORPSE_FOW_DECAY(4) via the ORIGINAL setter (this function does
    // not write unit.state itself -- llm_strat_unit_set_state writes only that field, per the struct's
    // own comment), THEN overload move_microstep as the saved sight radius for FoW cleanup (matches
    // the field's own struct comment exactly).
    c.unit_set_state(UNIT_STATE_DIE_EXPLODE_CORPSE_FOW_DECAY);
    u.move_microstep = proto.sight;

    // 0x004858d6-0x0048590b: FoW refresh at the unit's tile x/y; sight = the low byte of move_microstep
    // just written (same value, re-read as a byte -- matches the asm's own MOVZX byte reload of the
    // field rather than reusing the int32 local, kept literal for fidelity though value-identical).
    c.map_fow_UpdateFoWPlus(*v.cur_player, u.x, u.y, static_cast<uint8_t>(u.move_microstep));
    c.game_SetEvent(UNIT_STATE_DIE_EXPLODE_MAP_OBJECTS_REFRESH);
    c.unit_notify_ui(*v.cur_player, static_cast<uint32_t>(*v.cur_index));
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

void unit_state_die_explode() {
    sim_state st = state();
    detail::unit_state_die_explode(st.read, st.own, live_unit_state_die_explode_calls());
}


} // namespace mh::sim
