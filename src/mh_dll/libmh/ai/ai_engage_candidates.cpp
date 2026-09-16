//
// ai/ai_engage_candidates.cpp -- see ai_engage_candidates.h. Translated from the DISASSEMBLY
// (tmp/decomp_a3/llm_strat_ai_engage_candidate_add_004d50b3.asm), not from Ghidra's C: the decompile
// renders the dedup loop's stashed pre-increment count as a second local (`uVar2`) used only for the
// final store, which reads as intentional but is just Ghidra materialising a value that is, in the
// assembly, the SAME read of the count register re-issued at the append site. There is no
// register-provenance ambiguity here, just a decompile that looks more deliberate than it is.
//
#include "ai/ai_engage_candidates.h"


namespace mh::ai {
namespace detail {

void engage_candidate_add(const ai_view &v, const ai_store &own, uint32_t target_ref,
                          int32_t target_index) {
    // ---- liveness test -----------------------------------------------------------------------
    //
    // Roster selector: (target_ref & 0xa0) == 0 -> BUILDING (ref_is_building_by_a0) -- verified off
    // the `TEST CL,0xa0 / JZ <building-branch>` at 0x004d50c9, not assumed from the sibling
    // functions. This is the SAME polarity engage_select_and_commit's own liveness check
    // (target_ref_is_alive) and the sort/commit consumers use; ai_engage_scan.cpp's
    // engage_partition_turret_candidates and ai_engage.cpp's pick_first_survivable read the OTHER
    // live polarity (0x40) over the identical field.
    const uint32_t owner = ref_owner(target_ref);
    double         energy;
    if (ref_is_building_by_a0(target_ref)) {
        energy = building_of(v, owner, (int32_t)target_index).energy;
    } else {
        energy = unit_of(v, owner, (int32_t)target_index).energy;
    }

    // The original runs `FLDZ; FCOMP <energy>; FNSTSW AX; SAHF` on BOTH branches, then tests the
    // resulting CF: the unit branch takes `JC` INTO the append path (0x004d50e6), the building branch
    // takes `JNC` OUT to the return (0x004d5124). Both therefore proceed on the identical bit-level
    // condition CF==1, i.e. ST(0) < source (0.0 < energy) by the x87 FCOM convention (C0/C2/C3 ->
    // CF/PF/ZF via SAHF).
    //
    // THE PREDICATE IS SPELLED `energy <= 0.0` RATHER THAN `!(energy > 0.0)` ON PURPOSE, and the two
    // are not the same function. With the x87 exceptions masked (the game's setting) an UNORDERED
    // compare sets C3/C2/C0 = 1/1/1, so CF == 1 and the original APPENDS on a NaN energy. `!(energy >
    // 0.0)` is true for NaN and would return instead -- the one input class on which a natural
    // reading of this branch diverges from the machine. `energy <= 0.0` is false for NaN and falls
    // through to the append, which is what the original does. No known path produces a NaN here, so
    // this costs nothing and closes the case rather than carrying it.
    if (energy <= 0.0) return;

    // ---- dedup scan, over the WHOLE live count, both fields -----------------------------------
    //
    // `CMP EDX,[count] / JC` -- UNSIGNED compare, as everywhere else this scratch is walked.
    for (uint32_t i = 0; i < (uint32_t)*own.engage_scratch_count; ++i) {
        const engage_candidate &c = own.engage_scratch[i];
        if (c.target_ref == target_ref && c.target_index == target_index) return;
    }

    // ---- append --------------------------------------------------------------------------------
    //
    // Only the first two dwords of the record are written (0x004d5155 / 0x004d515b, both plain
    // 32-bit MOVs). The third, dist_sq at +0x8, is left stale on purpose -- engage_sort_candidates_by
    // _dist fills it later, and nothing here initialises it. No capacity check against
    // ENGAGE_SCRATCH_CAP; the original does not have one.
    const uint32_t slot                   = (uint32_t)*own.engage_scratch_count;
    own.engage_scratch[slot].target_ref   = target_ref;
    own.engage_scratch[slot].target_index = target_index;
    ++*own.engage_scratch_count;
}

} // namespace detail

// ---- the public wrapper -------------------------------------------------------------------------

void engage_candidate_add(uint32_t target_ref, int32_t target_index) {
    const ai_state st = state();
    detail::engage_candidate_add(st.read, st.own, target_ref, target_index);
}


} // namespace mh::ai
