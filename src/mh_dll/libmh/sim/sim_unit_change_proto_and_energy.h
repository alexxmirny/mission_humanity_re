//
// sim/sim_unit_change_proto_and_energy.h -- the unit proto/energy delta helper (RI-SIM / SIM1-G1).
//
// One function: llm_strat_unit_change_proto_and_energy @0x00489521 (0x74 bytes), `void __watcall
// llm_strat_unit_change_proto_and_energy(uint16_t player, int32_t unit_idx, int16_t proto_delta,
// int32_t unused, double energy_delta)` per the committed prototype
// (mh_calls.gen.h's sig_llm_strat_unit_change_proto_and_energy).
//
// ---- SHAPE (see the .asm/.c for the full byte-by-byte derivation) -----------------------------
// Two roster writes on `units[player][unit_idx]`, then one outward call:
//   1. (0x00489537-0x0048955d) `unit_proto_id += proto_delta` -- a 16-bit `ADD word ptr [...],AX`.
//      The field is `uint16_t`; the delta arrives signed (`short`, storage=BX:2). This is a
//      same-width wraparound add, not a widened/saturating one -- reproduced as
//      `static_cast<uint16_t>(unit_proto_id + proto_delta)`.
//   2. (0x0048955d-0x0048957f) `energy += energy_delta` -- a plain x87 FADD/FSTP, both operands
//      `double`. Recomputes the row/column IMUL a second time (same values as step 1's), matching
//      the original's own fresh-arithmetic-per-access idiom every sim TU reproduces via
//      `unit_at()`'s reference-not-pointer contract.
//   3. (0x00489582-0x0048958b) `llm_strat_unit_soldiers_start_walk_anim(player, unit_idx)` --
//      refreshes the unit's mounted-soldier walk animation. An ORIGINAL function outside this
//      batch, called through the `_calls` indirection like every other sim/ TU.
//
// ---- THE FOURTH PARAMETER (`unused`, ECX) IS GENUINELY DEAD ------------------------------------
// The prologue (0x00489537-0x0048953d) only spills EAX/EDX/EBX (player/unit_idx/proto_delta) to
// the stack; ECX is never read, stored, or passed on. The committed prototype still carries it
// (register-slot contract for the __watcall caller), so the parameter is declared here and simply
// never referenced in the body -- same convention sim_bldg_roster_queries.cpp's own `unused`
// parameter already establishes in this tree.
//
// ---- energy_delta's SIGN AT THE CALL SITE -------------------------------------------------------
// This function does not itself constrain the sign of `energy_delta` -- it is a plain add. (The
// batch context's note about a `-1.0` FADD constant belongs to two OTHER functions in this same
// slice, `remove_silent` and `teardown_mapped`, not to this one -- this function's energy_delta is
// a caller-supplied parameter, never an anonymous FP literal.)
//
#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// Indirected for the same reason as every other sim/ TU: a direct mh::call:: inside a detail::
// body reaches into the live game image, making the body untestable by net_selftest.exe simtest.
struct unit_change_proto_and_energy_calls {
    // llm_strat_unit_soldiers_start_walk_anim @0x004897c7 -- ORIGINAL, out of scope this batch.
    void (*unit_soldiers_start_walk_anim)(uint32_t player, int32_t unit_index);
};

const unit_change_proto_and_energy_calls &live_unit_change_proto_and_energy_calls();

namespace detail {

// llm_strat_unit_change_proto_and_energy @0x00489521. See the header derivation above.
void unit_change_proto_and_energy(sim_store &own, const unit_change_proto_and_energy_calls &c,
                                  uint16_t player, int32_t unit_idx, int16_t proto_delta,
                                  int32_t unused, double energy_delta);

} // namespace detail

// Live wrapper: the logic applied to state().own and live_unit_change_proto_and_energy_calls().
// Matches the original's committed __watcall(AX,EDX,BX,ECX,Stack[0x4]) shape
// (sig_llm_strat_unit_change_proto_and_energy).
void unit_change_proto_and_energy(uint16_t player, int32_t unit_idx, int16_t proto_delta,
                                  int32_t unused, double energy_delta);

namespace detail {
} // namespace detail

} // namespace mh::sim
