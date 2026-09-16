//
// sim/sim_bldg_construction_complete.cpp -- see sim_bldg_construction_complete.h. Translated from the
// DISASSEMBLY (tmp/decomp_sim/llm_strat_bldg_construction_complete_00478d70.asm), whose byte-level
// CMP/JC/JBE/JZ chain is the authority for the type dispatch -- NOT the batch context doc's prose
// summary, which the header's own banner documents as wrong (it collapses four single-value matches
// into two ranges). The Ghidra .c draft agrees with the assembly branch-for-branch once its enum-named
// comparisons are read literally, and is cross-checked here against three independent existing
// bindings of the same cfg_enum_E_BUILDING enum (see the header).
//
#include "sim/sim_bldg_construction_complete.h"

#include <cstring>

#include "addr/mh_calls.gen.h"     // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"           // ai_say / trace_budget -- the shared trace sink, not AI state
#include "sim/sim_order_enqueue.h" // BUILDING_TYPE_A_MOTHER/_A_SHUTTLE/_H_MOTHER/_H_SHUTTLE
#include "addr/mh_rebind.gen.h"    // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const bldg_construction_complete_calls &live_bldg_construction_complete_calls() {
    static const bldg_construction_complete_calls c = {
        MH_LIBMH_BIND(llm_strat_bldg_completion_dispatch),
        MH_LIBMH_BIND(llm_strat_bldg_start_special_anim),
        MH_LIBMH_BIND(llm_strat_bldg_anim_state_trigger),
        MH_LIBMH_BIND(llm_strat_bldg_set_staffed_flag),
        MH_LIBMH_BIND(llm_strat_refresh_building),
    };
    return c;
}

namespace {

// ---- this file's own literal operands, sourced from the Ghidra .c draft's resolved
// llm_strat_bldg_state enum member names (rule 17a: existing enum, not yet a real C++ type). Per-TU
// anonymous-namespace, same precedent every other sim/bldg_state_*.cpp / bldg_*.cpp in this module
// already follows (not shared cross-TU, to avoid an ODR collision with a differently-valued
// same-named constant in another sim/ TU). BLDG_STATE_CONSTRUCTION is independently redeclared by
// several sibling TUs at the same value (0x64) -- e.g. sim_bldg_add_workers.cpp,
// sim_order_dispatch_bldg.cpp. BLDG_STATE_LAND_ACTIVATE is this file's first binding of 0x85.
inline constexpr uint16_t BLDG_STATE_CONSTRUCTION  = 0x64; // Ghidra: CONSTRUCTION
inline constexpr uint16_t BLDG_STATE_LAND_ACTIVATE = 0x85; // Ghidra: LAND_ACTIVATE

// Splits GAME_CLOCK's raw 8-byte bit pattern into the two dwords llm_strat_bldg_anim_state_trigger's
// ALREADY-COMMITTED prototype expects as independent uint32_t param_5/param_6 (NOT a reconstructed
// double -- see the header's marshalling derivation). Same shape/precedent as
// sim_bldg_state_deploy.cpp's own split_game_clock() (not shared cross-TU, per this module's
// established per-TU-copy convention for small bit-pattern helpers).
void split_game_clock(double clock, uint32_t &lo, uint32_t &hi) {
    uint64_t bits;
    std::memcpy(&bits, &clock, sizeof(bits));
    lo = static_cast<uint32_t>(bits);
    hi = static_cast<uint32_t>(bits >> 32);
}

} // namespace

