//
// tact/tact_mission_disposition.cpp -- see tact_mission_disposition.h. Translated from the
// DISASSEMBLY (tmp/decomp_tact/llm_tact_mission_parse_disposition_spawn_00439526.asm), not from
// Ghidra's C: the exported .c mis-recovers this function's control flow (a trailing `for`/`do`
// block that is unreachable from the real branches, because the outer scan never actually falls
// out of its own loop the way the decompile's braces suggest). Every branch below is transcribed
// from the .asm labels directly instead.
//
#include "tact/tact_mission_disposition.h"

#include <cstdint>
#include <cstring>

#include "addr/mh_calls.gen.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h> // GetPrivateProfileIntA -- the [promote] gate, same as mh::sim's sites
#include "state/host_api.h"

namespace mh::tact {

const mission_disposition_calls &live_mission_disposition_calls() {
    static const mission_disposition_calls gc = {
        mh::tact_host().llm_fatal_cleanup,
        mh::tact_host().utils_abort,
    };
    return gc;
}

namespace {

// Every digit test in the original is an UNSIGNED byte compare (CMP byte ptr ...,0x30 / 0x39 with
// JC / JBE, never JL / JLE), so the zero-extending cast is load-bearing, not decoration.
inline bool is_ascii_digit(char c) {
    const uint8_t u = static_cast<uint8_t>(c);
    return u >= '0' && u <= '9';
}

} // namespace

namespace detail {

int32_t mission_parse_disposition_spawn(const mission_disposition_calls &calls, char *line,
                                        uint32_t *out_col, uint32_t *out_row) {
    int32_t  char_num     = 0;     // EBP-0x1c -- the CHARACTER number; the success-path return value
    uint8_t  col_byte     = 0;     // EBP-0x14 -- the first brace number ('col')
    uint8_t  row_byte     = 0;     // EBP-0x10 -- the second brace number ('row')
    bool     col_captured = false; // EBP-0x20
    uint32_t idx1         = 0;     // EBP-0x28 -- outer scan index (leading-number / '{' search)

outer_scan: // LAB_00439562
    // Recomputed every iteration on purpose: the original re-runs an inlined strlen() here every
    // time round the loop (REPNE SCASB @0x0043956a), not once. That quadratic cost is the original's
    // behaviour, not an oversight -- do not hoist it.
    if (idx1 > static_cast<uint32_t>(std::strlen(line))) {
        return 0; // LAB_00439737: ran off the end of the line without ever finding '{'.
    }

    if (line[idx1] == ';') {
        return -1; // LAB_0043958d: a comment starts here -- the whole line is discarded.
    }

    // LAB_00439599-0x004395bb: a decimal run immediately preceded by a space is the character id.
    // line[idx1 - 1] is a genuine look-behind -- at idx1==0 this reads line[-1], one byte before the
    // caller's buffer, exactly as the original does at 0x004395b7. Preserved as-is.
    if (is_ascii_digit(line[idx1]) && line[idx1 - 1] == ' ') {
        // LAB_004395bf: accumulate the full 32-bit character number and advance idx1 past it.
        while (is_ascii_digit(line[idx1])) {
            char_num = char_num * 10 + (static_cast<uint8_t>(line[idx1]) - '0');
            ++idx1;
        }
    }

    // LAB_004395f4: idx1 now sits either where it started, or right after the digits just consumed.
    if (line[idx1] != '{') {
        // LAB_00439732 -> LAB_0043957a: no brace here -- advance one character and retry the whole
        // outer scan. idx1 is NOT rewound past any digits just captured: a character id already
        // accumulated survives into the retry, matching the original (it never re-scans a number it
        // has already consumed).
        ++idx1;
        goto outer_scan;
    }

    {
        uint32_t idx2 = idx1 + 1; // EBP-0x24, set once at 0x00439607

    inner_scan: // LAB_0043960a
        if (idx2 > static_cast<uint32_t>(std::strlen(line))) {
            // LAB_0043962a's strlen guard failing -> LAB_00439732 -> LAB_0043957a: this '{' never
            // closed before end of line. Give up on it and retry the OUTER scan one past idx1 --
            // idx2 is simply discarded, idx1 is what advances.
            ++idx1;
            goto outer_scan;
        }

        if (is_ascii_digit(line[idx2]) && !col_captured) {
            // LAB_0043964a: the first brace number -> col_byte. Byte (mod-256) accumulation: the
            // original does an 8-bit MUL then an 8-bit ADD every digit (0x0043966d-0x00439676), and
            // a uint8_t accumulator reproduces that identically (modular addition commutes with the
            // truncation, so multiplying-then-truncating-then-adding-then-truncating each digit is
            // the same result as this single running uint8_t).
            while (is_ascii_digit(line[idx2])) {
                col_byte = static_cast<uint8_t>(col_byte * 10 +
                                                (static_cast<uint8_t>(line[idx2]) - '0'));
                ++idx2;
            }
            col_captured = true; // LAB_00439681
        }

        // LAB_00439688: the second number is only captured once the first has been (col_captured),
        // read from wherever idx2 now sits. The original does not explicitly skip the comma/space
        // between the two numbers here -- it falls through to the "neither branch taken" arm below,
        // which advances idx2 by one and re-enters the brace scan, one non-digit character at a
        // time, until a digit shows up with col_captured already true.
        if (is_ascii_digit(line[idx2]) && col_captured) {
            // LAB_004396ab: the second brace number -> row_byte, same byte accumulation as col_byte.
            while (is_ascii_digit(line[idx2])) {
                row_byte = static_cast<uint8_t>(row_byte * 10 +
                                                (static_cast<uint8_t>(line[idx2]) - '0'));
                ++idx2;
            }
        } else {
            // LAB_004396a6 -> LAB_0043972d -> LAB_00439622: not a capturable digit right now -- step
            // one character and re-enter the brace scan from the top.
            ++idx2;
            goto inner_scan;
        }

        // LAB_004396e2: scan forward for the closing '}'.
        while (line[idx2] != '}') {
            // THE GUARD HERE USES A DIFFERENT LENGTH FROM THE TWO SCANS ABOVE, and the difference
            // is one instruction. Both scan guards compute the inlined strlen and then do
            // `NOT ECX / DEC ECX / INC ECX` (0x0043956c-0x0043956f, 0x00439614-0x00439617), i.e.
            // strlen + 1, so they continue while `idx <= strlen`. THIS one omits the INC
            // (0x004396f7-0x004396f9), so ECX is strlen and `JA` continues only while
            // `strlen > idx2` -- it aborts at idx2 == strlen, one position EARLIER. Writing `>`
            // here instead of `>=` would still abort, one iteration later, after reading the byte
            // past the terminator; same verdict, one out-of-bounds read, and ASan would find it.
            if (idx2 >= static_cast<uint32_t>(std::strlen(line))) {
                // LAB_004396fd not taken -> the closing '}' never showed up before end of line.
                // THE FATAL PATH IS BEHAVIOUR, NOT ERROR HANDLING (0x004396ff-0x00439706): the
                // original terminates the process here and never returns. Reproduced literally.
                calls.llm_fatal_cleanup();
                calls.utils_abort(0);
                return 0; // unreachable: utils_abort tail-calls _exit; kept because the slot is int.
            }
            ++idx2; // LAB_0043970b
        }

        // LAB_00439713: success.
        *out_col = col_byte;
        *out_row = row_byte;
        return char_num;
    }
}

} // namespace detail

int32_t mission_parse_disposition_spawn(char *line, uint32_t *out_col, uint32_t *out_row) {
    return detail::mission_parse_disposition_spawn(live_mission_disposition_calls(), line, out_col,
                                                   out_row);
}

// ---- THE PROMOTED ARM IS GONE (fork F2E: tactical mode is demoted permanently) ------------------
//
// This TU used to carry counter-wrapped `promoted_arm::` adapters and an MH_EXPORT_REPLACE install
// for each, so our bodies could take the game's entry points. The fork's config selector has two
// hosted answers, `original` and `brokered`, and TACTICAL MODE IS ORIGINAL IN BOTH: the reimplemented
// spine the fork ships is the strategic one. So the install surface has no configuration left to be
// armed in, and an installer nothing can arm is not a dormant feature, it is a claim about what runs
// that is false in every run.
//
// THE BODIES ABOVE ARE UNTOUCHED and stay reachable two ways: the offline oracle (net_selftest
// tacttest) drives them directly, and their rebind rows survive (fork ruling Q2 -- the BIND survives,
// only the per-row runtime gate died), so a standalone host binds them unconditionally. What is gone
// is only the route that overwrote the game's own entry inside a hosted process.

} // namespace mh::tact
