#include "tact/tact_weapon_calc_scatter.h"

#include "addr/mh_calls.gen.h" // frontier callees (Law 4): llm_rand, llm_sqrt
#include "fp/x87.h"            // CRT-X87: the shared x87 truncation helpers
#include "crt/crt_select.h"    // LIB-CRT: MH_CRT() picks the vendored CRT in the standalone build

namespace mh::tact {

const weapon_calc_scatter_calls &live_weapon_calc_scatter_calls() {
    static const weapon_calc_scatter_calls c = {
        MH_CRT(llm_rand),
        MH_CRT(llm_sqrt),
    };
    return c;
}

namespace {

// utils_math_trunc @0x004d0596 -- same FPU-control-word truncate idiom as
// tact_unit_weapon_in_range.cpp's `trunc_dword` (MH_UNAVAILABLE__parameter_storage_not_marshallable
// in mh_calls.gen.h; re-declared privately per this project's per-TU convention).
int32_t trunc_dword(double x) {
    return ::mh::fp::trunc_i32(x);
}

} // namespace

namespace detail {

void weapon_calc_scatter(const tact_view &v, const weapon_calc_scatter_calls &c, int32_t building_id,
                         int32_t x, int32_t y, int32_t tx, int32_t ty, int32_t *out_dx,
                         int32_t *out_dy, int32_t weapon_slot) {
    const tact_unit &u = v.units[building_id];

    // @0x00430c98-0x00430cc9: same adjacent-byte idiom as tact_unit_weapon_in_range.h.
    const character_type &ct             = v.character_types[u.type];
    const int32_t         weapon_type_id = (&ct.number_gun1)[weapon_slot ^ u.active_gun];
    const fx_type        &wt             = v.fx_type_table[weapon_type_id];

    // @0x00430ccc-0x00430d0a
    const int32_t precise = (u.anim_state == 2 || u.anim_state == 3) ? wt.precise_kneel : wt.precise;

    // @0x00430d0a-0x00430d22
    const int32_t rand_val = c.rand_fn();
    const int32_t scaled   = (precise * rand_val) / 16384;
    const int32_t spread   = scaled - precise;

    // @0x00430d28-0x00430d5e
    const int32_t dx      = x - tx;
    const int32_t dy      = y - ty;
    const int32_t dist_sq = dx * dx + dy * dy;
    const int32_t dist    = trunc_dword(c.sqrt_fn((double)dist_sq));

    // @0x00430d61-0x00430d8f: scratch pre-values, both stored through the out-pointers first.
    *out_dx = (spread * dist) / 320;
    *out_dy = (spread * dist) / 426;

    // @0x00430d91-0x00430dbc: 24-way facing_dir -> one of 8 dir8_delta_table octants.
    int32_t octant = (u.facing_dir - 1) / 3 + 2;
    if (octant > 7) octant -= 8;

    // @0x00430dbc-0x00430de6
    *out_dx *= v.dir8_delta_table[octant].dx;
    *out_dy *= v.dir8_delta_table[octant].dy;
}

} // namespace detail

void weapon_calc_scatter(int32_t building_id, int32_t x, int32_t y, int32_t tx, int32_t ty,
                         int32_t *out_dx, int32_t *out_dy, int32_t weapon_slot) {
    tact_state st = state();
    detail::weapon_calc_scatter(st.read, live_weapon_calc_scatter_calls(), building_id, x, y, tx, ty,
                                out_dx, out_dy, weapon_slot);
}

} // namespace mh::tact
