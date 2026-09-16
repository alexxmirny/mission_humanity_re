//
// sim/sim_bldg_state_prod.cpp -- see sim_bldg_state_prod.h. Translated from the DISASSEMBLY
// (tmp/decomp_sim/llm_strat_bldg_state_prod_pick_next_00473a20.asm, _prod_working_00473ca8.asm,
// _prod_blocked_notify_00474139.asm, _prod_retry_wait_0047430b.asm), cross-checked against the
// Ghidra .c drafts -- which agree for all four except prod_pick_next's loop SHAPE (see the header's
// derivation note for why the asm's shape, not the draft's, is what is transcribed here).
//
#include "sim/sim_bldg_state_prod.h"

#include "addr/mh_calls.gen.h" // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"       // ai_say / trace_budget -- the shared trace sink, not AI state
#include "state/host_api.h"
#include "state/host_events.h"
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"
#include "crt/crt_select.h" // LIB-CRT: MH_CRT() picks the vendored CRT in the standalone build

namespace mh::sim {

const bldg_state_prod_pick_next_calls &live_bldg_state_prod_pick_next_calls() {
    static const bldg_state_prod_pick_next_calls c = {
        MH_LIBMH_BIND(llm_strat_prod_try_start_unit),
        MH_LIBMH_BIND(llm_strat_ai_notify_unit_lifecycle),
    };
    return c;
}

const bldg_state_prod_working_calls &live_bldg_state_prod_working_calls() {
    static const bldg_state_prod_working_calls c = {
        MH_LIBMH_BIND(llm_strat_bldg_completion_dispatch),
        mh::state::evt::snd_play,
        MH_LIBMH_BIND(llm_strat_bldg_get_coords),
        MH_CRT(w_sprintf__vss),
        mh::state::evt::text_print_u32,
        MH_LIBMH_BIND(game_SetEvent),
        MH_LIBMH_BIND(llm_strat_bldg_notify_ui),
    };
    return c;
}

const bldg_state_prod_blocked_notify_calls &live_bldg_state_prod_blocked_notify_calls() {
    static const bldg_state_prod_blocked_notify_calls c = {
        MH_LIBMH_BIND(llm_strat_reason_to_housing_bldg),
        MH_CRT(w_sprintf__vss),
        MH_CRT(w_sprintf__vsss),
        mh::state::evt::text_print_u32,
    };
    return c;
}

namespace {

// The tile<-fine conversion (`SAR EDX,0x1f / SHL EDX,0x5 / SBB EAX,EDX / SAR EAX,0x5`), used by
// prod_working's camera-pan-target write. Value-for-value C's truncating `/ 32` -- see
// sim_order_enqueue.cpp's fine_to_tile() for the verification; re-derived locally per this project's
// per-TU convention (sim_bldg_state_charge.cpp's own identical copy is the precedent).
inline int32_t fine_to_tile(int32_t fine) { return fine / 32; }

// The shared "%s: %s" message format (0x005012ec) -- read by BOTH prod_working and
// prod_blocked_notify's two-string branches in this same TU. Same literal address
// sim_bldg_state_charge.cpp's own TEXT_FMT_NAME_REASON precedent documents (duplicated per-TU, not
// shared cross-TU, per that file's own reasoning).
constexpr const wchar_t *TEXT_FMT_NAME_REASON = L"%s: %s";
// The "%s: %s (%s)" three-string format (0x0050133a) -- prod_blocked_notify's housing-shortage
// branch only.
constexpr const wchar_t *TEXT_FMT_NAME_REASON_HOUSING = L"%s: %s (%s)";

// ---- llm_strat_bldg_state building-state values (see header's DECLARED NEED note: no real C++ enum
// exists yet for llm_strat_bldg_state, so these are redeclared file-local here per the established
// per-TU precedent sim_bldg_state_charge.cpp/sim_bldg_add_workers.cpp/etc. already follow). ----------
inline constexpr uint16_t BLDG_STATE_IDLE_ACTIVATE       = 0x1;
inline constexpr uint16_t BLDG_STATE_PROD_PICK_NEXT      = 0x6c;
inline constexpr uint16_t BLDG_STATE_PROD_WORKING        = 0x6d;
inline constexpr uint16_t BLDG_STATE_PROD_BLOCKED_NOTIFY = 0x6e;
inline constexpr uint16_t BLDG_STATE_PROD_RETRY_WAIT     = 0x70;

// prod_blocked_notify's fixed "no reason recorded yet" text id (online_state==0 branch) and the
// housing-shortage-band bounds online_state is tested against.
inline constexpr uint16_t PROD_BLOCKED_REASON_TEXT_NONE = 0x17;
inline constexpr uint16_t PROD_BLOCKED_HOUSING_BAND_LO  = 0xf;
inline constexpr uint16_t PROD_BLOCKED_HOUSING_BAND_HI  = 0x12;

// prod_working's message reason text ids, keyed on `Unit[active_unit_type].soldier_count < 1`.
inline constexpr uint16_t PROD_WORKING_TEXT_UNIT_READY     = 0x76;
inline constexpr uint16_t PROD_WORKING_TEXT_SOLDIERS_READY = 0x9d;
// prod_working's voice-line operands: race==2 adds this offset; the two base sound ids are keyed the
// same way as the message reason (soldier_count<1 vs. >=1).
inline constexpr int32_t PROD_WORKING_VOICE_RACE2_OFFSET      = 0x12;
inline constexpr int32_t PROD_WORKING_VOICE_BASE_UNIT_READY   = 9;
inline constexpr int32_t PROD_WORKING_VOICE_BASE_SOLDIER_LOAD = 0xc;
inline constexpr int32_t PROD_WORKING_VOICE_VOLUME            = 100;
// game::e::event member 14 -- same value/name-shape as sim_bldg_state_destroyed.cpp's own
// BLDG_STATE_DESTROYED_MAP_OBJECTS_REFRESH / sim_map_create_building.cpp's
// CREATE_BUILDING_MAP_OBJECTS_REFRESH (per-TU redeclaration, not shared).
inline constexpr uint32_t PROD_WORKING_MAP_OBJECTS_REFRESH = 14u;

// prod_pick_next's bounded search: 100 attempts, unit-type ids wrap 1..99 (0 is never produced).
inline constexpr int32_t PROD_PICK_NEXT_MAX_ATTEMPTS = 100;
inline constexpr int32_t PROD_PICK_NEXT_SLOT_MODULUS = 99;
// try_start_unit's "reason == 6" sentinel -- an immediate-stop blocked reason, transcribed literally
// (no backing enum found for the try_start_unit reason-code domain; declared_needs[] candidate).
inline constexpr int32_t PROD_PICK_NEXT_STOP_REASON = 6;
// The merge-logic's "housing-shortage-family" band -- SAME bounds as prod_blocked_notify's
// PROD_BLOCKED_HOUSING_BAND_LO/_HI above (both read off the identical [0x89,0x8d]-vs-[0xf,0x12]
// literal pair in their respective disassemblies; NOT the same band as blocked_notify's, which is a
// DIFFERENT reason-code sub-range -- kept as separate constants rather than shared to avoid
// implying they are the same domain).
inline constexpr int32_t PROD_PICK_NEXT_MERGE_BAND_LO = 0x89;
inline constexpr int32_t PROD_PICK_NEXT_MERGE_BAND_HI = 0x8d;
inline constexpr int32_t PROD_PICK_NEXT_MERGE_UNIFIED = 0x89;

} // namespace

namespace detail {

void bldg_state_prod_pick_next(const sim_view &v, sim_store &own, const bldg_state_prod_pick_next_calls &c) {
    building &b      = own.cur_building(); // _G_LLM_STRAT_CUR_BUILDING dereferenced, read+write
    b.state          = BLDG_STATE_IDLE_ACTIVATE;
    b.online_state   = 2;
    b.cycle_progress = 0.0;

    // ---- the bounded search (0x00473a5a-0x00473c9e) ------------------------------------------------
    // slot starts at (active_unit_type % 99) + 1 and advances by the same recurrence at the BOTTOM of
    // each iteration -- see the header's derivation note on why this shape (not the .c draft's
    // top-of-loop recompute) is what is transcribed.
    //
    // *v.cur_player / b.sub_id are READ FRESH at each site below (0x00473a7a/0x00473add/0x00473b34/
    // 0x00473beb/0x00473c1b/0x00473c49/0x00473c8b for cur_player, and b.sub_id similarly), not cached
    // into a local -- confirmed by reimpl-verify's oracle fan-out (2026-08-22) re-reading the raw asm.
    // cur_player is provably invariant across this loop (the same 2026-08-22 whole-binary cross-
    // reference that cleared dismantle_finish's caching applies here: the only two writers of
    // _G_LLM_STRAT_CUR_PLAYER in the whole binary, sim_step and advisor_tick, are top-level
    // dispatchers unreachable from prod_try_start_unit's closure), but b.sub_id has no such proof --
    // matching the asm's own re-read cadence removes the question entirely rather than relying on it.
    uint8_t slot = static_cast<uint8_t>(
        v.productions[static_cast<uint32_t>(*v.cur_player) * v.caps.productions + b.sub_id].active_unit_type %
            PROD_PICK_NEXT_SLOT_MODULUS +
        1);

    int32_t fail_count = 0;
    for (int32_t iter = 0; iter < PROD_PICK_NEXT_MAX_ATTEMPTS; ++iter) {
        const uint16_t    player = *v.cur_player;
        const uint8_t     sub_id = b.sub_id;
        const production &prod   = v.productions[player * v.caps.productions + sub_id];
        if (prod.queued_count[slot] != 0 && v.cfg_buildings[b.building_id].unit_quant[slot] != 0.0) {
            // ---- BIT-TEST note (see header): the asm's raw dword TEST pair is bit-for-bit
            // equivalent to `!= 0.0` -- transcribed as the plain comparison, flagged in
            // uncertainties[] per the "any FP comparison" rule.
            const int32_t result = c.prod_try_start_unit(player, slot);
            if (result == 0) {
                // ---- success (0x00473bd9-0x00473c97): claim the slot, start the unit, return -----
                production &prod_mut = own.production_at(*v.cur_player, b.sub_id);
                prod_mut.queued_count[slot] -= 1;
                prod_mut.queued_count[0] -= 1;
                prod_mut.active_unit_type = slot;
                b.state                   = BLDG_STATE_PROD_WORKING;
                b.online_state            = 2;
                b.cycle_progress          = 0.0;
                c.ai_notify_unit_lifecycle(*v.cur_player, static_cast<uint16_t>(slot), 0, 0);
                return;
            }

            // ---- blocked (0x00473b4d-0x00473bd4): record the reason, maybe stop ------------------
            // The asm's online_state comparisons are all on the ZERO-EXTENDED 16-bit value (MOVZX /
            // unsigned word CMP+JC/JBE), so online_state_u is used throughout this block rather than
            // the int16_t field directly -- matters only if online_state were ever negative, which it
            // is not in practice, but this is the literal transcription.
            b.state = BLDG_STATE_PROD_BLOCKED_NOTIFY;
            ++fail_count;
            const uint16_t online_state_u = static_cast<uint16_t>(b.online_state);
            if (fail_count == 1 || online_state_u == static_cast<uint32_t>(result)) {
                b.online_state = static_cast<int16_t>(result);
            } else if (online_state_u >= PROD_PICK_NEXT_MERGE_BAND_LO && online_state_u <= PROD_PICK_NEXT_MERGE_BAND_HI &&
                       result >= PROD_PICK_NEXT_MERGE_BAND_LO && result <= PROD_PICK_NEXT_MERGE_BAND_HI) {
                b.online_state = PROD_PICK_NEXT_MERGE_UNIFIED;
            } else {
                b.online_state = 0;
            }
            if (result == PROD_PICK_NEXT_STOP_REASON)
                return;
        }
        slot = static_cast<uint8_t>(slot % PROD_PICK_NEXT_SLOT_MODULUS + 1);
    }
    // Ran out of attempts -- fall off the end (no notify_ui call anywhere in this function; see the
    // header's CONTRADICTS-THE-BATCH-CONTEXT note).
}

void bldg_state_prod_working(const sim_view &v, sim_store &own, const bldg_state_prod_working_calls &c,
                             uint32_t param_1, uint32_t param_2, uint32_t param_3, uint32_t param_4) {
    // param_1(EAX)/param_2(EDX) are DEAD: nothing in this function's body reads them -- it re-fetches
    // cur_player/cur_index from the globals throughout instead. Kept as real parameters (not dropped)
    // because the committed prototype (addr/mh_export.gen.h's sig_llm_strat_bldg_state_prod_working)
    // is what the shadow-replace hook binds against.
    (void)param_1;
    (void)param_2;

    building           &b   = own.cur_building(); // _G_LLM_STRAT_CUR_BUILDING dereferenced, read+write
    const uint16_t      bid = b.building_id;
    const cfg_building &cb  = v.cfg_buildings[bid];

    // ---- cycle_progress += tick_budget * (efficiency * unit_quant[active_type]) (0x00473cc0-0x00473d20)
    // PRE-dispatch reads (0x00473cc5, 0x00473d28) -- correct for this use, nothing has run yet.
    const production &prod_pre = v.productions[static_cast<uint32_t>(*v.cur_player) * v.caps.productions +
                                               b.sub_id];
    const double      rate     = b.efficiency * cb.unit_quant[prod_pre.active_unit_type];
    b.cycle_progress           = own.tick_budget() * rate + b.cycle_progress;

    // ---- completion gate (0x00473d51-0x00473d63): cycle_progress >= Unit[active_type].build_time ---
    // NAN-SAFE NEGATION (see header): a naive `<` would evaluate false for NaN, swapping this branch
    // for the completion-dispatch one; negated `!(>=)` preserves the hardware's JC-taken-on-unordered.
    if (!(b.cycle_progress >= v.cfg_units[prod_pre.active_unit_type].build_time)) {
        own.tick_budget() = 0.0;
    } else {
        c.completion_dispatch(*v.cur_player, *v.cur_index, param_3, param_4, *v.game_clock);

        // RE-DERIVE THE WHOLE PRODUCTION ROW (player, b.sub_id -- BOTH read fresh) AFTER
        // completion_dispatch, independently at EACH of the three sites below (0x00473d9b/0x00473dd6,
        // 0x00473faf) -- confirmed by re-reading the raw asm (reimpl-verify's oracle fan-out,
        // 2026-08-22) that the ORIGINAL never keeps sub_id or active_unit_type across this call: it
        // recomputes `productions[cur_player][cur_building->sub_id]`'s ADDRESS from scratch each time,
        // not just the active_unit_type field within a row fixed at entry. completion_dispatch is a
        // large (~0x14a8-byte) uninspected original function; per Law 2 this is reproduced literally
        // regardless of whether completion_dispatch is proven to write sub_id or active_unit_type.
        const production &prod_post1 =
            v.productions[static_cast<uint32_t>(*v.cur_player) * v.caps.productions + b.sub_id]; // 0x00473d9b
        if (*v.cur_player == static_cast<uint16_t>(*v.player_side) && prod_post1.queued_count[0] == 0) {
            const production &prod_post2 =
                v.productions[static_cast<uint32_t>(*v.cur_player) * v.caps.productions +
                              b.sub_id];                                        // 0x00473dd6
            const cfg_unit &cu_post = v.cfg_units[prod_post2.active_unit_type]; // fresh: 0x00473df7
            if (*v.sim_active != 0) {
                const bool    soldier_load = cu_post.soldier_count < 1;
                const int32_t race_off     = (*v.player_race == 2) ? PROD_WORKING_VOICE_RACE2_OFFSET : 0;
                c.snd_play(race_off + (soldier_load ? PROD_WORKING_VOICE_BASE_UNIT_READY
                                                    : PROD_WORKING_VOICE_BASE_SOLDIER_LOAD),
                           PROD_WORKING_VOICE_VOLUME);
            }

            c.bldg_get_coords(*v.cur_player, static_cast<int32_t>(*v.cur_index), &own.cam_pan_target_col(),
                              &own.cam_pan_target_row());
            own.cam_pan_target_col() = fine_to_tile(own.cam_pan_target_col());
            own.cam_pan_target_row() = fine_to_tile(own.cam_pan_target_row());

            const uint16_t reason_text =
                cu_post.soldier_count < 1 ? PROD_WORKING_TEXT_UNIT_READY : PROD_WORKING_TEXT_SOLDIERS_READY;
            c.w_sprintf__vss(own.text_scratch(), TEXT_FMT_NAME_REASON, v.text_ptrs[cb.id], v.text_ptrs[reason_text]);
            c.game_ui_PrintTextMessage(own.text_scratch());
        }

        // ---- RATE-ZERO-GUARD (see header): a plain bit-exact `== 0.0` check, no x87 landmine -------
        double divisor = rate;
        if (divisor == 0.0)
            divisor = 1.0;
        // Row + active_unit_type + build_time RE-DERIVED fresh (0x00473faf) -- same post-dispatch
        // reasoning as prod_post1/prod_post2 above.
        const production &prod_post3 =
            v.productions[static_cast<uint32_t>(*v.cur_player) * v.caps.productions + b.sub_id];
        own.tick_budget() = (b.cycle_progress - v.cfg_units[prod_post3.active_unit_type].build_time) / divisor;
        b.state           = BLDG_STATE_PROD_PICK_NEXT;
        c.set_event(PROD_WORKING_MAP_OBJECTS_REFRESH);
    }

    // ---- tail: runs on BOTH branches (0x00473fff-0x00474012) ---------------------------------------
    c.bldg_notify_ui(*v.cur_player, static_cast<uint32_t>(*v.cur_index));
}

void bldg_state_prod_blocked_notify(const sim_view &v, sim_store &own,
                                    const bldg_state_prod_blocked_notify_calls &c) {
    building &b      = own.cur_building(); // _G_LLM_STRAT_CUR_BUILDING dereferenced, read+write
    b.state          = BLDG_STATE_PROD_RETRY_WAIT;
    b.cycle_progress = 0.0;

    if (*v.cur_player == static_cast<uint16_t>(*v.player_side)) {
        const uint16_t      bid            = b.building_id;
        const cfg_building &cb             = v.cfg_buildings[bid];
        const uint16_t      online_state_u = static_cast<uint16_t>(b.online_state);

        if (online_state_u == 0) {
            c.w_sprintf__vss(own.text_scratch(), TEXT_FMT_NAME_REASON, v.text_ptrs[cb.id],
                             v.text_ptrs[PROD_BLOCKED_REASON_TEXT_NONE]);
        } else if (online_state_u < PROD_BLOCKED_HOUSING_BAND_LO || online_state_u > PROD_BLOCKED_HOUSING_BAND_HI) {
            c.w_sprintf__vss(own.text_scratch(), TEXT_FMT_NAME_REASON, v.text_ptrs[cb.id],
                             v.text_ptrs[online_state_u]);
        } else {
            const int32_t housing_bid = c.reason_to_housing_bldg(online_state_u, *v.player_race);
            if (housing_bid < 1) {
                c.w_sprintf__vss(own.text_scratch(), TEXT_FMT_NAME_REASON, v.text_ptrs[cb.id],
                                 v.text_ptrs[online_state_u]);
            } else {
                c.w_sprintf__vsss(own.text_scratch(), TEXT_FMT_NAME_REASON_HOUSING, v.text_ptrs[cb.id],
                                  v.text_ptrs[online_state_u], v.text_ptrs[v.cfg_buildings[housing_bid].id]);
            }
        }
        c.game_ui_PrintTextMessage(own.text_scratch());
    }

    // Unconditional tail (0x004742f6-0x004742fb) -- runs whether or not the message block did.
    b.online_state = 1;
}

void bldg_state_prod_retry_wait(const sim_view &v, sim_store &own) {
    building &b       = own.cur_building(); // _G_LLM_STRAT_CUR_BUILDING dereferenced, read+write
    b.cycle_progress  = own.tick_budget() + b.cycle_progress;
    own.tick_budget() = 0.0;

    // NO-NEGATION-NEEDED (see header): the naive `<=` already matches the hardware's NaN-skips-the-
    // block behaviour for this direction, unlike prod_working's gate.
    //
    // DECLARED NEED (see header): v.prod_retry_period / v.prod_retry_period_neg do not exist in
    // sim_view yet, despite the batch context claiming otherwise -- written AS IF they exist, pending
    // the conductor adding them.
    if (*v.prod_retry_period <= b.cycle_progress) {
        own.tick_budget() = b.cycle_progress + *v.prod_retry_period_neg;
        b.state           = BLDG_STATE_PROD_PICK_NEXT;
    }
}

} // namespace detail

// ---- the public wrappers ---------------------------------------------------------------------------

void bldg_state_prod_pick_next() {
    sim_state st = state();
    detail::bldg_state_prod_pick_next(st.read, st.own, live_bldg_state_prod_pick_next_calls());
}

void bldg_state_prod_working(uint32_t param_1, uint32_t param_2, uint32_t param_3, uint32_t param_4) {
    sim_state st = state();
    detail::bldg_state_prod_working(st.read, st.own, live_bldg_state_prod_working_calls(), param_1, param_2, param_3,
                                    param_4);
}

void bldg_state_prod_blocked_notify() {
    sim_state st = state();
    detail::bldg_state_prod_blocked_notify(st.read, st.own, live_bldg_state_prod_blocked_notify_calls());
}

void bldg_state_prod_retry_wait() {
    sim_state st = state();
    detail::bldg_state_prod_retry_wait(st.read, st.own);
}


} // namespace mh::sim
