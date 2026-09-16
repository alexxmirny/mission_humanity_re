#include "sim/sim_pathfind_route_leg_group_and_sort.h"

#include "addr/mh_calls.gen.h"  // typed callables for the ORIGINAL functions this closure still calls out to
#include "ai/ai_state.h"        // ai_say / trace_budget -- the shared trace sink, not AI state
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"
#include "fp/x87.h"         // CRT-X87: the shared x87 truncation helpers
#include "crt/crt_select.h" // LIB-CRT: MH_CRT() picks the vendored CRT in the standalone build

namespace mh::sim {

const pathfind_route_leg_group_and_sort_calls &live_pathfind_route_leg_group_and_sort_calls() {
    static const pathfind_route_leg_group_and_sort_calls c{
        MH_CRT(struct_array_malloc_impl),
        MH_CRT(utils_free),
        MH_CRT(qsort),
        MH_LIBMH_BIND(llm_strat_slot_dist_to_ref),
        MH_LIBMH_BIND(llm_strat_claim_free_slots_within_dist),
        MH_LIBMH_BIND(llm_strat_pathfind_route_leg_reconcile),
        MH_CRT_CMP(group_move_scratch_cmp_dist_004cbe0c, &detail::group_move_scratch_cmp_dist),
        MH_CRT_CMP(group_move_scratch_cmp_wave_rank_004cbe42, &detail::group_move_scratch_cmp_wave_rank),
    };
    return c;
}

namespace detail {

namespace {
// utils_math_trunc @0x004d0596 inlined + the caller's FILD/FMUL/FADD/FISTP, identical shape to
// sim_population_add.cpp's trunc_to_int32 (same call-site idiom: an x87 double already on ST(0),
// trunc toward zero, FISTP under the restored round-to-nearest word). 0x004cbb82-0x004cbb95.
int32_t trunc_to_int32(double val) {
    return ::mh::fp::trunc_i32(val);
}
} // namespace

// ---- FUN_004cbe0c / FUN_004cbe42: the two qsort comparators (LIB-CRT) ---------------------------
//
// Transcribed 2026-09-08. Both open with the inert `PUSH 0x24 / CALL utils_assert_stack_capacity`
// prologue and then do one subtraction; the rest of each body is Watcom spilling its two register
// arguments to the frame and reading them straight back (0x004cbe23-0x004cbe31), which is codegen,
// not behaviour.
//
// THE SUBTRACTION IS THE CONTRACT, not a comparison: `SUB EDX,[EAX]` wraps, so two operands more
// than INT32_MAX apart return a value whose SIGN IS INVERTED. That is what the game does, and both
// key domains here are bounded well inside it (a squared tile distance, and a wave rank), so the
// wrap is unreachable rather than tolerated. Spelled in unsigned arithmetic because reproducing a
// wrapping signed subtraction in signed C++ is undefined behaviour, not a faithful translation --
// the same reason crt/crt_string.h's atoi accumulates unsigned.
int32_t group_move_scratch_cmp_dist(void *a, void *b) {
    int32_t ka, kb;
    std::memcpy(&ka, a, sizeof(ka)); // 0x004cbe2c  MOV EDX,[EAX]
    std::memcpy(&kb, b, sizeof(kb)); // 0x004cbe31  SUB EDX,[EAX]
    return static_cast<int32_t>(static_cast<uint32_t>(ka) - static_cast<uint32_t>(kb));
}

int32_t group_move_scratch_cmp_wave_rank(void *a, void *b) {
    // Offset 8 is `wave_rank` in llm_strat_group_scratch_member (0x004cbe62 / 0x004cbe68).
    int32_t ka, kb;
    std::memcpy(&ka, static_cast<const uint8_t *>(a) + 8, sizeof(ka));
    std::memcpy(&kb, static_cast<const uint8_t *>(b) + 8, sizeof(kb));
    return static_cast<int32_t>(static_cast<uint32_t>(ka) - static_cast<uint32_t>(kb));
}

int32_t pathfind_route_leg_group_and_sort(const sim_view &v, sim_store &own,
                                          const pathfind_route_leg_group_and_sort_calls &c,
                                          uint32_t leg_count, int32_t ref_x, int32_t ref_y) {
    // 0x004cba44-0x004cba51: leg_count < 1, SIGNED compare (`CMP ..,0x1 / JGE`) -- nothing to do.
    if ((int32_t)leg_count < 1) return 0;

    // 0x004cba56-0x004cba6d: exactly one leg is trivially its own wave (rank 0); no sort/claim pass
    // is needed at all.
    if (leg_count == 1) {
        own.group_move_scratch_at(0).wave_rank = 0;
        return 1;
    }

    // 0x004cba72-0x004cba91: stash the caller's ref point and the map's half-extents for the
    // frontier callees (slot_dist_to_ref / claim_free_slots_within_dist) to read without an explicit
    // parameter -- already-wired sim_store accessors (SIM-G-PREP), no view/store gap here.
    own.group_move_dist_ref_x_mut() = ref_x;
    own.group_move_dist_ref_y_mut() = ref_y;
    // 0x004cba82-0x004cbaa5: `EDX=x; EDX=SAR(x,31); EAX=x-EDX; EAX=SAR(EAX,1)` -- the standard
    // truncating-toward-zero divide-by-2 sequence, bit-identical to C's `/2` for every int32 input
    // (see the header banner's note; NOT the general shift-vs-arithmetic hazard rule 8 warns about,
    // which is about idioms that DO differ from `/N` on negatives).
    own.group_move_dist_half_width_mut()  = *v.map_width / 2;
    own.group_move_dist_half_height_mut() = *v.map_height / 2;

    // 0x004cbaaa-0x004cbac7: a temporary {dist:int32, orig_index:int32} scratch pair per leg. On
    // allocation failure, bail out returning 0 (same "nothing to do" result as leg_count<1).
    void *scratch = c.struct_array_malloc_impl(leg_count, 8u);
    if (scratch == nullptr) return 0;
    auto *dist_buf = static_cast<int32_t *>(scratch); // [i*2+0]=dist, [i*2+1]=orig_index

    // 0x004cbacc-0x004cbb17: reset every leg's persistent wave_rank to "unclaimed" (-1) and fill the
    // scratch pair from llm_strat_slot_dist_to_ref.
    for (uint32_t i = 0; i < leg_count; ++i) {
        own.group_move_scratch_at((int32_t)i).wave_rank = -1;
        dist_buf[i * 2 + 0]                             = c.slot_dist_to_ref((int32_t)i);
        dist_buf[i * 2 + 1]                             = (int32_t)i;
    }

    // 0x004cbb19-0x004cbb29: sort the scratch pairs by ascending distance. The GAME's own qsort with
    // the GAME's comparator (group_move_scratch_cmp_dist_004cbe0c, `*a-*b` over {dist,orig_index}) --
    // never a host sort or a reimplemented comparator: a host sort permutes equal-distance ties
    // differently than Watcom's qsort, a real lockstep divergence for group movement (see the batch
    // context's qsort note).
    c.qsort(scratch, leg_count, 8u, c.cmp_dist);

    // 0x004cbb2e-0x004cbbb8: walk the distance-sorted scratch. For every leg not yet claimed by an
    // earlier wave, start a new wave at it and claim every other still-free leg within its computed
    // radius.
    int32_t next_wave = -1; // 0x004cba3d seeds -0x18's slot to -1; pre-incremented before first use
    for (uint32_t j = 0; j < leg_count; ++j) {
        const int32_t member_index = dist_buf[j * 2 + 1];
        // 0x004cbb59-0x004cbb64: skip (to the next j) any leg an earlier wave already claimed.
        if (own.group_move_scratch_at(member_index).wave_rank != -1) continue;

        // 0x004cbb66-0x004cbb73: EAX is loaded BEFORE the INC and reloaded AFTER it (the pre-INC
        // value is a dead store) -- net effect is a pre-increment: bump the running wave counter,
        // then stamp its NEW value onto this leg.
        ++next_wave;
        own.group_move_scratch_at(member_index).wave_rank = next_wave;

        // 0x004cbb79-0x004cbb95: radius = trunc(dist*scale + bias), where `dist` is dist_buf[j]'s
        // OWN distance field (the same j this iteration is on, which after the sort above holds
        // member_index's distance) -- FILD (int->double, exact) then FMUL/FADD in that order (FP is
        // not associative).
        const double scaled = static_cast<double>(dist_buf[j * 2 + 0]) * (*v.group_move_wave_dist_scale) +
                              (*v.group_move_wave_dist_bias);
        int32_t max_dist = trunc_to_int32(scaled);
        // 0x004cbb98-0x004cbb9e: clamp a zero radius up to 1 (a wave must always claim at least
        // itself's own slot at distance 0).
        if (max_dist == 0) max_dist = 1;

        // 0x004cbba5-0x004cbbb1: stamp `next_wave` onto every still-unassigned leg within `max_dist`
        // of `member_index`.
        c.claim_free_slots_within_dist((int32_t)leg_count, max_dist, next_wave, member_index);
    }

    // 0x004cbbb8-0x004cbbbb: the {dist,orig_index} scratch is no longer needed.
    c.utils_free(scratch);

    // 0x004cbbc0-0x004cbbd2: re-sort the PERSISTENT group-move scratch array itself, by assigned
    // wave_rank (group_move_scratch_cmp_wave_rank_004cbe42, offset 8 of llm_strat_group_scratch_member
    // -- see mh_structs.gen.h's field comment), so llm_strat_unit_state_group_marshal can commit one
    // wave at a time. `&own.group_move_scratch_at(0)` is the sanctioned address escape to an ORIGINAL
    // callee (same posture as sim_store::text_scratch()'s G_TEXT_TMP note) -- qsort needs a base
    // pointer, exactly as the assembly hands it the array's literal VA.
    c.qsort(&own.group_move_scratch_at(0), leg_count, 0x14u, c.cmp_wave_rank);

    // 0x004cbbd7-0x004cbc0a: A DEAD LOOP, NOT REPRODUCED. Counts the (now wave_rank-sorted) array's
    // leading run of wave_rank==0 entries into a stack local, breaking the loop entirely (not
    // skipping one iteration) at the first non-zero entry. That stack local is then unconditionally
    // overwritten by the reconcile call's return value below before it is ever read -- the loop calls
    // nothing and writes only that one now-discarded stack slot, so it has zero effect on the return
    // value, on tracked sim state, or on any callee argument. See the header banner.

    // 0x004cbc0c-0x004cbc1a: hand off to the sibling leg-reconcile step, called as the ORIGINAL per
    // this batch's shadow-isolation convention (never through its own detail:: form -- see the header
    // banner).
    return c.pathfind_route_leg_reconcile((int32_t)leg_count);
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

int32_t pathfind_route_leg_group_and_sort(uint32_t leg_count, int32_t ref_x, int32_t ref_y) {
    sim_state st = state();
    return detail::pathfind_route_leg_group_and_sort(st.read, st.own,
                                                     live_pathfind_route_leg_group_and_sort_calls(),
                                                     leg_count, ref_x, ref_y);
}


} // namespace mh::sim
