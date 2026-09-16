//
// sim/libtrans/sim_lt_str.cpp -- see sim_lt_str.h. Translated from the DISASSEMBLY
// (tmp/decomp_lib_trans/llm_str_char_subst_004546e8.asm), not from Ghidra's C draft.
//
#include "sim/libtrans/sim_lt_str.h"

namespace mh::sim {
namespace detail {

void str_char_subst(char *str, uint8_t mode, char c1, char c2) {
    // 0x004546f7: fixed 256-byte scratch, matching local_124[256] -- see the header for why no
    // bound is added here.
    char    scratch[256];
    int32_t out = 0;

    // 0x00454717-0x004547b8: walk the source string char by char (0x0045471d tests for NUL).
    for (int32_t i = 0; str[i] != '\0'; ++i) {
        const char ch = str[i];

        // 0x0045472f-0x00454748: UNSIGNED dispatch (JC/JBE) on mode. mode 0 takes the JC to
        // LAB_0045474f and mode >= 3 takes the trailing JMP at 0x0045474a -- both land on the
        // shared no-op tail (0x004547b5) without ever appending.
        if (mode == 1) {
            // 0x00454751-0x0045477f: STRIP. The write-cursor INC (0x0045477f) is INSIDE this arm,
            // not the shared tail -- collapsing it into the tail would append on every iteration
            // instead of only the ones that pass the STRIP test.
            if (ch != c1 && ch != c2) {
                scratch[out] = ch;
                ++out;
            }
        } else if (mode == 2) {
            // 0x00454784-0x004547b2: REPLACE. Always advances -- the INC lives at the shared tail
            // 0x004547b2, reached by both sub-arms of this branch (equal-to-c1 and not).
            scratch[out] = (ch == c1) ? c2 : ch;
            ++out;
        }
        // mode 0 / mode >= 3: no-op arm, see above -- nothing appended, `out` unchanged.
    }

    // 0x004547bd-0x004547c0: terminate at the output cursor.
    scratch[out] = '\0';

    // 0x004547c8-0x004547e8: copy the scratch buffer back over `str` in place. The original is
    // Watcom's unrolled two-bytes-per-iteration strcpy; this plain byte-at-a-time loop is
    // observably identical (see the header for why).
    for (int32_t i = 0;; ++i) {
        str[i] = scratch[i];
        if (scratch[i] == '\0') break;
    }
}

} // namespace detail

void str_char_subst(char *str, uint8_t mode, char c1, char c2) {
    detail::str_char_subst(str, mode, c1, c2);
}

} // namespace mh::sim
