//
// ai/ai_engage_candidates.h -- the engage-candidate scratch PRODUCER (RI-AI / AI1A batch A layer 3).
//
// One function: the append primitive every "offer this target to the engage pipeline" call site in
// the module funnels through (ai_engage.{h,cpp} documents the two CONSUMERS that filter/pick/commit
// from the same scratch; this is upstream of both, and of the ten other producer sites that are
// still original). Same scratch, same "reset-then-fill transient, no persistent state across AI
// ticks, no cap check" discipline those files document -- not repeated here.
//
#pragma once
#include <cstdint>

#include "ai/ai_state.h"

namespace mh::ai {

// The logic over an EXPLICIT state, so `net_selftest.exe aitest` can drive it over heap buffers with
// no game and no rig. The wrapper below is this applied to state(); the split costs one inlined call.
namespace detail {

// llm_strat_ai_engage_candidate_add @0x004d50b3.
//
// Appends (target_ref, target_index) to the engage-candidate scratch iff the target is currently
// alive (energy > 0.0) and not already present. The roster the energy read comes from is selected by
// (target_ref & 0xa0) == 0 -> BUILDING (ref_is_building_by_a0) -- the same polarity
// engage_select_and_commit's liveness check and the sort/partition/commit consumers use, NOT the
// 0x40 polarity engage_partition_turret_candidates / pick_first_survivable use over the same field.
//
// Writes only two of the record's three dwords: target_ref and target_index. dist_sq (offset +8) is
// left stale on purpose -- it is undefined until engage_sort_candidates_by_dist fills it, exactly as
// the field comment on mh_llm_strat_ai_engage_candidate::dist_sq says. There is no capacity check:
// the original grows the scratch unconditionally, relying on ENGAGE_SCRATCH_CAP as a physical extent
// it never tests.
void engage_candidate_add(const ai_view &v, const ai_store &own, uint32_t target_ref,
                          int32_t target_index);

} // namespace detail

void engage_candidate_add(uint32_t target_ref, int32_t target_index);

} // namespace mh::ai
