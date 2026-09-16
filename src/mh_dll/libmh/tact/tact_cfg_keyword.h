//
// tact/tact_cfg_keyword.h -- TACT1A: the mission-file KEYWORD matchers, and the
// '-'-prefixed movement-order-token parser built directly on top of one of them.
//
//   llm_tact_cfg_keyword_token_match     @0x00439103 (0xe4)  keyword vs. the line's FIRST token
//   llm_tact_cfg_match_keyword_at_offset @0x0043a5b9 (0x90)  keyword vs. text at a given offset
//   llm_tact_mission_parse_command_token @0x0043a1d9 (0x3e0) the '-'-prefixed order line
//
// ONE UNIT, because parse_command_token CALLS cfg_match_keyword_at_offset directly, once per
// keyword tested (0x0043a289 onward) -- inside the unit that goes through detail::, not
// mh::call::. cfg_keyword_token_match is NOT called by either of this unit's other two functions;
// it is translated here because it is one of this batch's three assigned functions, not because
// anything in this file uses it.
//
// NOT ARMED IN THIS SLICE -- and the reason is NOT that these bodies are un-armable. That claim was
// written here first, from the shape of the signatures, and it was WRONG: shadow_region_closure was
// never run before it was made. Run afterwards, it reports for cfg_match_keyword_at_offset,
// literally "1 function reachable, 0 regions written -- the snapshot set covers the closure", i.e.
// trivially armable; and for mission_parse_command_token 24 undeclared regions, every one at depth
// >= 1 under llm_fatal_cleanup, the teardown cascade on the malformed-input abort path. So the
// honest statement is that this slice was verified OFFLINE ONLY, by a session that reached for the
// fixture before it reached for the rig -- the inverse of the loop's step 5/6 ordering.
//
// WHAT AN ARMED RUN WOULD AND WOULD NOT SEE. It compares the RETURN VALUE (that is how
// tact_pilot.cpp's quantize_facing_dir is armed, and gen_dll_shadow.py does not call it vacuous)
// plus any DECLARED region. It does not see a write through a caller-supplied out-pointer, because
// that lands on the caller's stack, which no region covers. The two matchers return their whole
// result, so for them the return-value comparison is not a partial view -- it is the entire output.
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

