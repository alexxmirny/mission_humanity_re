//
// dinput_convert_selftest.cpp -- `net_selftest.exe dinputconvtest`: PT-INPUT1, the owned DirectInput's
// element conversion (mh/input/dinput_convert.{h,cpp}): scan code -> DIK (the E0 / E1 / fake-shift
// cases), DI5 auto-repeat filtering and the focus-loss release set, the relative remainder carry, the
// absolute -> exact-delta walk under the game's own IDIV + doubling + clamp, and the normalized
// coordinate round trip the harness's `rawmouse abs` relies on.
//
// A suite and not only a rig check: the absolute path is what a VM or RDP pointer takes and the rig's
// headless lanes have no pointer at all, and the arithmetic has to be right for EVERY divisor and
// threshold, not for the one a run happens to use. Pure: no window, no game memory.
//
#include <stdio.h>
#include <stdlib.h>

#include "input/dinput_convert.h"

namespace {

using namespace mh::input;

int g_checks = 0, g_fails = 0;

void check(const char *what, bool ok) {
    ++g_checks;
    if (!ok) {
        ++g_fails;
        printf("  FAIL: %s\n", what);
    }
}

// The game's llm_input_di_mouse_poll, written from the listing (0x004d0dc8..0x004d0e36 for X), NOT from
// game_axis_after -- so the model the queue uses is itself pinned.
int listing_axis(int pos, int32_t data, int div, int thr, int box) {
    int d = data / div; // IDIV: truncation toward zero
    int a = d > 0 ? d : -d;
    if (a > thr) d = d + d;
    pos += d;
    if (pos >= box) pos = box - 1;
    if (pos < 0) pos = 0;
    return pos;
}

struct game_sim {
    mouse_view v;
    int        buttons = 0, wheel = 0, events = 0;
    void       apply(const di_element *e, int n) {
        for (int i = 0; i < n; ++i) {
            ++events;
            switch (e[i].ofs) {
                case MOFS_X: v.last_x = listing_axis(v.last_x, (int32_t)e[i].data, v.div, v.thr, v.box_w); break;
                case MOFS_Y: v.last_y = listing_axis(v.last_y, (int32_t)e[i].data, v.div, v.thr, v.box_h); break;
                case MOFS_Z: wheel += (int32_t)e[i].data; break;
                case MOFS_B0: buttons = e[i].data ? (buttons | 1) : (buttons & ~1); break;
                case MOFS_B1: buttons = e[i].data ? (buttons | 2) : (buttons & ~2); break;
                default: break;
            }
        }
    }
};

mouse_view view(int x, int y, int div, int thr) {
    mouse_view v;
    v.last_x = x;
    v.last_y = y;
    v.box_w  = 640;
    v.box_h  = 480;
    v.div    = div;
    v.thr    = thr;
    return v;
}

void test_scan_to_dik() {
    check("plain A (0x1e) -> 0x1e", scan_to_dik(0x1e, false, false, 'A') == 0x1e);
    check("Up arrow (E0 48) -> 0xc8", scan_to_dik(0x48, true, false, 0x26) == 0xc8);
    check("Left arrow (E0 4b) -> 0xcb", scan_to_dik(0x4b, true, false, 0x25) == 0xcb);
    check("numpad 8 (48, no E0) -> 0x48", scan_to_dik(0x48, false, false, 0x68) == 0x48);
    check("right Ctrl (E0 1d) -> 0x9d", scan_to_dik(0x1d, true, false, 0x11) == 0x9d);
    check("left Ctrl (1d) -> 0x1d", scan_to_dik(0x1d, false, false, 0x11) == 0x1d);
    check("numpad Enter (E0 1c) -> 0x9c", scan_to_dik(0x1c, true, false, 0x0d) == 0x9c);
    check("right Alt (E0 38) -> 0xb8", scan_to_dik(0x38, true, false, 0x12) == 0xb8);
    check("PrtSc (E0 37) -> 0xb7", scan_to_dik(0x37, true, false, 0x2c) == 0xb7);
    check("fake shift (E0 2a, vkey 0xff) dropped", scan_to_dik(0x2a, true, false, 0xff) == 0);
    check("Pause (E1 1d, VK_PAUSE) -> 0xc5", scan_to_dik(0x1d, false, true, 0x13) == 0xc5);
    check("Pause's second half (45, vkey 0xff) dropped", scan_to_dik(0x45, false, false, 0xff) == 0);
    check("NumLock (45, VK_NUMLOCK) -> 0x45", scan_to_dik(0x45, false, false, 0x90) == 0x45);
    check("NumLock with E0 -> still 0x45", scan_to_dik(0x45, true, false, 0x90) == 0x45);
    check("make 0 dropped", scan_to_dik(0, false, false, 0x41) == 0);
    check("overrun 0xff dropped", scan_to_dik(0xff, false, false, 0x41) == 0);
    check("other E1 sequence dropped", scan_to_dik(0x1d, false, true, 0x11) == 0);
}

void test_key_tracker() {
    key_tracker t;
    check("first make accepted", t.accept(0xc8, true));
    check("auto-repeat make dropped", !t.accept(0xc8, true));
    check("second key accepted", t.accept(0x9d, true));
    check("break accepted", t.accept(0xc8, false));
    check("make after break accepted", t.accept(0xc8, true));
    check("stray break still delivered", t.accept(0x10, false));
    uint8_t   out[8];
    const int n = t.release_all(out, 8);
    check("release_all: both held keys", n == 2 && ((out[0] == 0x9d && out[1] == 0xc8) || (out[0] == 0xc8 && out[1] == 0x9d)));
    check("release_all: nothing held after", t.release_all(out, 8) == 0);
    check("a make after release is fresh", t.accept(0x9d, true));
}

void test_carry() {
    int32_t acc = 0;
    check("div 51: 20 -> 0", carry_step(acc, 20, 51) == 0 && acc == 20);
    check("div 51: +20 -> 0", carry_step(acc, 20, 51) == 0 && acc == 40);
    check("div 51: +20 -> 51, rest 9", carry_step(acc, 20, 51) == 51 && acc == 9);
    acc = 0;
    check("div 51: -30 -> 0", carry_step(acc, -30, 51) == 0 && acc == -30);
    check("div 51: -30 -> -51, rest -9", carry_step(acc, -30, 51) == -51 && acc == -9);
    check("reversal eats the carry, not the motion", carry_step(acc, 60, 51) == 51 && acc == 0);
    acc = 0;
    check("div 1 passes through", carry_step(acc, -7, 1) == -7 && acc == 0);
    check("div 0 treated as 1", carry_step(acc, 5, 0) == 5 && acc == 0);
    // A slow hand: 1000 packets of 1 count at div 40 move the game exactly 1000/40 pixels.
    acc     = 0;
    int pos = 0;
    for (int i = 0; i < 1000; ++i) pos += carry_step(acc, 1, 40) / 40;
    check("1000 x 1 count at div 40 = 25 px (the shipped poll: 0)", pos == 25 && acc == 0);
}

void test_axis_model() {
    struct {
        int pos, data, div, thr, box;
    } cases[] = {{320, 150, 1, 100, 640}, {320, -101, 1, 100, 640}, {320, 100, 1, 100, 640}, {630, 50, 1, 100, 640}, {10, -50, 1, 100, 640}, {320, 5100, 51, 100, 640}, {320, 5151, 51, 100, 640}, {320, -5202, 51, 100, 640}, {320, 50, 51, 100, 640}, {320, -50, 51, 100, 640}, {0, 0, 1, 100, 640}, {639, 1, 1, 100, 640}};
    bool ok   = true;
    for (const auto &c : cases) ok &= game_axis_after(c.pos, c.data, c.div, c.thr, c.box) == listing_axis(c.pos, c.data, c.div, c.thr, c.box);
    check("game_axis_after == the listing's IDIV/double/clamp on 12 cases", ok);
    check("150 counts at thr 100 doubles to +300", game_axis_after(0, 150, 1, 100, 1000) == 300);
    check("100 counts at thr 100 does not double", game_axis_after(0, 100, 1, 100, 1000) == 100);
}

// Push `raw`, then produce with room `cap` per call until nothing is pending, applying every batch.
void drain(mouse_queue &q, game_sim &g, int cap, uint32_t &seq, int *calls = nullptr, bool *steps_ok = nullptr) {
    di_element buf[512];
    for (int guard = 0; guard < 10000; ++guard) {
        const int n = q.produce(g.v, buf, cap, seq);
        if (steps_ok)
            for (int i = 0; i < n; ++i)
                if (buf[i].ofs == MOFS_X || buf[i].ofs == MOFS_Y) {
                    const int32_t d = (int32_t)buf[i].data;
                    if (d % g.v.div != 0 || abs(d / g.v.div) > (g.v.thr < 1 ? 1 : g.v.thr)) *steps_ok = false;
                }
        g.apply(buf, n);
        if (calls) ++*calls;
        if (q.pending() == 0 && n == 0) return;
        if (n == 0 && q.pending() != 0) return; // stuck -- the caller's check reports it
    }
}

void test_absolute() {
    const int divs[]    = {1, 7, 40, 51};
    const int thrs[]    = {100, 3, 1};
    const int tgts[][2] = {{0, 0}, {639, 479}, {1, 478}, {320, 240}, {17, 400}, {600, 3}};
    bool      all = true, steps = true;
    for (int div : divs)
        for (int thr : thrs)
            for (const auto &t : tgts) {
                mouse_queue q;
                game_sim    g;
                g.v          = view(320, 240, div, thr);
                uint32_t seq = 0;
                q.push({mouse_raw::abs, t[0], t[1], 1});
                drain(q, g, 256, seq, nullptr, &steps);
                if (g.v.last_x != t[0] || g.v.last_y != t[1]) {
                    all = false;
                    printf("    abs div=%d thr=%d -> (%d,%d) landed (%d,%d)\n", div, thr, t[0], t[1], g.v.last_x, g.v.last_y);
                }
            }
    check("absolute target lands EXACTLY for div {1,7,40,51} x thr {100,3,1} x 6 targets", all);
    check("every absolute step is a multiple of div and within the threshold", steps);

    // Out of the box: clamped to the box, not pinned against it with travel discarded.
    {
        mouse_queue q;
        game_sim    g;
        g.v          = view(100, 100, 51, 100);
        uint32_t seq = 0;
        q.push({mouse_raw::abs, 5000, -20, 1});
        drain(q, g, 256, seq);
        check("a target outside the box lands on its edge", g.v.last_x == 639 && g.v.last_y == 0);
    }
    // Coalescing: three absolute packets are one target.
    {
        mouse_queue q;
        game_sim    g;
        g.v          = view(320, 240, 1, 100);
        uint32_t seq = 0;
        q.push({mouse_raw::abs, 10, 10, 1});
        q.push({mouse_raw::abs, 500, 20, 2});
        q.push({mouse_raw::abs, 330, 250, 3});
        drain(q, g, 256, seq);
        check("a run of absolute packets coalesces to the last (2 elements, not ~20)", g.v.last_x == 330 && g.v.last_y == 250 && g.events == 2);
    }
    // A click between two absolute packets lands where the pointer was.
    {
        mouse_queue q;
        game_sim    g;
        g.v          = view(320, 240, 51, 100);
        uint32_t seq = 0;
        q.push({mouse_raw::abs, 112, 192, 1});
        q.push({mouse_raw::button, (int32_t)MOFS_B0, 1, 2});
        q.push({mouse_raw::button, (int32_t)MOFS_B0, 0, 3});
        q.push({mouse_raw::abs, 600, 400, 4});
        di_element buf[64];
        const int  n        = q.produce(g.v, buf, 64, seq);
        game_sim   g2       = g;
        int        down_at  = -1;
        bool       down_pos = false;
        for (int i = 0; i < n; ++i) {
            g2.apply(buf + i, 1);
            if (buf[i].ofs == MOFS_B0 && buf[i].data == 0x80) {
                down_at  = i;
                down_pos = g2.v.last_x == 112 && g2.v.last_y == 192;
            }
        }
        check("the button DOWN is emitted with the cursor exactly on the click target", down_at >= 0 && down_pos);
        check("and the walk continues to the next target after the click", g2.v.last_x == 600 && g2.v.last_y == 400);
        bool seq_ok = true;
        for (int i = 1; i < n; ++i) seq_ok &= buf[i].sequence == buf[i - 1].sequence + 1;
        check("sequence numbers are consecutive", seq_ok);
    }
    // Room for 2 elements per call: the walk resumes from the game's accumulator and still lands.
    {
        mouse_queue q;
        game_sim    g;
        g.v            = view(320, 240, 51, 10);
        uint32_t seq   = 0;
        int      calls = 0;
        q.push({mouse_raw::abs, 0, 479, 1});
        drain(q, g, 2, seq, &calls);
        check("a walk split over many small calls still lands exactly", g.v.last_x == 0 && g.v.last_y == 479 && calls > 10);
    }
    // The overlay's drain drops what it reads: the next target starts from where the game really is.
    {
        mouse_queue q;
        game_sim    g;
        g.v          = view(320, 240, 1, 100);
        uint32_t seq = 0;
        q.push({mouse_raw::abs, 100, 100, 1});
        di_element buf[64];
        q.produce(g.v, buf, 64, seq); // drained: NOT applied
        q.push({mouse_raw::abs, 200, 50, 2});
        drain(q, g, 256, seq);
        check("after a drain, the next target is reached from the game's real position", g.v.last_x == 200 && g.v.last_y == 50);
    }
}

void test_relative() {
    // A slow hand at div 51 (the dinputto8 recipe's divisor): nothing is swallowed over time.
    mouse_queue q;
    game_sim    g;
    g.v          = view(320, 240, 51, 100);
    uint32_t seq = 0;
    for (int i = 0; i < 510; ++i) q.push({mouse_raw::rel, -1, 2, (uint32_t)i});
    drain(q, g, 256, seq);
    check("510 packets of (-1,+2) counts at div 51 move exactly (-10,+20)", g.v.last_x == 310 && g.v.last_y == 260);
    // The shipped poll on the same packets: every one truncates to 0.
    int shipped = 320;
    for (int i = 0; i < 510; ++i) shipped = listing_axis(shipped, -1, 51, 100, 640);
    check("(the shipped per-element divide moves 0 on the same input)", shipped == 320);
    // div 1, the game's feel: a 150-count packet still doubles.
    mouse_queue q2;
    game_sim    g2;
    g2.v = view(100, 100, 1, 100);
    q2.push({mouse_raw::rel, 150, 0, 1});
    drain(q2, g2, 256, seq);
    check("div 1: a 150-count packet keeps the game's doubling (+300)", g2.v.last_x == 400);
    // A relative move then an absolute target: the walk starts where the relative move left the game.
    mouse_queue q3;
    game_sim    g3;
    g3.v = view(100, 100, 1, 100);
    q3.push({mouse_raw::rel, 150, 0, 1});
    q3.push({mouse_raw::abs, 50, 60, 2});
    drain(q3, g3, 256, seq);
    check("relative then absolute in one call still lands exactly", g3.v.last_x == 50 && g3.v.last_y == 60);
    // Buttons and the wheel.
    mouse_queue q4;
    game_sim    g4;
    g4.v = view(0, 0, 1, 100);
    q4.push({mouse_raw::button, (int32_t)MOFS_B1, 1, 1});
    q4.push({mouse_raw::wheel, -120, 0, 2});
    di_element buf[8];
    const int  n = q4.produce(g4.v, buf, 8, seq);
    check("right DOWN is ofs 0x0d data 0x80", n == 2 && buf[0].ofs == MOFS_B1 && buf[0].data == 0x80 && buf[0].timestamp == 1);
    check("wheel is ofs 8 with the raw delta", buf[1].ofs == MOFS_Z && (int32_t)buf[1].data == -120);
    // A full queue merges relative travel instead of losing it.
    mouse_queue *q5 = new mouse_queue;
    game_sim     g5;
    g5.v       = view(0, 0, 1, 1000);
    g5.v.box_w = g5.v.box_h = 100000;
    for (int i = 0; i < mouse_queue::CAP + 50; ++i) q5->push({mouse_raw::rel, 1, 0, 1});
    check("overflow is reported once", q5->take_overflow() && !q5->take_overflow());
    drain(*q5, g5, 256, seq);
    check("an overflowing queue keeps the total travel", g5.v.last_x == mouse_queue::CAP + 50);
    delete q5;
}

void test_norm_roundtrip() {
    const int ext[] = {640, 1024, 1280, 1920, 2560, 3840, 5120};
    bool      ok    = true;
    for (int e : ext)
        for (int px = 0; px < e; ++px) ok &= norm_to_pixel(pixel_to_norm(px, 0, e), 0, e) == px;
    check("pixel -> normalized -> pixel is the identity on 7 extents (every pixel)", ok);
    check("a negative origin (monitor left of primary) round-trips", norm_to_pixel(pixel_to_norm(-1500, -1920, 3840), -1920, 3840) == -1500);
    check("norm 0 is the first pixel, 65535 the last", norm_to_pixel(0, 0, 1024) == 0 && norm_to_pixel(65535, 0, 1024) == 1023);
}

} // namespace

int run_dinputconvtest() {
    printf("dinputconvtest: PT-INPUT1 owned DirectInput element conversion\n");
    test_scan_to_dik();
    test_key_tracker();
    test_carry();
    test_axis_model();
    test_absolute();
    test_relative();
    test_norm_roundtrip();
    printf("dinputconvtest: %d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
