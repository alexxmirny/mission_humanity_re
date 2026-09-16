// The mh.dll thin implementation of the HOOK-SERVICE ABI (fork F4D-PRE) -- the sibling of
// include/mh_hostapi_bind.h, and the same division of labour: the TABLE and its five forwarders are
// mh.dll's (seams/libmh_hook_host.cpp), the accessors module code calls are libmh's
// (state/hook_api.h), and neither side names the other's internals.
//
// HARNESS header. A libmh module TU must never include this -- it exists precisely so that none of
// them has to (check_libmh_outbound.py enforces that, source-side and object-side).
#pragma once

#include "../../libmh/include/libmh_hook.h"

namespace mh::hostapi {

// The mh.dll hook table: five forwarders onto mh::hook:: / MH_Harness_. Static storage, so the
// pointer handed to libmh_set_hook_api outlives every caller.
const libmh_hook_api &mhdll_hook_table();

} // namespace mh::hostapi

extern "C" {
// Bind the table. Returns libmh_set_hook_api's rc (0 = bound). Called from MH_Core_Arm_Early --
// AHEAD of MH_Harness_Init and therefore ahead of every [promote] installer -- and from
// net_selftest's main(), which is a host of libmh too. Idempotent.
//
// WHY IT IS NOT DEFERRED TO ARM TIME like the seam installs it serves: the installers run inside
// MH_Seam_Init, and a table bound after the first one has already refused would leave a promotion
// silently un-installed with no line saying so. That is the G104 earliest-common-point rule the two
// host-api binds beside it already follow, and the failure mode here is the quieter of the two.
int MH_LibMH_BindHookApi(void);
}
