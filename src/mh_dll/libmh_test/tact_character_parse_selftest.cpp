//
// tact_character_parse_selftest.cpp -- `net_selftest.exe tacttest` cases for
// llm_tact_character_parse_frame_table (tact/tact_character_parse.h/.cpp) @0x0043a649, TACT1A.
//
// NO SHADOW SITE (see the .h banner): the body writes only through a caller-supplied index into
// `_G_LLM_TACT_CHARACTER_TYPES`, indistinguishable from any other caller's write on an armed run,
// and the trajectory oracle parses each mission exactly once at entry. This file, and
// `net_selftest.exe tacttest`, are the only evidence these branches will ever get.
//
// RECORDERS: `strtok` is genuinely STATEFUL (the .h banner's warning) -- three of its call sites
// pass a null first argument to continue the previous scan, so the recorder here is the REAL
// libc `strtok`, over a MUTABLE local buffer each case owns (never a string literal). A canned
// stub returning one token would make the six-slot FRAMES loop read the same value forever; the
// hand-varied case below (C2) would catch that immediately, since it needs six DISTINCT tokens.
// `utils_str_cmp` is wrapped over the real `strcmp` (0 == equal, verified against 0x004d16d0).
//
#include "tact/tact_character_parse.h"

#include <cstdlib>
#include <cstring>

#include "tact_test_support.h"

namespace mh::tact::test {
namespace {

using mh::tact::character_parse_calls;

struct call_log {
    bool    fatal_fired  = false;
    int     fatal_n      = 0;
    bool    abort_fired  = false;
    int     abort_n      = 0;
    int32_t abort_status = 0;

