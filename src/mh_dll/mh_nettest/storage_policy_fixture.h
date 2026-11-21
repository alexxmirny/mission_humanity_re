#pragma once
#include "../mh_common/include/mh_storage_policy.h"
#include "../mh_common/include/mh_order_tasks.h"
#include "addr/mh_regions.gen.h"

namespace mh::test {
inline bool initialize_storage_policy(void *memory, uint32_t bytes,
                                      mh::storage_policy::dimensions shape = {}) {
    mh::storage_policy::state initial;
    return initial.initialize(memory, bytes, shape);
}
// Keep all synthetic native owners on one reserved roster geometry. These use the real
// codecs: random/fake headers would make a hash or snapshot test fail before its own arm.
inline mh::order_tasks::dimensions order_shape(mh::storage_policy::dimensions shape) {
    return {shape.players, shape.units, shape.width, shape.height, shape.buildings};
}
inline uint32_t canonical_native_bytes(mh::state::region_id           rid,
                                       mh::storage_policy::dimensions shape = {}) {
    switch (rid) {
        case mh::state::RID_STORAGE_POLICY: return mh::storage_policy::state::required_bytes(shape);
        case mh::state::RID_ORDER_TASKS: return mh::order_tasks::state::required_bytes(order_shape(shape));
        case mh::state::RID_ORDER_CAMPAIGN: return mh::order_tasks::campaign_state::BYTES;
        default: return 0;
    }
}
inline bool initialize_canonical_native(mh::state::region_id rid, void *memory, uint32_t bytes,
                                        mh::storage_policy::dimensions shape = {}) {
    switch (rid) {
        case mh::state::RID_STORAGE_POLICY: return initialize_storage_policy(memory, bytes, shape);
        case mh::state::RID_ORDER_TASKS: {
            mh::order_tasks::state           initial;
            const mh::order_tasks::world_key key = shape.width
                                                       ? mh::order_tasks::world_key{0, 0, 1}
                                                       : mh::order_tasks::world_key{};
            return initial.initialize(memory, bytes, order_shape(shape), key);
        }
        case mh::state::RID_ORDER_CAMPAIGN: {
            mh::order_tasks::campaign_state initial;
            uint32_t                        incarnation = 0;
            return initial.initialize(memory, bytes) &&
                   (!shape.width || initial.fresh_world(0, incarnation));
        }
        default: return false;
    }
}
} // namespace mh::test
