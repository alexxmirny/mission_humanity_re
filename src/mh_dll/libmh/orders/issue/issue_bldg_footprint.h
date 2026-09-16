//
// orders/issue/issue_bldg_footprint.h -- the ONE pure geometry helper in the O4A-C sweep
// (RI-ORDERS / O4-0). Not an order wrapper: llm_strat_bldg_footprint_random_point emits no order,
// has no golden site, and is UI/debug-rooted with no order_matrix entry, so it carries no shadow
// site either -- see tmp/decomp_orders_issue/_UNIT_bldg_footprint.md.
//
#pragma once
#include "orders/issue/issue_state.h"

namespace mh::orders::issue {

namespace detail {

// llm_strat_bldg_footprint_random_point @0x00449d41. Jitters an in/out pixel point (*out_x,*out_y)
// to a random spot inside buildings[player][bldg_idx]'s footprint (rand over width*16 / height*16,
// re-centred by a truncating half, wrapped by the map's bw_mask/bh_mask). `param_1`/`param_2` are
// the original's EAX/EDX parameters: stored to locals and never read again in this body -- dead in
// THIS function specifically, not a translation gap (see the .cpp for the address evidence).
void bldg_footprint_random_point(const issue_view &v, const order_sink &s, const issue_calls &c,
                                 uint32_t param_1, uint32_t param_2, uint32_t player,
                                 int32_t bldg_idx, uint32_t *out_x, uint32_t *out_y);

} // namespace detail

void bldg_footprint_random_point(uint32_t param_1, uint32_t param_2, uint32_t player,
                                 int32_t bldg_idx, uint32_t *out_x, uint32_t *out_y);

} // namespace mh::orders::issue
