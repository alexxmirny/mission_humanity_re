//
// tact_mission_parse_selftest.cpp -- `tacttest` cases for TACT1A: the five generic-token
// mission-file PARSERS (tact/tact_mission_parse.h/.cpp):
//
//   llm_tact_mission_parse_float          @0x004391e7
//   llm_tact_mission_parse_int_token      @0x00439bdf
//   llm_tact_mission_parse_keyword_int    @0x00439749
//   llm_tact_mission_parse_coord_pair     @0x00439382
//   llm_tact_mission_parse_quoted_string  @0x00439fa8
//
// EXPECTED BEHAVIOUR HAND-DERIVED FROM THE DISASSEMBLY (tmp/decomp_tact/llm_tact_mission_parse_*.asm)
// -- cross-checked instruction by instruction against the .cpp before any case below was written.
// Every address cited comes from the .asm, not from Ghidra's exported .c: per the oracle brief, this
// batch's exported .c mis-plates llm_tact_mission_parse_float's second parameter as "not real" while
// its own body (and the .asm) use it to gate the fatal-abort compare at 0x00439338 -- confirmed by
// direct re-read of the .asm during this file's authoring; no other .cpp/.asm divergence was found
// across the other four functions (each traced instruction-by-instruction below).
//
// NO SHADOW SITE (TACT1A, 2026-08-25). Every body here writes only through caller-supplied
// out-pointers or returns a plain scalar, so this file is the only evidence these five functions
// will ever have.
//
// REAL-TEXT PROVENANCE: every "real" line literal below is copied verbatim (same characters, same
// whitespace) from tmp/decomp_tact/_POZ_SAMPLES.md, which is itself literal text extracted from the
// six shipped POZ*.DAT files. Each such case says so and cites the doc line/section. Everything else
// is explicitly marked hand-built, and says why a real line could not cover it.
//
// A FINDING WORTH FLAGGING UP FRONT (see T-K1/T-K2 below): `mission_parse_keyword_int`'s "consume an
// immediately-following decimal run" loop tests `line[mpos] == ' '` as its FIRST check, before ever
// testing for a digit -- so if so much as one space separates the matched keyword from its value
// (true of EVERY real POZ line this parser could plausibly be run against, including "TIME = 100"
// and "ANGLE SEE      120"), the function returns 0 with `*out_value` left at 0, never reading the
// value at all. Confirmed against the .asm (0x00439822-0x0043982b); not a translation bug, and
// pinned deliberately rather than "fixed" to skip whitespace.
//
// ANOTHER FINDING (see T-C3): `mission_parse_coord_pair`'s "second `{` inherits the first's
// abandoned state" claim (the header banner, citing 0x004393a1-0x004393b0) is real -- the locals ARE
// initialised once and never reset -- but a literal two-OUTER-LOOP-attempt scenario where a first
// `{` genuinely abandons (runs off the true end of `text`) and a LATER, independently-rediscovered
// `{` then SUCCEEDS is not constructible from any input: the inner cursor's scan is a single
// monotonic forward pass per call, so any digit reachable by a later `{` is necessarily reachable
// (and would already have been consumed) by the earlier one's own unbroken scan -- an abandon
// therefore requires the ENTIRE remainder of `text` to be digit-free, which means any further `{`
// in that remainder is equally starved and can only abandon again, cascading to the top-level fatal
// (0x00439512). T-C3 below instead exercises the achievable, code-accurate form of the same "never
// reset" mechanism: a second `{` embedded mid-scan, swallowed as ordinary noise, with col found
// before it and row found only after passing over it -- see the case comment for the full argument.
//
#include "tact/tact_mission_parse.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>

#include "tact_test_support.h"

