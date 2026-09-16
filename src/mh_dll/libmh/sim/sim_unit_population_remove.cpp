//
// sim/sim_unit_population_remove.cpp -- see sim_unit_population_remove.h. Translated from the
// DISASSEMBLY (tmp/decomp/llm_strat_population_remove_00491486.asm), not from Ghidra's C: the draft's
// overall shape (decay-vs-direct-subtract, trunc/round, the two clamps, the layoff+event tail) reads
// correctly, but its `fVar1 = utils_math_trunc(...); pop_total = (int)ROUND(fVar1);` pair was re-walked
// against the raw CALL/FISTP bytes rather than trusted -- see the header HAZARD note on why that is a
// real ordinary call followed by a separate rounding store, not a decompiler double-rounding artifact.
//
#include "sim/sim_unit_population_remove.h"

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"        // ai_say / trace_budget -- the shared trace sink, not AI state
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"
#include "fp/x87.h" // CRT-X87: the shared x87 truncation helpers

namespace mh::sim {

const population_remove_calls &live_population_remove_calls() {
    static const population_remove_calls c = {
        MH_LIBMH_BIND(llm_strat_population_layoff_workers),
        MH_LIBMH_BIND(game_SetEvent),
    };
    return c;
}

namespace {

// utils_math_trunc @0x004d0596 (`MH_UNAVAILABLE__parameter_storage_not_marshallable` in
// mh_calls.gen.h -- ST0 in, ST0 out, x87-register-only, no stack-passable signature). This function's
// one call site (0x00491507) is an ORDINARY call to it, not a compiler-inlined copy of its body --
// same situation sim_unit_refund.cpp's refund_amount() / sim_unit_update_soldiers.cpp's
// trunc_axis_delta document, reproduced here as the identical instruction sequence rather than reached
// through mh::call or substituted with std::trunc.
//
// UNLIKE those two precedents (which FILD a dword int, then FMUL it into a double before truncating),
// this call site FLDs pop_fraction directly AS a double (0x00491501 `FLD double ptr [...]`) -- there is
// no int-to-double promotion or multiply folded into this particular call. The whole block -- the
// caller's FLD, the (inlined) trunc-toward-zero control-word swap, and the caller's own FISTP under its
// restored round-to-nearest control word -- is reproduced as ONE __asm block, matching the header
// HAZARD note on why this is "trunc, then a separate round" rather than one fused op.
//
// WIDTH VERIFIED AT THIS SITE: opcode bytes at 0x0049150c are `db 5d e0` -> 0xDB, ModRM 0x5D
// (mod=01,reg=011,rm=101) -> reg field 3 -> 0xDB /3 == FISTP m32int. Matches the 32-bit-FISTP
// precedent group (sim_unit_refund.cpp / sim_unit_update_soldiers.cpp), not the 64-bit alternative.
int32_t trunc_to_int32(double val) {
    return ::mh::fp::trunc_i32(val);
}

} // namespace

namespace detail {

void population_remove(const sim_view &v, sim_store &own, const population_remove_calls &c,
                       uint32_t player, int32_t count) {
    // Every use of `player` in the original reads only the low 16 bits of the stored parameter
    // (0x004914a9, 0x004915dc, 0x004915e8, and every pop_stats-index IMUL) -- see the header note.
    // Masked once here and reused for both the pop_stats index and the two outward-call/comparison
    // uses below.
    const uint16_t p = static_cast<uint16_t>(player);

    pop_stats &ps = own.population_at(p);

    // ---- (1) decay curve vs. direct subtract (0x004914a3-0x004914fa) ---------------------------
    if (count == 0) {
        // pop_fraction -= (pop_total_OLD - housing_prev) * POP_DECAY_FACTOR. `pop_total` here is its
        // value from BEFORE step (2) below overwrites it -- read first, matching the asm's own
        // read-before-recompute order. The (pop_total - housing_prev) difference is computed in the
        // INTEGER domain first (0x004914d6-0x004914dc, SUB not FSUB), THEN converted to double --
        // reproduced as `(double)(int - int)`, not `(double)int - (double)int`.
        ps.pop_fraction = ps.pop_fraction - (double)(ps.pop_total - ps.housing_prev) * (*v.pop_decay_factor);
    } else {
        ps.pop_fraction = ps.pop_fraction - (double)count;
    }

    // ---- (2) pop_total = round-to-int32(trunc(pop_fraction)) (0x004914fa-0x0049150f) -------------
    // See trunc_to_int32() above and the header HAZARD note.
    ps.pop_total = trunc_to_int32(ps.pop_fraction);

    // ---- (3) zero both fields if pop_total rounded to exactly 0 (0x0049151f-0x0049154a) ----------
    // The original does two separate dword stores covering the double's 8 bytes; a plain `= 0.0`
    // assignment reproduces the identical all-zero bit pattern.
    if (ps.pop_total == 0) {
        ps.pop_fraction = 0.0;
    }

    // ---- (4) clamp UP to human_in_field if pop_total fell below it (0x0049154a-0x0049159a) --------
    if (ps.pop_total < ps.human_in_field) {
        ps.pop_total    = ps.human_in_field;
        ps.pop_fraction = (double)ps.human_in_field;
    }

    // ---- (5) worker_count = human + human_in_field + workers_employed - pop_total (0x0049159a-
    // 0x004915d3) -----------------------------------------------------------------------------------
    const int32_t worker_count = ps.human + ps.human_in_field + ps.workers_employed - ps.pop_total;

    // ---- (6) layoff + local-player-only event (0x004915d3-0x004915fb) -----------------------------
    if (worker_count > 0) {
        c.population_layoff_workers(static_cast<int32_t>(p), worker_count);
        if (static_cast<int16_t>(p) == *v.player_side) {
            c.set_event(EVENT_BUILD_PROJECTS_REFRESH);
        }
    }
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

void population_remove(uint32_t player, int32_t count) {
    sim_state st = state();
    detail::population_remove(st.read, st.own, live_population_remove_calls(), player, count);
}


} // namespace mh::sim
