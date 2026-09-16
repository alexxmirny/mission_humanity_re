//
// ai/ai_scan_target_add.h -- the scan-side append to the GLOBAL AI target scratch (RI-AI / AI1A
// layer 3).
//
// One function: llm_strat_ai_scan_target_list_add @0x004ec2d0. It appends to
// _G_LLM_STRAT_AI_SCAN_TARGETS (ai_view::scan_targets / scan_target_count) -- the 2048-entry GLOBAL
// scratch, stride 0x16. This is NOT player_data::ai_target_list (stride 0x14, cap 0x40, appended by
// the DIFFERENTLY-named llm_strat_ai_target_list_add @0x004d6d67, already translated in layer 1).
// The two functions' names differ by exactly the word "scan" and fill two different arrays; do not
// conflate them.
//
#pragma once
#include <cstdint>

#include "ai/ai_state.h"

namespace mh::ai {

// The logic over an EXPLICIT state, so `net_selftest.exe aitest` can drive it over heap buffers with
// no game and no rig. The wrapper below is this applied to state(); the split costs one inlined call.
namespace detail {

// llm_strat_ai_scan_target_list_add @0x004ec2d0.
//
// Order of operations, read straight off the assembly (not the exported .c, which mis-transcribes
// three things -- see ai_scan_target_add.cpp's header comment):
//   1. Rejects immediately, before anything else, unless target_ref_is_alive(target_ref,
//      target_index). There is no capacity check anywhere in this function.
//   2. Linearly dedupes over the WHOLE live count [0, *scan_target_count): each stored entry's
//      target_ref/target_index are 16-bit fields, zero-extended (MOVZX) before being compared
//      against the full 32-bit arguments. A target_ref/target_index above 0xffff therefore dedupes
//      only against a stored entry whose low 16 bits happen to match -- transcribed faithfully, not
//      "fixed" into a 16-bit-truncated comparison on the argument side.
//   3. Writes a fresh entry at the current count: target_ref, target_index (both truncated to the
//      stored 16-bit width), priority_score = 0, then a SINGLE original dword store of 0 that covers
//      BOTH class_flags (+6) and reserved_0x08 (+8), then counter_target_ai_group = 0xffff.
//   4. Calls group_classify_target_object(player, &entry) to fill the rest (score/flags/position/
//      range/counter-target) THROUGH THE SAME RECORD POINTER -- it reads the record we just
//      partially initialised, so it must run after step 3.
//   5. Increments *scan_target_count only AFTER the classifier call returns.
void scan_target_list_add(const ai_view &v, const ai_store &own, const ai_calls &gc, uint32_t player,
                          uint32_t target_ref, int32_t target_index);

} // namespace detail

void scan_target_list_add(uint32_t player, uint32_t target_ref, int32_t target_index);

} // namespace mh::ai