namespace mh::tact::test {
namespace {

using namespace mh::tact;

// ---- the fatal-path + floor-call recorder ------------------------------------------------------
//
// llm_fatal_cleanup/utils_abort mean the real process terminates. The recorder sets flags instead
// and lets the C++ keep executing (exactly as the original's unreachable trailer does) -- every case
// that reaches this path asserts the FLAGS, never the return value in isolation, and names which
// malformed input reached it. `floor_calls` answers the header's own claim that the dead FCOMP at
// 0x00439329 does not mean the CALL to floor (0x00439324) was omitted.
struct fatal_log {
    bool    cleanup_called = false;
    bool    abort_called   = false;
    int32_t abort_status   = 0x5a5a5a5a; // poison: real calls always pass 0
    int     floor_calls    = 0;
    void    reset() {
        cleanup_called = false;
        abort_called   = false;
        abort_status   = 0x5a5a5a5a;
        floor_calls    = 0;
    }
};
fatal_log g_fatal;

const mission_parse_calls &rec_calls() {
    static const mission_parse_calls c = {
        []() -> void { g_fatal.cleanup_called = true; },
        [](int32_t status) -> void {
            g_fatal.abort_called = true;
            g_fatal.abort_status = status;
        },
        [](double x) -> double {
            ++g_fatal.floor_calls;
            return std::floor(x);
        },
    };
    return c;
}

// Reconstructs the expected double the SAME way the body does: acc = acc*10 + digit for every digit
// (the '.' itself contributes no digit here, only to frac_count in the original -- see the .cpp
// comment at 0x004392fb), then divide by 10.0 once per digit that actually follows the point. Doubles
// compare EXACTLY (tact_test_support.h's ck_eq_d banner) so this must be bit-for-bit the same
// arithmetic order as the original, not the decimal literal a reader would guess.
double expected_float(std::initializer_list<int> all_digits, int frac_digits) {
    double acc = 0.0;
    for (int d : all_digits) acc = acc * 10.0 + static_cast<double>(d);
    for (int i = 0; i < frac_digits; ++i) acc /= 10.0;
    return acc;
}

// =====================================================================================================
// llm_tact_mission_parse_float @0x004391e7
// =====================================================================================================
void test_mission_parse_float() {
    printf("-- T-F: llm_tact_mission_parse_float -- real POZ1L CHARACTER-block operands, the ';' "
           "comment sentinel, the max_value gate, floor's call count, and the line[-1] look-behind\n");

    // T-F1..T-F5: real POZ1L CHARACTER 1 block lines (_POZ_SAMPLES.md), verbatim spacing. Each
    // asserts the EXACT double via expected_float, plus floor_calls==1 (called once, unconditionally,
    // at 0x00439324) and fatal NOT reached.
    {
        char line[] = "       SPEED        0.010";
        g_fatal.reset();
        double got = detail::mission_parse_float(rec_calls(), line, 100.0);
        ck_eq_d(got, expected_float({0, 0, 1, 0}, 3),
                "T-F1: real POZ1L 'SPEED 0.010' -> acc*10+digit per digit then /10 x3 fractional "
                "digits (pre-decrement cancels the dot's own count, 0x004392fb) == 0.010");
        ck_eq((uint32_t)g_fatal.floor_calls, 1u,
              "T-F1: floor() called exactly once @0x00439324, unconditionally on the success path");
        ck(!g_fatal.cleanup_called, "T-F1: fatal not reached");
    }
    {
        char line[] = "       ROTATE       0.025";
        g_fatal.reset();
        double got = detail::mission_parse_float(rec_calls(), line, 100.0);
        ck_eq_d(got, expected_float({0, 0, 2, 5}, 3),
                "T-F2: real POZ1L 'ROTATE 0.025' -> 0.025 via the same digit-by-digit accumulation");
        ck_eq((uint32_t)g_fatal.floor_calls, 1u, "T-F2: floor() called exactly once @0x00439324");
    }
    {
        char line[] = "       SPEED        0.0025";
        g_fatal.reset();
        double got = detail::mission_parse_float(rec_calls(), line, 100.0);
        ck_eq_d(got, expected_float({0, 0, 0, 2, 5}, 4),
                "T-F3: 'SPEED 0.0025' -- the only FOUR-decimal value shipped (_POZ_SAMPLES.md's "
                "distinct-operand list) -- 4 fractional digits, 4 divisions, == 0.0025");
        ck_eq((uint32_t)g_fatal.floor_calls, 1u, "T-F3: floor() called exactly once @0x00439324");
    }
    {
        char line[] = "       REPEAT         0.7";
        g_fatal.reset();
        double got = detail::mission_parse_float(rec_calls(), line, 100.0);
        ck_eq_d(got, expected_float({0, 7}, 1),
                "T-F4: 'REPEAT 0.7' -- the only SINGLE-decimal value shipped -- 1 fractional digit, "
                "1 division, == 0.7");
        ck_eq((uint32_t)g_fatal.floor_calls, 1u, "T-F4: floor() called exactly once @0x00439324");
    }
    {
        char line[] = "       RUN              2";
        g_fatal.reset();
        double got = detail::mission_parse_float(rec_calls(), line, 100.0);
        ck_eq_d(got, expected_float({2}, 0),
                "T-F5: real POZ1L 'RUN 2' -- a BARE INTEGER on a float-valued key (no '.', seen_dot "
                "stays false, the scale-down loop @0x00439301 never runs) -- returns exactly 2.0");
        ck_eq((uint32_t)g_fatal.floor_calls, 1u,
              "T-F5: floor() is STILL called for a dot-less value -- the call at 0x00439324 does not "
              "depend on seen_dot");
    }

    // T-F6: a real ';' comment line (_POZ_SAMPLES.md line 32, POZ1L header). Returns exactly -1.0 --
    // the stored bit pattern low=0x00000000/high=0xbff00000 @0x00439257 IS the IEEE-754 double -1.0
    // itself, not a flag squeezed into the slot.
    {
        char line[] = ";SEE ENEMY                    ; obcy sa widoczni (odkrywaja teren)";
        g_fatal.reset();
        double got = detail::mission_parse_float(rec_calls(), line, 100.0);
        ck_eq_d(got, -1.0,
                "T-F6: real POZ1L ';SEE ENEMY...' comment line -> exact double -1.0 (0xbff00000 "
                "@0x00439257 IS -1.0, not a sentinel flag)");
        ck_eq((uint32_t)g_fatal.floor_calls, 0u,
              "T-F6: floor() is NEVER reached on the comment path -- it returns before 0x00439324");
        ck(!g_fatal.cleanup_called, "T-F6: fatal not reached on a comment line");
    }

    // T-F7/T-F8: the max_value gate (0x0043932f-0x00439338, `if (max_value < acc) fatal`). T-F7 sits
    // exactly AT the boundary (acc == max_value): JBE takes the non-fatal path since the compare is
    // strict '<', so equality still returns normally -- this IS "just under the trigger". T-F8 is one
    // whole unit past it.
    {
        char line[] = " 12";
        g_fatal.reset();
        double got = detail::mission_parse_float(rec_calls(), line, 12.0);
        ck_eq_d(got, expected_float({1, 2}, 0),
                "T-F7: acc(12.0) == max_value(12.0) -- JBE @0x00439338 takes the non-fatal path on "
                "EQUALITY (compare is strict '<'), so the boundary itself does not abort");
        ck(!g_fatal.cleanup_called, "T-F7: fatal NOT reached at acc == max_value");
        ck_eq((uint32_t)g_fatal.floor_calls, 1u, "T-F7: floor() still called before the gate check");
    }
    {
        char line[] = " 13";
        g_fatal.reset();
        (void)detail::mission_parse_float(rec_calls(), line, 12.0);
        ck(g_fatal.cleanup_called && g_fatal.abort_called,
           "T-F8: acc(13.0) > max_value(12.0) -- fatal-aborts @0x0043933a/0x00439341, one past the "
           "boundary from T-F7");
        ck_eq((uint32_t)g_fatal.floor_calls, 1u,
              "T-F8: floor() is STILL called (0x00439324) even though the value is about to be "
              "rejected -- the call happens BEFORE the max_value compare, not gated by it");
    }

    // T-F9: the look-behind gate's POSITIVE arm at pos==0. `line[pos-1]` at pos==0 wraps to
    // `line[-1]` (0x00439281) -- a real buffer byte, not a crash. Here it's ' ', so the digit at
    // line[0] IS treated as the start of a value.
    {
        char storage[8];
        storage[0] = ' '; // this is "line[-1]" from the callee's point of view
        storage[1] = '5';
        storage[2] = '\0';
        char *line = storage + 1;
        g_fatal.reset();
        double got = detail::mission_parse_float(rec_calls(), line, 100.0);
        ck_eq_d(got, expected_float({5}, 0),
                "T-F9: line[-1]==' ' (a real byte one before the buffer passed as `line`) -> the "
                "look-behind read @0x00439281 sees a space -> digit at pos==0 DOES start a value -> "
                "5.0");
        ck(!g_fatal.cleanup_called, "T-F9: fatal not reached");
    }

    // T-F10: the look-behind gate's NEGATIVE arm at pos==0. Same buffer shape, line[-1] is NOT a
    // space -> the digit at pos==0 is rejected as a value-start, the outer scan advances past it
    // (pos=1), finds nothing else in the (one-char) line, and runs off the end without EVER finding
    // a value-start -> the TOP guard's fatal (0x00439359), distinct from the max_value fatal above.
    {
        char storage[8];
        storage[0] = 'X'; // "line[-1]" is NOT a space this time
        storage[1] = '5';
        storage[2] = '\0';
        char *line = storage + 1;
        g_fatal.reset();
        (void)detail::mission_parse_float(rec_calls(), line, 100.0);
        ck(g_fatal.cleanup_called && g_fatal.abort_called,
           "T-F10: line[-1]=='X' -> the look-behind gate @0x00439281 rejects the pos==0 digit -> "
           "outer scan runs off the end of \"5\" without ever finding a value-start -> the TOP guard's "
           "fatal @0x00439359 (distinct from T-F8's max_value fatal)");
        ck_eq((uint32_t)g_fatal.floor_calls, 0u,
              "T-F10: floor() is never reached -- this fatal fires before any number is parsed");
    }

    // T-F11: the same "ran off the end without ever finding a value" fatal as T-F10, but via
    // ordinary text with no digit anywhere at all (not the look-behind trick) -- an independent input
    // shape for the same claim in the header banner.
    {
        char line[] = "no numbers here";
        g_fatal.reset();
        (void)detail::mission_parse_float(rec_calls(), line, 100.0);
        ck(g_fatal.cleanup_called && g_fatal.abort_called,
           "T-F11 (hand-built, no digit anywhere): 'no numbers here' -> fatal @0x00439359, the "
           "general 'scan ran off the end without finding a value' case");
        ck_eq((uint32_t)g_fatal.floor_calls, 0u, "T-F11: floor() never reached");
    }
}

// =====================================================================================================
// llm_tact_mission_parse_int_token @0x00439bdf
// =====================================================================================================
void test_mission_parse_int_token() {
    printf("-- T-I: llm_tact_mission_parse_int_token -- the FRAMES-style cursor tokenizer, all three "
           "exit arms, and the unconditional *out_value zero\n");

    // T-I1: a six-slot, hand-varied FRAMES-shaped list ("{ 1,2,3,4,5,6 }" -- _POZ_SAMPLES.md warns
    // the real 'FRAMES { 8,8,8,8,8,8 }' is all-equal and CANNOT catch a slot mix-up), driven through
    // six successive calls exactly as llm_tact_mission_load's FRAMES loop would, pinning ORDER (each
    // call sees the value the PREVIOUS call's *io_pos left) and the ','-delimiter arm (io_pos advanced
    // PAST the delimiter, return == the delimiter's own byte value, 0x2c).
    {
        char     line[] = "{ 1,2,3,4,5,6 }";
        uint32_t io_pos = 0;
        int32_t  slots[6];
        int32_t  rets[6];
        uint32_t pos_after[6];
        for (int i = 0; i < 6; ++i) {
            rets[i]      = detail::mission_parse_int_token(rec_calls(), line, &io_pos, &slots[i]);
            pos_after[i] = io_pos;
        }
        static const int32_t  expect_val[6]    = {1, 2, 3, 4, 5, 6};
        static const uint32_t expect_ret_comma = 0x2cu; // ','
        for (int i = 0; i < 5; ++i) {
            char what[128];
            std::snprintf(what, sizeof(what),
                          "T-I1 call %d: FRAMES slot value == %d (hand-varied list, not the real "
                          "all-8s one, so a slot mix-up is visible)",
                          i, expect_val[i]);
            ck_eq((uint32_t)slots[i], (uint32_t)expect_val[i], what);
            std::snprintf(what, sizeof(what),
                          "T-I1 call %d: ',' delimiter -> return == the delimiter's OWN byte value "
                          "0x2c @0x00439ca9, not a boolean",
                          i);
            ck_eq((uint32_t)rets[i], expect_ret_comma, what);
        }
        ck_eq((uint32_t)slots[5], 6u, "T-I1 call 5: sixth (last) FRAMES slot == 6");
        // The '}' arm: return 0, *io_pos left POINTING AT the '}' (index 14), NOT advanced past it --
        // the header's explicit contrast with the ','/'-' arm above.
        ck_eq((uint32_t)rets[5], 0u, "T-I1 call 5: the closing '}' arm returns 0 @0x00439cc4");
        ck_eq(pos_after[5], 14u,
              "T-I1 call 5: '}' arm leaves *io_pos AT the brace (index 14 in \"{ 1,2,3,4,5,6 }\"), "
              "unlike the ','/'-' arm which advances PAST the delimiter");
    }

    // T-I2: the end-of-line arm -- ALSO returns 0, but leaves *io_pos at the string's own length
    // (index 2, one past the last real char), NOT "at a brace" -- the observable difference from
    // T-I1's '}' arm that the return value alone cannot show.
    {
        char     line[] = "42";
        uint32_t io_pos = 0;
        int32_t  out    = -1;
        int32_t  ret    = detail::mission_parse_int_token(rec_calls(), line, &io_pos, &out);
        ck_eq((uint32_t)out, 42u, "T-I2: digits accumulate acc*10+digit -> 42");
        ck_eq((uint32_t)ret, 0u, "T-I2: end-of-line arm ALSO returns 0, same as the '}' arm");
        ck_eq(io_pos, 2u,
              "T-I2: end-of-line arm leaves *io_pos == strlen(line) == 2 -- NOT pointing at a brace, "
              "the fact the '}' arm's return-0 case cannot show by itself");
    }

    // T-I3: the '-' delimiter arm -- same shape as ',' (io_pos advanced PAST it, return == the
    // delimiter's own byte, 0x2d this time).
    {
        char     line[] = "7-3";
        uint32_t io_pos = 0;
        int32_t  out    = -1;
        int32_t  ret    = detail::mission_parse_int_token(rec_calls(), line, &io_pos, &out);
        ck_eq((uint32_t)out, 7u, "T-I3: digit run before '-' -> 7");
        ck_eq((uint32_t)ret, 0x2du, "T-I3: '-' delimiter -> return == 0x2d @0x00439ca9, same arm as ','");
        ck_eq(io_pos, 2u, "T-I3: *io_pos advanced PAST the '-' (index 2), same as the ',' arm");
    }

    // T-I4: *out_value is zeroed UNCONDITIONALLY at entry (0x00439bfe-0x00439c01), even on an arm
    // that never runs the digit loop. Pre-seed a sentinel to prove it, using the '}' arm.
    {
        char     line[] = "}";
        uint32_t io_pos = 0;
        int32_t  out    = static_cast<int32_t>(0xdeadbeef);
        int32_t  ret    = detail::mission_parse_int_token(rec_calls(), line, &io_pos, &out);
        ck_eq((uint32_t)out, 0u,
              "T-I4: *out_value forced to 0 @0x00439bfe-0x00439c01 UNCONDITIONALLY, even though the "
              "'}' arm below it never touches out_value itself -- sentinel 0xdeadbeef proves it was "
              "actually zeroed, not merely left alone by luck");
        ck_eq((uint32_t)ret, 0u, "T-I4: '}' at position 0 -> return 0");
        ck_eq(io_pos, 0u, "T-I4: '}' arm leaves *io_pos unadvanced (still 0)");
    }

    // T-I5: *io_pos is READ, not reset -- a call starting from a nonzero cursor (as every call after
    // the first in T-I1 already demonstrated) behaves identically to one starting at 0. Restated here
    // in isolation with a fresh sentinel *out_value, for a case that stands on its own.
    {
        char     line[] = "xx9,";
        uint32_t io_pos = 2; // caller-supplied starting cursor, mid-buffer
        int32_t  out    = 0x12345;
        int32_t  ret    = detail::mission_parse_int_token(rec_calls(), line, &io_pos, &out);
        ck_eq((uint32_t)out, 9u, "T-I5: starting *io_pos==2 (caller-supplied, mid-buffer) is honoured "
                                 "as-is, not reset to 0 -- digit at index 2 ('9') is found");
        ck_eq((uint32_t)ret, 0x2cu, "T-I5: ',' delimiter -> 0x2c");
        ck_eq(io_pos, 4u, "T-I5: *io_pos advances from the caller-supplied start, past the ','");
    }

    // T-I6: the malformed-byte fatal (0x00439ccd) -- any byte that is none of '{'/' '/digit/','/'-'/
    // '}' aborts. Hand-built: no real POZ FRAMES/DISPOSITION-coordinate text carries such a byte.
    {
        char     line[] = "8#";
        uint32_t io_pos = 0;
        int32_t  out    = -1;
        g_fatal.reset();
        (void)detail::mission_parse_int_token(rec_calls(), line, &io_pos, &out);
        ck(g_fatal.cleanup_called && g_fatal.abort_called,
           "T-I6 (hand-built \"8#\"): the malformed byte '#' after a digit run reaches the fatal arm "
           "@0x00439ccd -- io_pos was left pointing at the offending byte per the header, but it does "
           "not matter since utils_abort never returns");
    }
}

// =====================================================================================================
// llm_tact_mission_parse_keyword_int @0x00439749 -- takes NO calls struct (no outward calls; see the
// unit's .h/.cpp -- the exported .c's plate is right about the signature here, only float's plate is
// wrong).
// =====================================================================================================
void test_mission_parse_keyword_int() {
    printf("-- T-K: llm_tact_mission_parse_keyword_int -- real POZ1L lines, the -1 exits' differing "
           "*out_value treatment, and the mismatch skip-by-two quirk\n");

    // T-K1/T-K2: THE HEADLINE FINDING. Both are real POZ1L lines. The "consume an immediately-
    // following decimal run" loop tests `line[mpos]==' '` BEFORE it ever tests for a digit
    // (0x00439822-0x0043982b) -- so a keyword match that is not IMMEDIATELY followed by a digit (i.e.
    // every real line here, which all have at least one space before the value) returns 0 with
    // *out_value left at its initial 0, never reading "100" or "120" at all.
    {
        char    line[]    = "TIME = 100";
        char    keyword[] = "TIME";
        int32_t out_value = 0x7a7a7a7a; // sentinel: gets overwritten to 0 either way, but shows it
        int32_t ret       = detail::mission_parse_keyword_int(line, keyword, &out_value);
        ck_eq((uint32_t)ret, 0u,
              "T-K1: real POZ1L 'TIME = 100' vs keyword TIME -> full match @0x00439819 -> return 0");
        ck_eq((uint32_t)out_value, 0u,
              "T-K1: *out_value == 0, NOT 100 -- line[mpos]==' ' right after the match (the space "
              "before '=') hits the terminator check @0x00439828 BEFORE any digit is ever seen -- "
              "'=' is never skipped, '100' is never reached (see this file's header-comment finding)");
    }
    {
        char    line[]    = "       ANGLE SEE      120";
        char    keyword[] = "ANGLE SEE"; // the two-word key, verbatim including its embedded space
        int32_t out_value = 0x7a7a7a7a;
        int32_t ret       = detail::mission_parse_keyword_int(line, keyword, &out_value);
        ck_eq((uint32_t)ret, 0u,
              "T-K2: real POZ1L '       ANGLE SEE      120' vs keyword 'ANGLE SEE' -> full 9-char "
              "match (including the embedded space) @0x00439819 -> return 0");
        ck_eq((uint32_t)out_value, 0u,
              "T-K2: *out_value == 0, NOT 120 -- same terminator-before-digit gap as T-K1, this time "
              "with SIX spaces between the matched keyword and the value, proving it is not merely a "
              "single-space special case");
    }

    // T-K3 (hand-built: no real POZ line has zero gap between a keyword and its value for this
    // function -- see T-K1/T-K2): proves the decimal-run consumption loop DOES work when the value is
    // truly immediately adjacent, so T-K1/T-K2's result is a real property of the real data's
    // spacing, not evidence the loop is dead code.
    {
        char    line[]    = "TIME100";
        char    keyword[] = "TIME";
        int32_t out_value = -1;
        int32_t ret       = detail::mission_parse_keyword_int(line, keyword, &out_value);
        ck_eq((uint32_t)ret, 0u, "T-K3 (hand-built \"TIME100\", zero gap): match -> return 0");
        ck_eq((uint32_t)out_value, 100u,
              "T-K3: *out_value == 100 -- with NO space between keyword and digits, the decimal-run "
              "loop @0x0043986c-0x00439887 does accumulate acc*10+digit correctly");
    }

    // T-K4 (hand-built): a keyword match immediately followed by digits and THEN a non-space,
    // non-end-of-line byte forces *out_value to 0, DISCARDING the digits already accumulated that
    // call (0x00439857-0x00439860) -- not "return what was parsed so far".
    {
        char    line[]    = "X12#";
        char    keyword[] = "X";
        int32_t out_value = -1;
        int32_t ret       = detail::mission_parse_keyword_int(line, keyword, &out_value);
        ck_eq((uint32_t)ret, 0u, "T-K4 (hand-built \"X12#\"): still a keyword match -> return 0");
        ck_eq((uint32_t)out_value, 0u,
              "T-K4: *out_value == 0, NOT 12 -- the '#' terminator (neither ' ' nor a digit nor "
              "end-of-line) forces the accumulated \"12\" to be DISCARDED @0x00439857-0x00439860");
    }

    // T-K5: THE MISMATCH SKIP-BY-TWO QUIRK. "XAB" vs keyword "AB": the attempt at pos 0 mismatches
    // on its very first character ('X' != 'A'), so the outer cursor advances by TWO (0x004398ad then
    // 0x004397a3) -- landing on index 2 ('B'), NOT index 1 ('A'). Index 1 is exactly where "AB"
    // (the keyword) actually starts in this string, but it is never tried as a fresh match start: the
    // skipped byte "WOULD have matched" and does not. The second attempt (at index 2, 'B' vs 'A')
    // also mismatches immediately, and pos then hits the exact-end shortcut (0x004398bd-0x004398c9),
    // returning -1 WITHOUT writing *out_value -- proven with a sentinel.
    {
        char    line[]    = "XAB";
        char    keyword[] = "AB";
        int32_t out_value = 0x1234;
        int32_t ret       = detail::mission_parse_keyword_int(line, keyword, &out_value);
        ck_eq((uint32_t)ret, 0xffffffffu,
              "T-K5 (hand-built \"XAB\" vs \"AB\"): -1 -- \"AB\" IS present at index 1, but the "
              "failed attempt at index 0 skips index 1 outright (+2, not +1, @0x004398ad/0x004397a3), "
              "so it is never found");
        ck_eq((uint32_t)out_value, 0x1234u,
              "T-K5: *out_value == 0x1234 (UNCHANGED) -- the exact-end shortcut exit "
              "(0x004398bd-0x004398c9) is the ONE -1 exit that leaves *out_value untouched, unlike "
              "the top-of-loop guard below");
    }

    // T-K6: the TOP-OF-LOOP over-the-end guard (0x0043978b-0x0043979e, `pos > strlen(line)`) -- the
    // ONLY -1 exit that WRITES *out_value (forces it to 0, 0x004398d0-0x004398d9). "XXXX" vs "A" never
    // matches at any of its 4 positions and, unlike T-K5, has enough trailing bytes that pos reaches
    // this guard rather than the exact-end shortcut.
    {
        char    line[]    = "XXXX";
        char    keyword[] = "A";
        int32_t out_value = 0xcafe;
        int32_t ret       = detail::mission_parse_keyword_int(line, keyword, &out_value);
        ck_eq((uint32_t)ret, 0xffffffffu, "T-K6 (hand-built \"XXXX\" vs \"A\"): -1, never matches");
        ck_eq((uint32_t)out_value, 0u,
              "T-K6: *out_value forced to 0 @0x004398d0-0x004398d9 -- the TOP-OF-LOOP guard exit, "
              "the ONLY -1 arm that writes *out_value (contrast T-K5's sentinel surviving)");
    }

    // T-K7: the ';' comment exit (0x004397b1-0x004397bd) -- ends the search immediately and, per the
    // header, deliberately leaves *out_value untouched (a THIRD, distinct -1 exit from T-K5/T-K6).
    // Reuses the real POZ1L ';SEE ENEMY...' comment line for provenance.
    {
        char    line[]    = ";SEE ENEMY                    ; obcy sa widoczni (odkrywaja teren)";
        char    keyword[] = "SEE";
        int32_t out_value = 0xbeef;
        int32_t ret       = detail::mission_parse_keyword_int(line, keyword, &out_value);
        ck_eq((uint32_t)ret, 0xffffffffu,
              "T-K7: real POZ1L ';SEE ENEMY...' comment line -> -1 immediately, the ';' at pos 0 "
              "@0x004397b1 ends the search before any candidate is ever tried");
        ck_eq((uint32_t)out_value, 0xbeefu,
              "T-K7: *out_value == 0xbeef (UNCHANGED) -- the ';'-comment exit is deliberately left "
              "untouched, a THIRD distinct behaviour from T-K5 (also untouched, different site) and "
              "T-K6 (zeroed)");
    }

    // T-K8: THE REAL CALLER'S SHAPE, and it is what resolves the surprise the other cases raise.
    // T-K1/T-K2 drive real lines (`TIME = 100`, `ANGLE SEE      120`) and both return 0 with
    // *out_value still 0, because the digit loop's FIRST test @0x00439828 is `byte == ' '` and every
    // one of those lines has a gap between key and value. That reads like a broken parser until you
    // look at who actually calls it: llm_tact_mission_load passes the COLON-ATTACHED keywords --
    // `_G_LLM_TACT_KW_DIRECT_COLON` ("DIRECT:", 0x005006cc, loaded at 0x004388dc) and
    // `_G_LLM_TACT_KW_DEFENSE_NONE` (0x005006d4, loaded at 0x004388f9) -- against a DISPOSITION
    // line, where the digits follow the keyword with NO separator at all. This function parses the
    // `KEY:value` form only; the spaced `KEY   value` form is a different path in mission_load.
    {
        char    line[]    = "       2 { 86,25 }  DIRECT:13  DEFENSE:GUARD2";
        char    keyword[] = "DIRECT:";
        int32_t out_value = 0xbeef;
        int32_t ret       = detail::mission_parse_keyword_int(line, keyword, &out_value);
        ck_eq((uint32_t)ret, 0u,
              "T-K8: real POZ1L DISPOSITION line with the REAL keyword the game passes "
              "(_G_LLM_TACT_KW_DIRECT_COLON @0x005006cc, call site 0x004388e7) -> 0 (found)");
        ck_eq((uint32_t)out_value, 13u,
              "T-K8: *out_value == 13 from 'DIRECT:13' -- the digits abut the keyword, so the "
              "space test @0x00439828 does not fire and the accumulator @0x00439875 runs. THIS is "
              "the form the function exists for; T-K1/T-K2's 0 on spaced lines is the original's "
              "behaviour on input it never receives, not a defect");
    }

    // T-K9: the OTHER real keyword the same call site passes -- a colon keyword with NO digits after
    // it (0x004388f9). The scan matches, the digit loop immediately sees 'N' (not a space, not a
    // digit), and 0x00439857 forces *out_value to 0 while still returning 0. So the caller cannot
    // tell "matched with no number" from "matched the number 0" by the return alone -- it uses the
    // return only as a did-match flag (TEST EAX,EAX / JNZ @0x00438909).
    {
        char    line[]    = "       2 { 86,25 }  DIRECT:13  DEFENSE:NONE";
        char    keyword[] = "DEFENSE:NONE";
        int32_t out_value = 0xbeef;
        int32_t ret       = detail::mission_parse_keyword_int(line, keyword, &out_value);
        ck_eq((uint32_t)ret, 0u,
              "T-K9: real keyword _G_LLM_TACT_KW_DEFENSE_NONE @0x005006d4 (call site 0x00438904) "
              "matches -> 0");
        ck_eq((uint32_t)out_value, 0u,
              "T-K9: *out_value FORCED to 0 @0x0043985a because the byte after the keyword is "
              "neither a space nor a digit -- the poison 0xbeef is gone, so this is a real write, "
              "not an untouched slot");
    }
}

// =====================================================================================================
// llm_tact_mission_parse_coord_pair @0x00439382
// =====================================================================================================
void test_mission_parse_coord_pair() {
    printf("-- T-C: llm_tact_mission_parse_coord_pair -- real DISPOSITION lines, the byte-wrap "
           "accumulator, the persistence of col_done/accumulators across an embedded second '{', and "
           "all three end-of-string outcomes\n");

    // T-C1: real POZ1L DISPOSITION line (_POZ_SAMPLES.md 'DISPOSITION entries'), a genuine
    // DISPOSITION-header coordinate pair with UNEQUAL col/row so a swap would be caught.
    {
        char    line[] = "       2 { 86,25 }  DIRECT:13  DEFENSE:GUARD2";
        uint8_t col = 0xaa, row = 0xbb; // distinct poison, not each other's real value
        g_fatal.reset();
        detail::mission_parse_coord_pair(rec_calls(), line, &col, &row);
        ck_eq((uint32_t)col, 86u,
              "T-C1: real POZ1L '2 { 86,25 } ...' -> col accumulates via the byte MUL/ADD "
              "@0x00439450-0x00439459 -> 86");
        ck_eq((uint32_t)row, 25u,
              "T-C1: row accumulates via the sibling MUL/ADD @0x004394b1-0x004394ba -> 25, distinct "
              "from col so a col/row swap would fail this pair");
        ck(!g_fatal.cleanup_called, "T-C1: fatal not reached on a well-formed pair");
    }

    // T-C2: real POZ1L DISPOSITION line with a RIGHT-ALIGNED, TWO-SPACE-PADDED single digit
    // ('{  9,52 }') -- the case that separates "scan forward for the first digit" (what the code
    // actually does: the col-gate @0x0043940d/the "neither fired" fallback just skips non-digit bytes
    // one at a time) from "the digit is at a fixed column" (which would misread this as a 2-digit or
    // garbled value).
    {
        char    line[] = "       4 {  9,52 }  DIRECT:16  DEFENSE:GUARD2";
        uint8_t col = 0xaa, row = 0xbb;
        g_fatal.reset();
        detail::mission_parse_coord_pair(rec_calls(), line, &col, &row);
        ck_eq((uint32_t)col, 9u,
              "T-C2: real POZ1L '4 {  9,52 } ...' -- TWO leading spaces before the col digit -- the "
              "scan skips them one byte at a time (@0x0043940d's gate simply doesn't fire on ' ') and "
              "still finds col == 9, proving it scans forward rather than reading a fixed column");
        ck_eq((uint32_t)row, 52u, "T-C2: row == 52");
        ck(!g_fatal.cleanup_called, "T-C2: fatal not reached");
    }

    // T-C3: THE PERSISTENCE CLAIM, in the one input shape that actually exercises it (see this file's
    // header-comment finding for why a genuine two-OUTER-LOOP-attempt success is unconstructible from
    // any input -- the inner cursor's scan is a single monotonic forward pass per call, so any digit
    // reachable from a later '{' is also reachable, and would already be consumed, by an earlier
    // one's own unbroken scan). Here col completes from the FIRST '{', then a SECOND, literal '{'
    // appears mid-scan BEFORE any row digit is found -- since col_done/col_accum are initialised once
    // (0x004393a1-0x004393b0) and the inner loop treats '{' as ordinary non-digit noise (it is not
    // special until AFTER a row digit has been found), this second '{' is swallowed without resetting
    // anything, and the row digit found AFTER it still completes the pair using the FIRST '{'s col.
    // If col_done/col_accum WERE reset by the second '{', the row-gate @0x0043946b would not have
    // fired at all at that point (col_done would be false), the '3' would be mis-read as a NEW col
    // digit instead, and the function would then run off the end with only a stray '}' left -- i.e.
    // it would ABANDON rather than succeed. That it succeeds, using exactly col==7 (from before the
    // second '{') and row==3 (from after it), is the proof.
    {
        char    text[] = "{7Q{3}";
        uint8_t col = 0xaa, row = 0xbb;
        g_fatal.reset();
        detail::mission_parse_coord_pair(rec_calls(), text, &col, &row);
        ck_eq((uint32_t)col, 7u,
              "T-C3 (hand-built \"{7Q{3}\"): col == 7 -- accumulated from the FIRST '{' before the "
              "embedded second '{' ever appears");
        ck_eq((uint32_t)row, 3u,
              "T-C3: row == 3 -- found only AFTER passing over the embedded second '{' as ordinary "
              "noise; col_done/col_accum (init'd once @0x004393a1-0x004393b0) were never reset by it, "
              "so the row-gate @0x0043946b still fires here instead of mis-reading '3' as a fresh "
              "col digit (which would instead have abandoned the pair -- see the case comment)");
        ck(!g_fatal.cleanup_called,
           "T-C3: succeeds normally -- had the second '{' reset state, this input would abandon "
           "(no further digit exists after '3}' for a would-be fresh row), not succeed");
    }

    // T-C4: the accumulator is a genuine 8-bit WRAP (byte-only MUL/ADD @0x00439450-0x00439459), not a
    // clamp -- a 3-digit run silently wraps mod 256. "256" -> (((0*10+2)*10+5)*10+6) mod 256 == 0.
    {
        char    text[] = "{256,10}";
        uint8_t col = 0xaa, row = 0xbb;
        g_fatal.reset();
        detail::mission_parse_coord_pair(rec_calls(), text, &col, &row);
        ck_eq((uint32_t)col, 0u,
              "T-C4 (hand-built \"{256,10}\"): col == 0, NOT 256 -- the byte-only MUL/ADD "
              "@0x00439450-0x00439459 wraps 256 mod 256 to 0, silently, not clamped");
        ck_eq((uint32_t)row, 10u, "T-C4: row == 10, unaffected");
        ck(!g_fatal.cleanup_called, "T-C4: wrap is silent, not fatal");
    }

    // T-C5: an ABANDONED brace pair (running off the true end of `text` while still seeking a digit,
    // 0x004393ed-0x00439400/0x00439400's JMP to the "silently abandon" path) does NOT itself call
    // fatal and does NOT touch *out_col/*out_row -- it only resumes the OUTER scan, which here (no
    // further usable '{' exists) eventually reaches the TOP-LEVEL "no pair ever completed" fatal
    // (0x00439512) exactly once. Sentinels on out_col/out_row prove the abandon path itself is silent.
    {
        char    text[] = "{Z{Q"; // two abandon-prone '{'s, no digit anywhere
        uint8_t col = 0x11, row = 0x22;
        g_fatal.reset();
        detail::mission_parse_coord_pair(rec_calls(), text, &col, &row);
        ck(g_fatal.cleanup_called && g_fatal.abort_called,
           "T-C5 (hand-built \"{Z{Q\"): eventually fatal-aborts @0x00439512 -- neither '{' ever finds "
           "a digit, so both attempts silently abandon and the outer scan itself runs off the end");
        ck_eq((uint32_t)col, 0x11u,
              "T-C5: *out_col left at its sentinel 0x11 -- the silent-abandon path never writes it "
              "(only a SUCCESSFUL pair does, at 0x004394f6-0x00439504)");
        ck_eq((uint32_t)row, 0x22u, "T-C5: *out_row likewise left at its sentinel 0x22");
    }

    // T-C6: no '{' anywhere in `text` at all -- the simplest route to the same top-level fatal
    // (0x00439512) as T-C5, without ever entering the inner cursor loop.
    {
        char    text[] = "no brace here at all";
        uint8_t col = 0x33, row = 0x44;
        g_fatal.reset();
        detail::mission_parse_coord_pair(rec_calls(), text, &col, &row);
        ck(g_fatal.cleanup_called && g_fatal.abort_called,
           "T-C6 (hand-built, no '{' at all): fatal-aborts @0x00439512 -- the outer scan runs off the "
           "end having never found an opening brace");
        ck_eq((uint32_t)col, 0x33u, "T-C6: *out_col untouched");
        ck_eq((uint32_t)row, 0x44u, "T-C6: *out_row untouched");
    }

    // T-C7: BOTH digit runs found, but the closing '}' is missing before end -- the OTHER fatal site
    // (0x004394e2), distinct from T-C5/T-C6's top-level one: this is the "ran off the end while
    // seeking the closer, AFTER both digits" guard.
    {
        char    text[] = "{12,34"; // no closing '}'
        uint8_t col = 0x55, row = 0x66;
        g_fatal.reset();
        detail::mission_parse_coord_pair(rec_calls(), text, &col, &row);
        ck(g_fatal.cleanup_called && g_fatal.abort_called,
           "T-C7 (hand-built \"{12,34\", no closer): fatal-aborts @0x004394e2 -- both digit runs "
           "(col=12, row=34) were found, but no '}' precedes the end of `text`");
        ck_eq((uint32_t)col, 0x55u,
              "T-C7: *out_col untouched -- the write @0x004394f6 only happens on a genuine SUCCESS "
              "(closer found), never on this fatal path");
        ck_eq((uint32_t)row, 0x66u, "T-C7: *out_row likewise untouched");
    }
}

// =====================================================================================================
// llm_tact_mission_parse_quoted_string @0x00439fa8
// =====================================================================================================
void test_mission_parse_quoted_string() {
    printf("-- T-Q: llm_tact_mission_parse_quoted_string -- real MAP lines (one with a trailing "
           "comment AFTER the closing quote), poisoned output buffers, and both missing-quote "
           "fatals\n");

    // T-Q1: real POZ1L header line (_POZ_SAMPLES.md 'MAP    \"ALIEN_01.MAP\"'). out_buf is seeded with
    // a poison pattern first so a short OR long copy is visible, not just a wrong prefix.
    {
        char line[] = "MAP    \"ALIEN_01.MAP\"";
        char out_buf[32];
        std::memset(out_buf, 0xcc, sizeof(out_buf));
        g_fatal.reset();
        detail::mission_parse_quoted_string(rec_calls(), line, out_buf);
        ck(std::strcmp(out_buf, "ALIEN_01.MAP") == 0,
           "T-Q1: real POZ1L 'MAP    \"ALIEN_01.MAP\"' -> out_buf == \"ALIEN_01.MAP\" exactly, copied "
           "@0x0043a010-0x0043a044 and NUL-terminated @0x0043a046-0x0043a04c");
        ck_eq((uint32_t)(uint8_t)out_buf[std::strlen("ALIEN_01.MAP") + 1], 0xccu,
              "T-Q1: the poison byte right AFTER the NUL is untouched -- an exact-length copy, not an "
              "overrun past the terminator");
        ck(!g_fatal.cleanup_called, "T-Q1: fatal not reached on a well-formed quoted string");
    }

    // T-Q2: THE TRAP CASE. Real POZ1L line with a TRAILING COMMENT AFTER the closing quote
    // (_POZ_SAMPLES.md: 'MAP    \"alien_03.MAP\" ; teleport nightmare'). A naive terminator reading
    // (e.g. "copy to the next ';'") gets this wrong; the actual body only ever recognises '"' or NUL
    // as terminators (0x0043a010-0x0043a021), so the copy must stop exactly at the closing '"' and
    // never touch " ; teleport nightmare" at all.
    {
        char line[] = "MAP    \"alien_03.MAP\" ; teleport nightmare";
        char out_buf[48];
        std::memset(out_buf, 0xcc, sizeof(out_buf));
        g_fatal.reset();
        detail::mission_parse_quoted_string(rec_calls(), line, out_buf);
        ck(std::strcmp(out_buf, "alien_03.MAP") == 0,
           "T-Q2: real POZ1L 'MAP    \"alien_03.MAP\" ; teleport nightmare' -> out_buf == "
           "\"alien_03.MAP\" exactly -- the trailing ' ; teleport nightmare' AFTER the closing quote "
           "is never copied, since only '\"' or NUL terminate the copy loop");
        ck_eq((uint32_t)(uint8_t)out_buf[std::strlen("alien_03.MAP") + 1], 0xccu,
              "T-Q2: poison right after the NUL is untouched -- confirms the copy really stopped at "
              "the closing quote, not somewhere later that happened to also produce the right prefix");
        ck(!g_fatal.cleanup_called, "T-Q2: fatal not reached");
    }

    // T-Q3: missing OPENING quote anywhere in `line` -> fatal @0x00439ff7 (distinct site from T-Q4).
    {
        char line[] = "MAP ALIEN_01.MAP"; // no quotes at all
        g_fatal.reset();
        char out_buf[16];
        std::memset(out_buf, 0xcc, sizeof(out_buf));
        detail::mission_parse_quoted_string(rec_calls(), line, out_buf);
        ck(g_fatal.cleanup_called && g_fatal.abort_called,
           "T-Q3 (hand-built \"MAP ALIEN_01.MAP\", no quotes): fatal-aborts @0x00439ff7 -- the "
           "opening-quote search runs off the end of `line` without ever finding a '\"'");
    }

    // T-Q4: opening quote present, closing quote missing -> fatal @0x0043a05a (the OTHER site).
    {
        char line[] = "MAP \"ALIEN_01.MAP"; // opening quote, no closing quote
        g_fatal.reset();
        char out_buf[16];
        std::memset(out_buf, 0xcc, sizeof(out_buf));
        detail::mission_parse_quoted_string(rec_calls(), line, out_buf);
        ck(g_fatal.cleanup_called && g_fatal.abort_called,
           "T-Q4 (hand-built \"MAP \\\"ALIEN_01.MAP\", opening quote only): fatal-aborts @0x0043a05a "
           "-- the copy loop runs off the end of `line` looking for the closing '\"'");
    }
}

} // namespace

void run_mission_parse_tests() {
    test_mission_parse_float();
    test_mission_parse_int_token();
    test_mission_parse_keyword_int();
    test_mission_parse_coord_pair();
    test_mission_parse_quoted_string();
}

} // namespace mh::tact::test
