//
// sim/sim_bldg_state_charge.cpp -- see sim_bldg_state_charge.h. Translated from the DISASSEMBLY
// (tmp/decomp_sim/llm_strat_bldg_state_construction_004724af.asm, _charge_gate_00472674.asm,
// _charge_step_00472979.asm), cross-checked against the Ghidra .c drafts (tmp/decomp_sim/*.c) --
// which agree with the assembly for all three, including the two surprising details the header
// documents (construction's register-reuse hazard, charge_gate's _unnamed_0x26d[1] read).
//
#include "sim/sim_bldg_state_charge.h"

#include "addr/mh_calls.gen.h" // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"       // ai_say / trace_budget -- the shared trace sink, not AI state
#include "state/host_api.h"
#include "state/host_events.h"
#include "addr/mh_rebind.gen.h"
#include "state/rebind_targets.gen.h"
#include "crt/crt_select.h"        // LIB-CRT: MH_CRT() picks the vendored CRT in the standalone build
#include "state/promoted_select.h" // LIB-REF-SPLIT: MH_PROMOTED

namespace mh::sim {

const bldg_state_construction_calls &live_bldg_state_construction_calls() {
    static const bldg_state_construction_calls c = {
        MH_LIBMH_BIND(llm_strat_bldg_get_coords),
        mh::state::evt::snd_play,
        MH_CRT(w_sprintf__vss),
        mh::state::evt::text_print_u32,
        MH_LIBMH_BIND(llm_strat_bldg_construction_complete),
        MH_PROMOTED_ROW(llm_strat_ai_notify_bldg_constructed),
        MH_LIBMH_BIND(llm_strat_refresh_building),
        MH_LIBMH_BIND(llm_strat_bldg_notify_ui),
    };
    return c;
}

const bldg_state_charge_gate_calls &live_bldg_state_charge_gate_calls() {
    static const bldg_state_charge_gate_calls c = {
        MH_LIBMH_BIND(llm_strat_bldg_pay_cycle_inputs),
        MH_PROMOTED_ROW(llm_strat_ai_queue_release_order),
        MH_CRT(w_sprintf__vss),
        mh::state::evt::text_print_u32,
        MH_LIBMH_BIND(llm_strat_bldg_completion_dispatch),
        MH_LIBMH_BIND(llm_strat_bldg_notify_ui),
        MH_LIBMH_BIND(llm_strat_refresh_building),
    };
    return c;
}

const bldg_state_charge_step_calls &live_bldg_state_charge_step_calls() {
    static const bldg_state_charge_step_calls c = {
        MH_LIBMH_BIND(llm_strat_bldg_update_charge_pips),
        MH_LIBMH_BIND(llm_strat_bldg_notify_ui),
    };
    return c;
}

namespace {

// The tile<-fine conversion (`SAR EDX,0x1f / SHL EDX,0x5 / SBB EAX,EDX / SAR EAX,0x5`), used by
// construction's camera-pan-target write. Value-for-value C's truncating `/ 32` -- see
// sim_order_enqueue.cpp's fine_to_tile() for the verification; re-derived locally per this project's
// per-TU convention (sim_bldg_state_destroyed.cpp's own identical copy is the precedent).
inline int32_t fine_to_tile(int32_t fine) { return fine / 32; }

// RESOLVED (conductor, 2026-08-22): cfg_final_struct_Building's undifferentiated int32_t[4] blob at
// offset 0x26d is now named `state_transition_ids` in Ghidra (addr/mh_structs.gen.h) -- element [1]'s
// LOW 16 BITS are a REAL "state to transition to after a charge cycle completes" cfg value, read at
// BOTH charge_gate call sites (0x004727aa/0x0047289f: `MOV AX, word ptr [... + 0xd9eef1]`, i.e.
// cfg_buildings_base(0xd9ec80) + bid*0x842 + 0x271, exactly `state_transition_ids[1]`'s low word).
// Indices [0]/[2]/[3] and the upper 16 bits of [1] still have no confirmed reader.
uint16_t charge_next_state(const cfg_building &cb) {
    return static_cast<uint16_t>(static_cast<uint32_t>(cb.state_transition_ids[1]) & 0xffffu);
}

// The shared "%s: %s" message format (0x005012ec) -- read by BOTH construction and charge_gate in
// this same TU, so declared once here rather than twice. Same literal address
// sim_order_dispatch_bldg.cpp's / sim_bldg_state_destroyed.cpp's own TEXT_FMT_NAME_REASON precedent
// documents (duplicated per-TU, not shared cross-TU, per that file's own reasoning).
constexpr const wchar_t *TEXT_FMT_NAME_REASON = L"%s: %s";

// ---- construction's own literal operands, no backing Ghidra enum (rule 17a fallback) --------------
// File-local (anonymous namespace) rather than header/mh::sim scope, per the header's own note on why
// that placement is safer here than sim_bldg_state_destroyed.h's choice for its own (differently-
// valued) VOICE_LINE_* trio.
inline constexpr int32_t CONSTRUCTION_VOICE_RACE2_OFFSET  = 0x12; // (player_race==2) extra offset
inline constexpr int32_t CONSTRUCTION_VOICE_BASE_SOUND_ID = 4;    // base sound id, race offset added
inline constexpr int32_t CONSTRUCTION_VOICE_VOLUME        = 100;
inline constexpr int32_t TEXT_ID_CONSTRUCTION_COMPLETE    = 0x74; // G_TEXT_PTRS[0x74], the "reason" half
// llm_strat_ai_notify_bldg_constructed's stack param_6 -- always the literal 1 at this call site (no
// backing enum found; generic name already committed in addr/mh_calls.gen.h for this parameter).
inline constexpr uint32_t AI_NOTIFY_BLDG_CONSTRUCTED_PARAM6 = 1u;

// ---- charge_gate's own literal operands ------------------------------------------------------------
// BLDG_STATE_CHARGE_GATE/_CHARGE_STEP/_PROD_PICK_NEXT: same values FOUR other sim/ TUs already
// independently redeclare file-local (sim_bldg_add_workers.cpp, sim_bldg_finish_order.cpp,
// sim_bldg_remove_workers.cpp, sim_refresh_building.cpp for CHARGE_STEP; sim_order_dispatch_bldg.cpp
// for all three) -- see the header's DECLARED NEED note on hoisting these into sim_state.h.
inline constexpr uint16_t BLDG_STATE_CHARGE_GATE    = 0x69;
inline constexpr uint16_t BLDG_STATE_CHARGE_STEP    = 0x6a;
inline constexpr uint16_t BLDG_STATE_PROD_PICK_NEXT = 0x6c;

// DECLARED NEED (see the header banner): llm_strat_bldg_completion_dispatch's param_3/param_4 at BOTH
// charge_gate call sites trace to registers (EBX/ECX) whose true runtime value this function's own
// body cannot establish -- the Ghidra .c draft itself spells them `extraout_EBX`/`extraout_EBX_00`/
// `unaff_ECX` (decompiler placeholder tokens for "origin unresolved"), not values this translator
// invented. Documented placeholders, not plausible-looking guesses; see uncertainties[].
inline constexpr uint32_t COMPLETION_DISPATCH_PARAM3_UNRESOLVED = 0u;
inline constexpr uint32_t COMPLETION_DISPATCH_PARAM4_UNRESOLVED = 0u;

} // namespace

namespace detail {

void bldg_state_construction(const sim_view &v, sim_store &own, const bldg_state_construction_calls &c,
                             uint32_t param_1, uint32_t param_2, uint32_t param_3, uint32_t param_4) {
    // param_1(EAX)/param_2(EDX) are DEAD: nothing in this function's body reads them -- it re-fetches
    // _G_LLM_STRAT_CUR_PLAYER/_CUR_INDEX from the globals throughout instead. Kept as real parameters
    // (not dropped) because the committed prototype (addr/mh_export.gen.h's
    // sig_llm_strat_bldg_state_construction) is what the shadow-replace hook binds against.
    (void)param_1;
    (void)param_2;

    building           &b   = own.cur_building(); // _G_LLM_STRAT_CUR_BUILDING dereferenced, read+write
    const uint16_t      bid = b.building_id;      // read via the cur_building POINTER, reused throughout
    const cfg_building &cb  = v.cfg_buildings[bid];

    // ---- cycle_progress += tick_budget * efficiency (0x004724c7-0x004724de) -----------------------
    b.cycle_progress = own.tick_budget() * b.efficiency + b.cycle_progress;

    // ---- completion gate (0x004724e1-0x00472501): cycle_progress >= cfg build_time_2 --------------
    if (cb.build_time_2 <= b.cycle_progress) {
        if (*v.cur_player == static_cast<uint16_t>(*v.player_side)) {
            // Race-dependent voice line, SIM_ACTIVE-gated (0x0047251a-0x0047254c).
            if (*v.sim_active != 0) {
                const int32_t sound_id =
                    (*v.player_race == 2 ? CONSTRUCTION_VOICE_RACE2_OFFSET : 0) + CONSTRUCTION_VOICE_BASE_SOUND_ID;
                c.snd_play(sound_id, CONSTRUCTION_VOICE_VOLUME);
            }

            // ---- THE REGISTER-REUSE HAZARD (see header): param_3/param_4 are overwritten HERE with
            // the ADDRESSES of the CAM_PAN_TARGET globals, and never restored before the unconditional
            // construction_complete call below -- reproduced literally, not "fixed".
            param_3 = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&own.cam_pan_target_col()));
            param_4 = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&own.cam_pan_target_row()));

