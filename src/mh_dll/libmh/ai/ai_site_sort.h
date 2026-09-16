//
// ai/ai_site_sort.h -- the AI build-site candidate distance sort (RI-AI / AI1A batch A layer 5).
//
// One tiny void(void) function with no callees of its own. It sorts the SHARED build-site candidate
// list (ai_state.h's site_candidates / site_candidate_count) that all three layer-5 site scanners
// append to, ascending by .dist_sq, so the AI's construction planner (llm_strat_ai_plan_construction)
// gets its candidates nearest-first.
//
#pragma once
#include <cstdint>

#include "ai/ai_state.h"

namespace mh::ai {

// The logic over an EXPLICIT state, so `net_selftest.exe aitest` can drive it over heap buffers with
// no game and no rig. The wrapper below is this applied to state(); the split costs one inlined call.
namespace detail {

// llm_strat_ai_sort_site_candidates_by_dist @0x004e6228.
//
// In-place ascending bubble sort of v.site_candidates[0 .. *v.site_candidate_count) by .dist_sq.
//
//   - the early-exit flag is INVERTED from the obvious spelling: the original's ECX is "this pass
//     made NO swap" -- 0 on entry, set to 1 at the top of every pass, cleared by any swap, and the
//     pass loop exits AT ITS OWN TOP when it is already 1 (i.e. on the pass AFTER the last one that
//     swapped nothing changed);
//   - the key comparison is UNSIGNED (`CMP ... JBE skip`) and swaps only on STRICTLY GREATER, so
//     equal keys keep their relative order;
//   - the swap moves the WHOLE 16-byte record (tile_x, tile_y, kind, dist_sq) through a stack temp,
//     not just the sort key;
//   - count == 0 returns immediately (the very first check, before any pass runs); count == 1 makes
//     every pass's inner bound 0 (zero iterations) and the loop exits on the second visit to the
//     pass-loop top;
//   - it returns via a shared Watcom epilogue (`JMP 0x004e2d4c`), an ordinary return, not a tail
//     call.
//
// No callees (the opening `CALL assert_stack_capacity` is inert per the translator brief and is
// omitted).
void sort_site_candidates_by_dist(const ai_view &v, const ai_store &own);

} // namespace detail

void sort_site_candidates_by_dist();

} // namespace mh::ai
