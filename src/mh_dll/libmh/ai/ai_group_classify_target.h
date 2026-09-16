//
// ai/ai_group_classify_target.h -- the scan-target classifier (RI-AI / AI1C).
//
// One function: llm_strat_ai_group_classify_target_object @0x004eb48e. It is the FILLER half of the
// GLOBAL scan-target scratch pipeline that ai_scan_target_add.{h,cpp} documents -- that file's
// `scan_target_list_add` partially initialises a fresh `scan_target_entry` (target_ref, target_index,
// priority_score=0, class_flags=0, counter_target_ai_group=0xffff) and then calls THIS function,
// through the SAME record pointer, to derive everything else: the target's tile position, its
// (squared) weapon range, whether/what it is currently counter-attacking, and a priority_score +
// class_flags summary of all of that.
//
// TWO INDEPENDENT ARMS, cross-checked separately against the .asm rather than assumed symmetric:
//   - BUILDING target (`(target_ref & 0xa0) == 0`, ref_is_building_by_a0): reads buildings[]/Building[]
//     /Weapon[], and gets its counter-target from the `turrets[]` array (a building doesn't carry its
//     own target_ref/target_index the way a unit does).
//   - UNIT target (`(target_ref & 0xa0) != 0`): reads units[]/player_data's threat grid, and gets its
//     counter-target from the unit's OWN target_ref/target_index fields.
// They are NOT mirror images of each other -- see the counter-target classification comments in the
// .cpp for the one place they genuinely diverge (a confirming (ref & 0x40) test the unit arm has and
// the building arm does not).
//
// Both arms finish with the SAME tail: if the target's own object (unit or building, whichever arm)
// already has enough COMMITTED incoming damage to kill it before its energy would naturally deplete,
// the classifier backs the priority off by one and sets the 0x80 "already doomed, don't pile on"
// flag. Otherwise it returns having only ever ADDED to priority_score.
//
#pragma once
#include <cstdint>

#include "ai/ai_state.h"

namespace mh::ai {

// The logic over an EXPLICIT state, so `net_selftest.exe aitest` can drive it over heap buffers with
// no game and no rig. The wrapper below is this applied to state(); the split costs one inlined call.
namespace detail {

// llm_strat_ai_group_classify_target_object @0x004eb48e.
//
// Writes ONLY through `target_record` (tile_x, tile_y, range_sq, counter_target_ref,
// counter_target_index, counter_target_ai_group, priority_score, class_flags) -- it touches no
// shared/global state at all, so there is no `ai_store` parameter (same shape as an OUTPUT-POINTER
// helper: see ai_state.h's `pick_owned_tile_or_home` comment for the precedent).
//
// `target_record->target_ref` and `->target_index` are read-only INPUTS here; every other field is
// either read-then-overwritten or write-only. See the .cpp for the exact field-by-field derivation
// against the .asm's absolute addresses (0xc3d2a2-family for buildings/turrets, 0xdd8ccc-family for
// units), which were independently re-derived against addr/mh_structs.gen.h's offsetof asserts rather
// than trusted from either the .asm's literals or the exported .c (the .c and the .asm DISAGREE on
// one point -- the unit-branch weapon-range squaring -- and the .asm wins; see the .cpp).
void group_classify_target_object(const ai_view &v, const ai_calls &gc, uint32_t player_id,
                                  scan_target_entry &target_record);

} // namespace detail

// Matches sig_llm_strat_ai_group_classify_target_object (mh_export.gen.h). This used to take an
// untyped `void *` with a note that the committed prototype "predates the 2026-08-06 retype of this
// record's own field" -- the Ghidra prototype has since caught up (its second parameter is a real
// `llm_strat_ai_scan_target_entry *`), so the generated signature is typed and the casts at the two
// call sites are gone. Surfaced 2026-08-07 when the generated headers were re-dumped: the retype had
// landed in Ghidra without a re-run of mh_dump_call_protos.py, and that gap is invisible to
// lint_repo.py, which compares the generated headers to the committed JSON and never to Ghidra.
void group_classify_target_object(uint32_t player_id, scan_target_entry *target_record);

} // namespace mh::ai
