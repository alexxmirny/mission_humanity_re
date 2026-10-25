//
// tact/tact_scroll_pace.h -- TSCR1: the game-clock pacing of the tactical camera scroll. Header-only, no
// dependencies, so the retail splice (mh/seams/tact_scroll.cpp) and the libmh twin (tact_frame.cpp step 14)
// run the SAME arithmetic and the offline selftest can drive it with a fake clock.
//
// WHAT RETAIL DOES (llm_tact_frame @0x0042a121-0x0042a27f, EN): with no mouse button down and the mine-blast
// step (_G_LLM_TACT_MINE_BLAST_SHAKE_STEP) == 0, each of the four directions tests its arrow latch or the cursor at the
// screen edge and moves the camera ONE TILE, once per RENDERED frame. There is no timer, so the speed is
// fps x tile (60 fps: 1920 px/s across, 1440 px/s down; the 32x24 tile makes vertical 25% slower).
//
// THE PACE. `px_s` pixels per second of GAME CLOCK (time_GetCurrentTime, so a pin_wallclock replay stays
// deterministic and a focus loss pauses it). A direction becoming active steps at once (a tap = 1 tile), then
// every `tile_px / px_s` seconds while it stays active (32 px columns, 24 px rows: 480 px/s = 15 columns/s,
// 20 rows/s). At most kMaxStepsPerFrame steps per direction per frame; if the clock is still behind after that
// the schedule is re-based on `now` (a long frame never turns into a catch-up burst).
//
// A call gap longer than kGapReset (the block is not reached while a mouse button is down or the mine-blast
// step runs, and a new mission restarts the clock) forgets the held state, so the first frame after a gap
// steps once instead of replaying a stale schedule.
//
#pragma once

namespace mh::tact {

constexpr int    kScrollTilePxCol  = 32;  // tile width in pixels (columns)
constexpr int    kScrollTilePxRow  = 24;  // tile height in pixels (rows)
constexpr int    kScrollDefaultPxS = 480; // [tactical] scroll_px_s default (user, 2026-10-24; was 240 on 2026-10-23)
constexpr int    kMaxStepsPerFrame = 2;
constexpr double kScrollGapReset   = 0.5; // seconds of game clock

enum scroll_dir { SCROLL_LEFT  = 0,
                  SCROLL_UP    = 1,
                  SCROLL_RIGHT = 2,
                  SCROLL_DOWN  = 3 };

struct scroll_pacer {
    bool   held[4]{};
    double next[4]{};
    double last = 0.0;
    bool   seen = false;

    void reset() {
        for (int i = 0; i < 4; ++i) {
            held[i] = false;
            next[i] = 0.0;
        }
        seen = false;
    }

    // Call once per frame BEFORE the four steps() calls.
    void begin_frame(double now) {
        if (seen && (now < last || now - last > kScrollGapReset)) reset();
        last = now;
        seen = true;
    }

    // Whole steps this direction takes this frame (0..kMaxStepsPerFrame).
    int steps(scroll_dir d, bool active, double now, int px_s) {
        if (!active || px_s <= 0) {
            held[d] = false;
            return 0;
        }
        const int    tile     = (d == SCROLL_LEFT || d == SCROLL_RIGHT) ? kScrollTilePxCol : kScrollTilePxRow;
        const double interval = static_cast<double>(tile) / static_cast<double>(px_s);
        if (!held[d]) {
            held[d] = true;
            next[d] = now + interval;
            return 1;
        }
        int n = 0;
        while (now >= next[d] && n < kMaxStepsPerFrame) {
            ++n;
            next[d] += interval;
        }
        if (now >= next[d]) next[d] = now + interval; // still late: re-base, no catch-up burst
        return n;
    }
};

} // namespace mh::tact
