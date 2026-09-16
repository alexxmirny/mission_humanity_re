//
// tact/tact_unit_weapon_in_range.cpp -- see tact_unit_weapon_in_range.h. Translated from the
// DISASSEMBLY, not from Ghidra's C.
//
#include "tact/tact_unit_weapon_in_range.h"

#include "addr/mh_calls.gen.h" // MH_CRT(llm_sqrt)
#include "fp/x87.h"            // CRT-X87: the shared x87 truncation helpers
#include "crt/crt_select.h"    // LIB-CRT: MH_CRT() picks the vendored CRT in the standalone build

namespace mh::tact {

const unit_weapon_in_range_calls &live_unit_weapon_in_range_calls() {
    static const unit_weapon_in_range_calls c = {
        MH_CRT(llm_sqrt),
    };
    return c;
}

namespace {

// utils_math_trunc @0x004d0596 (`MH_UNAVAILABLE__parameter_storage_not_marshallable` in
// mh_calls.gen.h -- ST0 in, ST0 out, no stack-passable signature). This function's two call sites
// (0x00433cc7, 0x00433d25) each immediately follow the CALL to llm_sqrt with no intervening FSTP,
// but the sqrt call here is marshalled (`unit_weapon_in_range_calls::sqrt_fn`, an ordinary C++
// `double` return -- the interop layer already narrows the x87 return at that call boundary), so
// this helper takes that `double` by value. Same shape as sim_path_slot_dist.cpp's `trunc_dword` /
// sim_projectile_tick.cpp's `trunc_only`; re-declared privately per this project's per-TU
// convention (never called through `mh::call::`; the generated header itself blocks it).
int32_t trunc_dword(double x) {
    return ::mh::fp::trunc_i32(x);
}

} // namespace

namespace detail {

int32_t unit_weapon_in_range(const tact_view &v, const unit_weapon_in_range_calls &c, int32_t unit_idx,
                             int32_t target_x, int32_t target_y) {
    const tact_unit &u = v.units[unit_idx];

    // @0x00433c2c-0x00433c57: tile anchor -> pixel-space center point.
    const int32_t center_col_px = (int32_t)u.pos_col * 32 + 16;
    const int32_t center_row_px = (int32_t)u.pos_row * 32 + 16;

    // @0x00433c5a-0x00433c86: weapon_type_id via the character type's (gun1, gun2) adjacent-byte
    // pair, biased by active_gun -- same idiom as height_gun1 (see mh_tact_unit_record::active_gun's
    // own field comment).
    const character_type &ct             = v.character_types[u.type];
    const int32_t         weapon_type_id = (&ct.number_gun1)[u.active_gun];
    const fx_type        &wt             = v.fx_type_table[weapon_type_id];

    // @0x00433c89-0x00433d3a: dist = trunc(sqrt(dx^2 + dy^2)). Computed ONCE -- the original
    // recomputes this identical value a second time for the range_max gate (see header banner).
    const int32_t dx      = center_col_px - target_x;
    const int32_t dy      = center_row_px - target_y;
    const int32_t dist_sq = dx * dx + dy * dy;
    const int32_t dist    = trunc_dword(c.sqrt_fn((double)dist_sq));

    // @0x00433c8d-0x00433ce7: near-range gate, skipped entirely when range_min <= 0.
    if (wt.range_min > 0 && dist < wt.range_min) {
        return 0;
    }
    // @0x00433ceb-0x00433d45: far-range gate, skipped entirely when range_max <= 0.
    if (wt.range_max > 0 && dist > wt.range_max) {
        return 0;
    }
    return 1;
}

} // namespace detail

int32_t unit_weapon_in_range(int32_t unit_idx, int32_t target_x, int32_t target_y) {
    tact_state st = state();
    return detail::unit_weapon_in_range(st.read, live_unit_weapon_in_range_calls(), unit_idx, target_x,
                                        target_y);
}

} // namespace mh::tact


namespace mh::tact {


} // namespace mh::tact