    void reset() { *this = call_log{}; }
};
call_log g_log;

const character_parse_calls &recording_calls() {
    static const character_parse_calls gc = {
        [](char *s, char *d) -> char * { return std::strtok(s, d); },  // a REAL tokeniser -- libc's
                                                                       // own internal save-pointer,
                                                                       // over whatever mutable
                                                                       // buffer the case owns
        [](char *a, char *b) -> int32_t { return std::strcmp(a, b); }, // plain strcmp semantics
        [](char *s) -> int32_t { return std::atoi(s); },
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

// Distinct, non-round poison for every frame slot's start/count -- an untouched slot (6, 7: the
// loop fills only 6 of 8) or an untouched NEIGHBOUR character_type reads back as poison, not a
// silent zero-vs-zero agreement.
constexpr uint16_t kPoisonStart = 0xBEEFu;
constexpr uint8_t  kPoisonCount = 0xABu;

void poison_frames(character_type &ct) {
    for (int i = 0; i < TACT_ANIM_FRAME_SLOTS; ++i) {
        ct.frames[i].start = (uint16_t)(kPoisonStart + i);
        ct.frames[i].count = (uint8_t)(kPoisonCount + i);
    }
}

} // namespace

void run_character_parse_tests() {
    printf("-- C1: the REAL shipped line, 'FRAMES { 8,8,8,8,8,8 }' (verbatim POZ1L CHARACTER 1) "
           "-- pins the start accumulation chain across all six slots, plus the 6-of-8 fill and "
           "the write stride\n");
    {
        tact_fixture      fx;
        constexpr int32_t kIndex = 3; // non-zero, so a stride error into a neighbour shows
        poison_frames(fx.character_types[kIndex - 1]);
        poison_frames(fx.character_types[kIndex]);
        poison_frames(fx.character_types[kIndex + 1]);
        fx.character_types[kIndex].first_frame = 777; // distinctive non-zero seed for the SEED of
                                                      // frames[0].start (0x0043a752/0x0043a759)

        tact_store own = fx.store();
        char       line[64];
        std::strcpy(line, "FRAMES { 8,8,8,8,8,8 }");
        g_log.reset();
        mh::tact::detail::character_parse_frame_table(recording_calls(), own, line, kIndex);

        const character_type &ct = fx.character_types[kIndex];
        ck(!g_log.fatal_fired, "C1: a well-formed FRAMES line never reaches the fatal path");
        ck_eq((uint32_t)ct.frames[0].start, 777u,
              "C1: frames[0].start seeds from first_frame (0x0043a752/0x0043a759), NOT from the "
              "file's own token list");
        ck_eq((uint32_t)ct.frames[0].count, 8u, "C1: frames[0].count atoi'd from the first token");
        ck_eq((uint32_t)ct.frames[1].start, 777u + 8u * 8u,
              "C1: frames[1].start = frames[0].start + frames[0].count*8 (SHL EAX,0x3 "
              "@0x0043a777, ADD @0x0043a789)");
        ck_eq((uint32_t)ct.frames[2].start, 777u + 2u * 8u * 8u, "C1: frames[2].start accumulates again");
        ck_eq((uint32_t)ct.frames[3].start, 777u + 3u * 8u * 8u, "C1: frames[3].start accumulates again");
        ck_eq((uint32_t)ct.frames[4].start, 777u + 4u * 8u * 8u, "C1: frames[4].start accumulates again");
        ck_eq((uint32_t)ct.frames[5].start, 777u + 5u * 8u * 8u, "C1: frames[5].start accumulates again");
        for (int i = 1; i <= 5; ++i) {
            ck_eq((uint32_t)ct.frames[i].count, 8u, "C1: every slot's count is 8, from the real line");
        }
        // TACT_ANIM_FRAMES_PARSED(6) vs TACT_ANIM_FRAME_SLOTS(8): the loop fills SIX of EIGHT.
        ck_eq((uint32_t)ct.frames[6].start, (uint32_t)(kPoisonStart + 6),
              "C1: frames[6] is slot TACT_ANIM_FRAMES_PARSED -- the loop stops before it, poison "
              "survives");
        ck_eq((uint32_t)ct.frames[6].count, (uint32_t)(kPoisonCount + 6),
              "C1: frames[6].count poison survives");
        ck_eq((uint32_t)ct.frames[7].start, (uint32_t)(kPoisonStart + 7),
              "C1: frames[7].start poison survives");
        ck_eq((uint32_t)ct.frames[7].count, (uint32_t)(kPoisonCount + 7),
              "C1: frames[7].count poison survives");
        // The write lands via tact_store::character_type_at(kIndex) -- a stride error would spill
        // onto a neighbouring slot.
        ck_eq((uint32_t)fx.character_types[kIndex - 1].frames[0].start, (uint32_t)(kPoisonStart + 0),
              "C1: the PRECEDING character_type slot is untouched -- confirms the 0x6c stride, "
              "not an off-by-one into the neighbour");
        ck_eq((uint32_t)fx.character_types[kIndex + 1].frames[0].start, (uint32_t)(kPoisonStart + 0),
              "C1: the FOLLOWING character_type slot is untouched");
    }

    printf("-- C2: HAND-VARIED 'FRAMES { 1,2,3,4,5,6 }' (synthetic, NOT from a POZ file) -- six "
           "EQUAL counts in the real line cannot detect a slot mix-up, six DISTINCT ones can\n");
    {
        tact_fixture      fx;
        constexpr int32_t kIndex               = 9;
        fx.character_types[kIndex].first_frame = 500;
        tact_store own                         = fx.store();
        char       line[64];
        std::strcpy(line, "FRAMES { 1,2,3,4,5,6 }");
        g_log.reset();
        mh::tact::detail::character_parse_frame_table(recording_calls(), own, line, kIndex);

        const character_type &ct           = fx.character_types[kIndex];
        uint32_t              expect_start = 500;
        static const uint32_t counts[6]    = {1, 2, 3, 4, 5, 6};
        for (int i = 0; i < 6; ++i) {
            ck_eq((uint32_t)ct.frames[i].start, expect_start,
                  "C2: frames[i].start pinned by a hand-varied list -- i and the running total "
                  "are both distinct, so a slot swap would be caught");
            ck_eq((uint32_t)ct.frames[i].count, counts[i],
                  "C2: frames[i].count from its own distinct token");
            expect_start += counts[i] * 8;
        }
    }

    printf("-- C3: token 2 that is NOT '{' aborts -- JZ @0x0043a6cc skips the abort on a MATCH, "
           "so this test aborts on NOT-equal\n");
    {
        tact_fixture      fx;
        constexpr int32_t kIndex = 2;
        poison_frames(fx.character_types[kIndex]);
        tact_store own = fx.store();
        char       line[64];
        std::strcpy(line, "FRAMES X 8,8,8,8,8,8 }"); // token 2 is 'X', not '{'
        g_log.reset();
        mh::tact::detail::character_parse_frame_table(recording_calls(), own, line, kIndex);

        ck(g_log.fatal_fired,
           "C3: token 2 == 'X' != '{' -> fatal @0x0043a6ce; the OPPOSITE sense (abort-on-equal, "
           "C4's sense) would NOT fire here, so a swapped comparison would fail this check");
        ck_eq((uint32_t)g_log.abort_n, 1, "C3: utils_abort(0) fires exactly once");
        ck_eq((uint32_t)g_log.abort_status, 0u, "C3: utils_abort's status argument is 0");
        ck_eq((uint32_t)fx.character_types[kIndex].frames[0].start, (uint32_t)(kPoisonStart + 0),
              "C3: character_type_at() is only reached AFTER the '{' check succeeds -- frames "
              "are never touched on this fatal path");
    }

    printf("-- C4: a '}' seen before all six tokens are read aborts -- JNZ @0x0043a72e skips the "
           "abort on NOT-equal, so this test aborts on EQUAL, the OPPOSITE sense from C3\n");
    {
        tact_fixture      fx;
        constexpr int32_t kIndex = 5;
        poison_frames(fx.character_types[kIndex]);
        fx.character_types[kIndex].first_frame = 1000;
        tact_store own                         = fx.store();
        char       line[64];
        std::strcpy(line, "FRAMES { 8,8,8 }"); // only 3 numeric tokens, then '}' as the 4th token
        g_log.reset();
        mh::tact::detail::character_parse_frame_table(recording_calls(), own, line, kIndex);

        ck(g_log.fatal_fired,
           "C4: '}' shows up as the 4th FRAMES token -> fatal @0x0043a730; the OPPOSITE sense "
           "(abort-on-not-equal, C3's sense) would NOT fire here, so a swapped comparison would "
           "fail this check");
        ck_eq((uint32_t)g_log.abort_n, 1, "C4: utils_abort(0) fires exactly once");
        // The three slots read BEFORE the '}' was hit are real writes, not rolled back.
        ck_eq((uint32_t)fx.character_types[kIndex].frames[0].start, 1000u,
              "C4: frames[0].start was seeded from first_frame before the '}' token was reached");
        ck_eq((uint32_t)fx.character_types[kIndex].frames[0].count, 8u,
              "C4: slot 0 was written before the '}' token was reached");
        ck_eq((uint32_t)fx.character_types[kIndex].frames[2].count, 8u,
              "C4: slot 2 (the last one before the '}') was written");
        ck_eq((uint32_t)fx.character_types[kIndex].frames[3].start, (uint32_t)(kPoisonStart + 3),
              "C4: slot 3, where the '}' landed, is NEVER written -- poison survives (the check "
              "happens before that iteration's writes)");
    }

    printf("-- C5: frames[i].count is a BYTE; atoi's int32 result truncates on store (MOV byte "
           "ptr...,DL @0x0043a7b9)\n");
    {
        tact_fixture      fx;
        constexpr int32_t kIndex               = 11;
        fx.character_types[kIndex].first_frame = 42;
        tact_store own                         = fx.store();
        char       line[64];
        std::strcpy(line, "FRAMES { 8,8,8,8,8,300 }"); // last token drives atoi() above 255
        g_log.reset();
        mh::tact::detail::character_parse_frame_table(recording_calls(), own, line, kIndex);

        ck(!g_log.fatal_fired, "C5: six numeric tokens then '}' -- a well-formed line");
        ck_eq((uint32_t)fx.character_types[kIndex].frames[5].count, 44u,
              "C5: atoi(\"300\")==300 truncates to a uint8_t on store @0x0043a7b9 -- 300 & 0xff "
              "== 44, not clamped to 255 and not literal 300");
    }

    printf("-- C6: the leading token (normally 'FRAMES') is DISCARDED, never compared -- only "
           "the delimiter globals are checked\n");
    {
        tact_fixture      fx;
        constexpr int32_t kIndex               = 6;
        fx.character_types[kIndex].first_frame = 3;
        tact_store own                         = fx.store();
        char       line[64];
        std::strcpy(line, "XYZZY { 8,8,8,8,8,8 }"); // NOT the real keyword
        g_log.reset();
        mh::tact::detail::character_parse_frame_table(recording_calls(), own, line, kIndex);

        ck(!g_log.fatal_fired,
           "C6: a bogus leading keyword still parses cleanly -- the header's claim that token 1 "
           "is discarded, not validated, holds");
        ck_eq((uint32_t)fx.character_types[kIndex].frames[0].start, 3u,
              "C6: parsing proceeded normally despite the wrong leading keyword");
    }
}

} // namespace mh::tact::test
