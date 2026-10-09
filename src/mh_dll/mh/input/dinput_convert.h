//
// dinput_convert.h -- PT-INPUT1: the pure half of the owned DirectInput ([input] backend=own).
//
// Everything here is arithmetic on values handed in: no Win32 call, no game memory. dinput_own.cpp
// feeds it Raw Input packets and the game's live accumulator, and hands the game what comes out; the
// `dinputconvtest` suite (mh_nettest/dinput_convert_selftest.cpp) drives it directly.
//
// THE CONSUMER THIS HAS TO SATISFY is llm_input_di_mouse_poll (0x004d0cc4). Per buffered element:
//   X (ofs 0) / Y (ofs 4): delta = (int)dwData / DIVISOR (signed IDIV, remainder DROPPED); if
//                          |delta| > THRESHOLD then delta *= 2; LAST += delta; clamp to [0, box).
//   Z (ofs 8)            : wheel += (int)dwData, raw.
//   ofs 0x0c / 0x0d      : left / right button, dwData != 0 = down.
//   anything else        : still becomes a ring event, with type 0 (the middle button, ofs 0x0e).
// So a relative element is exact only if it is a MULTIPLE of the divisor (carry_step keeps the rest
// for the next packet), and an absolute target is reached exactly only in steps of at most THRESHOLD
// game pixels, each encoded as step * DIVISOR (abs_step).
//
#pragma once

#include <stdint.h>

