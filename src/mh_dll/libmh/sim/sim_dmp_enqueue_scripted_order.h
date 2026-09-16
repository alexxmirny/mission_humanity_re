#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// The order-container triad this function calls out to -- indirected for the same reason as every
// other module here (net_selftest.exe simtest has no live game image to call into). All three are
// ORIGINAL functions (see the header note above); production binds them to mh::call::.
struct dmp_enqueue_scripted_order_calls {
    void (*order_scratch_reset)();
    void (*order_scratch_set_field)(int32_t index, int32_t value);
    int32_t (*order_enqueue)(uint16_t unit_index, uint16_t owner_and_kind, int16_t param0,
                             uint16_t order_code);
};

const dmp_enqueue_scripted_order_calls &live_dmp_enqueue_scripted_order_calls();

namespace detail {

// llm_strat_dmp_enqueue_scripted_order @0x0046d873. Stages (x, y, unit_type_id) into scratch fields
// (4, 5, 1), then enqueues the fixed UNIT_CREATE order (0xeb/0xeb) tagged `owner_and_kind` (passed
// through unchanged -- see the header note). Always returns 1: the original never tests its own
// order_enqueue's result, same "unconditional success" shape as order_recruit_unit_enqueue /
// order_grant_resource_raw in sim_order_enqueue.cpp.
uint32_t dmp_enqueue_scripted_order(const dmp_enqueue_scripted_order_calls &c, uint32_t x, uint32_t y,
                                    uint32_t unit_type_id, uint16_t owner_and_kind);

} // namespace detail

// Live wrapper: the logic applied to live_dmp_enqueue_scripted_order_calls(). Matches the committed
// prototype (addr::exp::sig_llm_strat_dmp_enqueue_scripted_order) exactly:
// uint32_t(uint32_t, uint32_t, uint32_t, uint16_t).
uint32_t dmp_enqueue_scripted_order(uint32_t x, uint32_t y, uint32_t unit_type_id,
                                    uint16_t owner_and_kind);

namespace detail {
} // namespace detail

} // namespace mh::sim