            // ---- camera pan target (0x00472551-0x0047259f): get_coords writes directly into the two
            // globals, then both are truncated fine->tile in place.
            c.bldg_get_coords(*v.cur_player, static_cast<int32_t>(*v.cur_index), &own.cam_pan_target_col(),
                              &own.cam_pan_target_row());
            own.cam_pan_target_col() = fine_to_tile(own.cam_pan_target_col());
            own.cam_pan_target_row() = fine_to_tile(own.cam_pan_target_row());

            // ---- "%s: %s" message: name = Building[bid].id, reason = G_TEXT_PTRS[0x74] -------------
            c.w_sprintf__vss(own.text_scratch(), TEXT_FMT_NAME_REASON, v.text_ptrs[cb.id],
                             v.text_ptrs[TEXT_ID_CONSTRUCTION_COMPLETE]);
            c.game_ui_PrintTextMessage(own.text_scratch());
        }

        // ---- unconditional completion chain (0x004725e6-0x0047263e) -------------------------------
        c.bldg_construction_complete(*v.cur_player, *v.cur_index, param_3, param_4);
        c.ai_notify_bldg_constructed(*v.cur_player, b.x, *v.cur_index, b.building_id, b.y,
                                     AI_NOTIFY_BLDG_CONSTRUCTED_PARAM6);
        c.refresh_building(*v.cur_player, *v.cur_index);
    }

    // ---- tail: zero the shared tick-budget scratch, notify UI (0x00472643-0x0047266a) --------------
    // Runs UNCONDITIONALLY, gate taken or not (matches the asm's single shared fall-through target).
    own.tick_budget() = 0.0;
    c.bldg_notify_ui(*v.cur_player, static_cast<uint32_t>(*v.cur_index));
}

