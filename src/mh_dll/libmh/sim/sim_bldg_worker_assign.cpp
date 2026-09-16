#include "sim/sim_bldg_worker_assign.h"

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"        // ai_say / trace_budget -- the shared trace sink, not AI state
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const worker_assign_calls &live_worker_assign_calls() {
    static const worker_assign_calls gc = {
        MH_LIBMH_BIND(llm_strat_bldg_add_workers),
        MH_LIBMH_BIND(llm_strat_bldg_remove_workers),
        MH_LIBMH_BIND(llm_strat_bldg_notify_state_change),
    };
    return gc;
}

namespace detail {

int32_t assign_workers(const sim_view &v, sim_store &own, const worker_assign_calls &gc, uint32_t player,
                       uint32_t building_id, int32_t count) {
    // 0x00491b8e/0x00491b97: the full 32-bit player is stashed, but every subsequent use re-reads it
    // through a 16-bit MOVZX -- only the low 16 bits ever participate (see the header banner).
    const uint16_t p16 = (uint16_t)player;

    // 0x00491ba1-0x00491bb6: clamp the requested count DOWN to population[p16].human when it exceeds
    // it -- SIGNED compare (JLE skips the clamp, i.e. no clamp needed when count <= human). This is a
    // local variable, NOT a write through population[p16].human itself.
    int32_t clamped_count = count;
    if (clamped_count > v.population[p16].human) {
        clamped_count = v.population[p16].human;
    }

    // 0x00491bbf-0x00491bc8: call the ORIGINAL add_workers with the CLAMPED request (EBX=clamped_
    // count, EDX=building_id, EAX=p16). Its return is the ACTUAL number of workers moved -- the callee
    // can clamp further -- and THAT value, not clamped_count, drives every write below.
    const int32_t actual = gc.add_workers(p16, building_id, clamped_count);

    // 0x00491bcb-0x00491be8: opposite-direction bookkeeping by the actual amount moved -- idle human
    // population decreases, employed-workers count increases.
    own.population_at(p16).human -= actual;
    own.population_at(p16).workers_employed += actual;

    // 0x00491beb-0x00491bf2: unconditional notify, regardless of actual (even when it is 0).
    gc.notify_state_change(p16, building_id);

    return actual;
}

int32_t unassign_workers(sim_store &own, const worker_assign_calls &gc, uint16_t player,
                         uint32_t building_index, uint32_t count) {
    // 0x00491c27-0x00491c36: call the ORIGINAL remove_workers with the UNCLAMPED requested count
    // (EBX=count, EDX=building_index, EAX=player). Its return is the ACTUAL amount removed -- the
    // callee can clamp -- and THAT value, not count, drives every write below.
    const int32_t actual = (int32_t)gc.remove_workers(player, building_index, count);

    // 0x00491c39-0x00491c59: opposite-direction bookkeeping vs assign_workers -- idle human population
    // increases, employed-workers count decreases.
    own.population_at(player).human += actual;
    own.population_at(player).workers_employed -= actual;

    // 0x00491c5c-0x00491c60: unconditional notify.
    gc.notify_state_change(player, building_index);

    return actual;
}

} // namespace detail

// ---- the public surface --------------------------------------------------------------------------

int32_t assign_workers(uint32_t player, uint32_t building_id, int32_t count) {
    sim_state st = state();
    return detail::assign_workers(st.read, st.own, live_worker_assign_calls(), player, building_id, count);
}

int32_t unassign_workers(uint16_t player, uint32_t building_index, uint32_t count) {
    sim_state st = state();
    return detail::unassign_workers(st.own, live_worker_assign_calls(), player, building_index, count);
}


} // namespace mh::sim
