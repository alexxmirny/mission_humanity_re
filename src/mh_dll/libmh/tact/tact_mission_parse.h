//
// tact/tact_mission_parse.h -- TACT1A: the five generic-token mission-file PARSERS.
//
//   llm_tact_mission_parse_float          @0x004391e7 (0x19b)
//   llm_tact_mission_parse_int_token      @0x00439bdf (0x116)
//   llm_tact_mission_parse_keyword_int    @0x00439749 (0x1a2)
//   llm_tact_mission_parse_coord_pair     @0x00439382 (0x1a4)
//   llm_tact_mission_parse_quoted_string  @0x00439fa8 (0xc7)
//
// These are the low-level scanners llm_tact_mission_load (0x0043717f, not in this slice) builds
// every directive parser out of -- a decimal literal ("SPEED 0.010"), a brace-delimited int list
// ("FRAMES { 8,8,8,8,8,8 }", one int at a time), "KEYWORD value" pairs ("TIME = 100"), a
// "{col,row}" tile pair ("TARGET { 23,40 }"), and a '"'-quoted string ("MAP \"ALIEN_01.MAP\"").
// None of them touch tact_view/tact_store or any other global -- eight of this batch's ten
// functions are pure char* -> value/out-pointer scanners, and these five are all eight.
//
// NOT ARMED IN THIS SLICE -- and the reason is NOT that these bodies are un-armable. That claim was
// written here first, from the shape of the signatures, and it was WRONG: shadow_region_closure was
// never run before it was made. Run afterwards, it reports 24 undeclared regions for each of the
// five, EVERY ONE of them at depth >= 1 under llm_fatal_cleanup -- i.e. the process-teardown
// cascade on the malformed-input abort path, which no shipped POZ file reaches and which kills the
// process if it ever does. Nothing at depth 0. So the honest statement is that this slice was
// verified OFFLINE ONLY, by a session that reached for the fixture before it reached for the rig --
// the inverse of the loop's step 5/6 ordering.
//
// WHAT AN ARMED RUN WOULD AND WOULD NOT SEE. It compares the RETURN VALUE (that is how
// tact_pilot.cpp's quantize_facing_dir is armed, and gen_dll_shadow.py does not call it vacuous)
// plus any DECLARED region. It does not see a write through a caller-supplied out-pointer, because
// that lands on the caller's stack, which no region covers. These five write no global at all.
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
struct mission_parse_calls {
    void (*llm_fatal_cleanup)();
    void (*utils_abort)(int32_t status);
    double (*floor)(double x); // floor @0x004daadb
};

const mission_parse_calls &live_mission_parse_calls();

