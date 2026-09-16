//
// sim/sim_population_add.cpp -- see sim_population_add.h. Translated from the DISASSEMBLY
// (tmp/decomp/llm_strat_population_add_00491328.asm). The growth-formula FP operation order, the
// count==0-only housing clamp, the inlined trunc, and the PlayerSide-gated UI event were re-derived
// from the listing per house rules.
//
#include "sim/sim_population_add.h"

#include "addr/mh_calls.gen.h"  // mh::call::game_SetEvent
#include "ai/ai_state.h"        // ai_say / trace_budget -- the shared trace sink, not AI state
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"
#include "fp/x87.h" // CRT-X87: the shared x87 truncation helpers

namespace mh::sim {

const population_add_calls &live_population_add_calls() {
    static const population_add_calls c{
        MH_LIBMH_BIND(game_SetEvent),
    };
    return c;
}

namespace detail {

namespace {
// utils_math_trunc @0x004d0596 inlined + the caller's FLD/FISTP, identical to
// sim_unit_population_remove.cpp's helper (same call-site shape: FLD pop_fraction AS a double, trunc
// toward zero, FISTP under the restored round-to-nearest word). 0x004913bc FLD / 0x004913c2 CALL /
// 0x004913c7 FISTP.
int32_t trunc_to_int32(double val) {
    return ::mh::fp::trunc_i32(val);
}
} // namespace

void population_add(const sim_view &v, sim_store &own, const population_add_calls &c, uint16_t player,
                    int32_t count) {
    pop_stats &ps = own.population_at(player);

    if (count == 0) {
        // 0x00491363-0x004913af: FILD human / FMUL pop_growth_factor / FMULP (hp_sum+1) / FDIVP
        // (hp_max_sum+1) / FADD pop_fraction. Kept in that multiply order (FP is not associative).
        ps.pop_fraction = (static_cast<double>(ps.human) * (*v.pop_growth_factor) *
                           static_cast<double>(ps.colony_hp_sum + 1)) /
                              static_cast<double>(ps.colony_hp_max_sum + 1) +
                          ps.pop_fraction;
    } else {
        // 0x0049134b-0x0049135b: pop_fraction += (double)count.
        ps.pop_fraction = static_cast<double>(count) + ps.pop_fraction;
    }

    // 0x004913b5-0x004913d4: pop_total = trunc(pop_fraction).
    ps.pop_total = trunc_to_int32(ps.pop_fraction);

    // 0x004913da-0x00491431: clamp to housing ONLY on the growth path (count==0), and only if it
    // exceeds the latched capacity.
    if (count == 0 && ps.housing_prev < ps.pop_total) {
        ps.pop_total    = ps.housing_prev;
        ps.pop_fraction = static_cast<double>(ps.pop_total);
    }

    // 0x00491431-0x00491461: idle population = total - employed - in-field.
    ps.human = (ps.pop_total - ps.workers_employed) - ps.human_in_field;

    // 0x0049146a-0x00491478: notify the build/projects UI panel, local human player only (16-bit
    // compare against PlayerSide).
    if (player == static_cast<uint16_t>(*v.player_side)) {
        c.set_event(POPULATION_ADD_BUILD_PROJECTS_REFRESH);
    }
}

} // namespace detail

// ---- the public surface --------------------------------------------------------------------------

void population_add(uint16_t player, int32_t count) {
    sim_state st = state();
    detail::population_add(st.read, st.own, live_population_add_calls(), player, count);
}


} // namespace mh::sim


namespace mh::sim {
} // namespace mh::sim
