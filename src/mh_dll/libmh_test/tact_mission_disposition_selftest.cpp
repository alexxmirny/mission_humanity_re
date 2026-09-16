//
// tact_mission_disposition_selftest.cpp -- `net_selftest.exe tacttest` cases for
// llm_tact_mission_parse_disposition_spawn (tact/tact_mission_disposition.h/.cpp) @0x00439526,
// TACT1A.
//
// NO SHADOW SITE (see the .h banner): the body writes through two CALLER-supplied out-pointers
// that no declared region covers, and the trajectory oracle parses each mission exactly once at
// entry. This file, and `net_selftest.exe tacttest`, are the only evidence these branches will
// ever get.
//
// Recorders: `llm_fatal_cleanup`/`utils_abort` mean the process terminates in the original, so a
// case that reaches them is asserted on a FLAG the recorder sets, never on a return value -- the
// C++ keeps executing past the call exactly as the original's unreachable trailer does.
//
// Every case builds its line inside a larger backing buffer and never hands the parser a string
// literal or a c_str(): the space-before-digit look-behind at 0x004395b7 reads `line[-1]`, which
// for a leading digit (i==0) is one byte BEFORE the string's own storage. Only a real, owned
// buffer lets a case control that byte.
//
#include "tact/tact_mission_disposition.h"

#include <cstring>

#include "tact_test_support.h"

namespace mh::tact::test {
namespace {

using mh::tact::mission_disposition_calls;

struct call_log {
    bool    fatal_fired  = false;
    int     fatal_n      = 0;
    bool    abort_fired  = false;
    int     abort_n      = 0;
    int32_t abort_status = 0;

