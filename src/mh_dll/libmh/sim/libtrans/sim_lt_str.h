#pragma once
#include <cstdint>

namespace mh::sim {
namespace detail {

// llm_str_char_subst @0x004546e8.
//
// mode is an UNSIGNED byte -- the original dispatches with JC/JBE (0x0045472f-0x00454748), not
// signed compares:
//   mode 1 (STRIP)    -- append the char only when it differs from BOTH c1 and c2
//                         (0x00454751-0x0045477f).
//   mode 2 (REPLACE)  -- append c2 when the char equals c1, else the char; ALWAYS advances the
//                         output cursor (0x00454784-0x004547b2).
//   mode 0, mode >= 3 -- no-op arm (0x0045474f / 0x0045474a-0x004547b5): nothing is ever appended,
//                         so the writeback below TRUNCATES THE STRING TO EMPTY. That is the
//                         original's exact behaviour, not a bug -- preserve it.
//
// Builds the result in a FIXED 256-byte scratch buffer (matching the original's `local_124[256]` --
// SUB ESP,0x11c @0x004546f7 and the 300-byte stack-probe argument) with NO length check anywhere --
// a source string longer than 255 chars overruns this buffer exactly as it overran the original's
// stack frame. Do not add a bound; the oracle fixture must never feed this an over-long string (an
// ASan run under run_selftests.py would flag the resulting overflow as a defect, when it is in fact
// faithful to the original).
//
// Writes the result back over `str` in place (0x004547d2-0x004547e8). The original's copy-back is
// Watcom's unrolled two-bytes-per-iteration strcpy; a plain byte-at-a-time copy is observably
// identical here (the second byte of a pair is only ever stored when the first was non-NUL). The
// original's source-side cursor (`local_24`) is a dead local after the copy and is not modeled as
// an out-parameter.
void str_char_subst(char *str, uint8_t mode, char c1, char c2);

} // namespace detail

// Live wrapper -- identical to detail:: (this function touches no sim state). Matches the
// original's __watcall shape (EAX=str, DL=mode, BL=c1, CL=c2).
void str_char_subst(char *str, uint8_t mode, char c1, char c2);

} // namespace mh::sim
