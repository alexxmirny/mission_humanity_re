//
// sim/resid/sim_bldg_network_critical.cpp -- see sim_bldg_network_critical.h. Translated from the
// DISASSEMBLY (tmp/decomp_sim_resid/llm_strat_bldg_is_network_critical_00497405.asm), the exported
// `.c` being a draft.
//
#include "sim/resid/sim_bldg_network_critical.h"

#include "addr/mh_calls.gen.h"  // typed callables for the frontier original we still call OUT to
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"
#include "fp/x87.h" // CRT-X87: the shared x87 truncation helpers

namespace mh::sim {

const bldg_network_critical_calls &live_bldg_network_critical_calls() {
    static const bldg_network_critical_calls c = {
        MH_LIBMH_BIND(llm_strat_bldg_power_network_recompute),
    };
    return c;
}

namespace {

// utils_math_trunc @0x004d0596 (`MH_UNAVAILABLE__parameter_storage_not_marshallable` in
// mh_calls.gen.h -- ST0 in, ST0 out, x87-register-only ABI, not marshallable through mh::call::).
// Both call sites in this function (0x00497466, 0x0049754d) are ORDINARY calls to it, not
// compiler-inlined -- reproduced as this TU's own copy of the sim_bldg_find_mothership_position.cpp
// / sim_bldg_power_network_recompute.cpp precedent's exact instruction sequence, per the established
// per-TU convention (not a new shared helper -- rule 4). FISTP width confirmed 32-bit AT THESE SITES
// (opcode bytes `db 5d dc` @0x0049746b -> 0xDB ModRM 0x5D, reg field 3, i.e. 0xDB /3 == FISTP
// m32int; the second site's `db 5d dc` @0x00497552 is byte-identical).
int32_t trunc_to_int32(double val) {
    return ::mh::fp::trunc_i32(val);
}

} // namespace

namespace detail {

// ---- llm_strat_bldg_is_network_critical @0x00497405 ------------------------------------------------
int32_t bldg_is_network_critical(const sim_view &v, sim_store &own, const bldg_network_critical_calls &c,
                                 int32_t player, int32_t b_index) {
    const uint32_t p = static_cast<uint32_t>(player);

    // 0x0049741c-0x00497428: recompute the network first so the counts below reflect the CURRENT
    // state, not stale flags.
    c.bldg_power_network_recompute(static_cast<uint16_t>(player));

    // 0x0049742d-0x00497444: the candidate itself must currently be connected (built_flags & 0x1 --
    // "connected/reached, flood-fill from Mother", per mh_map_object_building's field comment; NOT
    // a generic "built" test) or it is trivially not critical.
    if ((building_of(v, p, b_index).built_flags & 0x1u) == 0) {
        return 0;
    }

    // `buildings[player][0]` is the roster's SENTINEL/COUNT slot, never a real building -- see the
    // header banner and sim_bldg_find_mothership_position.cpp's identical idiom. Held as one mutable
    // reference for the whole function; every use below re-reads/writes THROUGH it, matching the
    // original's own (redundant, non-CSE) per-site address recomputation value-for-value.
    building &sentinel = own.building_at(p, 0);

    // ==== BEFORE count: 0x00497452-0x004974cf =========================================================
    int32_t before_connected = 0;
    {
        int32_t remaining = trunc_to_int32(sentinel.energy);
        for (int32_t slot = 1; slot < v.caps.buildings && remaining != 0; ++slot) {
            const building &b = building_of(v, p, slot);
            // NaN-safe form (see header banner): !(energy <= 0.0), not the draft's `0.0 < energy`.
            if (!(b.energy <= 0.0)) {
                // 0x004974a8-0x004974ab: decrement happens unconditionally once energy>0, BEFORE the
                // built_flags gate below.
                --remaining;
                if ((b.built_flags & 0x1u) != 0) {
                    ++before_connected;
                }
            }
        }
    }

    // ==== simulate the candidate going offline: 0x004974cf-0x00497534 =================================
    building    &candidate    = own.building_at(p, b_index); // no bounds check -- matches the original
    const double saved_energy = candidate.energy;            // 0x004974df-0x004974ee: raw 8-byte snapshot
    candidate.energy          = 0.0;                         // 0x00497501-0x0049750b
    // 0x00497515-0x00497528: NOT the candidate -- adds the tuning constant to the SENTINEL slot's
    // `.energy` (the headcount field), book-keeping "one fewer live building" for the next scan.
    // DECLARED NEED: *v.bldg_count_offline_decrement does not exist yet -- see header banner.
    sentinel.energy = sentinel.energy + *v.bldg_count_offline_decrement;
    c.bldg_power_network_recompute(static_cast<uint16_t>(player));

    // ==== AFTER count: 0x00497539-0x004975b6 -- SAME TABLE as the BEFORE count above (see header
    // banner's "the two scan loops" note), just re-scanned after the simulated-offline recompute ====
    int32_t after_connected = 0;
    {
        int32_t remaining = trunc_to_int32(sentinel.energy);
        for (int32_t slot = 1; slot < v.caps.buildings && remaining != 0; ++slot) {
            const building &b = building_of(v, p, slot);
            if (!(b.energy <= 0.0)) {
                --remaining;
                if ((b.built_flags & 0x1u) != 0) {
                    ++after_connected;
                }
            }
        }
    }

    // 0x004975b6-0x004975f3: restore the candidate, bump the sentinel headcount by 1.0 (NOT
    // necessarily the exact inverse of the earlier decrement -- see header banner's PRESERVE-BUG
    // note), recompute a third time to leave real state as it was before this call.
    candidate.energy = saved_energy;
    sentinel.energy  = sentinel.energy + 1.0;
    c.bldg_power_network_recompute(static_cast<uint16_t>(player));

    // 0x004975f8-0x00497614: critical iff going offline drops the connected count by MORE than 1.
    return (after_connected + 1 < before_connected) ? 1 : 0;
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

int32_t bldg_is_network_critical(int32_t player, int32_t b_index) {
    sim_state st = state();
    return detail::bldg_is_network_critical(st.read, st.own, live_bldg_network_critical_calls(), player, b_index);
}

} // namespace mh::sim
