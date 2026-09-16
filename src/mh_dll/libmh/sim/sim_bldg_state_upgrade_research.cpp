//
// sim/sim_bldg_state_upgrade_research.cpp -- see sim_bldg_state_upgrade_research.h. Translated from
// the DISASSEMBLY (tmp/decomp_sim/llm_strat_bldg_state_upgrading_00472c42.asm,
// _researching_00472f3c.asm), cross-checked against the Ghidra .c drafts -- which agree with the
// assembly for both.
//
#include "sim/sim_bldg_state_upgrade_research.h"

#include "addr/mh_calls.gen.h" // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"       // ai_say / trace_budget -- the shared trace sink, not AI state
#include "state/host_api.h"
#include "state/host_events.h"
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"
#include "crt/crt_select.h" // LIB-CRT: MH_CRT() picks the vendored CRT in the standalone build

namespace mh::sim {

const bldg_state_upgrading_calls &live_bldg_state_upgrading_calls() {
    static const bldg_state_upgrading_calls c = {
        MH_LIBMH_BIND(llm_strat_ai_queue_release_order),
        mh::state::evt::snd_play,
        MH_LIBMH_BIND(llm_strat_bldg_get_coords),
        MH_CRT(w_sprintf__vss),
        mh::state::evt::text_print_u32,
        MH_LIBMH_BIND(llm_strat_bldg_completion_dispatch),
        MH_LIBMH_BIND(llm_strat_refresh_building),
        MH_LIBMH_BIND(llm_strat_bldg_notify_ui),
    };
    return c;
}

const bldg_state_researching_calls &live_bldg_state_researching_calls() {
    static const bldg_state_researching_calls c = {
        mh::state::evt::snd_play,
        MH_LIBMH_BIND(llm_strat_bldg_get_coords),
        MH_CRT(w_sprintf__vss),
        mh::state::evt::text_print_u32,
        MH_LIBMH_BIND(llm_strat_bldg_completion_dispatch),
        MH_LIBMH_BIND(llm_strat_bldg_notify_ui),
    };
    return c;
}

namespace {

// The tile<-fine conversion (`SAR EDX,0x1f / SHL EDX,0x5 / SBB EAX,EDX / SAR EAX,0x5`), used by both
// functions' camera-pan-target write. Value-for-value C's truncating `/ 32` -- see
// sim_order_enqueue.cpp's fine_to_tile() for the verification; re-derived locally per this project's
// per-TU convention (sim_bldg_state_charge.cpp's own identical copy is the precedent).
inline int32_t fine_to_tile(int32_t fine) { return fine / 32; }

// `Building[bid].state_transition_ids[1]`'s LOW 16 BITS -- see the header's declared-need note (the
// field's Ghidra comment lists charge_gate/land_activate as its confirmed readers; this file adds two
// more reading the exact same element/half-width). Redeclared file-local per this project's
// established per-TU convention (sim_bldg_state_charge.cpp's own `charge_next_state` is the
// precedent; not shared cross-TU).
uint16_t next_state_after_transition(const cfg_building &cb) {
    return static_cast<uint16_t>(static_cast<uint32_t>(cb.state_transition_ids[1]) & 0xffffu);
}

// The shared "%s: %s" message format (0x005012ec) -- read by BOTH functions in this TU. Same literal
// address sim_bldg_state_charge.cpp's own TEXT_FMT_NAME_REASON precedent documents (duplicated
// per-TU, not shared cross-TU).
constexpr const wchar_t *TEXT_FMT_NAME_REASON = L"%s: %s";

// BLDG_STATE_PROD_PICK_NEXT: same value FIVE other sim/ TUs already independently redeclare
// file-local (sim_bldg_state_charge.cpp, sim_bldg_add_workers.cpp, sim_bldg_finish_order.cpp,
// sim_bldg_remove_workers.cpp, sim_refresh_building.cpp, sim_order_dispatch_bldg.cpp) -- see
// sim_bldg_state_charge.h's DECLARED NEED note on hoisting these into sim_state.h; this file follows
// the same per-TU-anonymous-namespace precedent.
inline constexpr uint16_t BLDG_STATE_PROD_PICK_NEXT = 0x6c;

// ---- shared voice-line operands (both functions use the SAME race-offset/volume, different base
// sound ids) -- no backing Ghidra enum (rule 17a fallback), same posture as
// sim_bldg_state_charge.cpp's own CONSTRUCTION_VOICE_* trio (file-local, not shared cross-TU) -------
inline constexpr int32_t VOICE_RACE2_OFFSET           = 0x12; // (player_race==2) extra offset
inline constexpr int32_t VOICE_VOLUME                 = 100;
inline constexpr int32_t UPGRADE_VOICE_BASE_SOUND_ID  = 0xf; // upgrading's own base sound id
inline constexpr int32_t RESEARCH_VOICE_BASE_SOUND_ID = 0xa; // researching's own base sound id

// upgrading's "reason" text id (G_TEXT_PTRS[0x79]) / researching's (G_TEXT_PTRS[0x7a]) -- literal
// text-catalog indices, no backing enum, same posture as charge.cpp's TEXT_ID_CONSTRUCTION_COMPLETE.
inline constexpr int32_t TEXT_ID_UPGRADE_COMPLETE  = 0x79;
inline constexpr int32_t TEXT_ID_RESEARCH_COMPLETE = 0x7a;

// DECLARED NEED (see header banner): llm_strat_bldg_completion_dispatch's param_3/param_4 on the
// OTHER-PLAYER branch of either function trace to whatever garbage the CALLER of this void(void)
// function left in EBX/ECX (the Ghidra .c drafts spell them `extraout_EBX`/`unaff_ECX` --
// decompiler placeholders for "origin unresolved", not values this translator invented).
// completion_dispatch is CONFIRMED (sim_bldg_state_charge.h's identical note, re-verified against
// its own disassembly) to never consume param_3/param_4, so the placeholder chosen here is inert
// regardless of its value; 0 is used for readability, matching charge.cpp's own precedent.
inline constexpr uint32_t COMPLETION_DISPATCH_PARAM_UNRESOLVED = 0u;

} // namespace

namespace detail {

void bldg_state_upgrading(const sim_view &v, sim_store &own, const bldg_state_upgrading_calls &c) {
    building      &b   = own.cur_building(); // _G_LLM_STRAT_CUR_BUILDING dereferenced, read+write
    const uint16_t bid = b.building_id;      // read via the cur_building POINTER, reused below

    // ---- cycle_progress += tick_budget * efficiency (0x00472c5a-0x00472c71) -----------------------
    b.cycle_progress = own.tick_budget() * b.efficiency + b.cycle_progress;

    // ---- completion gate (0x00472c74-0x00472c9e): cycle_progress >= the UPGRADE TARGET type's own
    // build_time_2, via Building[bid].upgrade_index -- a DOUBLE INDIRECTION, not this building's own
    // threshold. See the header's NaN-SAFE RESTATEMENT note: this JC gate's naive `<=` is already
    // NaN-safe, no restatement needed.
    const cfg_building &upgrade_target = v.cfg_buildings[v.cfg_buildings[bid].upgrade_index];
    if (upgrade_target.build_time_2 <= b.cycle_progress) {
        // Unconditional, BEFORE the local/other-player split (unlike charge_gate's equivalent call,
        // which runs AFTER the message as part of the shared completion tail).
        c.ai_queue_release_order(*v.cur_player, *v.cur_index, /*mode=*/2);

        // Mutable locals for the REGISTER-REUSE HAZARD (see header): overwritten inside the
        // local-player branch below, forwarded to completion_dispatch either way.
        uint32_t param_3 = COMPLETION_DISPATCH_PARAM_UNRESOLVED;
        uint32_t param_4 = COMPLETION_DISPATCH_PARAM_UNRESOLVED;

        if (*v.cur_player == static_cast<uint16_t>(*v.player_side)) {
            if (*v.sim_active != 0) {
                const int32_t sound_id =
                    (*v.player_race == 2 ? VOICE_RACE2_OFFSET : 0) + UPGRADE_VOICE_BASE_SOUND_ID;
                c.snd_play(sound_id, VOICE_VOLUME);
            }

            // ---- THE REGISTER-REUSE HAZARD: param_3/param_4 are overwritten HERE with the
            // ADDRESSES of the CAM_PAN_TARGET globals, reproduced literally per translator-brief
            // rule 10 (see header for the full derivation).
            param_3 = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&own.cam_pan_target_col()));
            param_4 = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&own.cam_pan_target_row()));

