#include "orders/issue/issue_bldg_footprint.h"

namespace mh::orders::issue {

namespace {

// buildings[player][index] -- same arithmetic as bldg_at() in issue_bldg_orders.cpp. Duplicated
// here rather than shared: it is an anonymous-namespace helper, so it cannot cross translation
// units, and the sweep's per-unit convention is each TU carries its own copy of this idiom.
const mh::game::mh_map_object_building &bldg_at(const issue_view &v, uint32_t player, int32_t index) {
    return v.buildings[(int32_t)(uint16_t)player * v.caps.buildings + index];
}

// Signed divide-by-2, truncating toward zero -- the exact `SAR EDX,0x1f / SUB EAX,EDX / SAR EAX,0x1`
// idiom the original runs twice (0x00449dc0-0x00449dcb for the x axis, 0x00449ddc-0x00449de9 for
// the y axis). Written as the shift form per the translator brief's rule 8, not folded to `v / 2`.
int32_t half_trunc(int32_t v) { return (v - (v >> 31)) >> 1; }

} // namespace

void detail::bldg_footprint_random_point(const issue_view &v, const order_sink &, const issue_calls &c,
                                         uint32_t param_1, uint32_t param_2, uint32_t player,
                                         int32_t bldg_idx, uint32_t *out_x, uint32_t *out_y) {
    (void)param_1; // loaded to a local, never read again -- see the header/plate note
    (void)param_2; // same

    const mh::game::mh_map_object_building       &b   = bldg_at(v, player, bldg_idx);
    const mh::game::mh_cfg_final_struct_Building &cfg = v.cfg_buildings[b.building_id];

    const uint32_t width_px  = (uint32_t)cfg.width * 0x10;
    const uint32_t height_px = (uint32_t)cfg.height * 0x10;

    const int32_t rx = (int32_t)c.rand_below_fx(width_px);
    const int32_t ry = (int32_t)c.rand_below_fx(height_px);

    *out_x = (*out_x + (uint32_t)(rx - half_trunc((int32_t)width_px))) & v.geom->bw_mask;
    *out_y = (*out_y + (uint32_t)(ry - half_trunc((int32_t)height_px))) & v.geom->bh_mask;
}

void bldg_footprint_random_point(uint32_t param_1, uint32_t param_2, uint32_t player,
                                 int32_t bldg_idx, uint32_t *out_x, uint32_t *out_y) {
    detail::bldg_footprint_random_point(live_view(), live_sink(), live_calls(), param_1, param_2,
                                        player, bldg_idx, out_x, out_y);
}

} // namespace mh::orders::issue
