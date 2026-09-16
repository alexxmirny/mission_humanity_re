//
// sim/libtrans/sim_lt_prod_transfer.cpp -- see sim_lt_prod_transfer.h. Translated from the
// DISASSEMBLY (tmp/decomp_lib_trans/llm_strat_prod_transfer_progress_00490368.asm), not from
// Ghidra's C draft (the draft is a faithful transcription -- see the FP note below for the one place
// this translation looks past it anyway).
//
#include "sim/libtrans/sim_lt_prod_transfer.h"

#include <cmath> // std::isnan -- see journey_copy_is_arrived() below

#include "ai/ai_state.h" // ai_say / trace_budget -- the shared trace sink, not AI state

namespace mh::sim {

namespace {

// Reproduces 0x004903d7-0x004903e2 (FLDZ; FCOMP copy; FNSTSW AX; SAHF; JNC) bit-for-bit.
//
// FLDZ pushes 0.0 as ST(0); FCOMP compares ST(0) against `copy` (the memory operand) and leaves
// C3/C2/C0 in the FPU status word; FNSTSW AX + SAHF map C0->CF, C2->PF, C3->ZF. Per the standard x87
// condition-code table:
//     ST(0) >  copy : C3,C2,C0 = 0,0,0  -> CF=0
//     ST(0) <  copy : C3,C2,C0 = 0,0,1  -> CF=1
//     ST(0) == copy : C3,C2,C0 = 1,0,0  -> CF=0
//     unordered     : C3,C2,C0 = 1,1,1  -> CF=1
// so CF=1 for BOTH "0.0 < copy" and "unordered" (copy is NaN); JNC (the branch taken here, landing
// on the 1.0 result) fires exactly when CF=0, i.e. when the compare is ORDERED and "0.0 >= copy".
//
// That is bit-for-bit what a plain IEEE-754 relational operator already computes: C++ defines every
// relational comparison against a NaN operand to evaluate false, which excludes the unordered case
// the same way CF's "unordered => JNC not taken" does. So a plain `copy <= 0.0` would reproduce this
// branch exactly, NaN included, needing no extra case -- but this is written with the ordered check
// spelled out explicitly rather than folded into `<=`, because the batch's own hazard notes read
// this branch as "copy<=0.0 (and NaN) yields 1.0" and that is NOT what the condition-code table
// above says: a NaN `travel_duration_copy` leaves CF=1, so JNC is NOT taken, and the ORIGINAL falls
// through into the division below (whose own NaN then propagates out through the return value) --
// it does not take the 1.0 arm. Flagged in the translation report's uncertainties for a reviewer to
// re-check; this function is written to match the derivation above, not the hazard note's wording.
bool journey_copy_is_arrived(double copy) { return !std::isnan(copy) && copy <= 0.0; }

} // namespace

namespace detail {

double prod_transfer_progress(const sim_view &v, int32_t slot) {
    // 0x00490383: MOVZX, not MOVSX -- PlayerSide (sim_view::player_side, a signed int16_t) is
    // zero-extended, matching the original's unsigned widening (header hazard 2).
    const uint16_t player = static_cast<uint16_t>(*v.player_side);

    // 0x0049038d-0x0049039b (and re-derived three more times in the original -- see header hazard 3):
    // player * PROD_SHUTTLE_SLOTS_PER_PLAYER + slot is the same byte address as the asm's
    // player*0x1f18 + slot*0x31c, since 0x1f18 == PROD_SHUTTLE_SLOTS_PER_PLAYER * sizeof(record) and
    // 0x31c == sizeof(record). Resolved once here; the original's four re-derivations are an
    // optimisation difference, not a semantic one (batch context, confirmed).
    const int32_t            index = static_cast<int32_t>(player) * PROD_SHUTTLE_SLOTS_PER_PLAYER + slot;
    const prod_shuttle_slot &rec   = v.prod_shuttle_slots[index];

    // 0x0049039d/0x004903b7: both 16-bit compares (header hazard 4). Free/unbound slot
    // (type_ref_id == 0) or not currently in transit (status != 0xc8/200) both fall to -1.0.
    if (rec.type_ref_id == 0 || rec.status != 0xc8) {
        return -1.0; // {0, 0xbff00000} -- byte-identical to the original's dword-immediate pair
                     // (0x0049043b/0x00490442); see header hazard 6.
    }

    // 0x004903d7-0x004903e2: see journey_copy_is_arrived() above for the full x87 derivation.
    if (journey_copy_is_arrived(rec.travel_duration_copy)) {
        return 1.0; // {0, 0x3ff00000} -- byte-identical to the original's dword-immediate pair
                    // (0x0049042b/0x00490432); see header hazard 6.
    }

    // 0x00490404-0x00490420: FLD copy; FSUB duration; FDIV copy -- fixed order, do not rearrange
    // (header hazard 7).
    return (rec.travel_duration_copy - rec.travel_duration) / rec.travel_duration_copy;
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

double prod_transfer_progress(int32_t slot) {
    const sim_view v = state().read;
    return detail::prod_transfer_progress(v, slot);
}


} // namespace mh::sim
