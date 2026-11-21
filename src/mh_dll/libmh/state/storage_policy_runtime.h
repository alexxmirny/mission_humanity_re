// Standalone policy adapter. The host owns/binds the bytes; this module owns no allocation.
#pragma once
#include "include/mh_storage_policy.h"

extern "C" {
// Bind after a world/scene arena is committed and before any policy callback/sim step. Rebind on
// region replacement; same-address snapshot imports require no rebind. Returns1 success,0 refusal.
int32_t libmh_storage_policy_bind(void *region, uint32_t bytes);
// Required synchronous host callback implementation for a standalone replay host. Does not call
// mh::host().storage_policy recursively and never executes an original EXE address.
int32_t libmh_storage_policy_invoke(const mh_storage_policy_request *request, mh_storage_policy_result *result);
}

namespace mh {
namespace storage_policy {
namespace runtime {
// The existing borrowed span, never a second allocation. shape() is the canonical reserved-row
// geometry; callers must preserve it when validating/importing policy payloads.
state &bound_state();
// Actual currently bound native roster rows + RID_WIDTH/RID_HEIGHT tile counts, not canonical
// reserved rows. Invalid/unmapped map dimensions return both axes zero.
dimensions roster_dimensions();
} // namespace runtime
} // namespace storage_policy
} // namespace mh
