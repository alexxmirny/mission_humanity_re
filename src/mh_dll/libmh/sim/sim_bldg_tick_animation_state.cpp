//
// sim/sim_bldg_tick_animation_state.cpp -- see sim_bldg_tick_animation_state.h. Translated from the
// DISASSEMBLY (tmp/decomp/llm_strat_bldg_tick_animation_state_00476305.asm), not from the Ghidra .c
// draft -- the draft's overall if/else-if/else shape is right, but it renders the energy compare as
// `0.0 < energy` (NaN-wrong, see the header's FP note; same trap already caught on the sibling
// llm_strat_building_tick) and the efficiency compare as a manual 64-bit mask expression that plain
// `== 0.0` reproduces more simply and just as exactly (see the header).
//
#include "sim/sim_bldg_tick_animation_state.h"

#include <cstring>

#include "addr/mh_calls.gen.h"                    // typed callables for the original functions we still call OUT to
#include "addr/mh_export.gen.h"                   // MH_EXPORT_REPLACE -- the promotion entry thunk ([promote]
                                                  // bldg_tick_animation_state=1); the G13/G19 dispatcher oracle
#include "ai/ai_state.h"                          // ai_say / trace_budget -- the shared trace sink, not AI state
#include "sim/sim_register_bldg_type_callbacks.h" // note_first_bldg_type_dispatch -- the one-time
                                                  // "is the tick2 table ours?" line (SIM1-BLDGCB)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>            // GetPrivateProfileIntA -- the [promote] gate, same as sim_building_tick.cpp
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const bldg_tick_animation_state_calls &live_bldg_tick_animation_state_calls() {
    static const bldg_tick_animation_state_calls c = {
        MH_LIBMH_BIND(llm_cfg_anim_frame_at_progress),
    };
    return c;
}

namespace {
// mh_map_object_building::anim and mh_cfg_final_struct_Building::anim are both documented
// `cfg_t_frame_index[12]` (int32_t) but flattened by Ghidra to `uint8_t anim[48]`
// (addr/mh_structs.gen.h) -- same flattening sim_bldg_reset_construction_anim.cpp's frame_at()/
// set_frame_at() already document and work around; reused here in the same shape (file-local, not
// shared cross-TU, per that file's own "write only your own new files" reasoning).
int32_t frame_at(const uint8_t (&anim)[48], int32_t slot) {
    int32_t value;
    std::memcpy(&value, &anim[slot * 4], sizeof(value));
    return value;
}
void set_frame_at(uint8_t (&anim)[48], int32_t slot, int32_t value) {
    std::memcpy(&anim[slot * 4], &value, sizeof(value));
}
} // namespace

