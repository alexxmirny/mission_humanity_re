#pragma once
#include <cstdint>

#include "ai/ai_state.h"

namespace mh::ai {

// The logic over EXPLICIT state, so `net_selftest.exe aitest` can drive it over heap buffers with no
// game and no rig (this function's ONLY oracle -- see the OFFLINE-only note above).
namespace detail {

// WHICH ARM ACTUALLY RAN, and which of its sub-branches. Nothing in the game reads this; it exists
// so the offline oracle's coverage claim says more than "N calls, verdict clean" -- see
// ai_notify_removed.h's `notify_report` for the same reasoning applied to this hook's sibling.
// (Named `lifecycle_notify_report` since 2026-09-01: it used to SHARE that sibling's name while
// being a different struct in the same mh::ai::detail namespace -- a latent ODR violation that
// became a hard C2011 the first time one TU (ai_promote.cpp) included both headers.)
struct lifecycle_notify_report {
    uint32_t player                    = 0;
    int32_t  event                     = -1;    // raw param_4, or -1 if never dispatched (shouldn't happen)
    bool     ai_disabled_at_entry      = false; // top gate @0x004dbb67
    bool     out_of_range_event        = false; // @0x004dbb74, param_4 > 4 (unsigned)
    bool     threat_and_engage_cleared = false; // caseD_1 only, @0x004dbb93/0x004dbb9c
    bool     ai_disabled_recheck       = false; // caseD_1 @0x004dbba5 / caseD_4 @0x004dbd3c took the
                                                // shared "ai_group_index=0, return" arm
    bool group_links_reset         = false;     // the 3-field ai_group_next/prev/index reset ran
    bool pending_spawn_decremented = false;     // caseD_1 only, @0x004dbbe0
    bool classified                = false;     // reached a combat-type classify chain (state==0x1f
                                                // gate for caseD_1; always for caseD_4's non-early-out,
                                                // non-invasion-force path)
    bool    group_linked               = false; // ai_calls::group_member_link was actually called
    int32_t group_index_used           = -1;
    bool    heli_mother_early_out      = false; // caseD_4 only, @0x004dbd7e / 0x004dbd8b
    bool    invasion_force_direct_link = false; // caseD_4 only, @0x004dbd91
    bool    queue_reconciled           = false; // the shared build-queue scan found and marked an entry
};

// llm_strat_ai_notify_unit_lifecycle @0x004dbb38. See the header banner above for the full
// per-event derivation; the .cpp carries the per-write address citations.
lifecycle_notify_report notify_unit_lifecycle(const ai_view &v, const ai_store &own, const ai_calls &gc,
                                              uint16_t player_, uint16_t unit_type, uint32_t unit_id,
                                              uint32_t param_4);

} // namespace detail

void notify_unit_lifecycle(uint16_t player_, uint16_t unit_type, uint32_t unit_id, uint32_t param_4);

} // namespace mh::ai
