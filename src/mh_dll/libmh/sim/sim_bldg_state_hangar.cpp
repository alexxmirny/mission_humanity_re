//
// sim/sim_bldg_state_hangar.cpp -- see sim_bldg_state_hangar.h. Translated from the DISASSEMBLY
// (tmp/decomp_sim/llm_strat_bldg_state_hangar_recharge_check_00472b26.asm,
// _hangar_recharge_units_00472b9d.asm), cross-checked against the Ghidra .c drafts (tmp/decomp_sim/*.c)
// -- both agree with the assembly for both functions.
//
#include "sim/sim_bldg_state_hangar.h"

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"        // ai_say / trace_budget -- the shared trace sink, not AI state
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const bldg_state_hangar_recharge_check_calls &live_bldg_state_hangar_recharge_check_calls() {
    static const bldg_state_hangar_recharge_check_calls c = {
        MH_LIBMH_BIND(llm_strat_hangar_any_unit_needs_energy),
        MH_LIBMH_BIND(llm_strat_bldg_notify_ui),
        MH_LIBMH_BIND(llm_strat_refresh_building),
    };
    return c;
}

const bldg_state_hangar_recharge_units_calls &live_bldg_state_hangar_recharge_units_calls() {
    static const bldg_state_hangar_recharge_units_calls c = {
        MH_LIBMH_BIND(llm_strat_hangar_recharge_pulse),
        MH_LIBMH_BIND(llm_strat_bldg_notify_ui),
    };
    return c;
}

namespace {

// ---- this file's own literal operands, sourced from the Ghidra .c drafts' resolved
// llm_strat_bldg_state enum member names (rule 17a: existing enum, not yet a real C++ type -- see the
// header's declared_needs note). NOT re-derived/invented -- transcribed from tmp/decomp_sim's
// llm_strat_bldg_state_hangar_recharge_check_00472b26.c / _hangar_recharge_units_00472b9d.c. Per-TU
// anonymous-namespace, same precedent sim_bldg_state_charge.cpp / sim_bldg_state_reset_idle.cpp already
// follow (not shared cross-TU, to avoid an ODR collision with a differently-valued same-named constant
// in another sim/ TU).
inline constexpr uint16_t BLDG_STATE_IDLE_NOOP             = 0x77; // Ghidra: IDLE_NOOP
inline constexpr uint16_t BLDG_STATE_HANGAR_RECHARGE_CHECK = 0x78; // Ghidra: HANGAR_RECHARGE_CHECK
inline constexpr uint16_t BLDG_STATE_HANGAR_RECHARGE_UNITS = 0x79; // Ghidra: HANGAR_RECHARGE_UNITS

} // namespace

namespace detail {

void bldg_state_hangar_recharge_check(const sim_view &v, sim_store &own,
                                      const bldg_state_hangar_recharge_check_calls &c) {
    building &b = own.cur_building(); // _G_LLM_STRAT_CUR_BUILDING dereferenced, read+write

    // ---- gate (0x00472b4c-0x00472b6d): does any unit in this hangar still need energy? -------------
    const int32_t needs_energy =
        c.hangar_any_unit_needs_energy(static_cast<int32_t>(*v.cur_player), static_cast<int32_t>(*v.cur_index));
    if (needs_energy == 0) {
        b.state = BLDG_STATE_IDLE_NOOP;
    } else {
        b.state = BLDG_STATE_HANGAR_RECHARGE_UNITS;
    }

    // ---- tail: unconditional, both branches converge (0x00472b6d-0x00472b93) -----------------------
    c.bldg_notify_ui(*v.cur_player, static_cast<uint32_t>(*v.cur_index));
    c.refresh_building(*v.cur_player, *v.cur_index);
}

void bldg_state_hangar_recharge_units(const sim_view &v, sim_store &own,
                                      const bldg_state_hangar_recharge_units_calls &c) {
    building &b = own.cur_building(); // _G_LLM_STRAT_CUR_BUILDING dereferenced, read+write

    // ---- cycle_progress += tick_budget * efficiency (0x00472bb5-0x00472bd9) ------------------------
    // Same accumulation idiom bldg_state_construction's opening uses (sim_bldg_state_charge.cpp).
    b.cycle_progress  = own.tick_budget() * b.efficiency + b.cycle_progress;
    own.tick_budget() = 0.0;

    // ---- the FP gate (0x00472be8-0x00472bf4). See the header's FP-COMPARISON note: unlike
    // bldg_state_charge_step's two-compare OR chain, this single FCOMP/JC's "skip on unordered" reading
    // COINCIDES with the naive `HANGAR_RECHARGE_PERIOD <= cycle_progress` IEEE-754 evaluation (false on
    // any NaN operand, which is also the hardware's skip branch) -- no negated restatement needed here.
    if (*v.hangar_recharge_period <= b.cycle_progress) {
        // ---- do the recharge pulse (0x00472bf6-0x00472c33) ------------------------------------------
        c.hangar_recharge_pulse(*v.cur_player, static_cast<int32_t>(*v.cur_index));
        b.cycle_progress += *v.hangar_recharge_period_neg;
        b.state = BLDG_STATE_HANGAR_RECHARGE_CHECK;
        c.bldg_notify_ui(*v.cur_player, static_cast<uint32_t>(*v.cur_index));
    }
    // Else: falls straight to the epilogue -- no further reads/writes/calls (confirmed by the asm's
    // JC target landing directly on the shared epilogue, not on any intermediate code).
}

} // namespace detail

// ---- the public wrappers ---------------------------------------------------------------------------

void bldg_state_hangar_recharge_check() {
    sim_state st = state();
    detail::bldg_state_hangar_recharge_check(st.read, st.own, live_bldg_state_hangar_recharge_check_calls());
}

void bldg_state_hangar_recharge_units() {
    sim_state st = state();
    detail::bldg_state_hangar_recharge_units(st.read, st.own, live_bldg_state_hangar_recharge_units_calls());
}


} // namespace mh::sim