namespace detail {

void bldg_construction_complete(const sim_view &v, sim_store &own, const bldg_construction_complete_calls &c,
                                uint32_t player, uint32_t building_index, uint32_t param_3,
                                uint32_t param_4) {
    // ---- 1. (0x00478d9d, unconditional) --------------------------------------------------------------
    own.building_at(player, static_cast<int32_t>(building_index)).state = BLDG_STATE_CONSTRUCTION;

    // ---- 2. (0x00478da6-0x00478db9, unconditional) ---------------------------------------------------
    // param_3/param_4 are THIS function's own incoming EBX/ECX, forwarded verbatim -- never written
    // anywhere in this body (only PUSHed/POPed for callee-save at entry/exit, which is not a write to
    // the register itself). Same dead-but-forwarded shape sim_bldg_state_deploy.cpp's own
    // land_activate/deploy_start document for their own param_3/param_4.
    c.bldg_completion_dispatch(player & 0xffffu, building_index, param_3, param_4, *v.game_clock);

    // ---- 3. (0x00478dbe-0x00478de1) -------------------------------------------------------------------
    // building_id read FRESH here, i.e. AFTER step 2's call returns -- matching the asm's own
    // instruction order, not cached from before the call (the callee is an ORIGINAL function that
    // could in principle have touched this building's own record).
    const uint16_t bid      = own.building_at(player, static_cast<int32_t>(building_index)).building_id;
    const uint8_t  type     = v.cfg_buildings[bid].type;
    const uint16_t player16 = static_cast<uint16_t>(player);

    // ---- 4. the byte-range dispatch (0x00478de4-0x00478e02) -- see the header's correction: FOUR
    // single-value matches, not two ranges. ------------------------------------------------------------
    if (type == BUILDING_TYPE_A_MOTHER || type == BUILDING_TYPE_H_MOTHER) {
        // LAB_00478e62: anim_state_trigger, then THIS branch's own tail (a separate code copy from the
        // A_SHUTTLE/H_SHUTTLE tail below -- see the header).
        uint32_t clock_lo = 0, clock_hi = 0;
        split_game_clock(*v.game_clock, clock_lo, clock_hi);
        c.bldg_anim_state_trigger(player & 0xffffu, building_index, param_3, param_4, clock_lo, clock_hi);
        own.building_at(player, static_cast<int32_t>(building_index)).state = BLDG_STATE_LAND_ACTIVATE;
        c.bldg_set_staffed_flag(player16, static_cast<int32_t>(building_index));
        c.refresh_building(player16, static_cast<int32_t>(building_index));
        return;
    }

    if (type == BUILDING_TYPE_A_SHUTTLE) {
        // LAB_00478e17, falls straight through (no jump) into LAB_00478e2f's tail.
        c.bldg_start_special_anim(player16, static_cast<int32_t>(building_index), param_3, param_4,
                                  *v.game_clock);
        own.building_at(player, static_cast<int32_t>(building_index)).state = BLDG_STATE_LAND_ACTIVATE;
        c.bldg_set_staffed_flag(player16, static_cast<int32_t>(building_index));
        c.refresh_building(player16, static_cast<int32_t>(building_index));
        return;
    }

    if (type == BUILDING_TYPE_H_SHUTTLE) {
        // 0x00478e00 JZ 0x00478e2f: jumps DIRECTLY into the tail, SKIPPING start_special_anim entirely
        // -- a genuine asymmetry vs. A_SHUTTLE, preserved as-is (rule 10).
        own.building_at(player, static_cast<int32_t>(building_index)).state = BLDG_STATE_LAND_ACTIVATE;
        c.bldg_set_staffed_flag(player16, static_cast<int32_t>(building_index));
        c.refresh_building(player16, static_cast<int32_t>(building_index));
        return;
    }

    // Every other type: no further action (0x00478e07/0x00478e12/0x00478eab -- straight to epilogue).
}

} // namespace detail

// ---- the public wrapper ---------------------------------------------------------------------------

void bldg_construction_complete(uint32_t player, uint32_t building_index, uint32_t param_3,
                                uint32_t param_4) {
    sim_state st = state();
    detail::bldg_construction_complete(st.read, st.own, live_bldg_construction_complete_calls(), player,
                                       building_index, param_3, param_4);
}


} // namespace mh::sim
