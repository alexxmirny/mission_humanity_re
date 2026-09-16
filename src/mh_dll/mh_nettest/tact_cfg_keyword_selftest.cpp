#include "tact/tact_cfg_keyword.h"

#include <cstdint>

#include "tact_test_support.h"

namespace mh::tact::test {
namespace {

using namespace mh::tact;

// ---- the fatal-path recorder ---------------------------------------------------------------------
//
// llm_fatal_cleanup/utils_abort mean the real process terminates. The recorder sets a flag instead
// and lets the C++ keep executing (exactly as the original's unreachable trailer does) -- every case
// that reaches this path asserts the FLAG, never the return value in isolation, and names which
// malformed input reached it.
struct fatal_log {
    bool    cleanup_called = false;
    bool    abort_called   = false;
    int32_t abort_status   = 0x5a5a5a5a; // poison: real calls always pass 0
    void    reset() {
        cleanup_called = false;
        abort_called   = false;
        abort_status   = 0x5a5a5a5a;
    }
};
fatal_log g_fatal;

const cfg_keyword_calls &recording_calls() {
    static const cfg_keyword_calls c = {
        []() -> void { g_fatal.cleanup_called = true; },
        [](int32_t status) -> void {
            g_fatal.abort_called = true;
            g_fatal.abort_status = status;
        },
    };
    return c;
}

// ---- mission_parse_command_token helper: poison all 5 out-pointers with DISTINCT non-zero values,
// call, and hand back everything the caller needs. Every case below routes through this, so the
// "zeroed unconditionally at entry" property (0x0043a1fa-0x0043a22e) is exercised on EVERY call, not
// just the dedicated P0 cases -- a case that only checked the success arm would pass vacuously.
struct parse_out {
    int32_t ret    = -12345;
    int32_t opcode = 0x7a7a7a7a;
    int32_t arg0   = 0x11111111;
    int32_t arg1   = 0x22222222;
    int32_t arg2   = 0x33333333;
    int32_t arg3   = 0x44444444;
};

parse_out call_parse(const cfg_keyword_calls &calls, char *line) {
    parse_out r;
    r.ret = detail::mission_parse_command_token(calls, line, &r.opcode, &r.arg0, &r.arg1, &r.arg2,
                                                &r.arg3);
    return r;
}

} // namespace

// ==== K: llm_tact_cfg_keyword_token_match @0x00439103 =============================================
// Prefix match against the line's FIRST token; whitespace-skip is UNSIGNED <=0x20; ';' or
// end-of-string abort the scan with -1; no retry elsewhere in the line (single-shot, unlike
// mission_parse_command_token's outer dash-rescan).
namespace {

void test_token_match() {
    printf("-- K: llm_tact_cfg_keyword_token_match -- prefix match, whitespace skip, sentinels\n");

    // K1: real text, no leading whitespace. _POZ_SAMPLES.md line 34 (POZ1L header):
    // `MAP    "ALIEN_01.MAP"`. keyword "MAP" is a strict prefix of the token "MAP" -- exact length --
    // so this also proves the exact-length case returns 0, matched @0x004391a9-0x004391b9.
    {
        char line[] = "MAP    \"ALIEN_01.MAP\"";
        char kw[]   = "MAP";
        ck_eq((uint32_t)detail::cfg_keyword_token_match(line, kw), 0u,
              "K1: real POZ1L line 34 `MAP    \"ALIEN_01.MAP\"` vs keyword MAP -> match (0) @0x004391a9");
    }

    // K2: real text, 7 leading spaces. _POZ_SAMPLES.md line 59 (POZ1L CHARACTER 1 block):
    // `       NAME "0"`. Proves the whitespace-skip loop @0x00439172-0x00439175 actually walks past
    // MULTIPLE leading spaces, not just tolerates a single one.
    {
        char line[] = "       NAME \"0\"";
        char kw[]   = "NAME";
        ck_eq((uint32_t)detail::cfg_keyword_token_match(line, kw), 0u,
              "K2: real POZ1L line 59 `       NAME \"0\"` (7 leading spaces) vs NAME -> match (0) "
              "@0x00439172 whitespace skip");
    }

    // K3: the UNSIGNED <=0x20 compare @0x00439172 skips more than just the space character -- a tab
    // (0x09) counts too. No real POZ line carries a leading tab (the doc says the real alignment is
    // "one to seven spaces" only), so this is HAND-BUILT specifically to exercise the unsigned
    // compare's non-space bytes.
    {
        char line[] = "\tMOVE";
        char kw[]   = "MOVE";
        ck_eq((uint32_t)detail::cfg_keyword_token_match(line, kw), 0u,
              "K3 (hand-built, no real POZ line has a leading tab): a leading TAB (0x09) is skipped by "
              "the UNSIGNED <=0x20 compare @0x00439172, same as a space");
    }

    // K4: THE DISTINGUISHING CASE (highest value in this file). token_match is a strict PREFIX match
    // that returns the instant `keyword` is consumed, with NO check of what follows in `line`
    // (@0x004391a9-0x004391b9, verified above the mismatch/whitespace branches). "RUNNER" against
    // "RUN" therefore matches. Hand-built: no real POZ token happens to be a keyword-plus-suffix
    // shape, but the property is unambiguous from the .asm.
    {
        char line[] = "RUNNER";
        char kw[]   = "RUN";
        ck_eq((uint32_t)detail::cfg_keyword_token_match(line, kw), 0u,
              "K4 (hand-built): \"RUNNER\" vs keyword \"RUN\" -- token_match is a PREFIX match and "
              "returns 0 (matched) the instant RUN is consumed @0x004391a9, ignoring the trailing "
              "\"NER\"");
    }

    // K5: the ';' comment-marker exit, on a REAL full comment line. _POZ_SAMPLES.md line 32 (POZ1L
    // header): `;SEE ENEMY                    ; obcy sa widoczni (odkrywaja teren)`. The scan sees
    // ';' at position 0, before any whitespace skip is even needed, and returns -1 immediately
    // @0x0043915b-0x00439167 -- it never gets far enough to compare "SEE" against anything.
    {
        char line[] = ";SEE ENEMY                    ; obcy sa widoczni (odkrywaja teren)";
        char kw[]   = "SEE";
        ck_eq((uint32_t)detail::cfg_keyword_token_match(line, kw), 0xffffffffu,
              "K5: real POZ1L line 32 `;SEE ENEMY ...` -- the leading ';' aborts the scan with -1 "
              "@0x0043915b-0x00439167, before SEE is ever compared");
    }

    // K6a/K6b: end-of-string without ever finding a non-whitespace byte -> -1, via the OUTER loop's
    // own bound check @0x00439143-0x00439148 (JA/JMP), not the inner mismatch path. Hand-built (an
    // empty or blank input line is a real occurrence between POZ directives, but the doc does not
    // transcribe a literal blank line as a case).
    {
        char line[] = "";
        char kw[]   = "MOVE";
        ck_eq((uint32_t)detail::cfg_keyword_token_match(line, kw), 0xffffffffu,
              "K6a (hand-built): empty line -> -1, outer loop's length check fires on the first pass "
              "@0x00439143");
    }
    {
        char line[] = "   ";
        char kw[]   = "MOVE";
        ck_eq((uint32_t)detail::cfg_keyword_token_match(line, kw), 0xffffffffu,
              "K6b (hand-built): whitespace-only line -> -1, the whitespace-skip loop exhausts the "
              "string and THEN the outer bound check @0x00439143 fires");
    }

    // K7: a mismatching first token, on REAL text. _POZ_SAMPLES.md line 35 (POZ1L header):
    // `GROUND "PODLOGA.TLO"` vs keyword "MAP" -- 'G' != 'M' at the very first character
    // @0x00439187, single-shot return -1 (this function does NOT retry elsewhere in the line, unlike
    // mission_parse_command_token's outer dash-rescan).
    {
        char line[] = "GROUND \"PODLOGA.TLO\"";
        char kw[]   = "MAP";
        ck_eq((uint32_t)detail::cfg_keyword_token_match(line, kw), 0xffffffffu,
              "K7: real POZ1L line 35 `GROUND \"PODLOGA.TLO\"` vs keyword MAP -> mismatch -> -1 "
              "@0x00439187, no retry");
    }

    // K8: exact-length match at end-of-line (keyword IS the whole token, nothing trailing at all).
    // Real text, POZ1L DISPOSITION header (_POZ_SAMPLES.md line 102): `DISPOSITION`.
    {
        char line[] = "DISPOSITION";
        char kw[]   = "DISPOSITION";
        ck_eq((uint32_t)detail::cfg_keyword_token_match(line, kw), 0u,
              "K8: real POZ1L line 102 `DISPOSITION` (whole line) vs keyword DISPOSITION -> match (0) "
              "@0x004391a9, keyword and token end together");
    }
}

} // namespace

// ==== M: llm_tact_cfg_match_keyword_at_offset @0x0043a5b9 ==========================================
// Full-keyword equality at a caller-given offset PLUS a boundary byte (unsigned <0x21) immediately
// after. Fail sentinel is 0, NOT -1 -- the opposite convention from K's -1, and that difference is
// pinned explicitly below.
namespace {

void test_match_keyword_at_offset() {
    printf("-- M: llm_tact_cfg_match_keyword_at_offset -- exact match + boundary byte, sentinel is 0\n");

    // M1: exact match, boundary is NUL (end of string). Fragment lifted from _POZ_SAMPLES.md's
    // "command lines" table (the six shipped files' real non-MOVE command shapes, one of which is
    // `- RUN` with no trailing argument list at all).
    {
        char text[] = "- RUN";
        char kw[]   = "RUN";
        // offset 2 is 'R', matching how mission_parse_command_token itself would have landed here
        // (consume '-' at 0, no space to skip since none follows the dash in this fragment... this
        // fragment actually has one space: '-',' ','R','U','N' -> offset 2).
        ck_eq((uint32_t)detail::cfg_match_keyword_at_offset(text, 2, kw), 5u,
              "M1: real POZ command shape \"- RUN\" (_POZ_SAMPLES.md command-lines table), keyword RUN "
              "at offset 2 -- boundary is NUL -> returns 5 (past the match) @0x0043a638");
    }

    // M2: exact match, boundary is a SPACE (not NUL, not end of string). Real POZ command shape
    // `- WAIT 10` (_POZ_SAMPLES.md command-lines table).
    {
        char text[] = "- WAIT 10";
        char kw[]   = "WAIT";
        // '-',' ','W','A','I','T',' ','1','0' -> WAIT starts at offset 2, boundary (space) at offset 6.
        ck_eq((uint32_t)detail::cfg_match_keyword_at_offset(text, 2, kw), 6u,
              "M2: real POZ command shape \"- WAIT 10\", keyword WAIT at offset 2 -- boundary is the "
              "space before \"10\" -> returns 6 @0x0043a638");
    }

    // M3: the UNSIGNED <0x21 boundary compare accepts more than space/NUL -- a control byte (here a
    // tab) counts too. Hand-built (no real POZ command is followed by a raw control byte).
    {
        char text[] = "TELE\t";
        char kw[]   = "TELE";
        ck_eq((uint32_t)detail::cfg_match_keyword_at_offset(text, 0, kw), 4u,
              "M3 (hand-built): \"TELE\\t\" -- a trailing TAB (0x09) is an unsigned <0x21 boundary byte "
              "too, same as space/NUL @0x0043a62a");
    }

    // M4: mismatch mid-keyword -> the fail sentinel is 0. Built by pairing two real command shapes
    // from the same _POZ_SAMPLES.md table (WALK's real text, tested against WAIT's real keyword) --
    // the pairing itself is constructed, not a literal single line from a POZ file.
    {
        char text[] = "- WALK";
        char kw[]   = "WAIT";
        ck_eq((uint32_t)detail::cfg_match_keyword_at_offset(text, 2, kw), 0u,
              "M4 (constructed from two real POZ shapes): \"- WALK\" at offset 2 vs keyword WAIT -- "
              "'L' != 'I' -> loop exits early, fails the keyword[kw_pos]=='\\0' test -> 0, NOT -1");
    }

    // M5: THE DISTINGUISHING CASE, this function's half. "RUNNER" vs "RUN": the compare consumes
    // R-U-N, then the loop stops (keyword exhausted); the boundary check @0x0043a61f-0x0043a638 then
    // fails because text_base[3] is 'N' (0x4e), not <0x21. Returns 0 -- the FAIL sentinel -- even
    // though cfg_keyword_token_match (K4 above) called this exact same pair a MATCH. Same input,
    // same 0 return value as K4's SUCCESS case, opposite meaning: token_match's 0 means "matched",
    // this function's 0 means "did not match". Conflating the two conventions silently inverts the
    // verdict.
    {
        char text[] = "RUNNER";
        char kw[]   = "RUN";
        ck_eq((uint32_t)detail::cfg_match_keyword_at_offset(text, 0, kw), 0u,
              "M5 (hand-built, THE distinguishing case): \"RUNNER\" vs keyword \"RUN\" at offset 0 -- "
              "no boundary byte after RUN ('N' is not <0x21) -> 0 (NO match) @0x0043a627, while K4 "
              "proved cfg_keyword_token_match calls the identical pair a MATCH -- the two functions "
              "DISAGREE on this exact input, and that disagreement is the whole reason "
              "mission_parse_command_token uses this function, not the other one, for its 12-keyword "
              "chain");
    }

    // M6: THE UPPER EDGE OF THE BOUNDARY TEST, and it is here because a mutation run said it was
    // missing. Flipping the compare @0x0043a62a from `< 0x21` to `<= 0x21` survived the whole
    // suite: M1/M2 use NUL and space (0x00/0x20), M3 uses a tab (0x09), M5 uses 'N' (0x4e) -- not
    // one of them sits at 0x21 itself, so widening the boundary by exactly one byte changed no
    // verdict anywhere. '!' (0x21) is the only character that can tell the two readings apart, and
    // no shipped POZ line contains one, so this case is hand-built by necessity.
    {
        char text[] = "RUN!";
        char kw[]   = "RUN";
        ck_eq((uint32_t)detail::cfg_match_keyword_at_offset(text, 0, kw), 0u,
              "M6 (hand-built, the mutation-found gap): \"RUN!\" vs keyword \"RUN\" -- '!' is 0x21 "
              "EXACTLY, and the compare @0x0043a62a is a strict `< 0x21`, so this is NO match (0). "
              "A `<= 0x21` reading would return 3 here and pass every other case in this file");
    }
}

} // namespace

// ==== P: llm_tact_mission_parse_command_token @0x0043a1d9 ==========================================
namespace {

// ---- P0: the five out-pointers are zeroed UNCONDITIONALLY, including on the "no dash found" path.
void test_p0_unconditional_zero() {
    printf("-- P0: all 5 out-pointers zeroed unconditionally @0x0043a1fa-0x0043a22e, even on return 0\n");

    // Real text with no '-' anywhere. _POZ_SAMPLES.md line 31 (POZ1L header): `TIME = 100 ...`.
    {
        char line[] = "TIME = 100                    ; uplyw czasu w %";
        g_fatal.reset();
        parse_out r = call_parse(recording_calls(), line);
        ck_eq((uint32_t)r.ret, 0u, "P0a: real POZ1L line 31 has no '-' -- return 0 (nothing to parse)");
        ck_eq((uint32_t)r.opcode, 0u, "P0a: out_opcode zeroed on the no-dash path, not left at poison");
        ck_eq((uint32_t)r.arg0, 0u, "P0a: out_arg0 zeroed on the no-dash path");
        ck_eq((uint32_t)r.arg1, 0u, "P0a: out_arg1 zeroed on the no-dash path");
        ck_eq((uint32_t)r.arg2, 0u, "P0a: out_arg2 zeroed on the no-dash path");
        ck_eq((uint32_t)r.arg3, 0u, "P0a: out_arg3 zeroed on the no-dash path");
        ck(!g_fatal.cleanup_called && !g_fatal.abort_called,
           "P0a: an ordinary no-command line never reaches the fatal path");
    }

    // Hand-built: the empty line, the other "definitely nothing to parse" shape.
    {
        char      line[] = "";
        parse_out r      = call_parse(recording_calls(), line);
        ck_eq((uint32_t)r.ret, 0u, "P0b (hand-built): empty line -> return 0");
        ck_eq((uint32_t)r.opcode, 0u, "P0b: out_opcode zeroed");
        ck_eq((uint32_t)r.arg0, 0u, "P0b: out_arg0 zeroed");
        ck_eq((uint32_t)r.arg1, 0u, "P0b: out_arg1 zeroed");
        ck_eq((uint32_t)r.arg2, 0u, "P0b: out_arg2 zeroed");
        ck_eq((uint32_t)r.arg3, 0u, "P0b: out_arg3 zeroed");
    }
}

// ---- P1: the fixed twelve-arm keyword chain, one case per keyword, opcode AND every out_arg.
void test_p1_twelve_arm_chain() {
    printf("-- P1: the twelve-keyword chain -- opcode + out_args, real POZ text where it exists\n");

    // REPEAT -> opcode 0x40 @0x0043a2a5. Real POZ1L DISPOSITION line 108 (_POZ_SAMPLES.md):
    // `                   - REPEAT 2` (19 leading spaces -- the block's real indentation).
    {
        char      line[] = "                   - REPEAT 2";
        parse_out r      = call_parse(recording_calls(), line);
        ck_eq((uint32_t)r.ret, 1u, "P1 REPEAT: real POZ1L line 108 -- command parsed, return 1");
        ck_eq((uint32_t)r.opcode, 0x40u, "P1 REPEAT: opcode 0x40, store @0x0043a2a5");
        ck_eq((uint32_t)r.arg0, 2u, "P1 REPEAT: arg0 = 2, the only digit in the line");
        ck_eq((uint32_t)r.arg1, 0u, "P1 REPEAT: arg1 untouched (only one arg in the line)");
        ck_eq((uint32_t)r.arg2, 0u, "P1 REPEAT: arg2 untouched");
        ck_eq((uint32_t)r.arg3, 0u, "P1 REPEAT: arg3 untouched");
    }

    // MOVE -> opcode 0x1 @0x0043a2cc, arg0/arg1 = the two coordinates, DELIBERATELY DIFFERENT so a
    // swap fails. Real POZ1L DISPOSITION line 109: `                   -   MOVE 39,26` (dash, THREE
    // spaces, MOVE -- exercises the after-dash space-skip loop over more than one space).
    {
        char      line[] = "                   -   MOVE 39,26";
        parse_out r      = call_parse(recording_calls(), line);
        ck_eq((uint32_t)r.ret, 1u, "P1 MOVE: real POZ1L line 109 -- return 1");
        ck_eq((uint32_t)r.opcode, 0x1u, "P1 MOVE: opcode 0x1, store @0x0043a2cc");
        ck_eq((uint32_t)r.arg0, 39u, "P1 MOVE: arg0 = 39 (the FIRST coordinate)");
        ck_eq((uint32_t)r.arg1, 26u, "P1 MOVE: arg1 = 26 (the SECOND coordinate, distinct from arg0 so "
                                     "a swapped read fails)");
        ck_eq((uint32_t)r.arg2, 0u, "P1 MOVE: arg2 untouched");
        ck_eq((uint32_t)r.arg3, 0u, "P1 MOVE: arg3 untouched");
    }
    // A second real MOVE line, POZ1L line 110, distinct values again -- proves the parser isn't
    // accidentally reusing state from the P1-MOVE call above (each call is independent).
    {
        char      line[] = "                   -   MOVE 24,41";
        parse_out r      = call_parse(recording_calls(), line);
        ck_eq((uint32_t)r.opcode, 0x1u, "P1 MOVE (2nd line): opcode 0x1 again");
        ck_eq((uint32_t)r.arg0, 24u, "P1 MOVE (2nd line): real POZ1L line 110, arg0 = 24");
        ck_eq((uint32_t)r.arg1, 41u, "P1 MOVE (2nd line): arg1 = 41");
    }

    // RUN -> opcode 0x1e @0x0043a2f3, NO trailing args at all. Real POZ command shape `- RUN`
    // (_POZ_SAMPLES.md command-lines table).
    {
        char      line[] = "  - RUN";
        parse_out r      = call_parse(recording_calls(), line);
        ck_eq((uint32_t)r.opcode, 0x1eu, "P1 RUN: real POZ shape \"- RUN\", opcode 0x1e store @0x0043a2f3");
        ck_eq((uint32_t)r.arg0, 0u, "P1 RUN: arg0 stays 0 -- RUN fills no arguments");
        ck_eq((uint32_t)r.arg1, 0u, "P1 RUN: arg1 stays 0");
    }

    // WALK -> opcode 0x1c @0x0043a31a, no trailing args. Real POZ shape `- WALK`.
    {
        char      line[] = "  - WALK";
        parse_out r      = call_parse(recording_calls(), line);
        ck_eq((uint32_t)r.opcode, 0x1cu, "P1 WALK: real POZ shape \"- WALK\", opcode 0x1c store @0x0043a31a");
        ck_eq((uint32_t)r.arg0, 0u, "P1 WALK: arg0 stays 0 -- WALK fills no arguments");
    }

    // WAIT -> opcode 0x8 @0x0043a341, ONE trailing arg -- arg1..3 must stay 0. Real POZ shape
    // `- WAIT 10` (_POZ_SAMPLES.md command-lines table).
    {
        char      line[] = "  - WAIT 10";
        parse_out r      = call_parse(recording_calls(), line);
        ck_eq((uint32_t)r.opcode, 0x8u, "P1 WAIT: real POZ shape \"- WAIT 10\", opcode 0x8 store @0x0043a341");
        ck_eq((uint32_t)r.arg0, 10u, "P1 WAIT: arg0 = 10");
        ck_eq((uint32_t)r.arg1, 0u, "P1 WAIT: arg1 stays 0 -- WAIT fills only arg0");
        ck_eq((uint32_t)r.arg2, 0u, "P1 WAIT: arg2 stays 0");
        ck_eq((uint32_t)r.arg3, 0u, "P1 WAIT: arg3 stays 0");
    }

    // TELE -> opcode 0xa @0x0043a368, arg0/arg1 = two DIFFERENT coordinates. Real POZ shape
    // `- TELE 104,109` (_POZ_SAMPLES.md command-lines table).
    {
        char      line[] = "  - TELE 104,109";
        parse_out r      = call_parse(recording_calls(), line);
        ck_eq((uint32_t)r.opcode, 0xau, "P1 TELE: real POZ shape \"- TELE 104,109\", opcode 0xa store "
                                        "@0x0043a368");
        ck_eq((uint32_t)r.arg0, 104u, "P1 TELE: arg0 = 104");
        ck_eq((uint32_t)r.arg1, 109u, "P1 TELE: arg1 = 109, distinct from arg0 so a swap fails");
        ck_eq((uint32_t)r.arg2, 0u, "P1 TELE: arg2 untouched");
    }

    // DIRECT -> opcode 0x7 @0x0043a38f. Real POZ shape `- DIRECT 22` (_POZ_SAMPLES.md command-lines
    // table).
    {
        char      line[] = "  - DIRECT 22";
        parse_out r      = call_parse(recording_calls(), line);
        ck_eq((uint32_t)r.opcode, 0x7u, "P1 DIRECT: real POZ shape \"- DIRECT 22\", opcode 0x7 store "
                                        "@0x0043a38f");
        ck_eq((uint32_t)r.arg0, 22u, "P1 DIRECT: arg0 = 22");
    }

    // DEFENSE:GUARD2 -> opcode 0xb + arg0 = 2. Real POZ shape `- DEFENSE:GUARD2`
    // (_POZ_SAMPLES.md command-lines table).
    {
        char      line[] = "  - DEFENSE:GUARD2";
        parse_out r      = call_parse(recording_calls(), line);
        ck_eq((uint32_t)r.opcode, 0xbu, "P1 DEFENSE:GUARD2: real POZ shape, opcode 0xb store @0x0043a416");
        ck_eq((uint32_t)r.arg0, 2u, "P1 DEFENSE:GUARD2: arg0 = 2 (defense_stance::guard2) store "
                                    "@0x0043a41f -- NOT 1, see the ordering/swap case below");
        ck_eq((uint32_t)r.arg1, 0u, "P1 DEFENSE:GUARD2: arg1 untouched (no trailing args in this line)");
    }

    // DEFENSE:ATTACK -> opcode 0xb + arg0 = 3. Real POZ shape `- DEFENSE:ATTACK`
    // (_POZ_SAMPLES.md command-lines table).
    {
        char      line[] = "  - DEFENSE:ATTACK";
        parse_out r      = call_parse(recording_calls(), line);
        ck_eq((uint32_t)r.opcode, 0xbu, "P1 DEFENSE:ATTACK: real POZ shape, opcode 0xb store @0x0043a443");
        ck_eq((uint32_t)r.arg0, 3u, "P1 DEFENSE:ATTACK: arg0 = 3 (defense_stance::attack) store "
                                    "@0x0043a44c");
    }

    // DEFENSE:NONE -> opcode 0xb + arg0 = 0. HAND-BUILT: _POZ_SAMPLES.md states this keyword appears
    // in NO shipped POZ file at all (count 0), so there is no real line to copy.
    {
        char      line[] = "- DEFENSE:NONE";
        parse_out r      = call_parse(recording_calls(), line);
        ck_eq((uint32_t)r.opcode, 0xbu, "P1 DEFENSE:NONE (hand-built, 0 occurrences in any shipped "
                                        "POZ file per _POZ_SAMPLES.md): opcode 0xb store @0x0043a3b6");
        // arg0 == 0 here is the interesting one: it is BOTH the arm's stored value (defense_stance::
        // none == 0) AND the poison-free default -- so this case alone cannot tell "the arm ran" from
        // "the arm never ran and out_arg0 kept its zeroed-at-entry value". P0 already proves the
        // zeroing property on the no-command path; this case's opcode==0xb check is what proves the
        // NONE arm specifically fired (opcode is never 0xb by accident).
        ck_eq((uint32_t)r.arg0, 0u, "P1 DEFENSE:NONE: arg0 = 0 (defense_stance::none) store @0x0043a3bf "
                                    "-- proven to be the ARM firing (not just the entry zero) by the "
                                    "opcode==0xb check above");
    }

    // DEFENSE:GUARD1 -> opcode 0xb + arg0 = 1. HAND-BUILT, and NOT for the reason this unit's own
    // task brief claimed: _POZ_SAMPLES.md's "command lines" section explicitly says GUARD1's 40
    // occurrences are ALL in the DISPOSITION-header colon form, never as a '-'-prefixed command (see
    // the file banner's correction note).
    {
        char      line[] = "- DEFENSE:GUARD1";
        parse_out r      = call_parse(recording_calls(), line);
        ck_eq((uint32_t)r.opcode, 0xbu, "P1 DEFENSE:GUARD1 (hand-built -- _POZ_SAMPLES.md: GUARD1's 40 "
                                        "occurrences are all in the DISPOSITION header, never as a "
                                        "queued '-' command): opcode 0xb store @0x0043a3e6");
        ck_eq((uint32_t)r.arg0, 1u, "P1 DEFENSE:GUARD1: arg0 = 1 (defense_stance::guard1) store "
                                    "@0x0043a3ef -- NOT 2, see the ordering/swap case below");
    }

    // DEFENSE:SNIPER -> opcode 0xb + arg0 = 4, the twelfth arm this batch discovered. HAND-BUILT: not
    // in any shipped POZ file, and (per tact_cfg_keyword.h/.cpp) had no named Ghidra label before
    // 2026-08-25.
    {
        char      line[] = "- DEFENSE:SNIPER";
        parse_out r      = call_parse(recording_calls(), line);
        ck_eq((uint32_t)r.opcode, 0xbu, "P1 DEFENSE:SNIPER (hand-built, the twelfth arm, no shipped POZ "
                                        "occurrence): opcode 0xb store @0x0043a470");
        ck_eq((uint32_t)r.arg0, 4u, "P1 DEFENSE:SNIPER: arg0 = 4 (defense_stance::sniper) store "
                                    "@0x0043a479");
    }
}

// ---- P-ORDER: DEFENSE:GUARD1 and DEFENSE:GUARD2 share a 13-character prefix ("DEFENSE:GUARD"). The
// two arms sit adjacent in the chain (@0x0043a3ca-0x0043a3fa for GUARD1, @0x0043a3fa-0x0043a427 for
// GUARD2) and each stores a DIFFERENT arg0 (1 vs 2). Because cfg_match_keyword_at_offset requires the
// keyword to match EXACTLY (not as a prefix) before the boundary check, swapping the ORDER the two
// arms are tried in cannot make one keyword match the other's branch -- but swapping the VALUES the
// two branches assign (a real risk given how similar the two literals look side by side) would not
// be caught by anything that only checks "opcode == defense_stat". This pair pins the exact value
// each keyword must produce; either value swapped would fail one of these two checks.
void test_p_order_guard1_guard2_no_swap() {
    printf("-- P-ORDER: DEFENSE:GUARD1 vs DEFENSE:GUARD2 -- the 13-char-shared-prefix pair\n");

    char g1[] = "- DEFENSE:GUARD1";   // hand-built, see P1's note
    char g2[] = "  - DEFENSE:GUARD2"; // real, _POZ_SAMPLES.md command-lines table

    parse_out r1 = call_parse(recording_calls(), g1);
    parse_out r2 = call_parse(recording_calls(), g2);

    ck_eq((uint32_t)r1.opcode, 0xbu, "P-ORDER: DEFENSE:GUARD1 -> opcode 0xb");
    ck_eq((uint32_t)r1.arg0, 1u,
          "P-ORDER: DEFENSE:GUARD1 -> arg0 == 1 EXACTLY -- if the two arms' stored constants were "
          "swapped this would read 2 instead, and this assertion is what catches it");
    ck_eq((uint32_t)r2.opcode, 0xbu, "P-ORDER: DEFENSE:GUARD2 -> opcode 0xb");
    ck_eq((uint32_t)r2.arg0, 2u,
          "P-ORDER: DEFENSE:GUARD2 -> arg0 == 2 EXACTLY -- if the two arms' stored constants were "
          "swapped this would read 1 instead, and this assertion is what catches it");
    ck(r1.arg0 != r2.arg0,
       "P-ORDER: the two 13-char-shared-prefix keywords must never collapse to the same arg0");
}

// ---- P-QUIRK: a DEFENSE:* arm's out_arg0 is NOT reset before the trailing digit loop runs -- the
// digit loop's `*out_arg0 = *out_arg0 * 10 + digit` (@0x0043a4d3-0x0043a4f0) reuses whatever the arm
// already stored as the accumulator SEED. A stray leading digit after a DEFENSE:* keyword therefore
// multiplies the arm's value into the digit rather than starting fresh at 0. The .cpp's own comment
// calls this out as "the original's behaviour ... not modelled around" -- hand-built, since no
// shipped POZ line puts a digit directly after a DEFENSE:* keyword.
void test_p_quirk_defense_arg0_not_reset_before_digits() {
    printf("-- P-QUIRK: a DEFENSE:* arm's out_arg0 seeds the trailing digit accumulator, unreset\n");

    char      line[] = "- DEFENSE:ATTACK 7"; // hand-built
    parse_out r      = call_parse(recording_calls(), line);

    ck_eq((uint32_t)r.opcode, 0xbu, "P-QUIRK: DEFENSE:ATTACK still matches with a trailing digit");
    // ATTACK's arm stores arg0=3 @0x0043a44c; the trailing " 7" is a space (no-op) then digit '7'
    // with arg_fill_index still 0 (the digit loop never resets it), so
    // out_arg0 = out_arg0*10 + 7 = 3*10 + 7 = 37, NOT 7 and NOT 3.
    ck_eq((uint32_t)r.arg0, 37u,
          "P-QUIRK (hand-built): \"- DEFENSE:ATTACK 7\" -> arg0 = 37 (3*10+7), because the digit loop "
          "@0x0043a4d3 multiplies INTO the arm's already-stored value 3, not into a fresh 0 -- a "
          "translation that reset arg0 before the digit loop, or that started the accumulator at 0, "
          "would read 7 here instead and this assertion would catch it");
}

// ---- P2: a failed keyword match at one '-' resumes the scan for the NEXT '-' later in the same
// line, rather than failing the whole call. Hand-built: no shipped POZ line contains a malformed
// dash-token followed by a valid one.
void test_p2_retry_on_failed_dash() {
    printf("-- P2: a non-matching '-'-token does not fail the call -- the scan resumes at the NEXT '-'\n");

    char      line[] = "-XYZQ - RUN"; // hand-built; "XYZQ" matches none of the 12 keywords
    parse_out r      = call_parse(recording_calls(), line);

    ck_eq((uint32_t)r.ret, 1u,
          "P2 (hand-built): \"-XYZQ - RUN\" -- the first '-XYZQ' matches nothing (all 12 arms fail), "
          "the outer loop resumes scanning @0x0043a5a1->0x0043a24c and finds the SECOND '-', which "
          "parses as RUN -- return 1, not a failure of the whole call");
    ck_eq((uint32_t)r.opcode, 0x1eu, "P2: the recovered command is RUN (opcode 0x1e), from the SECOND "
                                     "dash, not the first");
    ck(!g_fatal.cleanup_called && !g_fatal.abort_called,
       "P2: an unmatched dash-token is silently skipped, never the fatal path");
}

// ---- P3: indentation before the '-' is irrelevant (the outer scan is a plain character walk that
// finds '-' wherever it sits); the SAME keyword/value pair parses identically at very different
// indentation depths. Real text: _POZ_SAMPLES.md line 128, `             - DIRECT 18` (13 leading
// spaces), contrasted with P1's `- DIRECT 22` (2 leading spaces).
void test_p3_indentation_is_irrelevant() {
    printf("-- P3: leading indentation before '-' does not affect parsing (real POZ text, 13 spaces)\n");

    char      line[] = "             - DIRECT 18";
    parse_out r      = call_parse(recording_calls(), line);

    ck_eq((uint32_t)r.opcode, 0x7u,
          "P3: real POZ1L line 128 `             - DIRECT 18` (13 leading spaces) -> opcode 0x7 (DIRECT), "
          "same as the 2-space-indented \"- DIRECT 22\" case in P1 -- indentation before '-' does not "
          "matter to the outer scan");
    ck_eq((uint32_t)r.arg0, 18u, "P3: arg0 = 18, distinct from P1 DIRECT's 22 so the two cases cannot "
                                 "be confused with each other");
}

// ---- P4: the trailing argument list's SPACE no-op. `c == ' '` falls through without touching
// arg_fill_index or ending the line (@0x0043a576-0x0043a58d) -- a space between a comma and the next
// digit does not break the argument list. Hand-built (the real POZ MOVE lines above have no space
// after their commas).
void test_p4_space_in_arg_list_is_a_noop() {
    printf("-- P4: a space inside the trailing argument list is a no-op, not a terminator\n");

    char      line[] = "  - MOVE 39, 26"; // hand-built: space AFTER the comma
    parse_out r      = call_parse(recording_calls(), line);

    ck_eq((uint32_t)r.ret, 1u, "P4 (hand-built): \"- MOVE 39, 26\" (space after the comma) still parses "
                               "to completion, return 1");
    ck_eq((uint32_t)r.opcode, 0x1u, "P4: opcode 0x1 (MOVE)");
    ck_eq((uint32_t)r.arg0, 39u, "P4: arg0 = 39, unaffected by the later space");
    ck_eq((uint32_t)r.arg1, 26u,
          "P4: arg1 = 26 -- the space between ',' and '2' was skipped as a no-op @0x0043a576, not "
          "treated as ending the line or as a malformed character");
}

// ---- P5: the ';'-comment terminator inside the trailing argument list ends parsing successfully,
// WITHOUT consuming anything after it. Hand-built (no shipped '-'-command line in the samples carries
// a trailing comment; only a non-command MAP line does).
void test_p5_semicolon_terminates_arg_list() {
    printf("-- P5: ';' inside the trailing argument list ends the line successfully, mid-parse\n");

    char      line[] = "  - WAIT 10;reszta ignorowana"; // hand-built
    parse_out r      = call_parse(recording_calls(), line);

    ck_eq((uint32_t)r.ret, 1u, "P5 (hand-built): \"- WAIT 10;reszta ignorowana\" -- the ';' ends the "
                               "argument list successfully @0x0043a4ac, return 1");
    ck_eq((uint32_t)r.opcode, 0x8u, "P5: opcode 0x8 (WAIT)");
    ck_eq((uint32_t)r.arg0, 10u, "P5: arg0 = 10, parsed before the ';'");
    ck_eq((uint32_t)r.arg1, 0u, "P5: arg1 stays 0 -- \"reszta ignorowana\" after the ';' is never "
                                "touched, proving the comment genuinely stops the scan rather than "
                                "merely being non-digit noise that happens not to match anything");
}

// ---- P6: the two fatal arms. Both must fire the recorder flag (never the return value alone), and
// each case names which arm and asserts the partial out_arg state at the moment of the abort.
void test_p6_fatal_paths() {
    printf("-- P6: the two fatal arms -- a 5th comma, and a malformed argument-list character\n");

    // P6a: a 5th comma (arg_fill_index would become 4, > 3) -- fatal @0x0043a568.
    {
        char line[] = "- MOVE 1,2,3,4,5"; // hand-built: MOVE fills no args itself, so this is a clean
                                          // 5-slot digit-loop test with no arm-seed interference.
        g_fatal.reset();
        parse_out r = call_parse(recording_calls(), line);

        ck(g_fatal.cleanup_called,
           "P6a (hand-built \"- MOVE 1,2,3,4,5\"): a 5th ',' -> llm_fatal_cleanup() fired @0x0043a568");
        ck(g_fatal.abort_called, "P6a: utils_abort() fired right after cleanup");
        ck_eq((uint32_t)g_fatal.abort_status, 0u, "P6a: utils_abort was called with status 0");
        // The digit loop had already filled arg0..arg3 with 1,2,3,4 before the 5th comma was hit --
        // the oracle's recorder does not terminate the process, so the C++ keeps running past the
        // (unreachable-in-the-original) `return 1;` and these values are observable.
        ck_eq((uint32_t)r.arg0, 1u, "P6a: arg0 was already set to 1 before the fatal comma");
        ck_eq((uint32_t)r.arg1, 2u, "P6a: arg1 was already set to 2 before the fatal comma");
        ck_eq((uint32_t)r.arg2, 3u, "P6a: arg2 was already set to 3 before the fatal comma");
        ck_eq((uint32_t)r.arg3, 4u, "P6a: arg3 was already set to 4 before the fatal comma");
    }

    // P6b: a malformed argument-list character (not digit, comma, space, or ';') -- fatal
    // @0x0043a581.
    {
        char line[] = "- WAIT 1X"; // hand-built: 'X' is not digit/comma/space/';'
        g_fatal.reset();
        parse_out r = call_parse(recording_calls(), line);

        ck(g_fatal.cleanup_called,
           "P6b (hand-built \"- WAIT 1X\"): a malformed char 'X' -> llm_fatal_cleanup() fired "
           "@0x0043a581");
        ck(g_fatal.abort_called, "P6b: utils_abort() fired right after cleanup");
        ck_eq((uint32_t)g_fatal.abort_status, 0u, "P6b: utils_abort was called with status 0");
        ck_eq((uint32_t)r.arg0, 1u, "P6b: arg0 was already set to 1 (from the leading '1') before the "
                                    "fatal 'X'");
    }

    // P6c: the two fatal arms are DIFFERENT trigger conditions, and this pins that a 4-arg line (the
    // maximum LEGAL count) does NOT fire the 5th-comma arm -- the boundary from the other side.
    {
        char line[] = "- MOVE 1,2,3,4"; // hand-built: exactly 4 args, no 5th comma
        g_fatal.reset();
        parse_out r = call_parse(recording_calls(), line);

        ck(!g_fatal.cleanup_called && !g_fatal.abort_called,
           "P6c (hand-built \"- MOVE 1,2,3,4\"): exactly 4 args (3 commas) -- the maximum legal count -- "
           "never reaches the 5th-comma fatal arm");
        ck_eq((uint32_t)r.ret, 1u, "P6c: returns 1 normally");
        ck_eq((uint32_t)r.arg3, 4u, "P6c: arg3 = 4, the fourth and last legal slot, filled without "
                                    "aborting");
    }
}

} // namespace

void run_cfg_keyword_tests() {
    test_token_match();
    test_match_keyword_at_offset();
    test_p0_unconditional_zero();
    test_p1_twelve_arm_chain();
    test_p_order_guard1_guard2_no_swap();
    test_p_quirk_defense_arg0_not_reset_before_digits();
    test_p2_retry_on_failed_dash();
    test_p3_indentation_is_irrelevant();
    test_p4_space_in_arg_list_is_a_noop();
    test_p5_semicolon_terminates_arg_list();
    test_p6_fatal_paths();
}

} // namespace mh::tact::test