            // ---- camera pan target (0x00472d06-0x00472d54): get_coords writes directly into the two
            // globals, then both are truncated fine->tile in place.
            c.bldg_get_coords(*v.cur_player, static_cast<int32_t>(*v.cur_index), &own.cam_pan_target_col(),
                              &own.cam_pan_target_row());
            own.cam_pan_target_col() = fine_to_tile(own.cam_pan_target_col());
            own.cam_pan_target_row() = fine_to_tile(own.cam_pan_target_row());

            // ---- "%s: %s" message: name = Building[bid].id, reason = G_TEXT_PTRS[0x79] -------------
            c.w_sprintf__vss(own.text_scratch(), TEXT_FMT_NAME_REASON, v.text_ptrs[v.cfg_buildings[bid].id],
                             v.text_ptrs[TEXT_ID_UPGRADE_COMPLETE]);
            c.game_ui_PrintTextMessage(own.text_scratch());
        }

        // ---- completion_dispatch + state transition (0x00472d9b-0x00472ddf) -----------------------
        c.completion_dispatch(*v.cur_player, *v.cur_index, param_3, param_4, *v.game_clock);
        b.state = next_state_after_transition(v.cfg_buildings[bid]); // via cur_building POINTER's bid

        // ---- production-in-progress override (0x00472dda-0x00472e73), ROSTER-derived building_id --
        // a DIFFERENT expression from `bid` above (same "two building_id expressions" hazard
        // sim_bldg_state_destroyed.h documents for its own function).
        const uint16_t roster_bid = building_of(v, *v.cur_player, *v.cur_index).building_id;
        const uint8_t  type       = v.cfg_buildings[roster_bid].type;
        if ((type == BUILDING_TYPE_H_PRODUCTION || type == BUILDING_TYPE_A_PRODUCTION) &&
            v.productions[*v.cur_player * v.caps.productions + b.sub_id].queued_count[0] != 0) {
            b.state = BLDG_STATE_PROD_PICK_NEXT;
        }

        c.refresh_building(*v.cur_player, *v.cur_index);
    }

    // ---- tail: zero the shared tick-budget scratch, notify UI (0x00472e86-0x00472ea8) --------------
    // Runs UNCONDITIONALLY, gate taken or not.
    own.tick_budget() = 0.0;
    c.bldg_notify_ui(*v.cur_player, static_cast<uint32_t>(*v.cur_index));
}

