//
// tact/tact_character_parse.h -- TACT1A: the CHARACTER block's FRAMES-table parser.
//
//   llm_tact_character_parse_frame_table @0x0043a649 (0x184)  writes _G_LLM_TACT_CHARACTER_TYPES
//
// The one function in this slice that touches state (see tmp/decomp_tact/_CONTEXT.md Sect. 3): it
// fills six of a character type's eight `frames[]` slots from the mission script's brace-delimited
// numeric list, e.g. `FRAMES { 8,8,8,8,8,8 }`. Slots 6 and 7 are left as loaded -- both numbers are
// named as TACT_ANIM_FRAME_SLOTS / TACT_ANIM_FRAMES_PARSED in tact_state.h.
//
// NOT ARMED IN THIS SLICE -- and the reason is NOT that these bodies are un-armable. That claim was
// written here first, from the shape of the signatures, and it was WRONG: shadow_region_closure was
// never run before it was made. Run afterwards, it reports _G_LLM_TACT_CHARACTER_TYPES at DEPTH 0
// -- a named region in the registry (RID_TACT_CHARACTER_TYPES) and one of the tactical hash slices.
// This function is plainly armable: one extra_regions entry and the comparison is real. The rest of
// its closure is the llm_fatal_cleanup teardown cascade at depth >= 1. So the honest statement is
// that this slice was verified OFFLINE ONLY, by a session that reached for the fixture before it
// reached for the rig -- the inverse of the loop's step 5/6 ordering.
//
// WHAT AN ARMED RUN WOULD AND WOULD NOT SEE. It compares the RETURN VALUE (that is how
// tact_pilot.cpp's quantize_facing_dir is armed, and gen_dll_shadow.py does not call it vacuous)
// plus any DECLARED region. It does not see a write through a caller-supplied out-pointer, because
// that lands on the caller's stack, which no region covers. This body writes no out-pointer at all
// -- its whole output is the declared region above, so an armed run here would see ALL of it. The
// original claim that an armed run "cannot distinguish this from another caller's write on a busy
// frame" was simply a bad argument: a shadow site snapshots and restores around the call.
//
// THE INSTRUMENT THAT SEES ALL OF IT is neither of those: promote the bodies and diff the per-frame
// state-hash trajectory of a promoted run against a stock golden. Every out-pointer write these
// parsers make lands in a table --tact-determinism already hashes (tact_char_types, tact_doors,
// tact_teleports, tact_fx_types, tact_units), because the caller filling those tables IS
// llm_tact_mission_load. That is T1 evidence and it needs no region declaration at all. It is not
// wired for this domain yet -- see tracker item TACT-AB.
//
// The evidence that DOES exist: net_selftest tacttest's parser cases, driven from real POZ*.DAT
// text and mutation-checked. That is T2 -- self-consistency with our reading of the disassembly.
//
#pragma once
#include <cstdint>

#include "tact/tact_state.h"

namespace mh::tact {

// The outward callees this unit makes, behind function pointers so `net_selftest tacttest` can
// substitute recorders: mh::call::<fn> marshals to an absolute game VA, which is unmapped in the
// offline oracle's process. Same shape as sim_ai_bldg_queue_construction.h.
//
// NOTE: `strtok` is STATEFUL -- the original passes a null first argument to continue scanning the
// same underlying line (see the three `mh::call::strtok(nullptr, ...)` call sites in the .cpp), so
// an oracle recorder standing in for this member must be a real tokeniser (its own internal
// save-pointer across calls), not a stub returning one canned string -- a canned stub would return
// the same token forever and the six-slot FRAMES loop would never terminate correctly.
struct character_parse_calls {
    char *(*strtok)(char *str, char *delim);    // strtok @0x004dab17
    int32_t (*utils_str_cmp)(char *a, char *b); // utils_str_cmp @0x004d16d0
    int32_t (*atoi)(char *nptr);                // atoi @0x004daa6e
    void (*llm_fatal_cleanup)();                // llm_fatal_cleanup @0x0042613b
    void (*utils_abort)(int32_t status);        // utils_abort @0x004da944
};

const character_parse_calls &live_character_parse_calls();

namespace detail {

// llm_tact_character_parse_frame_table @0x0043a649.
//
// `frames_directive_line` is the FULL line handed in by the mission loader -- the first strtok call
// below consumes and discards its leading token (the `FRAMES` keyword in every real mission file;
// the parser itself never compares against that word, only against the delimiter globals). The
// SECOND token must equal `_G_LLM_TACT_CFG_FRAME_TABLE_OPEN` ("{") or the mission is malformed and
// the game terminates (0x0043a6ca: `utils_str_cmp` returns 0 on a match, and JZ skips the abort --
// so this test aborts on NOT-equal).
//
// Then six comma/space-delimited numeric tokens are read (LIST_DELIM ", ", both characters are
// delimiters). Encountering `_G_LLM_TACT_CFG_FRAME_TABLE_CLOSE` ("}") before all six are read is
// ALSO fatal (0x0043a72c: JNZ skips the abort -- so this second test aborts on EQUAL, the opposite
// sense from the first). Each token's numeric value is atoi'd into `frames[i].count` (a byte, so
// the atoi result truncates on store, matching the original's `MOV byte ptr ...,DL`).
//
// `frames[i].start` is never read from the file -- it accumulates: slot 0 seeds from the character
// type's own `first_frame`; every later slot is `frames[i-1].start + frames[i-1].count * 8`, the
// `* 8` being the eight facing directions rendered per animation frame (SHL EAX,0x3 @0x0043a777).
void character_parse_frame_table(const character_parse_calls &calls, tact_store &own,
                                 char *frames_directive_line, int32_t character_type_index);

} // namespace detail

// Live wrapper: the logic applied to state().own. Signature matches the original's __watcall shape
// (EAX=frames_directive_line, EDX=character_type_index).
void character_parse_frame_table(char *frames_directive_line, int32_t character_type_index);

} // namespace mh::tact
