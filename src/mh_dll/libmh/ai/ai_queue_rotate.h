//
// ai/ai_queue_rotate.h -- promote the newest AI build-queue entry to the front (RI-AI / AI1B,
// layer 4).
//
// llm_strat_ai_queue_rotate_newest_to_front @0x004e2cac, 166 bytes. Called immediately after a fresh
// enqueue by llm_strat_ai_plan_construction, llm_strat_ai_scan_bldg_repair_upgrade and
// llm_strat_ai_plan_mine_construction, so that the thing just decided is the thing
// llm_strat_ai_bldg_queue_process looks at first.
//
// NOT A POP. ai_bldg_queue_count is never touched -- the whole body is a right-rotate by one:
//
//     tmp = q[count-1];  for (i = count-1; i > 0; --i) q[i] = q[i-1];  q[0] = tmp;
//
// with the copies done as `MOVSD.REP` x4 + `MOVSW`, i.e. exactly the 0x12-byte entry, no more.
//
// TWO COMPARISONS WITH DIFFERENT SIGNEDNESS, and they are not interchangeable:
//   * the early-out is UNSIGNED -- CMP dword ptr [.. count],0x1 / JBE @0x004e2cd5, so counts 0 and 1
//     both return, and a NEGATIVE count would fall through into the rotate rather than skip it.
//   * the loop guard is SIGNED -- TEST EBX,EBX / JG @0x004e2d33, which is what stops the walk at
//     index 0 rather than wrapping.
//   The pair only matters for a corrupt count, which is precisely when it matters.
//
// The player row base is RECOMPUTED inside the loop (0x004e2d1d-0x004e2d31, once per iteration)
// rather than hoisted. That is a codegen artifact with no observable effect and is not reproduced.
//
#pragma once
#include <cstdint>

#include "ai/ai_state.h"

namespace mh::ai {

namespace detail {

// Takes the STORE, not the view: the count it reads and the entries it moves are the same object,
// and reading the count through a const alias would suggest the two could differ.
void queue_rotate_newest_to_front(const ai_store &own, int32_t player);

} // namespace detail

void queue_rotate_newest_to_front(int32_t player);

} // namespace mh::ai