namespace mh::tact {

// The outward callees this unit makes, behind function pointers so `net_selftest tacttest` can
// substitute recorders: mh::call::<fn> marshals to an absolute game VA, which is unmapped in the
// offline oracle's process. Same shape as sim_ai_bldg_queue_construction.h.
struct cfg_keyword_calls {
    void (*llm_fatal_cleanup)();         // llm_fatal_cleanup @0x0042613b
    void (*utils_abort)(int32_t status); // utils_abort        @0x004da944
};

const cfg_keyword_calls &live_cfg_keyword_calls();

namespace detail {

// ---- the two closed discriminant domains this parser produces --------------------------------
//
// NEITHER has a Ghidra enum yet: docs/symbols.md tags llm_tact_mission_parse_command_token
// `todo:enum`. Every value below was re-derived independently from the .asm and then
// cross-checked against the tactical-probe work's "(b) The mission file is itself the scenario
// script" paragraph (MOVE 1, WAIT 8, TELE 0xa, DIRECT 7, RUN 0x1e, WALK 0x1c, DEFENSE: 0xb,
// REPEAT 0x40) -- the two sources agree on every keyword that prose names. Declared as a need
// (see the translation report) rather than left as scattered local constants (brief 17a): the
// conductor should promote this to a real `/llm` enum (e.g. E_TACT_MISSION_OPCODE) and clear the
// todo tag; these `enum class`es exist so the C++ in the meantime is typed and readable rather
// than a wall of magic numbers.
//
// NOTE this is a DIFFERENT domain from mh_tact_unit_record's `+0x01 op` command-queue field
// documented in docs/structs.md (1 move, 7 turn-then-attack, 8 timed wait, 0xa teleport-jump,
// 0xb advance-with-defstat, 0x1c/0x1e markers, 0x40 run, ...): that table's 0x40 means "run" and
// this parser's 0x40 means REPEAT. The two spaces share several numeric values by coincidence,
// not because they are the same enum -- do not fold them together.
enum class mission_opcode : int32_t {
    move         = 0x1,  // MOVE keyword,          opcode store @0x0043a2cc
    direct       = 0x7,  // DIRECT keyword,         opcode store @0x0043a38f
    wait         = 0x8,  // WAIT keyword,           opcode store @0x0043a341
    teleport     = 0xa,  // TELE keyword,           opcode store @0x0043a368
    defense_stat = 0xb,  // any DEFENSE:* keyword,  opcode store @0x0043a3b6/3e6/416/443/470
    walk         = 0x1c, // WALK keyword,           opcode store @0x0043a31a
    run          = 0x1e, // RUN keyword,            opcode store @0x0043a2f3
    repeat       = 0x40, // REPEAT keyword,         opcode store @0x0043a2a5
};

// The DEFENSE:* keyword's stance selector, written into out_arg0 alongside mission_opcode::defense_stat.
enum class defense_stance : int32_t {
    none   = 0, // DEFENSE:NONE,   arg0 store @0x0043a3bf
    guard1 = 1, // DEFENSE:GUARD1, arg0 store @0x0043a3ef
    guard2 = 2, // DEFENSE:GUARD2, arg0 store @0x0043a41f
    attack = 3, // DEFENSE:ATTACK, arg0 store @0x0043a44c
    sniper = 4, // DEFENSE:SNIPER, arg0 store @0x0043a479 -- declared_needs: this keyword's own
                // .rdata string (0x005007b0) has no _G_LLM_TACT_KW_ label yet.
};

// llm_tact_cfg_keyword_token_match @0x00439103.
//
// Skips leading whitespace (any byte <= 0x20, unsigned) in `line`, then compares `line` from the
// first non-whitespace byte against `keyword`, character-for-character. Returns 0 the INSTANT
// every character of `keyword` has matched -- it does NOT check what follows in `line`, so this
// is a PREFIX match against the line's first token, not a whole-token equality test (contrast
// cfg_match_keyword_at_offset below, which DOES require a boundary byte after the match; verified
// at the instruction level, not inferred -- see the translation report). Returns -1 on any
// mismatch, on hitting a ';' comment marker at the current scan position, or on reaching end of
// string without finding a non-whitespace byte. Not called by either of this unit's other two
// functions.
int32_t cfg_keyword_token_match(char *line, char *keyword);

// llm_tact_cfg_match_keyword_at_offset @0x0043a5b9.
//
// Compares `keyword` against `text_base` starting at the character offset `start_offset`,
// character-for-character. Matches only if `keyword` is fully consumed AND the text byte
// immediately after it is a boundary byte (unsigned < 0x21 -- whitespace, control, or NUL all
// count). On success returns the offset in `text_base` just past the matched keyword (always > 0
// for a non-empty keyword); returns 0 on any mismatch. `start_offset` is a CHARACTER index, and it
// is NOT bounds-checked against any buffer length -- the loop is bounded only by the two strings'
// NUL terminators, so an out-of-range offset is the caller's responsibility exactly as in the
// original.
int32_t cfg_match_keyword_at_offset(char *text_base, int32_t start_offset, char *keyword);

// llm_tact_mission_parse_command_token @0x0043a1d9.
//
// Finds the next '-'-prefixed movement-order token in `mission_line`, scanning character-by-
// character for '-' (not just testing the line's first byte -- a failed keyword match at one '-'
// resumes the scan for the NEXT '-' later in the same line rather than failing outright), skips
// spaces after the dash, then matches the fixed TWELVE-keyword chain below in this EXACT order:
// REPEAT, MOVE, RUN, WALK, WAIT, TELE, DIRECT, DEFENSE:NONE, DEFENSE:GUARD1, DEFENSE:GUARD2,
// DEFENSE:ATTACK, DEFENSE:SNIPER. (The batch brief said the chain tests ELEVEN keywords; the .asm
// tests twelve -- DEFENSE:SNIPER is a real twelfth arm at 0x0043a454. See the translation report.)
//
// On a match, parses a trailing comma-delimited decimal-digit list into out_arg0..out_arg3, in
// that order, until ';', end-of-line, or a malformed character.
//
// ALL FIVE out-pointers are zeroed unconditionally at entry, on every call, including the
// "nothing found" (return 0) path. A matched arm then may overwrite out_opcode alone, or both
// out_opcode and out_arg0 (the DEFENSE:* arms); the trailing digit loop may further overwrite
// out_arg0..out_arg3 in sequence as commas are seen -- so a DEFENSE:* line's out_arg0, already set
// by the arm, can be overwritten again by a stray leading digit in the remainder of the line. That
// is the original's behaviour (the digit-loop's fill index is never reset by a keyword match), not
// modelled around.
//
// Returns 0 if no '-'-token is found anywhere in the line (nothing to parse); 1 if a command was
// found and its argument list ran to ';', to end-of-string, or otherwise terminated normally.
// Never returns on a malformed argument-list character or a 5th ',' -- the original calls
// llm_fatal_cleanup() then utils_abort(0) and terminates the process (the fatal path is
// behaviour, not error handling).
int32_t mission_parse_command_token(const cfg_keyword_calls &calls, char *mission_line,
                                    int32_t *out_opcode, int32_t *out_arg0, int32_t *out_arg1,
                                    int32_t *out_arg2, int32_t *out_arg3);

} // namespace detail

// Live wrappers. None of the three touches tact_view/tact_store -- these are thin forwards to
// detail::, kept for the same reason tact_pilot.cpp keeps one for quantize_facing_dir (which also
// takes no view/store parameter): a stable public entry point, matching the originals' shapes, for
// the offline oracle (net_selftest tacttest) to call.
int32_t cfg_keyword_token_match(char *line, char *keyword);
int32_t cfg_match_keyword_at_offset(char *text_base, int32_t start_offset, char *keyword);
int32_t mission_parse_command_token(char *mission_line, int32_t *out_opcode, int32_t *out_arg0,
                                    int32_t *out_arg1, int32_t *out_arg2, int32_t *out_arg3);

} // namespace mh::tact
