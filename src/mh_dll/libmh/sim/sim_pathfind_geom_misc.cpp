//
// sim/sim_pathfind_geom_misc.cpp -- see sim_pathfind_geom_misc.h. Translated from the DISASSEMBLY,
// address-by-address; no Ghidra .c draft.
//
#include "sim/sim_pathfind_geom_misc.h"

#include "addr/mh_calls.gen.h" // typed callables for the original functions we still call OUT to
#include "fp/x87.h"            // CRT-X87: the shared x87 truncation helpers
#include "crt/crt_select.h"    // LIB-CRT: MH_CRT() picks the vendored CRT in the standalone build

namespace mh::sim {

const dir_step_toroidal_dist_calls &live_dir_step_toroidal_dist_calls() {
    static const dir_step_toroidal_dist_calls c = {
        MH_CRT(llm_sqrt),
    };
    return c;
}

namespace {

// utils_math_trunc @0x004d0596 (`MH_UNAVAILABLE__parameter_storage_not_marshallable` in
// mh_calls.gen.h -- ST0 in, ST0 out, x87-register-only, no stack-passable signature). This function's
// one call site (0x004cbd4f) immediately follows the CALL to llm_sqrt with no intervening FLD, so
// llm_sqrt's ST0 result feeds utils_math_trunc directly in the original. Here the sqrt call is
// marshalled (`dir_step_toroidal_dist_calls::sqrt_fn`, an ordinary C++ `double` return), so this
// helper takes that `double` by value and reproduces ONLY the utils_math_trunc instruction sequence --
// the IDENTICAL sequence sim_path_slot_dist.cpp's own `trunc_dword()` uses for the same sqrt-then-trunc
// pair, copied rather than shared per this project's per-TU convention for this non-marshallable-FP
// case (never called through `mh::call::`; see the header banner).
int32_t trunc_dword(double x) {
    return ::mh::fp::trunc_i32(x);
}

} // namespace

namespace detail {

void pathfind_mark_group_member_regions(const sim_view &v, sim_store &own) {
    // 0x00420931-0x004209a8: for (i = 0; i < *v.group_member_count; ++i) -- the bound is RE-READ from
    // _G_LLM_STRAT_GROUP_MEMBER_COUNT at the top of every pass (0x0042093b), not hoisted into a local,
    // so this loop does not hoist it either (translator-brief rule 16).
    for (int32_t i = 0; i < *v.group_member_count; ++i) {
        // 0x0042094d-0x0042096d: region_cell_at(cur_col, cur_row).region -- may be null.
        const group_member &m      = v.group_members[i];
        llm_map_region     *region = own.region_cell_at(m.cur_col, m.cur_row).region;
        if (region == nullptr) continue; // 0x00420974: JZ skip -- no write for this member

        region->route_mark = 2;
    }
}

void pathfind_target_hook_stub(int32_t target_col, int32_t target_row) {
    // 0x004219f4-0x004219f7: both parameters are stored to stack locals and never read again before
    // the epilogue. Genuine no-op -- see the header banner (2). Nothing to do.
    (void)target_col;
    (void)target_row;
}

int32_t dir_step_toroidal_dist(const sim_view &v, sim_store &own, const dir_step_toroidal_dist_calls &c,
                               int32_t a_index, int32_t b_index) {
    // 0x004cbcb8-0x004cbce9: the two scratch members' tile_col/tile_row.
    const group_scratch_member &a = v.group_move_scratch[a_index];
    const group_scratch_member &b = v.group_move_scratch[b_index];

    // 0x004cbcec-0x004cbd05: per-axis absolute delta via the classic CDQ/XOR/SUB idiom (sign-extend
    // the diff into an all-0s/all-1s mask, XOR then SUB it back out) -- reproduced with the same bit
    // ops rather than std::abs() so a diff of INT_MIN behaves identically (wraps) instead of hitting
    // std::abs's UB there, matching sim_geom_toroidal.cpp's own precedent for this exact idiom.
    const int32_t dcol_raw  = a.tile_col - b.tile_col;
    const int32_t dcol_sign = dcol_raw >> 31; // CDQ: all-1s if negative, else all-0s
    int32_t       dcol_abs  = (dcol_raw ^ dcol_sign) - dcol_sign;

    const int32_t drow_raw  = a.tile_row - b.tile_row;
    const int32_t drow_sign = drow_raw >> 31;
    int32_t       drow_abs  = (drow_raw ^ drow_sign) - drow_sign;

    // 0x004cbd08-0x004cbd18: toroidal wrap on the column axis -- subtract the FULL map width once
    // when the absolute delta exceeds the half-extent. The result can go negative; that is correct
    // (the next step squares it) -- see the header banner (3) for the derivation.
    if (dcol_abs > own.group_move_dist_half_width_mut()) dcol_abs -= *v.map_width;

    // 0x004cbd1b-0x004cbd2b: same wrap on the row axis.
    if (drow_abs > own.group_move_dist_half_height_mut()) drow_abs -= *v.map_height;

    // 0x004cbd2e-0x004cbd3e: dist_sq = dcol_abs*dcol_abs + drow_abs*drow_abs -- ordinary 32-bit signed
    // arithmetic, widened to double only by the FILD that follows (translator-brief rule 7).
    const int32_t dist_sq = dcol_abs * dcol_abs + drow_abs * drow_abs;

    // 0x004cbd41-0x004cbd57: (int32_t)trunc(sqrt((double)dist_sq)).
    return trunc_dword(c.sqrt_fn(static_cast<double>(dist_sq)));
}

} // namespace detail

// ---- the public wrappers --------------------------------------------------------------------------

void pathfind_mark_group_member_regions() {
    sim_state st = state();
    detail::pathfind_mark_group_member_regions(st.read, st.own);
}

void pathfind_target_hook_stub(int32_t target_col, int32_t target_row) {
    detail::pathfind_target_hook_stub(target_col, target_row);
}

int32_t dir_step_toroidal_dist(int32_t a_index, int32_t b_index) {
    sim_state st = state();
    return detail::dir_step_toroidal_dist(st.read, st.own, live_dir_step_toroidal_dist_calls(), a_index,
                                          b_index);
}

} // namespace mh::sim