namespace mh::input {

// DIDEVICEOBJECTDATA as DirectInput 5 lays it out (16 bytes; DI8 appended uAppData).
struct di_element {
    uint32_t ofs;
    uint32_t data;
    uint32_t timestamp;
    uint32_t sequence;
};
static_assert(sizeof(di_element) == 16, "DI5 DIDEVICEOBJECTDATA is 16 bytes");

// c_dfDIMouse offsets (DIMOUSESTATE) -- the `ofs` of each mouse element.
enum : uint32_t { MOFS_X  = 0,
                  MOFS_Y  = 4,
                  MOFS_Z  = 8,
                  MOFS_B0 = 0x0c,
                  MOFS_B1 = 0x0d,
                  MOFS_B2 = 0x0e,
                  MOFS_B3 = 0x0f };

// ---- keyboard ------------------------------------------------------------------------------------

// Raw Input RAWKEYBOARD -> the DIK code DirectInput reports as the element's ofs (0 = drop the packet).
// DIK = set-1 make code | 0x80 when E0-prefixed; the game indexes its keystate with `ofs & 0x7f` and
// takes `ofs & 0x80` as the extended flag. Special cases, from the scan-code tables:
//   * vkey 0xFF (the E0 2A / E0 AA "fake shift" around PrtSc and the numpad-with-NumLock keys, and the
//     second half of Pause's E1 1D 45) -> dropped, as DirectInput does;
//   * Pause (E1 1D, vkey VK_PAUSE) -> 0xC5 (DIK_PAUSE); NumLock (vkey VK_NUMLOCK) -> 0x45 whatever
//     prefix the keyboard sent;
//   * make 0 / 0xFF (overrun) -> dropped.
uint8_t scan_to_dik(uint16_t make, bool e0, bool e1, uint16_t vkey);

// DI5 keyboard semantics over Raw Input: no auto-repeat (a make of a key already down is dropped), and
// the set of held keys, so a focus loss can release every one of them.
struct key_tracker {
    uint8_t held[256] = {};
    // Returns true when the transition should become an element (false = an auto-repeat make).
    bool accept(uint8_t dik, bool down) {
        if (down) {
            if (held[dik]) return false;
            held[dik] = 1;
            return true;
        }
        held[dik] = 0;
        return true; // an up is always delivered: the game's `&=` is idempotent
    }
    // Fill `out` with every held DIK (up to cap) and forget them all; returns the count.
    int release_all(uint8_t *out, int cap) {
        int n = 0;
        for (int k = 1; k < 256; ++k)
            if (held[k]) {
                if (n < cap) out[n++] = (uint8_t)k;
                held[k] = 0;
            }
        return n;
    }
};

// ---- mouse, relative -----------------------------------------------------------------------------

// Add `raw` counts to the axis carry and return the part the game's IDIV will pass exactly (a multiple
// of `div`, same sign convention as IDIV: truncation toward zero), keeping the remainder in `acc`.
// `div` < 1 is treated as 1 (the game would fault on 0; mouse_div refuses it).
inline int32_t carry_step(int32_t &acc, int32_t raw, int32_t div) {
    if (div < 1) div = 1;
    acc += raw;
    const int32_t q = acc / div; // C++ `/` truncates toward zero, like IDIV
    acc -= q * div;
    return q * div;
}

// ---- mouse, relative: Windows pointer ballistics -------------------------------------------------
//
// SYSTEM DirectInput does not hand the game raw hardware counts: on Windows 10/11 its relative X / Y are
// the counts AFTER the Control Panel's pointer settings, i.e. what moves the desktop pointer. Measured
// 2026-10-09 (Win11 26200 VM, real SendInput into a dinput.dll mouse, NONEXCLUSIVE and EXCLUSIVE alike,
// per-packet paced 1..8 ms -- the result does not depend on the packet rate, only on its size):
//   * "Enhance pointer precision" OFF: counts * T[speed], T = the 20-step table below (speed 10 = 1.0).
//   * ON: the smooth-mouse curve. out = (speed / 10) * 0.8 * Y(|v| / 3.5) along the packet's direction (each axis times the same gain),
//     |v| = max(|dx|,|dy|) + min(|dx|,|dy|)/2 counts (a diagonal n,n packet reads as 1.5 n, not 1.41 n), Y = the piecewise-linear curve through (SmoothMouseXCurve[i],
//     SmoothMouseYCurve[i]) (16.16 fixed, HKCU\Control Panel\Mouse), extended past the last point with
//     the last segment's slope. Fits the 10 measured sizes (1..80 counts) to <0.3% (1.5% on the 1- and
//     2-count packets, which are integer-rounded) and the 150..2500-count packets to <1%.
//   * the remainder is carried (a stream of 1-count packets at gain 0.56 sums to 0.56 per packet).
//   * the display's DPI scale multiplies all of it (125% -> x1.25 for a per-monitor-aware process); a
//     mode switch to a low resolution (what the real DirectDraw does) takes the factor back to 1.0. The
//     owned device applies NO such factor: that is the retail feel at the game's own display mode.
// Raw Input (what the owned device reads) carries none of this, so a 20000 dpi mouse fed 1:1 moved the
// game cursor far more per hand movement than system DirectInput did. pointer_scaler puts it back.

// The 20 steps of SPI_GETMOUSESPEED with "enhance pointer precision" off.
inline double pointer_speed_factor(int speed) {
    static const double T[20] = {0.03125, 0.0625, 0.125, 0.25, 0.375, 0.5, 0.625, 0.75, 0.875, 1.0, 1.25, 1.5, 1.75, 2.0, 2.25, 2.5, 2.75, 3.0, 3.25, 3.5};
    if (speed < 1) speed = 1;
    if (speed > 20) speed = 20;
    return T[speed - 1];
}

// The control-panel state the gain depends on. Default = the stock Windows 10/11 curve.
struct pointer_settings {
    int     speed  = 10;                                         // SPI_GETMOUSESPEED, 1..20
    bool    epp    = false;                                      // enhance pointer precision (SPI_GETMOUSE acceleration flag)
    int32_t in[5]  = {0, 0x6E15, 0x14000, 0x3DC29, 0x280000};    // SmoothMouseXCurve, 16.16: |v| / 3.5
    int32_t out[5] = {0, 0x111FD, 0x42400, 0x12FC00, 0x1BBC000}; // SmoothMouseYCurve, 16.16
    bool    is_identity() const { return !epp && speed == 10; }
};

// Pointer-gain at a packet of |v| = mag counts: output counts / input counts (> 0).
double pointer_gain(const pointer_settings &s, double mag);

// Raw relative counts -> the counts Windows would have moved the pointer by, remainder carried per axis.
class pointer_scaler {
public:
    // Scale one packet; returns false (outputs untouched = the packet unchanged) when the settings are the
    // identity. The outputs are whole counts; the sub-count rest waits in the carry for the next packet.
    bool apply(const pointer_settings &s, int32_t dx, int32_t dy, int32_t *ox, int32_t *oy);
    void reset() { ax_ = ay_ = 0; }

private:
    int64_t ax_ = 0, ay_ = 0; // carry, in 1/65536 count
};

// ---- mouse, absolute -----------------------------------------------------------------------------

// A normalized Raw Input absolute coordinate (0..65535 over `extent` pixels starting at `origin`) ->
// the pixel Windows moves the pointer to: floor(norm * extent / 65536), clamped into the range.
inline int norm_to_pixel(int32_t norm, int origin, int extent) {
    if (extent <= 0) return origin;
    long long p = ((long long)norm * extent) >> 16;
    if (p < 0) p = 0;
    if (p > extent - 1) p = extent - 1;
    return origin + (int)p;
}

// Its inverse, for the harness (a game pixel -> the normalized value that lands on it): the centre of
// pixel `px`, so norm_to_pixel(pixel_to_norm(px)) == px for every in-range px.
inline int32_t pixel_to_norm(int px, int origin, int extent) {
    if (extent <= 0) return 0;
    const long long rel = px - origin;
    return (int32_t)(((rel * 2 + 1) << 16) / (2LL * extent));
}

// The next step from `from` toward `to`, in game pixels: |step| <= thr, so the game's doubling never
// fires and the step lands exactly. thr < 1 is treated as 1.
inline int abs_step(int from, int to, int thr) {
    if (thr < 1) thr = 1;
    int d = to - from;
    if (d > thr) d = thr;
    if (d < -thr) d = -thr;
    return d;
}

// llm_input_di_mouse_poll's arithmetic for one X or Y element, as a model: IDIV, double above the
// threshold, accumulate, clamp to [0, box) (box <= 0: no clamp). The selftest pins it against the
// listing; produce() uses it to know where the game will be after a relative element.
inline int game_axis_after(int pos, int32_t data, int div, int thr, int box) {
    if (div < 1) div = 1;
    int d = data / div;
    if ((d < 0 ? -d : d) > thr) d += d;
    pos += d;
    if (box > 0) {
        if (pos >= box) pos = box - 1;
        if (pos < 0) pos = 0;
    }
    return pos;
}

// What the game currently believes, read at GetDeviceData time. box = the clamp box
// ([0,box_w) x [0,box_h), llm_input_mouse_init), div / thr = the divisor and doubling threshold.
struct mouse_view {
    int last_x, last_y;
    int box_w, box_h;
    int div, thr;
};

// One captured mouse packet, in capture order.
struct mouse_raw {
    enum kind_t : uint8_t { rel,
                            abs,
                            button,
                            wheel } kind;
    int32_t  a, b; // rel: dx, dy counts; abs: target game pixel x, y; button: ofs, down; wheel: delta
    uint32_t time; // GetTickCount at capture
};

// The owned mouse device's queue: raw packets in, DI elements out.
//   * relative packets carry their remainder (carry_step) so the divisor never eats motion;
//   * absolute packets become a TARGET, and each produce() walks the game's live accumulator to it in
//     exact steps -- consecutive absolute packets coalesce (only the last position matters), but a
//     button or wheel between two of them keeps its place, so a click lands where the pointer was;
//   * a produce() that runs out of room mid-walk resumes from the game's accumulator next time.
class mouse_queue {
public:
    static constexpr int CAP = 1024;

    // Queue one packet. Returns false (and counts an overflow) when full; a relative move then merges
    // into the newest queued relative move instead of being lost.
    bool push(const mouse_raw &e);

    // Produce up to `cap` elements into `out` (null = count only, discarding: the DI flush form).
    // `seq` is the caller's running sequence number. Returns the element count.
    int produce(const mouse_view &v, di_element *out, int cap, uint32_t &seq);

    void clear() {
        head_ = tail_ = count_ = 0;
        have_target_           = false;
    }
    bool take_overflow() {
        const bool o = overflow_;
        overflow_    = false;
        return o;
    }
    int  pending() const { return count_ + (have_target_ ? 1 : 0); }
    long overflows() const { return overflows_; }

private:
    mouse_raw q_[CAP];
    int       head_ = 0, tail_ = 0, count_ = 0;
    bool      overflow_  = false;
    long      overflows_ = 0;
    int32_t   acc_x_ = 0, acc_y_ = 0; // relative carries, in raw counts
    bool      have_target_ = false;
    int       tx_ = 0, ty_ = 0;
    uint32_t  ttime_ = 0;
};

} // namespace mh::input
