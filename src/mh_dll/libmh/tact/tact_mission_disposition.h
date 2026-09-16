//
// tact/tact_mission_disposition.h -- TACT1A: the DISPOSITION line's per-entry parser.
//
//   llm_tact_mission_parse_disposition_spawn @0x00439526 (0x223)
//
// Parses one line under a mission's DISPOSITION section, e.g.
//   "       2 { 86,25 }  DIRECT:13  DEFENSE:GUARD2"
// into a character-type id (the leading "2") and a spawn tile {col,row}, e.g. that line yields
// return value 2, *out_col == 86, *out_row == 25.
//
// NOT ARMED IN THIS SLICE -- and the reason is NOT that these bodies are un-armable. That claim was
// written here first, from the shape of the signatures, and it was WRONG: shadow_region_closure was
// never run before it was made. Run afterwards, it reports 24 undeclared regions, every one at
// depth >= 1 under llm_fatal_cleanup -- the process-teardown cascade on the unterminated-brace
// abort path. Nothing at depth 0. So the honest statement is that this slice was verified OFFLINE
// ONLY, by a session that reached for the fixture before it reached for the rig -- the inverse of
// the loop's step 5/6 ordering.
//
// WHAT AN ARMED RUN WOULD AND WOULD NOT SEE. It compares the RETURN VALUE (that is how
// tact_pilot.cpp's quantize_facing_dir is armed, and gen_dll_shadow.py does not call it vacuous)
// plus any DECLARED region. It does not see a write through a caller-supplied out-pointer, because
// that lands on the caller's stack, which no region covers. Here that is *out_col / *out_row; the
// character-id RETURN value would still be compared.
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
struct mission_disposition_calls {
    void (*llm_fatal_cleanup)();         // llm_fatal_cleanup @0x0042613b
    void (*utils_abort)(int32_t status); // utils_abort @0x004da944
};

const mission_disposition_calls &live_mission_disposition_calls();

namespace detail {

// llm_tact_mission_parse_disposition_spawn @0x00439526.
//
// `out_col`/`out_row` are `uint32_t *` -- NOT the `uchar *` pair llm_tact_mission_parse_coord_pair
// uses elsewhere in this batch. That width difference is a fact about the two callers, not an
// inconsistency: confirmed from this function's own store sites (0x0043971a / 0x00439723, both
// dword stores) rather than assumed from the header.
//
// Scans `line` from the start for the first position that is either:
//   - ';'                       -> the rest of the line is a comment; returns -1 immediately,
//                                   *out_col/*out_row left untouched.
//   - end of line, no '{' found -> returns 0, *out_col/*out_row left untouched. Not an error path:
//                                   it is how the parser skips leading junk before the real line.
//   - a decimal run immediately preceded by a space (line[i-1]==' ') -> accumulated as the
//     CHARACTER-type id, a 32-bit int and the function's return value on the success path. The
//     scan then continues immediately after those digits, looking for '{'.
//   - '{' (whether or not a character id preceded it) -> opens the coordinate pair: the first
//     decimal run inside is the COLUMN (*out_col), the second is the ROW (*out_row). Each is
//     accumulated as an 8-bit value -- the original does the multiply/add with a byte MUL and an
//     8-bit ADD (0x0043966d-0x00439676 / 0x004396ce-0x004396d7), i.e. it wraps mod 256 at every
//     digit, not just once at the end. A single `uint8_t` accumulator reproduces that exactly
//     (modular addition commutes with the final truncation -- verified, not assumed).
//
// If no '{' is ever found, the OUTER scan retries starting one character further right, all the way
// to end of line (where it returns 0) -- a captured character id is NOT un-done on a failed retry;
// the scan only ever moves forward.
//
// FATAL PATH IS BEHAVIOUR, NOT ERROR HANDLING (0x004396ff-0x00439706): once inside a '{', if the
// scan for the matching '}' runs off the end of the line, the original calls llm_fatal_cleanup()
// then utils_abort(0) and never returns -- an unterminated brace is a malformed mission file, and
// the game terminates. Reproduced literally, not converted to a return code.
//
// LOOK-BEHIND (0x004395b7): the space-before-digit test reads line[i-1]. At i==0 that reads
// line[-1], one byte before the caller's buffer. This is the original's own behaviour, preserved
// as-is -- see the translation's uncertainties.
int32_t mission_parse_disposition_spawn(const mission_disposition_calls &calls, char *line,
                                        uint32_t *out_col, uint32_t *out_row);

} // namespace detail

// Live wrapper: this function touches no global state, so it is a plain pass-through -- the same
// shape as tact_pilot.cpp's quantize_facing_dir, which also takes no view/store parameter.
int32_t mission_parse_disposition_spawn(char *line, uint32_t *out_col, uint32_t *out_row);

} // namespace mh::tact
