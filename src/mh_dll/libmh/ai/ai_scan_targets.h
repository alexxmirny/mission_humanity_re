//
// ai/ai_scan_targets.h -- two read-only re-scans over player_data::ai_target_list, both funnelling
// matches through the SCAN-side add entry point (RI-AI / AI1A, batch A layer 2).
//
// player_data::ai_target_list is the roster written by llm_strat_ai_target_list_add (@0x004d6d67,
// ai_target.h/.cpp) and read by llm_strat_ai_scan_targets_for_engage (@0x004ed130, also
// ai_target.h/.cpp). The two functions here are a SEPARATE pair over the SAME roster: they read it,
// filter it, and forward matches to llm_strat_ai_scan_target_list_add (@0x004ec2d0) -- which is NOT
// the roster writer above, and does NOT write ai_target_list. It appends to a different, 22-byte-
// record array (_G_LLM_STRAT_AI_SCAN_TARGETS). Neither function here writes ai_target_list, and
// neither writes anything else of its own -- their whole effect is the forwarding call.
//
//   target_list_refresh_mothers  @0x004ec58a. Re-offers every entry whose victim_ref has bit 0x40
//                                 set ("mother" class), unconditionally on that one test.
//   target_list_invalidate_by_id @0x004ec51d. Re-offers every entry whose victim_ref has any of
//                                 0xa0 set AND whose... see the field-identity note below. Both are
//                                 "sibling" filtered-re-add passes (same shape: walk the roster,
//                                 test a filter, forward (aggressor_ref, position) to the scan-side
//                                 add, count the forwards), differing only in the filter and in how
//                                 they get the count into EAX (see the return-value finding below).
//
// FIELD-IDENTITY FINDING (0x004ec550, target_list_invalidate_by_id): the function's second argument
// -- named "target_id" only because that is what Ghidra's low-confidence auto-name calls it -- is
// compared against the byte at entry-base+0x8, which is mh_llm_strat_ai_target_entry::ai_group_index
// (target_id itself sits at entry-base+0x0 and is never referenced in this function's body at all).
// The writer (target_list_add) fills ai_group_index from llm_strat_unit_get_ai_group_index(player,
// target_id) when the player's AI is enabled, else writes 0 -- so this reader is comparing its
// argument against an AI-GROUP INDEX, not an object/target id, regardless of what its own parameter
// is named. This is the exact disagreement addr/mh_structs.gen.h's ai_group_index field comment
// already flags as unresolved ("Resolve before relying on either reading"); this
// translation reproduces the comparison literally (against ai_group_index) and does not attempt to
// resolve which name is the bug.
//
// RETURN-VALUE FINDING, RAISED BY THE TRANSLATOR AND RESOLVED BY THE CONDUCTOR
// (target_list_invalidate_by_id only). The assembly ends its match-loop with a plain
// `JMP 0x004ec516` -- an address OUTSIDE this function's own byte range (7 bytes BEFORE its start)
// and therefore outside the exported .asm, with no `MOV EAX,EDI` anywhere on the path into it. That
// looked like a candidate for the Watcom "fake return" pattern. It is not: 0x004ec516 disassembles
// as `MOV EAX,EDI / JMP 0x004ec874`, i.e. a SHARED EPILOGUE that Ghidra attributes to the
// neighbouring llm_strat_ai_holding_pen_scan_targets and that this function tail-jumps into. EDI is
// the running match count in both, so the return IS the count. This is the SIXTH shared Watcom
// epilogue found in this binary; the lesson each time is the same -- a jump to an address outside
// the function is a tail-call into shared code, and reading it costs one disassembly query.
//
#pragma once
#include <cstdint>

#include "ai/ai_state.h"

namespace mh::ai {

// The logic over an EXPLICIT state, so `net_selftest.exe aitest` can drive it over heap buffers with
// no game and no rig. The wrappers below are these applied to state(); the split costs one inlined
// call. Neither function writes through `own` -- both are pure re-scans whose only effect is the
// forwarding call to gc.scan_target_list_add, so (unlike ai_target.h's writer) they take ai_view
// only, no ai_store.
namespace detail {

// llm_strat_ai_target_list_invalidate_by_id @0x004ec51d. Walks player's ai_target_list; for every
// entry with any of victim_ref 0xa0 set (a BYTE test, `TEST byte ptr ...,0xa0`, over the dword field
// -- masking only the low byte, so byte-vs-dword makes no observable difference here) AND whose
// ai_group_index equals `target_id` (see the field-identity finding above -- this is NOT a
// target_id-to-target_id comparison), forwards (aggressor_ref, position) to
// gc.scan_target_list_add(player, aggressor_ref, position) -- note the callee's own declared parameter
// names ("target_id", "target_owner") do not match what is actually passed; see ai_state.h's
// scan_target_list_add comment for the same kind of name/value mismatch elsewhere in this cluster.
// Returns the number forwarded -- through the shared epilogue at 0x004ec516, per the return-value
// finding above.
int32_t target_list_invalidate_by_id(const ai_view &v, const ai_calls &gc, int32_t player,
                                     int32_t target_id);

// llm_strat_ai_target_list_refresh_mothers @0x004ec58a. Walks player's ai_target_list; for every
// entry with victim_ref bit 0x40 set (a BYTE test, `TEST byte ptr ...,0x40`), unconditionally
// forwards (aggressor_ref, position) to gc.scan_target_list_add(player, aggressor_ref, position). Returns
// the number forwarded -- EAX is explicitly loaded from the running count (`MOV EAX,EDI`) before this
// one's own (non-shared) epilogue, so this return value IS established by the assembly.
int32_t target_list_refresh_mothers(const ai_view &v, const ai_calls &gc, int32_t player);

} // namespace detail

int32_t target_list_invalidate_by_id(int32_t player, int32_t target_id);
int32_t target_list_refresh_mothers(int32_t player);

} // namespace mh::ai
