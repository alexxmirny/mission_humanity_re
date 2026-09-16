//
// sim/sim_bldg_instant_construct.cpp -- see sim_bldg_instant_construct.h. Translated from the
// DISASSEMBLY (tmp/decomp/llm_bldg_queue_construction_00462d89.asm), not from the Ghidra .c draft
// (the draft's guessed llm_bldg_construct_finalize call-argument order does not match the raw
// register setup at the call site -- see the header banner's step 3a for the re-derivation against
// that sibling's own already-committed prototype).
//
#include "sim/sim_bldg_instant_construct.h"

#include "addr/mh_calls.gen.h"  // mh::call::llm_bldg_footprint_is_clear / game_HandleProgress /
                                // llm_bldg_construct_finalize -- bound live in
                                // live_bldg_instant_construct_calls()
#include "ai/ai_state.h"        // ai_say / trace_budget -- the shared trace sink, not AI state
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const bldg_instant_construct_calls &live_bldg_instant_construct_calls() {
    static const bldg_instant_construct_calls gc = {
        MH_LIBMH_BIND(llm_bldg_footprint_is_clear),
        MH_LIBMH_BIND(game_HandleProgress),
        MH_LIBMH_BIND(llm_bldg_construct_finalize),
    };
    return gc;
}

namespace detail {

int32_t bldg_instant_construct(const sim_view &v, sim_store &own,
                               const bldg_instant_construct_calls &c, int32_t player,
                               int32_t building_type, int32_t x, int32_t y) {
    // 0x00462daa: local_10 (the return value / created building's roster slot) starts at 0
    // (failure/blocked).
    int32_t result = 0;

    // 0x00462db1-0x00462dc6: placement check, viewer hardcoded 8 (omniscient/server-side check).
    // Register setup at the call site (EAX=x, EDX=y, EBX=building_type, ECX=viewer) matches
    // llm_bldg_footprint_is_clear's own committed prototype exactly. A blocked footprint (0) skips
    // the whole rest of the body and returns 0.
    if (c.footprint_is_clear(x, y, building_type, 8) != 0) {
        // Read once; a real reference into live cfg memory (not a snapshot), so nothing below can
        // observe a stale value even across the outward calls -- see the header banner.
        const cfg_building &b = v.cfg_buildings[building_type];

        // 0x00462dcc-0x00462ded: progress[player][invention].available == 0 (false) gates the
        // auto-unlock. progress_of() is sim_state.h's own player*PROGRESS_ROW_COUNT+row helper.
        if (progress_of(v, player, b.invention).available == 0) {
            // 0x00462def-0x00462e01: game_HandleProgress(player, invention) -- EAX=player (word-
            // truncated, matching the committed uint16_t param), EDX=invention.
            c.handle_progress(static_cast<uint16_t>(player), b.invention);
        }

        // 0x00462e0b-0x00462e24: llm_bldg_construct_finalize(param_1=-2, y_b=y, player, param_4=1,
        // x_b=x, building_id=building_type) -- argument order and types match the sibling's own
        // already-committed prototype exactly (addr/mh_calls.gen.h), not the Ghidra .c draft's
        // guessed ordering.
        result = c.construct_finalize(0xfffffffeu, y, static_cast<uint16_t>(player),
                                      static_cast<char>(1), x, building_type);

        if (result != 0) {
            // 0x00462e2d-0x00462e50: seed the new instance's construction-progress clock.
            // _DAT_0050114c is the double constant -3.0 (confirmed by read-memory, per the batch
            // context) -- written as a literal `- 3.0`, not a named global, per the conductor's
            // instruction.
            own.building_at(static_cast<uint32_t>(player), result).cycle_progress =
                b.build_time_2 - 3.0;
        }
    }

    return result;
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

int32_t bldg_instant_construct(int32_t player, int32_t building_type, int32_t x, int32_t y) {
    sim_state st = state();
    return detail::bldg_instant_construct(st.read, st.own, live_bldg_instant_construct_calls(),
                                          player, building_type, x, y);
}


} // namespace mh::sim