namespace detail {

// llm_tact_mission_parse_float @0x004391e7.
//
// Scans `line` for the first space-prefixed decimal literal (an integer part, optionally a '.'
// and a fractional part) and returns it as a double. `max_value` is a REAL second parameter
// (Stack[0x4]:8 in the .asm header) -- the exported .c's PLATE calls it "not a real param" and
// that PLATE is wrong: 0x0043932f-0x00439337 compares the parsed value against it and fatal-aborts
// if it is exceeded.
//
// Returns -1.0 (bit pattern low=0x00000000/high=0xbff00000 -- the IEEE-754 double -1.0 itself, not
// a sentinel flag squeezed into an otherwise-unused slot) if the current scan position is a ';'
// comment. Fatal-aborts (llm_fatal_cleanup + utils_abort, never returns) if the scan runs off the
// end of `line` without finding a value, or if the parsed value exceeds `max_value`.
//
// A digit only starts a value if the byte immediately BEFORE it is a space (0x00439281, the
// look-behind read) -- preserved exactly, including its read of line[-1] when the very first byte
// of `line` is a digit (see the translation report's uncertainties).
//
// The '.' character is itself counted into the fractional-digit tally, and the scale-down loop
// pre-decrements that tally once before it starts (0x004392fb-0x004392fe) -- the two off-by-ones
// cancel, so the loop runs exactly once per digit AFTER the point (3 divides for "0.010"'s 3
// fractional digits), which is the correct answer despite how the counting looks. See the .cpp for
// the arithmetic, worked out digit-by-digit.
//
// floor(value) IS called (0x00439324) but its comparison against `value`
// (0x00439329-0x0043932e: FCOMP/FNSTSW/SAHF) is never branched on before being overwritten by the
// NEXT compare's FNSTSW/SAHF (the real max_value test) -- the comparison is dead, but the CALL
// itself IS reproduced (through `calls.floor`), precisely so an oracle case can assert it happened;
// see the .cpp comment at the call site for the full address trace.
double mission_parse_float(const mission_parse_calls &calls, char *line, double max_value);

// llm_tact_mission_parse_int_token @0x00439bdf.
//
// Cursor-based tokenizer over `line`, driven by the caller-owned in/out cursor `*io_pos`: skips
// '{' and ' ' bytes, accumulates a decimal run into `*out_value` (zeroed unconditionally at
// entry), and stops at the first byte that is none of '{'/' '/digit. That stopping byte decides
// the return:
//   ','  or '-'  -> `*io_pos` is advanced PAST the delimiter, and the delimiter's own byte value
//                   is returned (as an int, e.g. 0x2c or 0x2d).
//   '}'          -> returns 0; `*io_pos` is left POINTING AT the '}', not advanced past it.
//   end of line  -> returns 0; `*io_pos` is left at its current value (unadvanced).
//   anything else -> llm_fatal_cleanup + utils_abort, never returns.
// `*io_pos` is read (not written) before any write -- the caller supplies the starting cursor, and
// this function never resets it to 0.
int32_t mission_parse_int_token(const mission_parse_calls &calls, char *line, uint32_t *io_pos,
                                int32_t *out_value);

// llm_tact_mission_parse_keyword_int @0x00439749.
//
// Scans `line` for `keyword`, trying a match at every byte that is unsigned > 0x20 (i.e. not
// whitespace/control); on a full match, an immediately-following decimal run (if any) is parsed
// into `*out_value`.
// Returns 0 on a match (`*out_value` = the parsed run, or 0 if none/malformed), -1 otherwise
// (`*out_value` is zeroed ONLY on the top-level "ran off the end of line" exit -- every other -1
// return leaves it untouched, including the ';'-comment exit and the mid-scan end-of-line exit
// reached after a failed match attempt; see the translation report).
//
// A failed match attempt advances the outer scan cursor by TWO, not one (0x004398ad then
// 0x004397a3) -- the byte immediately after a failed attempt's start is never itself tried as a
// fresh match start, a ';', or a whitespace-skip. Preserved as found, not "fixed" to +1.
//
// A keyword match followed by digits and then something other than ' '/end-of-line (e.g. a stray
// punctuation byte) forces `*out_value` to 0, DISCARDING any digits already accumulated that call
// (0x00439857-0x00439860) -- it does not return what was parsed so far.
int32_t mission_parse_keyword_int(char *line, char *keyword, int32_t *out_value);

// llm_tact_mission_parse_coord_pair @0x00439382.
//
// Scans `text` for a '{col,row}'-style braced pair of decimal BYTE values and writes them to
// `*out_col`/`*out_row` (uchar* per the .asm header -- llm_tact_mission_parse_disposition_spawn's
// sibling coord parse in this batch uses uint32_t* out-pointers instead; that width difference is
// a fact about the two callers, not an inconsistency). Each accumulator is a genuine 8-bit wrap
// (`col = col*10 + digit` computed with a byte-only MUL/ADD, 0x00439450-0x00439459) -- a
// three-digit value like "256" silently wraps to 0, not clamped.
//
// `col_done`/the two accumulators are function-scope, NOT reset per '{' encountered: if a first
// brace pair is abandoned partway through (see below) and the scan later finds a second '{' on the
// same line, that second attempt inherits whatever col_done/accumulator state the first one left
// behind. Preserved exactly (0x004393a1-0x004393b0 initialise these locals ONCE, before the outer
// scan loop, and nothing in the function resets them again).
//
// If `text` runs out before a col digit, a row digit, or the closing '}' is found, the brace pair
// is silently ABANDONED -- `*out_col`/`*out_row` are left untouched -- and the outer scan resumes
// one byte past the '{' that started the abandoned attempt (0x0043950d -> the outer "advance and
// continue" path), NOT past whatever partial content was consumed. Once a closing '}' IS found
// after both digit runs, `*out_col = col_accum; *out_row = row_accum;` and the function returns
// normally. Running off the end of `text` at the very top (no '{' ever found) or while seeking the
// closing '}' after both digit runs is fatal (llm_fatal_cleanup + utils_abort, never returns).
void mission_parse_coord_pair(const mission_parse_calls &calls, char *text, uint8_t *out_col,
                              uint8_t *out_row);

// llm_tact_mission_parse_quoted_string @0x00439fa8.
//
// Scans `line` for the first '"', then copies every byte up to the NEXT '"' (or NUL) into
// `out_buf`, NUL-terminating it. `out_buf`'s capacity is NOT a parameter of this function --
// nothing here bounds the copy against the caller's actual buffer size; the only terminators the
// copy loop recognises are '"' and NUL. Fatal-aborts (llm_fatal_cleanup + utils_abort, never
// returns) if either the opening or the closing '"' is missing (i.e. `line` runs out first).
void mission_parse_quoted_string(const mission_parse_calls &calls, char *line, char *out_buf);

} // namespace detail

// Live wrappers. None of the five touches tact_view/tact_store -- these are thin forwards to
// detail::, kept for the same reason tact_pilot.cpp keeps one for quantize_facing_dir and
// tact_cfg_keyword.h keeps them for its three functions (both also take no view/store parameter):
// a stable public entry point, matching the originals' shapes, for the offline oracle
// (net_selftest tacttest) to call.
double  mission_parse_float(char *line, double max_value);
int32_t mission_parse_int_token(char *line, uint32_t *io_pos, int32_t *out_value);
int32_t mission_parse_keyword_int(char *line, char *keyword, int32_t *out_value);
void    mission_parse_coord_pair(char *text, uint8_t *out_col, uint8_t *out_row);
void    mission_parse_quoted_string(char *line, char *out_buf);

} // namespace mh::tact