namespace {

// The shared "complete this charge cycle" tail -- BYTE-IDENTICAL in the assembly at both call sites
// (0x00472859-0x00472949 duplicates 0x00472764-0x00472854 exactly), factored into one helper here
// rather than transcribed twice (both sites are within this SAME original function, so this is not a
// new cross-TU abstraction -- see the header note).
void charge_gate_complete_cycle(const sim_view &v, sim_store &own, const bldg_state_charge_gate_calls &c,
                                uint16_t cur_player, uint16_t cur_index) {
    c.ai_queue_release_order(cur_player, cur_index, /*mode=*/1);

    // DECLARED NEED (see header + the file-scope constants above): param_3/param_4 here are
    // Ghidra's own extraout_EBX/unaff_ECX -- documented placeholders, not resolved values.
    c.completion_dispatch(cur_player, cur_index, COMPLETION_DISPATCH_PARAM3_UNRESOLVED,
                          COMPLETION_DISPATCH_PARAM4_UNRESOLVED, *v.game_clock);

    building &b = own.cur_building();
    b.state     = charge_next_state(v.cfg_buildings[b.building_id]); // via cur_building POINTER's bid

    // ROSTER-derived building_id -- a DIFFERENT expression from `b.building_id` above (same "two
    // building_id expressions" hazard sim_bldg_state_destroyed.h documents for its own function).
    const uint16_t roster_bid = building_of(v, cur_player, cur_index).building_id;
    const uint8_t  type       = v.cfg_buildings[roster_bid].type;
    if ((type == BUILDING_TYPE_H_PRODUCTION || type == BUILDING_TYPE_A_PRODUCTION) &&
        v.productions[cur_player * v.caps.productions + b.sub_id].queued_count[0] != 0) {
        b.state = BLDG_STATE_PROD_PICK_NEXT;
    }
}

} // namespace