namespace detail {

void bldg_tick_animation_state(const sim_view &v, sim_store &own,
                               const bldg_tick_animation_state_calls &c) {
    building &b = own.cur_building(); // _G_LLM_STRAT_CUR_BUILDING dereferenced, read+write

    // building_id, and the cfg record it selects, read ONCE here rather than re-derived at every asm
    // reference (the asm re-fetches CUR_BUILDING and re-indexes cfg_buildings fresh at each of
    // 0x00476391/0x00476396, 0x004763de/0x004763e3, 0x00476402/0x00476407 -- Watcom's usual
    // unoptimized codegen). Nothing this function calls (llm_cfg_anim_frame_at_progress is a PURE
    // function of its two arguments) can write building_id or the cfg table, so caching is
    // behaviourally identical to the asm's redundant re-fetch -- same reasoning
    // sim_bldg_reset_construction_anim.cpp's own `const cfg_building &cb = ...` hoist relies on.
    const cfg_building &cb = v.cfg_buildings[b.building_id];

    // ---- GATE (0x00476321-0x0047638b) -------------------------------------------------------------
    //
    // (a) built_flags != 3 && energy > 0.0 (0x00476321-0x00476369). `built_flags` is read via the
    // roster array in the asm (buildings[cur_player][cur_index]), a DIFFERENT expression than every
    // other field access in this function (all through the cur_building pointer) -- see the header's
    // uncertainty note on why `b.built_flags` (through own.cur_building(), matching the rest of the
    // function) is used here rather than adding a new roster-index view member. The energy half is
    // the x87 FLDZ/FCOMP/FNSTSW/SAHF/JC idiom -- `!(energy <= 0.0)` reproduces JC's "0.0 < energy"
    // NaN-inclusive-on-the-false-side semantics exactly (see the header's FP note; a NaN energy never
    // satisfies `<= 0.0`, so this stays on the same side as the assembly for a NaN input, unlike the
    // naive `0.0 < energy` which the .c draft renders).
    //
    // (b) efficiency bit-exact +-0.0 (0x0046636b-0x0047637d). NOT an x87 compare -- a TEST+CMP on the
    // raw dwords (high dword's low 31 bits, and the whole low dword). Plain `== 0.0` reproduces this
    // exactly (ordered IEEE equality: false for NaN, true only for +-0.0) -- see the header's FP note
    // on why a manual bit_cast/TEST reproduction would be redundant, not more faithful.
    //
    // (c) state == RUBBLE_SIGHT_DECAY (0x00476381-0x0047638b).
    const bool freeze_or_reset =
        ((b.built_flags != 3) && !(b.energy <= 0.0)) || (b.efficiency == 0.0) ||
        (b.state == BLDG_STATE_RUBBLE_SIGHT_DECAY);

    if (freeze_or_reset) {
        // ---- reset every sprite's anim timer to the current game clock (0x0047638d-0x004763b3) ----
        // Loop bound is Building[building_id].sprite_quantity (uint8_t), re-read off the cfg record
        // at the top of every iteration in the asm (0x00476396) -- reproduced here by simply not
        // caching a separate copy of it (cb is already hoisted above, but sprite_quantity itself
        // cannot change mid-loop since nothing in this body writes cfg data).
        for (uint8_t i = 0; i < cb.sprite_quantity; ++i) { b.anim_dur[i] = *v.game_clock; }
    } else if (b.online_state == 0) {
        // ---- recompute anim[0] (0x004763d2-0x0047642b) ---------------------------------------------
        const int32_t start_frame       = frame_at(cb.anim, 0);
        const double  progress_fraction = b.cycle_progress / cb.build_time_2;
        const int32_t new_frame         = c.cfg_anim_frame_at_progress(start_frame, progress_fraction);
        set_frame_at(b.anim, 0, new_frame);
    } else {
        // ---- dispatch through the per-building-type tick2 table (0x0047642b-0x0047643d) -----------
        // _G_LLM_STRAT_BLDG_TICK2_FUNCS[building_id](). Dispatched through only, never resolved,
        // named, or inlined -- same posture as sim_unit_tick.cpp's unit_state_funcs dispatch.
        //
        // SIM1-BLDGCB (2026-08-23): the one-time note that says WHOSE callback this table holds.
        // This is the site the item's done_when names, and it is deliberately here rather than
        // inferred from the state tables' `; [dispatch]` line -- that is a different registry,
        // filled by a different registrar at a different boot stage, and the two can disagree.
        mh::sim::note_first_bldg_type_dispatch(
            "tick2", reinterpret_cast<const void *>(v.bldg_tick2_funcs[b.building_id]));
        v.bldg_tick2_funcs[b.building_id]();
    }
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

void bldg_tick_animation_state() {
    sim_state st = state();
    detail::bldg_tick_animation_state(st.read, st.own, live_bldg_tick_animation_state_calls());
}


// ---- the PROMOTION arm (G13/G19's answer for this dispatcher) -------------------------------------
//
// Under `[promote] bldg_tick_animation_state=1` the original entry is overwritten with a JMP here.
// The one callee (llm_cfg_anim_frame_at_progress) and the tick2-table dispatch stay ORIGINAL. See
// sim_building_tick.cpp's own promoted_arm:: for the full rationale (same shape, same G13/G19
// reasoning) -- anonymous-namespace internal linkage for the same ODR reason.
namespace promoted_arm {
namespace {
bool          g_installed = false;
volatile long g_calls     = 0;

void bldg_tick_animation_state() {
    const long c = ++g_calls;
    if (c == 1 || c == 1000 || c == 100000)
        mh::ai::ai_say("; [promote] bldg_tick_animation_state body served call #%ld from game code\n",
                       c);
    sim_state st = state();
    detail::bldg_tick_animation_state(st.read, st.own, live_bldg_tick_animation_state_calls());
}

bool tick_animation_state_promoted() { return g_installed; }
void mark_tick_animation_state_installed(bool on) { g_installed = on; }
} // namespace
} // namespace promoted_arm

} // namespace mh::sim


MH_EXPORT_REPLACE(llm_strat_bldg_tick_animation_state, mh::sim::promoted_arm::bldg_tick_animation_state)

namespace mh::sim {

int install_promotion_bldg_tick_animation_state(int default_on) {
    if (default_on == 0) return 0;
    if (!mh_export_install_llm_strat_bldg_tick_animation_state()) {
        mh::ai::ai_say(
            "; [promote] bldg_tick_animation_state REFUSED -- entry guard mismatch, NOT promoted\n");
        return 0;
    }
    promoted_arm::mark_tick_animation_state_installed(true);
    mh::ai::ai_say("; [promote] bldg_tick_animation_state: llm_strat_bldg_tick_animation_state is "
                   "LIVE -- ours IS the function, there is no original arm in this run\n");
    return 1;
}

} // namespace mh::sim
