//
// tact/tact_character_parse.cpp -- see tact_character_parse.h. Translated from the DISASSEMBLY
// (tmp/decomp_tact/llm_tact_character_parse_frame_table_0043a649.asm), not from Ghidra's C.
//
#include "tact/tact_character_parse.h"

#include "addr/mh_calls.gen.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h> // GetPrivateProfileIntA -- the [promote] gate, same as mh::sim's sites
#include "state/host_api.h"
#include "crt/crt_select.h" // LIB-CRT: MH_CRT() picks the vendored CRT in the standalone build

namespace mh::tact {
namespace {

// ---- the four delimiter/marker literals this parser uses -------------------------------------
//
// Frozen .rdata strings with no mh_addrs.gen.h entry (they don't need one -- brief Sect. 5): the
// literal is embedded here and the symbol + VA are cited so the next reader can find them in
// Ghidra. Kept as non-const `char[]` (not `const char *`) because strtok()/utils_str_cmp() are
// prototyped `char *`, matching the original's own untyped-literal calling convention -- no
// const_cast needed anywhere below.
char g_frame_token_delim[] = " ";  // _G_LLM_TACT_CFG_FRAME_TOKEN_DELIM  @0x005007bf
char g_frame_table_open[]  = "{";  // _G_LLM_TACT_CFG_FRAME_TABLE_OPEN   @0x005007c1
char g_frame_list_delim[]  = ", "; // _G_LLM_TACT_CFG_FRAME_LIST_DELIM   @0x005007c3 (BOTH ',' and
                                   // ' ' are delimiters -- a strtok delimiter SET, not a literal
                                   // two-char token)
char g_frame_table_close[] = "}";  // _G_LLM_TACT_CFG_FRAME_TABLE_CLOSE  @0x005007c6

// Copies a strtok()'d token into the parser's 80-byte scratch buffer, two bytes per iteration --
// the original's inlined strcpy idiom (MOV AL,[ESI]/MOV [EDI],AL/CMP AL,0/... ADD ESI,2/ADD EDI,2),
// transcribed identically from the three sites it appears at (0x0043a679, 0x0043a6a4, 0x0043a706).
// `src` is `strtok`'s return value directly -- never NULL-checked in the original, so neither is
// this: a mission file that runs out of tokens mid-directive is not a case the original guards.
void copy_token(char *dst, char *src) {
    for (;;) {
        char c0 = src[0];
        dst[0]  = c0;
        if (c0 == '\0') break;
        char c1 = src[1];
        src += 2;
        dst[1] = c1;
        dst += 2;
        if (c1 == '\0') break;
    }
}

} // namespace

const character_parse_calls &live_character_parse_calls() {
    static const character_parse_calls gc = {
        MH_CRT(strtok),
        MH_CRT(utils_str_cmp),
        MH_CRT(atoi),
        mh::tact_host().llm_fatal_cleanup,
        mh::tact_host().utils_abort,
    };
    return gc;
}

namespace detail {

void character_parse_frame_table(const character_parse_calls &calls, tact_store &own,
                                 char *frames_directive_line, int32_t character_type_index) {
    // Ghidra's frame accounting sizes this scratch buffer at 80 bytes (EBP-0x6c..EBP-0x1c); every
    // real POZ*.DAT token is a handful of characters, so the size is generous, not tight.
    char token_buf[80];

    // ---- token 1: the directive's leading keyword ('FRAMES' in every real mission line). The
    // parser itself never compares against that word -- only against the delimiter globals below
    // -- so it is discarded, not validated. ----
    copy_token(token_buf, calls.strtok(frames_directive_line, g_frame_token_delim));

    // ---- token 2: must be '{' or the mission is malformed ----
    copy_token(token_buf, calls.strtok(nullptr, g_frame_token_delim));
    // utils_str_cmp is plain strcmp semantics -- 0 means EQUAL (verified against 0x004d16d0's
    // body). JZ @0x0043a6cc skips the abort on a MATCH, so this test aborts on NOT-equal: the
    // token must literally be "{".
    if (calls.utils_str_cmp(token_buf, g_frame_table_open) != 0) {
        calls.llm_fatal_cleanup();
        calls.utils_abort(0);
        return; // unreachable: utils_abort tail-calls _exit and never returns (Sect. 4)
    }

    character_type &ct = own.character_type_at(character_type_index);

    // ---- the six-element FRAMES list, e.g. "FRAMES { 8,8,8,8,8,8 }" ----
    for (int32_t i = 0; i < TACT_ANIM_FRAMES_PARSED; ++i) {
        copy_token(token_buf, calls.strtok(nullptr, g_frame_list_delim));
        // OPPOSITE sense from the '{' test above: JNZ @0x0043a72e skips the abort on NOT-equal, so
        // THIS test aborts on EQUAL -- a '}' seen before all six tokens are read is malformed.
        if (calls.utils_str_cmp(token_buf, g_frame_table_close) == 0) {
            calls.llm_fatal_cleanup();
            calls.utils_abort(0);
            return; // unreachable, same as above
        }

        if (i == 0) {
            // Slot 0's start is not on the list at all -- it is seeded from the character type's
            // own FIRST FRAME (+0x1a), a raw 16-bit copy (MOV DX,[first_frame] / MOV [start],DX).
            ct.frames[0].start = static_cast<uint16_t>(ct.first_frame);
        } else {
            // Every later slot accumulates: start[i-1] + count[i-1] * 8. The *8 is the eight
            // facing directions rendered per animation frame (SHL EAX,0x3 @0x0043a777). The 16-bit
            // ADD @0x0043a789 truncates on overflow exactly like this cast does.
            ct.frames[i].start =
                static_cast<uint16_t>(ct.frames[i - 1].start + ct.frames[i - 1].count * 8);
        }
        // atoi's int32 result truncates to the byte-wide `count` field on store, matching the
        // original's `MOV EDX,EAX` then `MOV byte ptr ...,DL`.
        ct.frames[i].count = static_cast<uint8_t>(calls.atoi(token_buf));
    }
}

} // namespace detail

void character_parse_frame_table(char *frames_directive_line, int32_t character_type_index) {
    tact_state st = state();
    detail::character_parse_frame_table(live_character_parse_calls(), st.own,
                                        frames_directive_line, character_type_index);
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
