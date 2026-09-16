//
// sim/sim_path_slot_dist.cpp -- see sim_path_slot_dist.h. Translated from the DISASSEMBLY
// (tmp/decomp_sim/llm_strat_claim_free_slots_within_dist_004cbc28.asm,
// tmp/decomp_sim/llm_strat_slot_dist_to_ref_004cbd63.asm), not from any Ghidra .c draft.
//
#include "sim/sim_path_slot_dist.h"

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"        // ai_say / trace_budget -- the shared trace sink, not AI state
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"
#include "fp/x87.h"         // CRT-X87: the shared x87 truncation helpers
#include "crt/crt_select.h" // LIB-CRT: MH_CRT() picks the vendored CRT in the standalone build

namespace mh::sim {

const claim_free_slots_within_dist_calls &live_claim_free_slots_within_dist_calls() {
    static const claim_free_slots_within_dist_calls c = {
        MH_LIBMH_BIND(llm_strat_dir_step_toroidal_dist),
    };
    return c;
}

const slot_dist_to_ref_calls &live_slot_dist_to_ref_calls() {
    static const slot_dist_to_ref_calls c = {
        MH_CRT(llm_sqrt),
    };
    return c;
}

namespace {

// utils_math_trunc @0x004d0596 (`MH_UNAVAILABLE__parameter_storage_not_marshallable` in
// mh_calls.gen.h -- ST0 in, ST0 out, x87-register-only, no stack-passable signature). This function's
// one call site (0x004cbdf7) immediately follows the CALL to llm_sqrt with NO intervening FLD
// (0x004cbdf2-0x004cbdf7) -- llm_sqrt's ST0 result feeds utils_math_trunc directly in the original.
// Here the sqrt call is marshalled (`slot_dist_to_ref_calls::sqrt_fn`, an ordinary C++ `double`
// return -- the interop layer already narrows the x87 return to a `double` at that call boundary, per
// sim_weapon_damage_calc.h's identical sqrt-then-trunc pair), so this helper takes that `double` by
// value and reproduces ONLY the utils_math_trunc instruction sequence
// (FSTCW/mov ah,0x1f/FLDCW/FRNDINT/FLDCW-restore/FISTP dword), matching the dword-FISTP shape
// sim_projectile_tick.cpp's `trunc_only` already uses for this exact "trunc(x) alone" case -- re-
// declared privately here per this project's per-TU convention (never called through `mh::call::`;
// see the header banner).
int32_t trunc_dword(double x) {
    return ::mh::fp::trunc_i32(x);
}

} // namespace

namespace detail {

void claim_free_slots_within_dist(sim_store &own, const claim_free_slots_within_dist_calls &c,
                                  int32_t record_count, int32_t max_dist, int32_t claim_value,
                                  int32_t origin_index) {
    // 0x004cbc50-0x004cbc94: for (i = 0; i < record_count; ++i) -- signed JL, ordinary bounded loop,
    // unconditional `++i` at LAB_004cbc5a reached from every path (skip-unassigned, skip-out-of-range,
    // and the write itself all funnel back here) -- there is no early exit anywhere in this function.
    for (int32_t i = 0; i < record_count; ++i) {
        // 0x004cbc62-0x004cbc6d: group_move_scratch[i].wave_rank == -1 (unassigned). A NON-(-1) slot
        // (already claimed by an earlier wave) is skipped without computing a distance at all.
        group_scratch_member &m = own.group_move_scratch_at(i);
        if (m.wave_rank != -1) continue;

        // 0x004cbc6f-0x004cbc7a: llm_strat_dir_step_toroidal_dist(origin_index, i) -- EDX=i is loaded
        // first, then EAX=origin_index (a separate register, not overwriting EDX), so at the CALL
        // EAX=origin_index/EDX=i, matching addr/mh_calls.gen.h's own s_u32_EAX_EDX(a_index=EAX,
        // b_index=EDX) parameter order: (a_index=origin_index, b_index=i).
        const int32_t dist = c.dir_step_toroidal_dist(origin_index, i);

        // 0x004cbc7d-0x004cbc83: signed `dist > max_dist` skips the write (JG); `dist <= max_dist`
        // (inclusive) falls through to claim the slot.
        if (dist > max_dist) continue;

        // 0x004cbc85-0x004cbc8c: group_move_scratch[i].wave_rank = claim_value. Claims EVERY matching
        // slot -- no break, the loop keeps scanning to record_count regardless.
        m.wave_rank = claim_value;
    }
}

int32_t slot_dist_to_ref(const sim_view &v, sim_store &own, const slot_dist_to_ref_calls &c,
                         int32_t slot_index) {
    // 0x004cbd7e-0x004cbd95: group_move_scratch[slot_index].tile_col/.tile_row -- the struct's own
    // offset-0/offset-4 fields, read via the sim_view read-only sibling of the mutable accessor (1)
    // above uses (no write anywhere in this function).
    const group_scratch_member &m = v.group_move_scratch[slot_index];

    // 0x004cbd98-0x004cbdad: dx = tile_col - ref_x; dy = tile_row - ref_y. The four scratch globals
    // have no const accessor on sim_view (see the header banner) -- read here through the SAME
    // `_mut()` accessors sim_pathfind_route_leg_group_and_sort.cpp writes through, purely to read.
    int32_t dx = m.tile_col - own.group_move_dist_ref_x_mut();
    int32_t dy = m.tile_row - own.group_move_dist_ref_y_mut();

    // 0x004cbdb0-0x004cbdc3: toroidal wrap on the X axis only when dx overflows the positive half-
    // extent -- `dx > half_width` (signed JLE skips the subtract), then subtract the FULL map width
    // (*v.map_width, not half_width) once. Same shape as sim_geom_toroidal.cpp's wrap helper, hand-
    // derived here per translator-brief rule 4 (no new cross-TU helper for one reused shape).
    if (dx > own.group_move_dist_half_width_mut()) dx -= *v.map_width;

    // 0x004cbdc3-0x004cbdd6: same wrap on the Y axis against half_height/map_height.
    if (dy > own.group_move_dist_half_height_mut()) dy -= *v.map_height;

    // 0x004cbdd6-0x004cbde6: dist_sq = dx*dx + dy*dy -- ordinary 32-bit signed multiply/add,
    // WRAPPING on overflow (widened to double only by the FILD that follows, matching
    // sim_weapon_damage_calc.h's "int32 arithmetic first, then widen" idiom for its own dx*dx+dy*dy;
    // translator-brief rule 7, no early promotion).
    const int32_t dist_sq = dx * dx + dy * dy;

    // 0x004cbde9-0x004cbe02: (int32_t)trunc(sqrt((double)dist_sq)). llm_sqrt is a marshallable
    // frontier call; utils_math_trunc is reproduced as the private trunc_dword() helper above (see the
    // header banner for why it cannot be reached through mh::call::).
    return trunc_dword(c.sqrt_fn(static_cast<double>(dist_sq)));
}

} // namespace detail

// ---- the public wrappers --------------------------------------------------------------------------

void claim_free_slots_within_dist(int32_t record_count, int32_t max_dist, int32_t claim_value,
                                  int32_t origin_index) {
    sim_state st = state();
    detail::claim_free_slots_within_dist(st.own, live_claim_free_slots_within_dist_calls(),
                                         record_count, max_dist, claim_value, origin_index);
}

int32_t slot_dist_to_ref(int32_t slot_index) {
    sim_state st = state();
    return detail::slot_dist_to_ref(st.read, st.own, live_slot_dist_to_ref_calls(), slot_index);
}


} // namespace mh::sim