void bldg_state_charge_gate(const sim_view &v, sim_store &own, const bldg_state_charge_gate_calls &c) {
    building           &b   = own.cur_building(); // _G_LLM_STRAT_CUR_BUILDING dereferenced, read+write
    const uint16_t      bid = b.building_id;
    const cfg_building &cb  = v.cfg_buildings[bid];

    // ---- gate (0x0047268c-0x004726ac): energy >= cfg cap ------------------------------------------
    if (b.energy >= cb.energy) {
        // Already at/above the charge cap: run the shared completion tail directly (0x00472859).
        charge_gate_complete_cycle(v, own, c, *v.cur_player, *v.cur_index);
    } else {
        const int32_t paid = c.bldg_pay_cycle_inputs(*v.cur_player, *v.cur_index);
        if (paid == 0) {
            // Payment failed (0x004726ce-0x004726ee): fall back to CHARGE_STEP, release the order,
            // skip the completion tail entirely.
            b.state = BLDG_STATE_CHARGE_STEP;
            c.ai_queue_release_order(*v.cur_player, *v.cur_index, /*mode=*/0);
        } else {
            // Payment succeeded: LOCAL-player-only message, `paid` reused as BOTH the success flag
            // AND (unusually, but confirmed by both the asm and the draft) the message's "reason"
            // text id.
            if (*v.cur_player == static_cast<uint16_t>(*v.player_side)) {
                const uint16_t roster_bid = building_of(v, *v.cur_player, *v.cur_index).building_id;
                c.w_sprintf__vss(own.text_scratch(), TEXT_FMT_NAME_REASON,
                                 v.text_ptrs[v.cfg_buildings[roster_bid].id], v.text_ptrs[paid]);
                c.game_ui_PrintTextMessage(own.text_scratch());
            }
            charge_gate_complete_cycle(v, own, c, *v.cur_player, *v.cur_index);
        }
    }

    // ---- tail: both branches converge here ----------------------------------------------------------
    c.bldg_notify_ui(*v.cur_player, static_cast<uint32_t>(*v.cur_index));
    c.refresh_building(*v.cur_player, *v.cur_index);
}

void bldg_state_charge_step(const sim_view &v, sim_store &own, const bldg_state_charge_step_calls &c) {
    building           &b  = own.cur_building(); // _G_LLM_STRAT_CUR_BUILDING dereferenced, read+write
    const cfg_building &cb = v.cfg_buildings[b.building_id];

    // ---- efficiency<=0 short-circuit (0x00472996-0x004729d7+JMP) -----------------------------------
    if (b.efficiency <= 0.0) {
        own.tick_budget() = 0.0;
        return;
    }

    // ---- step = (build_time_d / energy_d) / efficiency (0x004729a0-0x004729d2) --------------------
    const double step = (cb.build_time_d / cb.energy_d) / b.efficiency;

    // ---- not-enough-budget-yet / degenerate-step short-circuit (0x004729f0-0x00472b12) ------------
    // FIXED (reimpl-verify, 2026-08-22): the original's two FCOMP/FNSTSW/SAHF/JC pairs treat an
    // UNORDERED comparison (either operand NaN) as "jump taken", i.e. as if the left operand were
    // less than the right -- x87's standard JC-after-unordered-compare quirk (ghidra-retro-re skill).
    // A naive `<`/`<=` translation evaluates false for NaN under IEEE 754, which would silently swap
    // this short-circuit for the increment branch. Restated as negated `>=`/`>` so NaN forces this
    // branch exactly as the hardware does, while remaining identical to the naive form for every
    // ordered (non-NaN) input. (step can only be NaN here if `cb.energy_d` and `cb.build_time_d` are
    // BOTH exactly 0.0 in the shipped cfg data -- not confirmed reachable, but Law 2 preserves the
    // original's behaviour regardless of whether real data reaches it.)
    if (!(own.tick_budget() >= step) || !(step > 0.0)) {
        b.last_tick_time -= own.tick_budget();
        own.tick_budget() = 0.0;
        return;
    }

    // ---- do the increment (0x00472a0d-0x00472af5) --------------------------------------------------
    b.cycle_progress += step;
    own.tick_budget() -= step;
    b.energy += 1.0;

    // TWO INDEPENDENT (not else-if) threshold overrides -- both can fire on the same call, matching
    // the original's two separate, unconditional CMP/state-store pairs.
    if (cb.energy <= b.energy) {
        b.energy = cb.energy;
        b.state  = BLDG_STATE_CHARGE_GATE;
    }
    if (cb.build_time_d <= b.cycle_progress) {
        b.cycle_progress -= cb.build_time_d;
        b.state = BLDG_STATE_CHARGE_GATE;
    }

    c.bldg_update_charge_pips(*v.cur_player, static_cast<uint32_t>(*v.cur_index));
    c.bldg_notify_ui(*v.cur_player, static_cast<uint32_t>(*v.cur_index));
}

} // namespace detail

// ---- the public wrappers ---------------------------------------------------------------------------

void bldg_state_construction(uint32_t param_1, uint32_t param_2, uint32_t param_3, uint32_t param_4) {
    sim_state st = state();
    detail::bldg_state_construction(st.read, st.own, live_bldg_state_construction_calls(), param_1, param_2,
                                    param_3, param_4);
}

void bldg_state_charge_gate() {
    sim_state st = state();
    detail::bldg_state_charge_gate(st.read, st.own, live_bldg_state_charge_gate_calls());
}

void bldg_state_charge_step() {
    sim_state st = state();
    detail::bldg_state_charge_step(st.read, st.own, live_bldg_state_charge_step_calls());
}


} // namespace mh::sim
