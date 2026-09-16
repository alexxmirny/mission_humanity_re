//
// sim/sim_bldg_state_mine.cpp -- see sim_bldg_state_mine.h. Translated from the DISASSEMBLY
// (tmp/decomp_sim/llm_strat_bldg_state_mine_check_deposits_004743e1.asm, _mine_extracting_0047457a.asm,
// _mine_depleted_0047464e.asm, _mine_rescan_wait_0047468e.asm), cross-checked against the Ghidra .c
// drafts (tmp/decomp_sim/*.c) -- all four agree with the assembly.
//
#include "sim/sim_bldg_state_mine.h"

#include <cstring>

#include "addr/mh_calls.gen.h" // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"       // ai_say / trace_budget -- the shared trace sink, not AI state
#include "state/host_api.h"
#include "state/host_events.h"
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"
#include "crt/crt_select.h" // LIB-CRT: MH_CRT() picks the vendored CRT in the standalone build

namespace mh::sim {

const bldg_state_mine_check_deposits_calls &live_bldg_state_mine_check_deposits_calls() {
    static const bldg_state_mine_check_deposits_calls c = {
        MH_LIBMH_BIND(llm_strat_bldg_get_coords),
        mh::state::evt::snd_play,
        MH_CRT(w_sprintf__vss),
        mh::state::evt::text_print_u32,
        MH_LIBMH_BIND(llm_strat_bldg_notify_ui),
    };
    return c;
}

const bldg_state_mine_extracting_calls &live_bldg_state_mine_extracting_calls() {
    static const bldg_state_mine_extracting_calls c = {
        MH_LIBMH_BIND(llm_strat_bldg_completion_dispatch),
    };
    return c;
}

namespace {

// The tile<-fine conversion (`SAR EDX,0x1f / SHL EDX,0x5 / SBB EAX,EDX / SAR EAX,0x5`), used by
// mine_check_deposits' camera-pan-target write. Value-for-value C's truncating `/ 32` -- see
// sim_bldg_state_charge.cpp's identical fine_to_tile() for the derivation; re-derived locally per
// this project's per-TU convention.
inline int32_t fine_to_tile(int32_t fine) { return fine / 32; }

// RESOLVED 2026-08-22 (SIM1-G4): mh_map_object_mine::deposit_slot is now
// `mh_llm_mine_deposit_slot[4]` in addr/mh_structs.gen.h (the DECLARED NEED this comment used to
// describe -- the type was already in Ghidra, just not applied to the array; applied).
// Real field access replaces the raw-byte-offset workaround below.
inline constexpr int32_t DEPOSIT_SLOTS = 4; // mh_map_object_mine::deposit_slot[4]

// ---- this family's own literal state operands, no backing Ghidra enum (rule 17a fallback; the
// llm_strat_bldg_state enum name is Ghidra's own comment only, not a real C++ type) -- see the
// header's DECLARED NEED note. Per-TU anonymous-namespace, same precedent every other
// sim_bldg_state_*.cpp in this closure already follows (not shared cross-TU).
inline constexpr uint16_t BLDG_STATE_MINE_SCAN_DEPOSITS  = 0x72;
inline constexpr uint16_t BLDG_STATE_MINE_CHECK_DEPOSITS = 0x73;
inline constexpr uint16_t BLDG_STATE_MINE_EXTRACTING     = 0x74;
inline constexpr uint16_t BLDG_STATE_MINE_DEPLETED       = 0x75;
inline constexpr uint16_t BLDG_STATE_MINE_RESCAN_WAIT    = 0x76;

// mine_check_deposits' own voice-line / message literals (see header derivation).
inline constexpr int32_t CHECK_DEPOSITS_VOICE_RACE2_OFFSET  = 0x12; // (player_race==2) extra offset
inline constexpr int32_t CHECK_DEPOSITS_VOICE_BASE_SOUND_ID = 3;    // base sound id, race offset added
inline constexpr int32_t CHECK_DEPOSITS_VOICE_VOLUME        = 100;
inline constexpr int32_t TEXT_ID_MINE_DEPOSITS_DEPLETED     = 14; // G_TEXT_PTRS[14], the "reason" half

// The shared "%s: %s" message format (0x005012ec) -- same literal address
// sim_bldg_state_charge.cpp's own TEXT_FMT_NAME_REASON precedent documents (duplicated per-TU, not
// shared cross-TU, per that file's own reasoning).
constexpr const wchar_t *TEXT_FMT_NAME_REASON = L"%s: %s";

} // namespace

namespace detail {

void bldg_state_mine_check_deposits(const sim_view &v, sim_store &own,
                                    const bldg_state_mine_check_deposits_calls &c) {
    building           &b      = own.cur_building(); // _G_LLM_STRAT_CUR_BUILDING dereferenced, read+write
    const uint8_t       sub_id = b.sub_id;           // read once, reused for the mine-roster address
    const cfg_building &cb     = v.cfg_buildings[b.building_id];

    b.cycle_progress = 0.0; // unconditional (0x0047440d-0x0047441b)

    // ---- sum the four deposit slots' extract_rate (0x00474429-0x0047445b) -------------------------
    const mine &m   = v.mines[static_cast<uint32_t>(*v.cur_player) * v.caps.mines + sub_id];
    int32_t     sum = 0;
    for (int32_t i = 0; i < DEPOSIT_SLOTS; ++i) {
        sum += static_cast<int32_t>(m.deposit_slot[i].extract_rate);
    }

    if (sum > 0) {
        // ---- deposits remain: hand off to EXTRACTING, no further effect (0x00474463-0x0047446e) ----
        b.state = BLDG_STATE_MINE_EXTRACTING;
    } else {
        // ---- no deposits: DEPLETED, LOCAL-player-only voice line + camera pan + message ------------
        b.state = BLDG_STATE_MINE_DEPLETED;
        if (*v.cur_player == static_cast<uint16_t>(*v.player_side)) {
            if (*v.sim_active != 0) {
                const int32_t sound_id = (*v.player_race == 2 ? CHECK_DEPOSITS_VOICE_RACE2_OFFSET : 0) +
                                         CHECK_DEPOSITS_VOICE_BASE_SOUND_ID;
                c.snd_play(sound_id, CHECK_DEPOSITS_VOICE_VOLUME);
            }

            c.bldg_get_coords(*v.cur_player, static_cast<int32_t>(*v.cur_index), &own.cam_pan_target_col(),
                              &own.cam_pan_target_row());
            own.cam_pan_target_col() = fine_to_tile(own.cam_pan_target_col());
            own.cam_pan_target_row() = fine_to_tile(own.cam_pan_target_row());

            c.w_sprintf__vss(own.text_scratch(), TEXT_FMT_NAME_REASON, v.text_ptrs[cb.id],
                             v.text_ptrs[TEXT_ID_MINE_DEPOSITS_DEPLETED]);
            c.game_ui_PrintTextMessage(own.text_scratch());
        }
    }

    // ---- tail: unconditional, both branches converge (0x0047455d-0x00474570) -----------------------
    c.bldg_notify_ui(*v.cur_player, static_cast<uint32_t>(*v.cur_index));
}

void bldg_state_mine_extracting(const sim_view &v, sim_store &own, const bldg_state_mine_extracting_calls &c,
                                uint32_t param_1, uint32_t param_2, uint32_t param_3, uint32_t param_4) {
    // param_1(EAX)/param_2(EDX) are genuinely dead: nothing in this function's body reads or forwards
    // them. param_3(EBX)/param_4(ECX) ARE forwarded to completion_dispatch below -- see the REGISTER
    // FORWARDING note in the header. Kept as real parameters because the committed prototype
    // (addr/mh_export.gen.h's sig_llm_strat_bldg_state_mine_extracting) is what the shadow-replace
    // hook binds against.
    (void)param_1;
    (void)param_2;

    building &b = own.cur_building(); // _G_LLM_STRAT_CUR_BUILDING dereferenced, read+write

    // ---- cycle_progress += tick_budget * efficiency (0x00474597-0x004745a9) -----------------------
    b.cycle_progress = own.tick_budget() * b.efficiency + b.cycle_progress;

    // ---- gate (0x004745b4-0x00474630): cycle_progress >= MINE_EXTRACT_PERIOD -----------------------
    // See the header's FP-COMPARISON note: the SKIP branch is the smaller one, so this is written as
    // the negated `>=` (NaN-safe: false for NaN, so !(...) is true, matching the hardware's
    // skip-on-unordered JC) rather than a naive `<` (which would wrongly fall into the do-block on
    // NaN).
    if (!(b.cycle_progress >= *v.mine_extract_period)) {
        own.tick_budget() = 0.0;
        return;
    }

    // ---- do the extraction completion (0x004745bf-0x00474628) -------------------------------------
    // param_3/param_4 forwarded VERBATIM -- see the REGISTER FORWARDING note in the header.
    c.bldg_completion_dispatch(*v.cur_player, *v.cur_index, param_3, param_4, *v.game_clock);
    b.state = BLDG_STATE_MINE_CHECK_DEPOSITS;

    // ---- efficiency==0.0 self-heal-to-1.0 guard, avoids the FDIV below by zero (0x004745ee-0x00474609) --
    // A pure bit test on efficiency's raw representation (sign-masked high dword nonzero, OR low
    // dword nonzero -> skip): exact for +/-0.0, and NaN's nonzero exponent bits correctly fail the
    // "is zero" test too -- no x87 unordered-compare hazard here (this is not an FCOMP).
    if (b.efficiency == 0.0) {
        b.efficiency = 1.0;
    }

    // ---- tick_budget = (cycle_progress - MINE_EXTRACT_PERIOD) / efficiency (0x00474610-0x00474628) --
    // cycle_progress itself is NOT written back here, only read.
    own.tick_budget() = (b.cycle_progress + *v.mine_extract_period_neg) / b.efficiency;
}

void bldg_state_mine_depleted(sim_store &own) {
    building &b      = own.cur_building(); // _G_LLM_STRAT_CUR_BUILDING dereferenced, read+write
    b.state          = BLDG_STATE_MINE_RESCAN_WAIT;
    b.cycle_progress = 0.0;
    // No calls at all -- not even notify_ui (confirmed: the asm has no CALL besides the inert
    // opening stack-capacity probe).
}

void bldg_state_mine_rescan_wait(const sim_view &v, sim_store &own) {
    building &b = own.cur_building(); // _G_LLM_STRAT_CUR_BUILDING dereferenced, read+write

    // ---- cycle_progress += tick_budget (NO efficiency multiply, unlike mine_extracting) -----------
    b.cycle_progress  = own.tick_budget() + b.cycle_progress;
    own.tick_budget() = 0.0; // unconditional

    // ---- gate (0x004746d3-0x004746dc): cycle_progress >= MINE_RESCAN_PERIOD ------------------------
    // See the header's FP-COMPARISON note: the DO branch is written directly as `>=`, which is
    // already false for NaN under IEEE 754 -- coincides exactly with the hardware's skip-on-unordered
    // JC with no negation needed (same coincidence sim_bldg_state_hangar.cpp's
    // bldg_state_hangar_recharge_units documents).
    if (b.cycle_progress >= *v.mine_rescan_period) {
        // cycle_progress itself is NOT written back here, only read.
        own.tick_budget() = b.cycle_progress + *v.mine_rescan_period_neg;
        b.state           = BLDG_STATE_MINE_SCAN_DEPOSITS;
    }
    // Else: falls straight to the epilogue -- no further reads/writes/calls.
}

} // namespace detail

// ---- the public wrappers ---------------------------------------------------------------------------

void bldg_state_mine_check_deposits() {
    sim_state st = state();
    detail::bldg_state_mine_check_deposits(st.read, st.own, live_bldg_state_mine_check_deposits_calls());
}

void bldg_state_mine_extracting(uint32_t param_1, uint32_t param_2, uint32_t param_3, uint32_t param_4) {
    sim_state st = state();
    detail::bldg_state_mine_extracting(st.read, st.own, live_bldg_state_mine_extracting_calls(), param_1,
                                       param_2, param_3, param_4);
}

void bldg_state_mine_depleted() {
    sim_state st = state();
    detail::bldg_state_mine_depleted(st.own);
}

void bldg_state_mine_rescan_wait() {
    sim_state st = state();
    detail::bldg_state_mine_rescan_wait(st.read, st.own);
}


} // namespace mh::sim
