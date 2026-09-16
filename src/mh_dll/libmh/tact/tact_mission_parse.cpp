//
// tact/tact_mission_parse.cpp -- see tact_mission_parse.h. Translated from the DISASSEMBLY
// (tmp/decomp_tact/llm_tact_mission_parse_float_004391e7.asm,
// tmp/decomp_tact/llm_tact_mission_parse_int_token_00439bdf.asm,
// tmp/decomp_tact/llm_tact_mission_parse_keyword_int_00439749.asm,
// tmp/decomp_tact/llm_tact_mission_parse_coord_pair_00439382.asm,
// tmp/decomp_tact/llm_tact_mission_parse_quoted_string_00439fa8.asm), not from Ghidra's .c --
// exported alongside these five, the .c drafts get real behaviour right in four of five bodies but
// llm_tact_mission_parse_float's own PLATE comment lies about its signature (see below).
//
#include "tact/tact_mission_parse.h"

#include <cstring>

#include "addr/mh_calls.gen.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h> // GetPrivateProfileIntA -- the [promote] gate, same as mh::sim's sites
#include "state/host_api.h"
#include "crt/crt_select.h" // LIB-CRT: MH_CRT() picks the vendored CRT in the standalone build

namespace mh::tact {

const mission_parse_calls &live_mission_parse_calls() {
    static const mission_parse_calls gc = {
        mh::tact_host().llm_fatal_cleanup,
        mh::tact_host().utils_abort,
        MH_CRT(floor),
    };
    return gc;
}

namespace detail {

double mission_parse_float(const mission_parse_calls &calls, char *line, double max_value) {
    bool     seen_dot   = false; // EBP-0x24: has a '.' been seen yet
    int32_t  frac_count = 0;     // EBP-0x20: the '.' itself, plus every digit seen after it
    double   acc        = 0.0;   // EBP-0x2c: the accumulator
    uint32_t pos        = 0;     // EBP-0x18: scan cursor into `line`

    for (;;) {
        // 0x00439225-0x00439238: strlen(line) recomputed from the ORIGINAL `line` pointer every
        // outer-loop pass (quadratic, the batch's shape -- brief Sect. 6, not hoisted). NOT
        // ECX/DEC ECX/INC ECX nets to strlen+1, so this guard is `pos > strlen(line)`, not `>=`.
        if (pos > static_cast<uint32_t>(std::strlen(line))) {
            calls.llm_fatal_cleanup();
            calls.utils_abort(0);
            // 0x00439365-0x0043936c: unreachable after abort, but the original still writes the
            // return slot to 0.0 there rather than leaving it alone -- transcribed, not dropped.
            return 0.0;
        }
        if (line[pos] == ';') {
            // 0x00439250-0x0043925e: comment line. The stored bit pattern is low=0x00000000,
            // high=0xbff00000 -> the IEEE-754 double -1.0 itself, not a flag squeezed into the
            // slot -- returned as a real -1.0 here.
            return -1.0;
        }
        // 0x0043927b/0x00439281: a digit only starts a value if the byte immediately BEFORE it is
        // a space. Read unconditionally, exactly as the original does -- at pos==0 this reads
        // line[-1] (uint32_t pos-1 wraps to 0xffffffff, which is address `line - 1` on this 32-bit
        // target, matching the original's own out-of-buffer read byte-for-byte). See the
        // translation report's uncertainties.
        if (static_cast<uint8_t>(line[pos]) >= '0' && static_cast<uint8_t>(line[pos]) <= '9' &&
            line[pos - 1] == ' ') {
            // 0x0043928c onward: the number body. Re-tests digit-or-'.' every iteration rather
            // than reusing the look-behind gate above, which is checked exactly once.
            for (;;) {
                uint8_t c        = static_cast<uint8_t>(line[pos]);
                bool    is_digit = c >= '0' && c <= '9';
                bool    is_dot   = c == '.';
                if (!is_digit && !is_dot) {
                    break; // 0x004392f5
                }
                if (is_dot) {
                    seen_dot = true; // 0x004392b8
                } else {
                    // acc = acc*10 + digit, 0x004392d0-0x004392de (FILD/FLD/FMUL/FADDP/FSTP).
                    acc = acc * 10.0 /* _G_LLM_DECIMAL_DIGIT_MUL @0x0050078c */ +
                          static_cast<double>(c - '0');
                }
                if (seen_dot) {
                    // The '.' itself is counted here too -- its own branch (0x004392b8) falls
                    // straight into this increment (0x004392e1-0x004392ea) before looping. See the
                    // scale-down loop below for why counting the dot is exactly right, not an
                    // off-by-one bug.
                    ++frac_count;
                }
                ++pos;
            }
            if (seen_dot) {
                // 0x004392fb-0x004392fe: pre-decrement ONCE before the scale loop starts. Worked
                // example, "0.010": digits processed are '0' (before the dot, frac_count still 0),
                // '.' (frac_count -> 1), '0' (-> 2), '1' (-> 3), '0' (-> 4). Pre-decremented to 3,
                // then the loop below divides exactly 3 times -- i.e. once per digit AFTER the
                // point (there are 3: '0','1','0'). The dot's own +1 and this -1 cancel, so the
                // count of divisions always equals the count of fractional digits, not
                // fractional-digits+1 or -1.
                for (int32_t scale = frac_count - 1; scale > 0; --scale) {
                    acc = acc / 10.0; // _G_LLM_DECIMAL_DIGIT_DIV @0x00500794 -- a SECOND constant
                                      // that happens to also hold 10.0; kept as a separate literal
                                      // use rather than reusing DIGIT_MUL's, per the batch context.
                }
            }
            const double floored = calls.floor(acc);
            (void)floored; // 0x00439324: the result feeds FCOMP @0x00439329, whose flags are then
                           // OVERWRITTEN by the FCOMP/FNSTSW/SAHF pair at 0x0043932f-0x00439337
                           // before the JBE at 0x00439338 reads them. So the comparison is dead --
                           // but the CALL is not omitted, because an oracle case can then assert it
                           // happened, which is what makes "the compare is dead" a checked claim
                           // rather than a comment.
            if (max_value < acc) {
                // 0x0043933a-0x00439341: fatal-aborts if the parsed value exceeds max_value --
                // proof that max_value (Stack[0x4]:8 in the .asm header) is a REAL second
                // parameter, not decoration; the exported .c's own PLATE claims otherwise while its
                // own body performs exactly this comparison.
                calls.llm_fatal_cleanup();
                calls.utils_abort(0);
                return acc; // unreachable
            }
            return acc; // 0x00439346-0x00439352
        }
        ++pos;
    }
}

int32_t mission_parse_int_token(const mission_parse_calls &calls, char *line, uint32_t *io_pos,
                                int32_t *out_value) {
    *out_value = 0; // 0x00439bfe-0x00439c01, unconditional, before *io_pos is ever read

    for (;;) {
        // 0x00439c07-0x00439c19: strlen(line) recomputed every outer pass; NOT ECX/DEC ECX (no
        // extra INC here, unlike mission_parse_float/_keyword_int/_coord_pair's outer guards), so
        // this is `pos >= strlen(line)`, not `>`. *io_pos is read here for the first time in the
        // call -- never written before this.
        if (static_cast<uint32_t>(std::strlen(line)) <= *io_pos) {
            return 0; // *io_pos left as-is: end of line, no delimiter found
        }
        uint8_t c = static_cast<uint8_t>(line[*io_pos]);
        if (c == '{' || c == ' ') {
            ++*io_pos; // 0x00439c3d/0x00439cd9: skip, advance, keep scanning
            continue;
        }
        if (c >= '0' && c <= '9') {
            // 0x00439c62-0x00439c7d
            *out_value = *out_value * 10 + (c - '0');
            ++*io_pos;
            continue;
        }
        if (c == ',' || c == '-') {
            // 0x00439c9f-0x00439cac: *io_pos is advanced PAST the delimiter, and the delimiter
            // BYTE ITSELF is the return value (not a boolean/index).
            ++*io_pos;
            return static_cast<uint8_t>(line[*io_pos - 1]);
        }
        if (c == '}') {
            // 0x00439cc4: *io_pos is left POINTING AT the '}', NOT advanced past it -- unlike the
            // ','/'-' arm above.
            return 0;
        }
        // 0x00439ccd-0x00439cd4: any other byte is malformed. *io_pos is left at its current
        // value, pointing at the offending byte -- never advanced or reset on this arm, though it
        // does not matter since utils_abort never returns.
        calls.llm_fatal_cleanup();
        calls.utils_abort(0);
        return 0; // unreachable
    }
}

int32_t mission_parse_keyword_int(char *line, char *keyword, int32_t *out_value) {
    int32_t  accum = 0; // EBP-0x14: accumulated digit value, once `keyword` has fully matched
    uint32_t pos   = 0; // EBP-0x20: outer scan cursor into `line`

    for (;;) {
        // 0x0043978b-0x0043979e: strlen(line)+1 idiom (extra INC ECX, matching
        // mission_parse_float's outer guard) -> `pos > strlen(line)`.
        if (pos > static_cast<uint32_t>(std::strlen(line))) {
            *out_value = 0; // 0x004398d0-0x004398d9: the ONLY -1 exit that writes *out_value
            return -1;
        }
        if (line[pos] == ';') {
            // 0x004397b1-0x004397bd: a ';' comment anywhere in the scan ends the search.
            // *out_value is deliberately left untouched here.
            return -1;
        }
        if (static_cast<uint8_t>(line[pos]) > 0x20) {
            uint32_t mpos    = pos; // EBP-0x18: candidate keyword-match start
            uint32_t kpos    = 0;   // EBP-0x1c: reset fresh for every attempt
            bool     matched = false;
            for (;;) {
                if (line[mpos] != keyword[kpos]) {
                    break; // 0x004397e5-0x004397e7: mismatch, attempt fails
                }
                if (kpos >= static_cast<uint32_t>(std::strlen(keyword))) {
                    break; // 0x004397f6-0x004397f9: defensive, not reachable via normal flow
                }
                ++mpos;
                ++kpos;
                if (kpos == static_cast<uint32_t>(std::strlen(keyword))) {
                    matched = true; // 0x00439819-0x0043981c: keyword fully consumed
                    break;
                }
            }
            if (matched) {
                // LAB_00439822 onward: consume an optional decimal run right after the keyword.
                for (;;) {
                    if (line[mpos] == ' ' ||
                        mpos >= static_cast<uint32_t>(std::strlen(line))) {
                        *out_value = accum; // 0x00439889-0x00439895
                        return 0;
                    }
                    if (static_cast<uint8_t>(line[mpos]) < '0' ||
                        static_cast<uint8_t>(line[mpos]) > '9') {
                        // 0x00439857-0x00439860: any OTHER terminator forces the value to 0,
                        // DISCARDING digits already accumulated this call -- not "return what was
                        // parsed so far".
                        *out_value = 0;
                        return 0;
                    }
                    accum = accum * 10 + (static_cast<uint8_t>(line[mpos]) - '0');
                    ++mpos;
                }
            }
            // Attempt failed. The original advances `pos` by TWO from mpos here (0x004398ad then
            // 0x004397a3), so the byte at mpos+1 is never itself re-examined as a ';', a
            // whitespace-skip, or a fresh match start -- it is skipped outright. Preserved, not
            // "fixed" to +1.
            ++pos;
            if (pos == static_cast<uint32_t>(std::strlen(line))) {
                // 0x004398bd-0x004398c9: exact-end shortcut. Returns -1 WITHOUT writing
                // *out_value, unlike the top-of-loop over-the-end guard above.
                return -1;
            }
            ++pos;
            continue;
        }
        ++pos;
    }
}

void mission_parse_coord_pair(const mission_parse_calls &calls, char *text, uint8_t *out_col,
                              uint8_t *out_row) {
    bool     col_done  = false; // EBP-0x18: function-scope, NOT reset per '{' -- see the header
    uint8_t  col_accum = 0;     // EBP-0x14
    uint8_t  row_accum = 0;     // EBP-0x10
    uint32_t pos       = 0;     // EBP-0x20: outer scan cursor into `text`

    for (;;) {
        // 0x004393b7-0x004393ca: strlen(text)+1 idiom -> `pos > strlen(text)`.
        if (pos > static_cast<uint32_t>(std::strlen(text))) {
            calls.llm_fatal_cleanup();
            calls.utils_abort(0);
            return; // unreachable
        }
        if (text[pos] != '{') {
            ++pos; // 0x0043950d/0x004393cf
            continue;
        }

        uint32_t cursor    = pos + 1; // EBP-0x1c
        bool     abandoned = false;
        for (;;) {
            // 0x004393ed-0x00439400: strlen(text)+1 idiom again -> `cursor > strlen(text)`.
            if (cursor > static_cast<uint32_t>(std::strlen(text))) {
                // Silently abandon this brace pair (no fatal here, unlike the other four
                // functions' end-of-string guards) -- resume the OUTER scan one byte past the '{'
                // that started this attempt, per the continue below, not from `cursor`.
                abandoned = true;
                break;
            }
            // 0x0043940d: first digit-range test (col-loop gate).
            if (!col_done && static_cast<uint8_t>(text[cursor]) >= '0' &&
                static_cast<uint8_t>(text[cursor]) <= '9') {
                // 0x0042942d-0x00439462: col accumulation. Genuine 8-bit wrap (byte-only MUL/ADD
                // in the original, 0x00439450-0x00439459) -- a 3-digit run silently wraps mod 256.
                while (static_cast<uint8_t>(text[cursor]) >= '0' &&
                       static_cast<uint8_t>(text[cursor]) <= '9') {
                    col_accum = static_cast<uint8_t>(
                        col_accum * 10u + (static_cast<uint8_t>(text[cursor]) - '0'));
                    ++cursor;
                }
                col_done = true; // 0x00439464
            }
            // 0x0043946b: second digit-range test (row-loop gate). Re-reads text[cursor] fresh:
            // if the col loop just ran this pass, cursor has moved on to the byte that stopped it
            // (guaranteed non-digit, so this test is redundant there); if col_done was ALREADY
            // true coming into this pass (a later pass, after skipping a separator byte), cursor
            // is unchanged from the top of this iteration and this is the FIRST real digit test
            // on it, since the block above short-circuited on `!col_done` without even reading the
            // byte.
            if (col_done && static_cast<uint8_t>(text[cursor]) >= '0' &&
                static_cast<uint8_t>(text[cursor]) <= '9') {
                // 0x0043948e-0x004394c3: row accumulation, same byte-wrap shape as col.
                while (static_cast<uint8_t>(text[cursor]) >= '0' &&
                       static_cast<uint8_t>(text[cursor]) <= '9') {
                    row_accum = static_cast<uint8_t>(
                        row_accum * 10u + (static_cast<uint8_t>(text[cursor]) - '0'));
                    ++cursor;
                }
                break; // row run consumed; fall out to the '}'-seek loop below
            }
            ++cursor; // neither loop fired: skip one separator byte and retry from the top
        }
        if (abandoned) {
            ++pos;
            continue;
        }

        // 0x004394c5 onward: seek the closing '}'.
        for (;;) {
            if (text[cursor] == '}') {
                *out_col = col_accum;
                *out_row = row_accum;
                return;
            }
            if (cursor >= static_cast<uint32_t>(std::strlen(text))) {
                // 0x004394e2-0x004394e9: ran off the end without a closing '}' -- fatal, unlike
                // the silent abandon above (that one runs out of string BEFORE either digit run;
                // this one runs out AFTER both, looking only for the brace).
                calls.llm_fatal_cleanup();
                calls.utils_abort(0);
                return; // unreachable
            }
            ++cursor;
        }
    }
}

void mission_parse_quoted_string(const mission_parse_calls &calls, char *line, char *out_buf) {
    uint32_t pos = 0; // EBP-0x14: search cursor into `line`
    while (line[pos] != '"' && line[pos] != '\0') {
        ++pos;
    }
    if (line[pos] == '\0') {
        // 0x00439ff7-0x00439ffe: no opening '"' anywhere in the line.
        calls.llm_fatal_cleanup();
        calls.utils_abort(0);
        return; // unreachable
    }
    ++pos; // 0x0043a003: step past the opening quote

    uint32_t out_i = 0; // EBP-0x18: write cursor into `out_buf`
    // 0x0043a010-0x0043a044: copies verbatim into `out_buf`, which this function never receives a
    // size for -- nothing here bounds the copy against the caller's actual buffer capacity. The
    // only two terminators the copy loop recognises are '"' and NUL.
    while (line[pos] != '"' && line[pos] != '\0') {
        out_buf[out_i] = line[pos];
        ++pos;
        ++out_i;
    }
    out_buf[out_i] = '\0'; // 0x0043a046-0x0043a04c: NUL-terminated unconditionally, even if the
                           // loop above stopped on end-of-line rather than a closing quote
    if (line[pos] == '\0') {
        // 0x0043a05a-0x0043a061: ran off the end without a closing '"'.
        calls.llm_fatal_cleanup();
        calls.utils_abort(0);
        return; // unreachable
    }
}

} // namespace detail

double mission_parse_float(char *line, double max_value) {
    return detail::mission_parse_float(live_mission_parse_calls(), line, max_value);
}

int32_t mission_parse_int_token(char *line, uint32_t *io_pos, int32_t *out_value) {
    return detail::mission_parse_int_token(live_mission_parse_calls(), line, io_pos, out_value);
}

int32_t mission_parse_keyword_int(char *line, char *keyword, int32_t *out_value) {
    return detail::mission_parse_keyword_int(line, keyword, out_value);
}

void mission_parse_coord_pair(char *text, uint8_t *out_col, uint8_t *out_row) {
    detail::mission_parse_coord_pair(live_mission_parse_calls(), text, out_col, out_row);
}

void mission_parse_quoted_string(char *line, char *out_buf) {
    detail::mission_parse_quoted_string(live_mission_parse_calls(), line, out_buf);
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
