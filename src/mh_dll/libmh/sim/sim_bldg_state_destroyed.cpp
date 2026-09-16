//
// sim/sim_bldg_state_destroyed.cpp -- see sim_bldg_state_destroyed.h. Translated from the
// DISASSEMBLY (tmp/decomp/llm_strat_bldg_state_destroyed_00473194.asm), not from the Ghidra .c draft
// (whose get_coords out-param naming is backwards from the real register mapping -- see the header).
//
#include "sim/sim_bldg_state_destroyed.h"

#include <cstring>

#include "addr/mh_calls.gen.h" // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"       // ai_say / trace_budget -- the shared trace sink, not AI state
#include "state/host_api.h"
#include "state/host_events.h"
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"
#include "crt/crt_select.h" // LIB-CRT: MH_CRT() picks the vendored CRT in the standalone build

namespace mh::sim {

const bldg_state_destroyed_calls &live_bldg_state_destroyed_calls() {
    static const bldg_state_destroyed_calls c = {
        MH_LIBMH_BIND(llm_strat_bldg_get_coords),
        MH_LIBMH_BIND(llm_strat_fx_anim_spawn),
        MH_LIBMH_BIND(llm_rand_below),
        MH_LIBMH_BIND(llm_strat_apply_area_damage),
        mh::state::evt::snd_play_at,
        mh::state::evt::snd_play,
        mh::state::evt::text_queue_id,
        MH_LIBMH_BIND(llm_game_sp_outcome_announce),
        MH_CRT(w_sprintf__vss),
        mh::state::evt::text_print_u32,
        MH_LIBMH_BIND(llm_strat_game_check_players_mothership_alive),
        MH_LIBMH_BIND(llm_strat_invasion_chance_roll),
        mh::state::evt::fx_debris_burst,
        MH_LIBMH_BIND(llm_strat_prod_unbind_planet),
        MH_LIBMH_BIND(llm_strat_prod_shuttle_slot_release),
        MH_LIBMH_BIND(llm_strat_bldg_unmap_footprint), // sibling unit, called as an ORIGINAL callee
        MH_LIBMH_BIND(game_SetEvent),
        MH_LIBMH_BIND(llm_strat_unit_purge_unregistered),
        MH_LIBMH_BIND(llm_strat_player_presence_lost),
        MH_LIBMH_BIND(llm_strat_sight_add_circle),
        MH_LIBMH_BIND(llm_strat_bldg_notify_ui),
    };
    return c;
}

namespace {

// The tile<-fine conversion (`SAR EDX,0x1f / SHL EDX,0x5 / SBB EAX,EDX / SAR EAX,0x5`), 5 occurrences
// in this function. Value-for-value C's truncating `/ 32` -- see sim_order_enqueue.cpp's fine_to_tile()
// for the verification; re-derived locally per this project's per-TU convention.
inline int32_t fine_to_tile(int32_t fine) { return fine / 32; }

// mh_map_object_building::anim / mh_cfg_final_struct_Building::anim are both `cfg_t_frame_index[12]`
// flattened to uint8_t[48] (addr/mh_structs.gen.h) -- same flattening sim_bldg_reset_construction_anim
// .cpp's frame_at()/set_frame_at() already document and work around; reused here in the same shape
// (file-local, not shared cross-TU, per that file's own "write only your own new files" reasoning).
int32_t frame_at(const uint8_t (&anim)[48], int32_t slot) {
    int32_t value;
    std::memcpy(&value, &anim[slot * 4], sizeof(value));
    return value;
}
void set_frame_at(uint8_t (&anim)[48], int32_t slot, int32_t value) {
    std::memcpy(&anim[slot * 4], &value, sizeof(value));
}

// The debris-intensity x87 chain (0x004734d3-0x004734e7, ex-debris_intensity() with its resolved
// DAT_00501312 = 2000.0 divisor) lives in seams/host_event_sink.cpp now -- the fx_debris_burst
// record moved the whole camera-dependent derivation host-side (the LIFT-NOTIFY offscreen conversion).

inline constexpr uint32_t BLDG_STATE_DESTROYED_MAP_OBJECTS_REFRESH = 14u; // game::e::event member 14

// The format at 0x005011be (a wide "%s: %s"); supplied as our own literal rather than read out of the
// image, matching sim_order_dispatch_bldg.cpp's identical TEXT_FMT_NAME_REASON precedent (duplicated
// locally per the same reasoning, not shared cross-TU).
constexpr const wchar_t *TEXT_FMT_NAME_REASON = L"%s: %s";

} // namespace

namespace detail {

void bldg_state_destroyed(const sim_view &v, sim_store &own, const bldg_state_destroyed_calls &c) {
    building           &b   = own.cur_building(); // _G_LLM_STRAT_CUR_BUILDING dereferenced, read+write
    const uint16_t      bid = b.building_id;      // read ONCE, reused for every cur_building-pointer cfg lookup
    const cfg_building &cb  = v.cfg_buildings[bid];

    // ---- coords (0x004731ac-0x004731c0): fine_x = [EBP-0x1c] (out_x/EBX), fine_y = [EBP-0x18]
    // (out_y/ECX) -- see the header's note on why these are NOT the .c draft's local_20/local_1c order.
    int32_t fine_x = 0, fine_y = 0;
    c.bldg_get_coords(*v.cur_player, static_cast<int32_t>(*v.cur_index), &fine_x, &fine_y);

    // ---- first fx_anim_spawn (0x004731c5-0x004731f8): Building[bid].anim[7], elapsed = game_clock -
    // tick_budget, flag=1. `elapsed` has no writer between here and its second read below (nothing
    // this function calls up to that point touches game_clock/tick_budget), so it is computed once and
    // reused rather than re-evaluated -- behaviourally identical to the asm's own redundant re-fetch.
    const double elapsed = *v.game_clock - *v.tick_budget;
    c.fx_anim_spawn(static_cast<uint32_t>(fine_x), static_cast<uint32_t>(fine_y),
                    static_cast<uint32_t>(frame_at(cb.anim, 7)), elapsed, FX_ANIM_SPAWN_PARAM5_PRIMARY);

    // ---- second fx_anim_spawn (0x004731fd-0x00473245): death_anim_table[trace*4 + rand_below(4)],
    // same elapsed, flag=0. `trace<<4` is computed BEFORE the rand_below call in the asm; reproduced in
    // that order even though it has no observable effect (pure read, no side effect).
    const int32_t death_row  = cb.trace;
    const int32_t death_roll = c.rand_below(4);
    c.fx_anim_spawn(static_cast<uint32_t>(fine_x), static_cast<uint32_t>(fine_y),
                    static_cast<uint32_t>(v.death_anim_table[death_row * 4 + death_roll]), elapsed,
                    FX_ANIM_SPAWN_PARAM5_SECONDARY);

    // ---- area damage (0x0047324a-0x00473285) -------------------------------------------------------
    c.apply_area_damage(fine_to_tile(fine_x), fine_to_tile(fine_y), DEATH_BLAST_TARGET_KIND,
                        DEATH_BLAST_DAMAGE, DEATH_BLAST_RING_COUNT, /*owner_filter_zeroed=*/0u,
                        /*killer_info=*/0u, /*killer_unit_index=*/0);

    // ---- explosion sound, SIM_ACTIVE-gated (0x0047328a-0x004732da) ---------------------------------
    // The original offscreen_snd_volume(0x0047328a)+snd_play(0x004732c5) pair, fused into ONE
    // position-carrying record (the LIFT-NOTIFY offscreen conversion): the hosted sink re-runs that exact pair
    // synchronously at emit; a fork host attenuates/pans from ITS camera.
    if (*v.sim_active != 0) {
        c.snd_play_at(cb.sound_explo, fine_to_tile(fine_x), fine_to_tile(fine_y));
    }

    // ---- local-player vs. other-player split (0x004732da-0x0047340c compare) -----------------------
    const bool is_local_player = *v.cur_player == static_cast<uint16_t>(*v.player_side);

    if (is_local_player) {
        // Race-dependent voice line, SIM_ACTIVE-gated (0x004732ed-0x00473324).
        if (*v.sim_active != 0) {
            const int32_t sound_id =
                (*v.player_race == 2 ? VOICE_LINE_RACE2_OFFSET : 0) + VOICE_LINE_BASE_SOUND_ID;
            c.snd_play(sound_id, VOICE_LINE_VOLUME);
        }

        // Mothership-type check (0x00473324-0x00473386): re-derives building_id from the ROSTER
        // (buildings[cur_player][cur_index].building_id), NOT via the cur_building pointer -- see the
        // header's note on why this is kept as a literally different expression from `bid` above.
        const uint16_t roster_bid = building_of(v, *v.cur_player, *v.cur_index).building_id;
        const uint8_t  type       = v.cfg_buildings[roster_bid].type;

        if (type == BUILDING_TYPE_H_MOTHER || type == BUILDING_TYPE_A_MOTHER) {
            c.ui_print_queue_text_id(TEXT_ID_MOTHERSHIP_DESTROYED);
            c.game_sp_outcome_announce();
        } else {
            c.w_sprintf__vss(own.text_scratch(), TEXT_FMT_NAME_REASON, v.text_ptrs[cb.id],
                             v.text_ptrs[TEXT_ID_BUILDING_DESTROYED_NAME]);
            c.game_ui_PrintTextMessage(own.text_scratch());
        }

        // Camera pan target (0x004733db-0x00473407) -- unconditional in this arm, regardless of which
        // outcome branch above ran.
        own.cam_pan_target_col() = fine_to_tile(fine_x);
        own.cam_pan_target_row() = fine_to_tile(fine_y);
    } else {
        // Same roster re-derivation as the local-player arm, independently recomputed (0x0047340c-
        // 0x0047343c) -- the two arms never share this read since only one arm executes.
        const uint16_t roster_bid = building_of(v, *v.cur_player, *v.cur_index).building_id;
        const uint8_t  type       = v.cfg_buildings[roster_bid].type;

        if ((type == BUILDING_TYPE_H_MOTHER || type == BUILDING_TYPE_A_MOTHER) &&
            c.game_check_players_mothership_alive() != 0) {
            own.planet_mother_lost_time_at(*v.planet_index) = *v.game_clock;
            c.invasion_chance_roll(0);
        }
    }

    // ---- debris burst (0x00473496-0x004734ed) ------------------------------------------------------
    // The original chain -- offscreen_fx_scale(0x00473496) -> the x87 intensity computation
    // (0x004734d3-0x004734e7, the former debris_intensity() here) -> spawn_debris_burst(0x004734ed)
    // -- fused into ONE record (the LIFT-NOTIFY offscreen conversion). Every step is camera-dependent presentation
    // (screen-shake keyframes from the cosmetic PRNG slot), so the whole derivation is the host's:
    // the hosted sink reproduces it verbatim at emit (seams/host_event_sink.cpp carries the relocated
    // x87 helper); a fork host shakes its own camera however it likes.
    c.fx_debris_burst(fine_to_tile(fine_x), fine_to_tile(fine_y), static_cast<int32_t>(cb.energy));

    // ---- shuttle/production teardown (0x004734f2-0x00473556) --------------------------------------
    if (b.shuttle_slot != 0) {
        if (static_cast<uint32_t>(b.shuttle_slot) ==
            static_cast<uint32_t>(v.profiles[*v.cur_player].prod_queue_slot[*v.planet_index])) {
            c.prod_unbind_planet(*v.cur_player, *v.planet_index);
        }
        c.prod_shuttle_slot_release(*v.cur_player, b.shuttle_slot);
    }

    // ---- unmap footprint, then the FIRST cycle_progress zero (0x00473556-0x00473575) ---------------
    // llm_strat_bldg_unmap_footprint is a SIBLING unit in this same batch -- called as an ORIGINAL
    // callee (mh::call::), not through its own detail:: translation, per the standing rule.
    c.bldg_unmap_footprint(*v.cur_player, static_cast<int32_t>(*v.cur_index));
    b.cycle_progress = 0.0; // double field, TWO dword-zero stores in the asm -- see the header note

    // ---- per-planet alive/lost counters + presence-lost (0x00473575-0x004735fa) --------------------
    own.profile_at(*v.cur_player).buildings_lost_total[*v.planet_index] += 1;
    own.profile_at(*v.cur_player).buildings_alive[*v.planet_index] -= 1;
    c.set_event(BLDG_STATE_DESTROYED_MAP_OBJECTS_REFRESH);
    if (own.profile_at(*v.cur_player).buildings_alive[*v.planet_index] == 0) {
        c.unit_purge_unregistered(*v.cur_player);
        c.player_presence_lost(*v.cur_player, 0);
    }

    // ---- RUBBLE transition + SECOND cycle_progress zero + sight circle (0x004735fa-0x00473698) -----
    b.cycle_progress = 0.0; // second, independent zero -- real in the asm, not a decompiler duplicate
    b.state          = BLDG_STATE_RUBBLE_SIGHT_DECAY;

    // anim[0] = cfg_buildings[bid].sight -- a byte-width cfg field zero-extended into the 4-byte
    // frame_index slot (0x00473633: `MOV dword ptr [cur_building+0x93],EDX` where EDX came from a
    // MOVZX byte load). Reading it back for sight_add_circle's 5th arg (0x0047363e's PUSH) is the SAME
    // value we just stored -- nothing writes anim[0] in between -- so `sight` is reused directly rather
    // than re-reading through frame_at().
    const uint8_t sight = cb.sight;
    set_frame_at(b.anim, 0, static_cast<int32_t>(sight));
    c.sight_add_circle(*v.cur_player, b.x, b.y, bid, sight);

    // ---- tail: zero the shared tick-budget scratch, notify UI (0x00473671-0x00473698) ---------------
    own.tick_budget() = 0.0; // _G_LLM_STRAT_TICK_BUDGET, shared with the unit-tick driver
    c.bldg_notify_ui(*v.cur_player, static_cast<uint32_t>(*v.cur_index));
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

void bldg_state_destroyed() {
    sim_state st = state();
    detail::bldg_state_destroyed(st.read, st.own, live_bldg_state_destroyed_calls());
}


} // namespace mh::sim