    void reset() { *this = call_log{}; }
};
call_log g_log;

const mission_disposition_calls &recording_calls() {
    static const mission_disposition_calls gc = {
        []() {
            g_log.fatal_fired = true;
            ++g_log.fatal_n;
        },
        [](int32_t status) {
            g_log.abort_fired = true;
            ++g_log.abort_n;
            g_log.abort_status = status;
        },
    };
    return gc;
}

// Places NUL-terminated `text` at storage+1, with `before` planted at storage[0] -- so a case
// that hits the look-behind at i==0 (0x004395b7) reads a REAL, controlled byte one before the
// returned line, instead of undefined memory before a string literal.
char *make_line(char *storage, char before, const char *text) {
    storage[0] = before;
    std::strcpy(storage + 1, text);
    return storage + 1;
}

// Distinct, non-symmetric poison for the two out-pointers -- if a comment/no-brace exit wrote
// either one, or the two stores were swapped, these values would show it.
constexpr uint32_t kPoisonCol = 0xDEADBEEFu;
constexpr uint32_t kPoisonRow = 0x00C0FFEEu;

} // namespace

void run_mission_disposition_tests() {
    printf("-- D1: real POZ1L DISPOSITION line -- the headline case, col != row so a swap fails\n");
    {
        char     storage[64];
        char    *line = make_line(storage, ' ', "       2 { 86,25 }  DIRECT:13  DEFENSE:GUARD2");
        uint32_t col = kPoisonCol, row = kPoisonRow;
        g_log.reset();
        int32_t ret = mh::tact::detail::mission_parse_disposition_spawn(recording_calls(), line,
                                                                        &col, &row);
        ck_eq((uint32_t)ret, 2u,
              "D1: character id '2' from real POZ1L text (leading digit preceded by a space, "
              "0x004395b7) -- verbatim tmp/decomp_tact/_POZ_SAMPLES.md line 104");
        ck_eq(col, 86u, "D1: col from '{ 86,25 }', byte MUL/ADD @0x0043966d-0x00439676");
        ck_eq(row, 25u,
              "D1: row from '{ 86,25 }', byte MUL/ADD @0x004396ce-0x004396d7 -- 86 != 25, so a "
              "swap of the two out-pointer stores (0x0043971a / 0x00439723) fails this pair");
        ck(!g_log.fatal_fired, "D1: the well-formed line never reaches the fatal path");
    }

    printf("-- D2: real POZ1L line with TWO spaces before a single-digit column\n");
    {
        char     storage[64];
        char    *line = make_line(storage, ' ', "       4 {  9,52 }  DIRECT:16  DEFENSE:GUARD2");
        uint32_t col = kPoisonCol, row = kPoisonRow;
        g_log.reset();
        int32_t ret = mh::tact::detail::mission_parse_disposition_spawn(recording_calls(), line,
                                                                        &col, &row);
        ck_eq((uint32_t)ret, 4u, "D2: character id '4' -- verbatim _POZ_SAMPLES.md line 112");
        ck_eq(col, 9u,
              "D2: col is 9, found by SCANNING FORWARD past two spaces (inner_scan's non-digit "
              "arm, 0x0043960a-0x00439648) before the digit at 0x0043964a -- a parser that assumed "
              "the column sat at a FIXED offset instead of scanning for it would misparse this "
              "right-aligned single digit");
        ck_eq(row, 52u, "D2: row from '52' after the comma, byte accumulation @0x004396ce-0x004396d7");
    }

    printf("-- D3: a ';' comment line returns -1 and leaves BOTH out-pointers untouched\n");
    {
        char     storage[64];
        char    *line = make_line(storage, ' ', "       ; nasi");
        uint32_t col = kPoisonCol, row = kPoisonRow;
        g_log.reset();
        int32_t ret = mh::tact::detail::mission_parse_disposition_spawn(recording_calls(), line,
                                                                        &col, &row);
        ck_eq((uint32_t)ret, (uint32_t)-1,
              "D3: a ';' comment (verbatim _POZ_SAMPLES.md line 103) returns -1 @0x0043958d");
        ck_eq(col, kPoisonCol, "D3: *out_col is left UNTOUCHED on the comment path");
        ck_eq(row, kPoisonRow, "D3: *out_row is left UNTOUCHED on the comment path");
    }

    printf("-- D4: a line with no '{' ever found returns 0, ALSO leaving both out-pointers "
           "untouched -- but 0 is NOT D3's -1\n");
    {
        char     storage[64];
        char    *line = make_line(storage, ' ', "GROUND \"PODLOGA.TLO\"");
        uint32_t col = kPoisonCol, row = kPoisonRow;
        g_log.reset();
        int32_t ret = mh::tact::detail::mission_parse_disposition_spawn(recording_calls(), line,
                                                                        &col, &row);
        // "Nothing was parsed" two ways, two DIFFERENT return values: a case that only checks
        // "not a valid spawn" cannot tell D3 and D4 apart -- this pair can.
        ck_eq((uint32_t)ret, 0u,
              "D4: no '{' anywhere in the whole line (verbatim _POZ_SAMPLES.md line 35) returns 0 "
              "@0x00439737, the outer scan's run-off-the-end exit -- DIFFERENT from D3's -1, even "
              "though both parsed nothing");
        ck_eq(col, kPoisonCol, "D4: *out_col is left UNTOUCHED on the no-brace path");
        ck_eq(row, kPoisonRow, "D4: *out_row is left UNTOUCHED on the no-brace path");
    }

    printf("-- D5: the 8-bit accumulators wrap mod 256 EVERY digit -- the original's own bug, "
           "preserved deliberately\n");
    {
        // SYNTHETIC (not from a shipped POZ file): id and col are BOTH driven to 300 on the same
        // digit run, so one case shows the id (32-bit int, 0x004395e0-0x004395e9) does NOT wrap
        // while col (8-bit byte MUL/ADD, 0x0043966d-0x00439676) DOES.
        char     storage[64];
        char    *line = make_line(storage, ' ', "300 { 300,25 }");
        uint32_t col = kPoisonCol, row = kPoisonRow;
        g_log.reset();
        int32_t ret = mh::tact::detail::mission_parse_disposition_spawn(recording_calls(), line,
                                                                        &col, &row);
        ck_eq((uint32_t)ret, 300u,
              "D5: the character id accumulates as a full 32-bit int (0x004395e0-0x004395e9) -- "
              "300 comes out as 300, NOT wrapped");
        ck_eq(col, 44u,
              "D5: col wraps mod 256 AT EVERY DIGIT (byte MUL @0x0043966d, byte ADD @0x00439674) "
              "-- 300 mod 256 == 44, not clamped to 255 and not literal 300. This is the "
              "ORIGINAL's own bug, preserved deliberately, not a translation defect");
        ck_eq(row, 25u, "D5: row unaffected (25 < 256, nothing to wrap)");
    }

    printf("-- D6: look-behind POSITIVE arm -- a digit at line[0] preceded by a REAL space "
           "captures the id\n");
    {
        char     storage[64];
        char    *line = make_line(storage, ' ' /* the byte at line[-1] */, "2 { 5,6 }");
        uint32_t col = kPoisonCol, row = kPoisonRow;
        g_log.reset();
        int32_t ret = mh::tact::detail::mission_parse_disposition_spawn(recording_calls(), line,
                                                                        &col, &row);
        ck_eq((uint32_t)ret, 2u,
              "D6: line[-1]==' ' at the look-behind site 0x004395b7 (a real, controlled byte one "
              "before this buffer's own storage, not a string literal) -- the id IS captured");
        ck_eq(col, 5u, "D6: col from '{ 5,6 }'");
        ck_eq(row, 6u, "D6: row from '{ 5,6 }'");
    }

    printf("-- D7: look-behind NEGATIVE arm -- the SAME content, only the byte before line[0] "
           "differs\n");
    {
        char     storage[64];
        char    *line = make_line(storage, 'X' /* the byte at line[-1] */, "2 { 5,6 }");
        uint32_t col = kPoisonCol, row = kPoisonRow;
        g_log.reset();
        int32_t ret = mh::tact::detail::mission_parse_disposition_spawn(recording_calls(), line,
                                                                        &col, &row);
        ck_eq((uint32_t)ret, 0u,
              "D7: line[-1]=='X' (not a space) at the SAME look-behind site 0x004395b7 -- the id "
              "is NOT captured, so the return value is 0 (char_num's initial value), even though "
              "the brace pair below is byte-identical to D6");
        ck_eq(col, 5u, "D7: col is unaffected by the failed id capture -- '{ 5,6 }' still parses");
        ck_eq(row, 6u, "D7: row is unaffected by the failed id capture");
    }

    printf("-- D8: an unterminated '{' is FATAL -- the recorder fires, not a return code\n");
    {
        char     storage[64];
        char    *line = make_line(storage, ' ', "4 { 24,41 DIRECT:1");
        uint32_t col = kPoisonCol, row = kPoisonRow;
        g_log.reset();
        (void)mh::tact::detail::mission_parse_disposition_spawn(recording_calls(), line, &col,
                                                                &row);
        ck(g_log.fatal_fired,
           "D8: an unterminated '{' (no closing '}' anywhere in the line) reaches "
           "llm_fatal_cleanup() @0x004396ff -- a malformed DISPOSITION entry terminates the "
           "process, it is not an error return");
        ck_eq((uint32_t)g_log.abort_n, 1, "D8: utils_abort(0) fires exactly once @0x00439706");
        ck_eq((uint32_t)g_log.abort_status, 0u, "D8: utils_abort's status argument is 0");
        ck_eq(col, kPoisonCol, "D8: *out_col is never reached on the fatal path");
        ck_eq(row, kPoisonRow, "D8: *out_row is never reached on the fatal path");
    }

    printf("-- D9: the closing-brace guard is `idx2 >= strlen`, not `>` -- the boundary that "
           "tells the two readings apart\n");
    {
        // The two SCAN guards (INC ECX @0x0043956f and @0x00439617) compute strlen+1 and so
        // continue while idx <= strlen. THIS guard (0x004396f7-0x004396f9) omits that INC, so it
        // continues only while strlen > idx2 -- it fires fatal at idx2 == strlen, one position
        // EARLIER than the other two. Build a line whose real content has no '}', ending exactly
        // where the closing-brace scan reaches idx2 == strlen(line), and plant a DECOY '}' one
        // byte PAST that terminator -- memory the CORRECT (`>=`) guard must never touch, because
        // it fatals from the terminator byte alone. A `>` reading would instead take one more
        // loop iteration, read the decoy '}', and return SUCCESS instead of fataling: a directly
        // observable divergence, not just an out-of-bounds read.
        char storage[32];
        std::memset(storage, 'Z', sizeof(storage)); // filler; never dereferenced by a correct run
        char *line = storage + 1;
        std::strcpy(line, "2 { 5,6"); // no closing '}'; strlen(line) == 7, terminator at line[7]
        line[8] = '}';                // the decoy, ONE byte past the terminator

        uint32_t col = kPoisonCol, row = kPoisonRow;
        g_log.reset();
        (void)mh::tact::detail::mission_parse_disposition_spawn(recording_calls(), line, &col,
                                                                &row);
        ck(g_log.fatal_fired,
           "D9: fatals from the terminator alone (idx2==strlen==7, the `>=` guard @0x004396f7-"
           "0x004396f9) WITHOUT ever reading the decoy '}' at line[8] -- a `>` reading would read "
           "it and return success instead, which this assertion would catch as fatal_fired==false");
        ck_eq(col, kPoisonCol,
              "D9: *out_col untouched -- confirms the fatal path was taken, not the "
              "decoy-driven success path");
        ck_eq(row, kPoisonRow, "D9: *out_row untouched, same reason");
    }

    printf("-- D10: a captured character id is NOT re-scanned or undone by a failed '{' retry\n");
    {
        // " 2 X { 7,8 }": the id '2' is captured on the first outer-scan pass (preceded by a real
        // space). The very next character is not '{', so the outer scan retries one character at
        // a time (0x00439732 -> 0x0043957a) through the stray 'X' token and the space after it --
        // neither re-enters the id-capture block (0x004395bf-0x004395f4 only fires on a digit
        // preceded by a space, and 'X' is not a digit) -- until it finds the real '{'. char_num is
        // never reset anywhere on that path.
        char     storage[64];
        char    *line = make_line(storage, ' ', " 2 X { 7,8 }");
        uint32_t col = kPoisonCol, row = kPoisonRow;
        g_log.reset();
        int32_t ret = mh::tact::detail::mission_parse_disposition_spawn(recording_calls(), line,
                                                                        &col, &row);
        ck_eq((uint32_t)ret, 2u,
              "D10: the id captured before the failed '{' search (the scan retried past the "
              "stray 'X') SURVIVES into the eventual successful brace parse -- a reset-on-retry "
              "bug would return 0 here instead");
        ck_eq(col, 7u, "D10: col from the eventually-found '{ 7,8 }'");
        ck_eq(row, 8u, "D10: row from the eventually-found '{ 7,8 }'");
    }
}

} // namespace mh::tact::test
