//
// tact/tact_cfg_keyword.cpp -- see tact_cfg_keyword.h. Translated from the DISASSEMBLY
// (tmp/decomp_tact/llm_tact_cfg_keyword_token_match_00439103.asm,
// tmp/decomp_tact/llm_tact_cfg_match_keyword_at_offset_0043a5b9.asm and
// tmp/decomp_tact/llm_tact_mission_parse_command_token_0043a1d9.asm), not from Ghidra's .c.
//
#include "tact/tact_cfg_keyword.h"

#include <cstring>

#include "addr/mh_calls.gen.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h> // GetPrivateProfileIntA -- the [promote] gate, same as mh::sim's sites
#include "state/host_api.h"

namespace mh::tact {
namespace {

// ---- the twelve keyword literals this parser matches against ---------------------------------
//
// Frozen .rdata strings, no mh_addrs.gen.h entry needed (brief Sect. 5): the literal is embedded
// here and the symbol + VA are cited so the next reader can find them in Ghidra. Non-const
// `char[]` (not `const char *`), matching the original's own untyped-literal calling convention
// and this module's established style (tact_character_parse.cpp) -- no const_cast needed at any
// call site below.
char kw_repeat[]         = "REPEAT";         // _G_LLM_TACT_KW_REPEAT          @0x0050064d
char kw_move[]           = "MOVE";           // _G_LLM_TACT_KW_MOVE            @0x0050079c
char kw_run[]            = "RUN";            // _G_LLM_TACT_KW_RUN             @0x00500627
char kw_walk[]           = "WALK";           // _G_LLM_TACT_KW_WALK            @0x005007a1
char kw_wait[]           = "WAIT";           // _G_LLM_TACT_KW_WAIT            @0x005007a6
char kw_tele[]           = "TELE";           // _G_LLM_TACT_KW_TELE            @0x005007ab
char kw_direct[]         = "DIRECT";         // _G_LLM_TACT_KW_DIRECT          @0x00500676
char kw_defense_none[]   = "DEFENSE:NONE";   // _G_LLM_TACT_KW_DEFENSE_NONE    @0x005006d4
char kw_defense_guard1[] = "DEFENSE:GUARD1"; // _G_LLM_TACT_KW_DEFENSE_GUARD1 @0x005006e1
char kw_defense_guard2[] = "DEFENSE:GUARD2"; // _G_LLM_TACT_KW_DEFENSE_GUARD2 @0x005006f0
char kw_defense_attack[] = "DEFENSE:ATTACK"; // _G_LLM_TACT_KW_DEFENSE_ATTACK @0x005006ff
// NO _G_LLM_TACT_KW_ label exists for this one yet -- Ghidra still carries it as the auto string
// label `s_DEFENSE:SNIPER_005007b0` (declared_needs: name it _G_LLM_TACT_KW_DEFENSE_SNIPER).
char kw_defense_sniper[] = "DEFENSE:SNIPER"; // @0x005007b0, unnamed

} // namespace

const cfg_keyword_calls &live_cfg_keyword_calls() {
    static const cfg_keyword_calls gc = {
        mh::tact_host().llm_fatal_cleanup,
        mh::tact_host().utils_abort,
    };
    return gc;
}

namespace detail {

int32_t cfg_keyword_token_match(char *line, char *keyword) {
    // Recomputes strlen(line) from the ORIGINAL `line` pointer every outer-loop pass (REPNE SCASB
    // from EBP-0x24, never advanced) @0x00439135-0x00439142 -- quadratic, and the original's shape
    // (brief Sect. 6): transcribed as a strlen() call inside the loop, not hoisted.
    int32_t line_pos = 0;
    for (;;) {
        // CMP ECX,[EBP-0x1c] / JA 0x00439155 @0x00439143-0x00439146: the end-of-string check runs
        // BEFORE the character test, so line_pos == strlen(line) (the NUL) returns -1, not 0.
        if (std::strlen(line) <= static_cast<size_t>(line_pos)) {
            return -1;
        }
        if (line[line_pos] == ';') {
            return -1;
        }
        // JBE 0x004391cf @0x00439172-0x00439175: an UNSIGNED compare, so every byte 0x00-0x20
        // counts as whitespace/skip here, not just the space character.
        if (static_cast<unsigned char>(line[line_pos]) <= 0x20) {
            ++line_pos;
            continue;
        }
        // Found the first non-whitespace byte of the token -- try to match `keyword` from here.
        // @0x00439177 onward. This inner loop returns 0 the INSTANT keyword is fully consumed; it
        // never checks what follows in `line` (contrast cfg_match_keyword_at_offset below), so a
        // keyword that is a strict prefix of the line's token still matches here.
        int32_t kw_pos = 0;
        for (;;) {
            if (line[line_pos] != keyword[kw_pos]) {
                return -1; // @0x00439187
            }
            // Recomputed strlen(keyword) from the original pointer again here @0x00439189-0x00439196
            // (the original calls this SCASB scan twice per inner iteration -- once before the
            // increment to check "already exhausted", once after to check "just completed";
            // transcribed as two separate strlen() calls to match, not one hoisted length).
            if (std::strlen(keyword) <= static_cast<size_t>(kw_pos)) {
                return -1; // @0x0043919b (fallthrough of the JA at 0x00439199)
            }
            ++line_pos;
            ++kw_pos;
            if (std::strlen(keyword) == static_cast<size_t>(kw_pos)) { // @0x004391a9-0x004391b9
                return 0;
            }
            // else: full match not yet reached, loop back to 0x00439177 and compare the next char.
        }
    }
}

int32_t cfg_match_keyword_at_offset(char *text_base, int32_t start_offset, char *keyword) {
    int32_t pos    = start_offset;
    int32_t kw_pos = 0;
    // @0x0043a5df-0x0043a617: advances both indices while text_base[pos] and keyword[kw_pos] are
    // both non-NUL and equal. Bounded ONLY by the two NUL terminators -- start_offset is a
    // character index into text_base with NO explicit range check against a buffer length; an
    // out-of-range start_offset is the caller's responsibility, exactly as in the original.
    while (text_base[pos] != '\0' && keyword[kw_pos] != '\0' && text_base[pos] == keyword[kw_pos]) {
        ++pos;
        ++kw_pos;
    }
    // Success needs BOTH: keyword fully consumed, AND the next text_base byte is a boundary byte
    // (unsigned < 0x21 -- whitespace, control, or NUL all count) @0x0043a61f-0x0043a638.
    if (keyword[kw_pos] == '\0' && static_cast<unsigned char>(text_base[pos]) < 0x21) {
        return pos;
    }
    return 0;
}

int32_t mission_parse_command_token(const cfg_keyword_calls &calls, char *mission_line,
                                    int32_t *out_opcode, int32_t *out_arg0, int32_t *out_arg1,
                                    int32_t *out_arg2, int32_t *out_arg3) {
    // Unconditional, @0x0043a1fa-0x0043a22e: every one of the five out-pointers is zeroed before
    // any keyword is even looked for, on every call -- including the "no command found" (return 0)
    // path.
    int32_t arg_fill_index = 0; // EBP-0x10: which out_arg the digit loop is currently filling
    *out_opcode            = 0;
    *out_arg0              = 0;
    *out_arg1              = 0;
    *out_arg2              = 0;
    *out_arg3              = 0;

    int32_t line_pos = 0; // EBP-0x18

    for (;;) {
        // ---- outer loop: scan for the next '-' in mission_line, one character at a time -------
        // @0x0043a235-0x0043a247: strlen(mission_line) recomputed from the ORIGINAL pointer every
        // pass (quadratic, the original's shape -- brief Sect. 6), not hoisted.
        if (std::strlen(mission_line) <= static_cast<size_t>(line_pos)) {
            return 0; // end of line with no dash found: nothing to parse
        }
        if (mission_line[line_pos] != '-') {
            ++line_pos;
            continue;
        }
        ++line_pos; // consume the '-' itself @0x0043a263-0x0043a266

        // Skip spaces after the dash @0x0043a269-0x0043a287. The original re-tests the SAME byte
        // against 0 immediately after testing it against ' ' -- provably dead (a space is never
        // NUL), so it can only ever take the "increment and loop" edge; collapsed to a plain while
        // rather than reproduced as a second branch. Flagged in the translation report so a
        // reviewer can re-check the "provably dead" claim rather than trust it blindly.
        while (mission_line[line_pos] == ' ') {
            ++line_pos;
        }

        // ---- the fixed TWELVE-keyword chain, tried in this exact order @0x0043a289-0x0043a479 --
        int32_t kw_end = 0; // EBP-0x14: cfg_match_keyword_at_offset's result; also the digit-loop's
                            // starting cursor on a successful match
        if ((kw_end = detail::cfg_match_keyword_at_offset(mission_line, line_pos, kw_repeat)) > 0) {
            *out_opcode = static_cast<int32_t>(mission_opcode::repeat); // @0x0043a2a5
        } else if ((kw_end = detail::cfg_match_keyword_at_offset(mission_line, line_pos, kw_move)) >
                   0) {
            *out_opcode = static_cast<int32_t>(mission_opcode::move); // @0x0043a2cc
        } else if ((kw_end = detail::cfg_match_keyword_at_offset(mission_line, line_pos, kw_run)) >
                   0) {
            *out_opcode = static_cast<int32_t>(mission_opcode::run); // @0x0043a2f3
        } else if ((kw_end = detail::cfg_match_keyword_at_offset(mission_line, line_pos, kw_walk)) >
                   0) {
            *out_opcode = static_cast<int32_t>(mission_opcode::walk); // @0x0043a31a
        } else if ((kw_end = detail::cfg_match_keyword_at_offset(mission_line, line_pos, kw_wait)) >
                   0) {
            *out_opcode = static_cast<int32_t>(mission_opcode::wait); // @0x0043a341
        } else if ((kw_end = detail::cfg_match_keyword_at_offset(mission_line, line_pos, kw_tele)) >
                   0) {
            *out_opcode = static_cast<int32_t>(mission_opcode::teleport); // @0x0043a368
        } else if ((kw_end = detail::cfg_match_keyword_at_offset(mission_line, line_pos, kw_direct)) >
                   0) {
            *out_opcode = static_cast<int32_t>(mission_opcode::direct); // @0x0043a38f
        } else if ((kw_end = detail::cfg_match_keyword_at_offset(mission_line, line_pos,
                                                                 kw_defense_none)) > 0) {
            *out_opcode = static_cast<int32_t>(mission_opcode::defense_stat); // @0x0043a3b6
            *out_arg0   = static_cast<int32_t>(defense_stance::none);         // @0x0043a3bf
        } else if ((kw_end = detail::cfg_match_keyword_at_offset(mission_line, line_pos,
                                                                 kw_defense_guard1)) > 0) {
            *out_opcode = static_cast<int32_t>(mission_opcode::defense_stat); // @0x0043a3e6
            *out_arg0   = static_cast<int32_t>(defense_stance::guard1);       // @0x0043a3ef
        } else if ((kw_end = detail::cfg_match_keyword_at_offset(mission_line, line_pos,
                                                                 kw_defense_guard2)) > 0) {
            *out_opcode = static_cast<int32_t>(mission_opcode::defense_stat); // @0x0043a416
            *out_arg0   = static_cast<int32_t>(defense_stance::guard2);       // @0x0043a41f
        } else if ((kw_end = detail::cfg_match_keyword_at_offset(mission_line, line_pos,
                                                                 kw_defense_attack)) > 0) {
            *out_opcode = static_cast<int32_t>(mission_opcode::defense_stat); // @0x0043a443
            *out_arg0   = static_cast<int32_t>(defense_stance::attack);       // @0x0043a44c
        } else if ((kw_end = detail::cfg_match_keyword_at_offset(mission_line, line_pos,
                                                                 kw_defense_sniper)) > 0) {
            *out_opcode = static_cast<int32_t>(mission_opcode::defense_stat); // @0x0043a470
            *out_arg0   = static_cast<int32_t>(defense_stance::sniper);       // @0x0043a479
        }

        // @0x0043a47f-0x0043a485: none of the twelve matched (*out_opcode is still the zero it was
        // initialised to -- the out-pointer itself is the original's "did-match" flag, since a
        // failing arm never touches it) -> go back and look for the NEXT '-' later in the line,
        // rather than failing outright.
        if (*out_opcode <= 0) {
            ++line_pos;
            continue;
        }

        // ---- the trailing argument list: digits accumulate into whichever out_arg
        // arg_fill_index currently names, commas advance it, ';' or end-of-string end the line
        // successfully, anything else is a malformed mission file and the game terminates
        // @0x0043a48b-0x0043a593.
        int32_t arg_pos = kw_end; // EBP-0x14, reused as the digit-loop cursor
        for (;;) {
            if (std::strlen(mission_line) <= static_cast<size_t>(arg_pos)) {
                return 1; // ran off the end of the line: success @0x0043a598
            }
            char c = mission_line[arg_pos];
            if (c == ';') {
                return 1; // comment marker: success, nothing more to parse @0x0043a4ac
            }
            // JC/JBE @0x0043a4c1/0x0043a4cc are UNSIGNED comparisons against '0'/'9' (both < 0x80),
            // so an unsigned-char cast reproduces them exactly (and is provably equivalent to a
            // plain signed `char` compare here too, since no byte >= 0x80 can satisfy either form).
            if (static_cast<unsigned char>(c) >= '0' && static_cast<unsigned char>(c) <= '9') {
                int32_t digit = c - '0';
                switch (arg_fill_index) {
                    case 0:
                        *out_arg0 = *out_arg0 * 10 + digit; // @0x0043a4d3-0x0043a4f0
                        break;
                    case 1:
                        *out_arg1 = *out_arg1 * 10 + digit; // @0x0043a4f2-0x0043a50f
                        break;
                    case 2:
                        *out_arg2 = *out_arg2 * 10 + digit; // @0x0043a511-0x0043a52e
                        break;
                    case 3:
                        *out_arg3 = *out_arg3 * 10 + digit; // @0x0043a530-0x0043a54d
                        break;
                    default:
                        break; // unreachable: the comma path below aborts before the index can exceed 3
                }
            } else if (c == ',') {
                ++arg_fill_index; // @0x0043a55c-0x0043a55f
                if (arg_fill_index > 3) {
                    // @0x0043a568-0x0043a56f: a 5th argument slot was requested -- fatal. The
                    // original terminates the game rather than returning an error (behaviour, not
                    // error handling).
                    calls.llm_fatal_cleanup();
                    calls.utils_abort(0);
                    return 1; // unreachable: utils_abort never returns
                }
            } else if (c == ' ') {
                // no-op: falls through to the increment below @0x0043a576-0x0043a58d
            } else {
                // @0x0043a581-0x0043a588: any other character in the argument list is fatal too.
                calls.llm_fatal_cleanup();
                calls.utils_abort(0);
                return 1; // unreachable
            }
            ++arg_pos;
        }
    }
}

} // namespace detail

int32_t cfg_keyword_token_match(char *line, char *keyword) {
    return detail::cfg_keyword_token_match(line, keyword);
}

int32_t cfg_match_keyword_at_offset(char *text_base, int32_t start_offset, char *keyword) {
    return detail::cfg_match_keyword_at_offset(text_base, start_offset, keyword);
}

int32_t mission_parse_command_token(char *mission_line, int32_t *out_opcode, int32_t *out_arg0,
                                    int32_t *out_arg1, int32_t *out_arg2, int32_t *out_arg3) {
    return detail::mission_parse_command_token(live_cfg_keyword_calls(), mission_line, out_opcode,
                                               out_arg0, out_arg1, out_arg2, out_arg3);
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
