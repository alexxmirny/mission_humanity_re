//
// sim/sim_bldg_state_power.cpp -- see sim_bldg_state_power.h. Translated from the DISASSEMBLY
// (tmp/decomp_sim/llm_strat_bldg_state_power_primary_check_00474707.asm,
// _power_generate_0047477a.asm), cross-checked against the Ghidra .c drafts (tmp/decomp_sim/*.c) --
// both agree with the assembly for both functions.
//
#include "sim/sim_bldg_state_power.h"

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"        // ai_say / trace_budget -- the shared trace sink, not AI state
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const bldg_state_power_primary_check_calls &live_bldg_state_power_primary_check_calls() {
    static const bldg_state_power_primary_check_calls c = {
        MH_LIBMH_BIND(llm_strat_bldg_notify_ui),
    };
    return c;
}

namespace {

// ---- this file's own literal operands, sourced from the Ghidra .c drafts' resolved
// llm_strat_bldg_state enum member names (rule 17a: existing enum, not yet a real C++ type -- see the
// header's declared_needs note). NOT re-derived/invented -- transcribed from tmp/decomp_sim's
// llm_strat_bldg_state_power_primary_check_00474707.c. IDLE_NOOP_8C's VALUE is independently
// cross-confirmed by sim_bldg_mother_reelect_primary.cpp's own anonymous-namespace copy of the
// same constant (see the header banner) -- redeclared here per-TU rather than shared, same precedent
// every other sim/ TU with a locally-scoped bldg_state constant follows (avoids an ODR collision with a
// differently-valued same-named constant in another sim/ TU).
inline constexpr uint16_t BLDG_STATE_POWER_GENERATE = 0x8b; // Ghidra: POWER_GENERATE
inline constexpr uint16_t BLDG_STATE_IDLE_NOOP_8C   = 0x8c; // Ghidra: IDLE_NOOP_8C

} // namespace

namespace detail {

void bldg_state_power_primary_check(const sim_view &v, sim_store &own,
                                    const bldg_state_power_primary_check_calls &c) {
    const uint16_t player = *v.cur_player;
    const uint16_t index  = *v.cur_index;
    const int32_t  planet = *v.planet_index;

    building &b = own.cur_building(); // _G_LLM_STRAT_CUR_BUILDING dereferenced, write-only here

    // ---- the gate (0x0047471f-0x0047475d): is this building the roster's tracked primary mother
    // building for its planet? `_G_LLM_STRAT_PLAYERS[player].primary_mother_bldg[planet]` -- see the
    // header banner for the derivation resolving this from a raw offset to the already-named/typed
    // field (the SAME field sim_bldg_mother_reelect_primary.cpp writes).
    if (static_cast<int32_t>(index) == v.profiles[player].primary_mother_bldg[planet]) {
        b.state = BLDG_STATE_POWER_GENERATE;
    } else {
        b.state = BLDG_STATE_IDLE_NOOP_8C;
    }

    // ---- tail: unconditional, both branches converge (0x0047475d-0x00474770). ONE call only -- no
    // llm_strat_refresh_building here, unlike hangar_recharge_check's sibling shape.
    c.bldg_notify_ui(player, static_cast<uint32_t>(index));
}

void bldg_state_power_generate(const sim_view &v, sim_store &own) {
    // ---- the whole body (0x00474792-0x004747c2): plain int32_t accumulate, no branch, no callee. ----
    // `_G_LLM_STRAT_POWER_STATS[cur_player].generated += Building[cur_building->building_id]
    // .electric_power;` -- cur_building is read-only here (only .building_id is read, never written),
    // so the const view member suffices; the write lands entirely in power_stats.
    own.power_stats_at(*v.cur_player).generated += v.cfg_buildings[v.cur_building->building_id].electric_power;

    // `tick_budget = 0.0;` -- the SAME shared scratch double sim_bldg_state_charge.cpp /
    // sim_bldg_state_hangar.cpp already zero at the end of their own tick-consuming handlers. Written
    // as the two raw zero-dword stores the asm actually performs (0x004747b8/0x004747c2), which is
    // bit-identical to assigning 0.0 (IEEE-754 zero is the all-zero bit pattern).
    own.tick_budget() = 0.0;
}

} // namespace detail

// ---- the public wrappers ---------------------------------------------------------------------------

void bldg_state_power_primary_check() {
    sim_state st = state();
    detail::bldg_state_power_primary_check(st.read, st.own, live_bldg_state_power_primary_check_calls());
}

void bldg_state_power_generate() {
    sim_state st = state();
    detail::bldg_state_power_generate(st.read, st.own);
}


} // namespace mh::sim
