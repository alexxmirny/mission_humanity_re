//
// info_avi_selftest.cpp -- `net_selftest.exe infoavitest`: mp:X2g, the info-screen media tick's
// guard (seams/ui_info_avi.h), the arithmetic the replaced llm_ui_info_media_frame_tick runs.
//
// THE CRASH IT GUARDS. After a failed llm_ui_avi_open the media context is zero-filled, so
// video_stream.stream_length_time is 0, and retail's tick wrapped `playback_pos %= 0` -- IDIV by
// zero at 0x004cab52 (rc4 field report, 0xc0000094). The arms below drive plan_tick / video_on with
// exactly that state (len = 0, surface NULL or stale), then retail's own looping arithmetic, which
// must be unchanged, then the test knob's name matcher (`[net] info_avi_test_absent`).
//
// A suite and not a rig check: the rig scenario (registry row info_avi_absent) proves the game
// survives ONE failed open; the wrap arithmetic on a live clip (the thing the guard must NOT change)
// is only visited when a clip loops, which no UI scenario waits for.
//
#include <stdio.h>
#include <stdint.h>
#include <limits.h>

#include "seams/ui_info_avi.h"

namespace {

int g_checks = 0, g_fails = 0;

void check(const char *what, bool ok) {
    ++g_checks;
    if (!ok) {
        ++g_fails;
        printf("  FAIL: %s\n", what);
    }
}

// Retail's step for a positive length, spelled independently of plan_tick (the oracle for arm B).
int32_t retail_pos(int32_t len, int32_t pos, uint32_t now, uint32_t last) {
    int32_t p = pos + (int32_t)(now - last);
    if (len <= p) p = p % len;
    return p;
}

} // namespace

int run_infoavitest() {
    printf("=== infoavitest (mp:X2g: the info-screen media tick guard) ===\n");
    using namespace mh::info_avi;

    // A. THE FIELD STATE: a failed open zero-fills the context. stream_length_time = 0, and the tick
    //    must neither divide nor touch the video. (Reaching the check at all is the no-IDIV proof;
    //    the harness would die on 0xc0000094 otherwise.)
    {
        const tick_step s = plan_tick(0, 0, 1000u, 984u);
        check("A1: len 0 -> no wrap (retail IDIVs here)", !s.wrapped);
        check("A2: len 0 -> position parked at 0", s.pos == 0);
        check("A3: len 0 -> the frame delta is still stored (the MP keepalive reads it)", s.delta == 16);
        check("A4: no surface -> no video", !video_on(false, 0));
        check("A5: a STALE surface with len 0 -> no video (the field's own state: Lock worked, IDIV died)",
              !video_on(true, 0));
        check("A6: no surface, positive length -> no video (first-ever open that failed)", !video_on(false, 5000));
        check("A7: a negative length is not a clip", !video_on(true, -1) && !plan_tick(-1, 3, 10u, 0u).wrapped);
        // Many frames in a row on the no-video screen: the position never escapes, nothing divides.
        int32_t  pos = 0;
        uint32_t t   = 5000u;
        bool     ok  = true;
        for (int i = 0; i < 10000; ++i) {
            const tick_step f = plan_tick(0, pos, t + 16u, t);
            ok                = ok && f.pos == 0 && !f.wrapped && f.delta == 16;
            pos               = f.pos;
            t += 16u;
        }
        check("A8: 10000 no-video frames: parked, never wrapped", ok);
        check("A9: the tick clock wrapping at 2^32 still gives the right delta",
              plan_tick(0, 0, 5u, 0xFFFFFFF0u).delta == 21);
    }

    // B. A LIVE CLIP IS UNCHANGED: every (len, pos, delta) the retail arithmetic handles must come out
    //    bit-identical -- the guard exists for len <= 0 only.
    {
        const int32_t lens[] = {1, 7, 1000, 66733, 400000};
        bool          same = true, wrapflag = true;
        for (int32_t len : lens)
            for (int32_t pos = 0; pos < len && pos < 3000; pos += (len > 100 ? 97 : 1))
                for (uint32_t d = 0; d < 3 * (uint32_t)len && d < 5000u; d += 13u) {
                    const tick_step s = plan_tick(len, pos, 100000u + d, 100000u);
                    same              = same && s.pos == retail_pos(len, pos, 100000u + d, 100000u);
                    wrapflag          = wrapflag && s.wrapped == (len <= pos + (int32_t)d);
                    same              = same && s.delta == (int32_t)d;
                }
        check("B1: positive lengths -> position identical to retail's", same);
        check("B2: the -1 wrap-mark store happens exactly when retail's branch runs", wrapflag);
        check("B3: surface + clip -> video", video_on(true, 1) && video_on(true, INT_MAX));
        check("B4: exactly at the end (pos == len) wraps to 0", plan_tick(1000, 990, 10u, 0u).pos == 0 &&
                                                                    plan_tick(1000, 990, 10u, 0u).wrapped);
        check("B5: one before the end does not wrap", !plan_tick(1000, 989, 10u, 0u).wrapped);
    }

    // C. THE TEST KNOB'S MATCHER (`[net] info_avi_test_absent=`).
    {
        check("C1: empty never matches", !name_listed("", "res\\H_INV.AVI") && !name_listed(nullptr, "x"));
        check("C2: * matches any clip", name_listed("*", "res\\H_INV.AVI") && name_listed("*", "c:\\g\\res\\A.avi"));
        check("C3: basename, case-insensitive", name_listed("h_inv.avi", "res\\H_INV.AVI"));
        check("C4: a comma list with spaces", name_listed("A_INV.AVI, H_INV.AVI", "D:\\mh\\res\\h_inv.avi"));
        check("C5: a prefix is not a match", !name_listed("H_INV", "res\\H_INV.AVI"));
        check("C6: a longer name is not a match", !name_listed("H_INV.AVIX", "res\\H_INV.AVI"));
        check("C7: another clip is not a match", !name_listed("A_INV.AVI", "res\\H_INV.AVI"));
        check("C8: forward slashes split too", name_listed("h_inv.avi", "res/H_INV.AVI"));
    }

    printf("infoavitest: %d checks, %d failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