void bldg_state_researching(const sim_view &v, sim_store &own, const bldg_state_researching_calls &c) {
    building &b = own.cur_building(); // _G_LLM_STRAT_CUR_BUILDING dereferenced, read+write

    // ---- cycle_progress += tick_budget * efficiency (0x00472f54-0x00472f6b) -----------------------
    b.cycle_progress = own.tick_budget() * b.efficiency + b.cycle_progress;

    // labs[cur_player][cur_building->sub_id].active_project_id -> the Project this lab is working on.
    // Re-read INSIDE each block below rather than hoisted to a local, matching the asm's own repeated
    // recomputation (three independent occurrences: the completion gate, the overflow gate, and the
    // subtraction) -- see sim_state.h W1 on why re-reading through a const view is not a style choice.
    const auto active_project = [&]() -> const cfg_project & {
        const int32_t proj_id = v.labs[*v.cur_player * v.caps.labs + b.sub_id].active_project_id;
        return v.cfg_projects[proj_id];
    };

    // ---- completion gate (0x00472f6e-0x00472fa8): cycle_progress >= the active project's build_time.
    // See the header's NaN-SAFE RESTATEMENT note: this JC gate's naive `<=` is already NaN-safe.
    if (active_project().build_time <= b.cycle_progress) {
        // Mutable locals for the REGISTER-REUSE HAZARD (see header, SAME shape as upgrading's).
        uint32_t param_3 = COMPLETION_DISPATCH_PARAM_UNRESOLVED;
        uint32_t param_4 = COMPLETION_DISPATCH_PARAM_UNRESOLVED;

        if (*v.cur_player == static_cast<uint16_t>(*v.player_side)) {
            if (*v.sim_active != 0) {
                const int32_t sound_id =
                    (*v.player_race == 2 ? VOICE_RACE2_OFFSET : 0) + RESEARCH_VOICE_BASE_SOUND_ID;
                c.snd_play(sound_id, VOICE_VOLUME);
            }

            param_3 = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&own.cam_pan_target_col()));
            param_4 = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&own.cam_pan_target_row()));

            c.bldg_get_coords(*v.cur_player, static_cast<int32_t>(*v.cur_index), &own.cam_pan_target_col(),
                              &own.cam_pan_target_row());
            own.cam_pan_target_col() = fine_to_tile(own.cam_pan_target_col());
            own.cam_pan_target_row() = fine_to_tile(own.cam_pan_target_row());

            // ---- "%s: %s" message: name = Projects[...].name (the PROJECT's own name, NOT the
            // building's -- unlike every other sibling in this closure), reason = G_TEXT_PTRS[0x7a] --
            c.w_sprintf__vss(own.text_scratch(), TEXT_FMT_NAME_REASON, v.text_ptrs[active_project().name],
                             v.text_ptrs[TEXT_ID_RESEARCH_COMPLETE]);
            c.game_ui_PrintTextMessage(own.text_scratch());
        }

        c.completion_dispatch(*v.cur_player, *v.cur_index, param_3, param_4, *v.game_clock);
        b.state = next_state_after_transition(v.cfg_buildings[b.building_id]); // via cur_building POINTER's bid
        // NO production-override, NO refresh_building call here -- confirmed absent from the asm
        // (see header).
    }

    // ---- the SECOND stage: carry the overflow forward as a new tick_budget (0x004730e4-0x00473177),
    // ALWAYS reached (gate taken or not). See the header's NaN-SAFE RESTATEMENT note: this is a JBE
    // gate, so the "do the subtraction" branch is written as `>`, NOT the naive `<=`-then-zero form.
    if (b.cycle_progress > active_project().build_time) {
        own.tick_budget() = b.cycle_progress - active_project().build_time;
    } else {
        own.tick_budget() = 0.0;
    }

    c.bldg_notify_ui(*v.cur_player, static_cast<uint32_t>(*v.cur_index));
}

} // namespace detail

// ---- the public wrappers ---------------------------------------------------------------------------

void bldg_state_upgrading() {
    sim_state st = state();
    detail::bldg_state_upgrading(st.read, st.own, live_bldg_state_upgrading_calls());
}

void bldg_state_researching() {
    sim_state st = state();
    detail::bldg_state_researching(st.read, st.own, live_bldg_state_researching_calls());
}


} // namespace mh::sim
