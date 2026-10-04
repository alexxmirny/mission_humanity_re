//
// dinput_convert.cpp -- PT-INPUT1: the pure conversion half of the owned DirectInput. See the header.
//
#include "input/dinput_convert.h"

namespace mh::input {

namespace {
constexpr uint16_t VKEY_PAUSE   = 0x13; // VK_PAUSE
constexpr uint16_t VKEY_NUMLOCK = 0x90; // VK_NUMLOCK
constexpr uint16_t VKEY_NONE    = 0xFF; // Raw Input's "part of a longer sequence" marker
} // namespace

uint8_t scan_to_dik(uint16_t make, bool e0, bool e1, uint16_t vkey) {
    if (vkey == VKEY_NONE) return 0;
    if (vkey == VKEY_PAUSE) return 0xC5;   // DIK_PAUSE (E1 1D 45, or Ctrl+Break's E0 46 as Pause)
    if (vkey == VKEY_NUMLOCK) return 0x45; // DIK_NUMLOCK
    if (e1) return 0;                      // any other E1 sequence: nothing DirectInput names
    if (make == 0 || make >= 0x80) return 0;
    return (uint8_t)(make | (e0 ? 0x80 : 0));
}

bool mouse_queue::push(const mouse_raw &e) {
    if (count_ >= CAP) {
        ++overflows_;
        overflow_ = true;
        if (e.kind == mouse_raw::rel) {
            // Merge into the newest queued relative move rather than lose the travel.
            const int last = (tail_ + CAP - 1) % CAP;
            if (q_[last].kind == mouse_raw::rel) {
                q_[last].a += e.a;
                q_[last].b += e.b;
                return true;
            }
        } else if (e.kind == mouse_raw::abs) {
            const int last = (tail_ + CAP - 1) % CAP;
            if (q_[last].kind == mouse_raw::abs) {
                q_[last] = e;
                return true;
            }
        }
        return false;
    }
    q_[tail_] = e;
    tail_     = (tail_ + 1) % CAP;
    ++count_;
    return true;
}

int mouse_queue::produce(const mouse_view &v, di_element *out, int cap, uint32_t &seq) {
    int       n  = 0;
    int       vx = v.last_x, vy = v.last_y; // where the game will be after the elements so far
    const int div = v.div < 1 ? 1 : v.div;
    auto      put = [&](uint32_t ofs, int32_t data, uint32_t time) {
        if (out) {
            out[n].ofs       = ofs;
            out[n].data      = (uint32_t)data;
            out[n].timestamp = time;
            out[n].sequence  = ++seq;
        }
        ++n;
    };
    auto clamp_to_box = [&](int &x, int &y) {
        if (v.box_w > 0) x = x < 0 ? 0 : x >= v.box_w ? v.box_w - 1
                                                      : x;
        if (v.box_h > 0) y = y < 0 ? 0 : y >= v.box_h ? v.box_h - 1
                                                      : y;
    };
    for (;;) {
        if (have_target_) {
            int tx = tx_, ty = ty_;
            clamp_to_box(tx, ty);
            while ((vx != tx || vy != ty) && n < cap) {
                if (vx != tx) {
                    const int d = abs_step(vx, tx, v.thr);
                    put(MOFS_X, d * div, ttime_);
                    vx += d;
                    continue;
                }
                const int d = abs_step(vy, ty, v.thr);
                put(MOFS_Y, d * div, ttime_);
                vy += d;
            }
            if (vx != tx || vy != ty) return n; // out of room: resume from the game's accumulator next time
            have_target_ = false;
        }
        if (count_ == 0) return n;
        const mouse_raw &e = q_[head_];
        switch (e.kind) {
            case mouse_raw::abs: {
                // Coalesce a run of absolute packets: only the last position of the run matters.
                int last = head_, left = count_;
                while (left > 1) {
                    const int nx = (last + 1) % CAP;
                    if (q_[nx].kind != mouse_raw::abs) break;
                    last = nx;
                    --left;
                }
                tx_            = q_[last].a;
                ty_            = q_[last].b;
                ttime_         = q_[last].time;
                have_target_   = true;
                const int used = count_ - left + 1;
                head_          = (last + 1) % CAP;
                count_ -= used;
                continue;
            }
            case mouse_raw::rel: {
                // Both axes or neither: an element pair is not split across two calls.
                int32_t   ax = acc_x_, ay = acc_y_;
                const int qx   = carry_step(ax, e.a, div);
                const int qy   = carry_step(ay, e.b, div);
                const int need = (qx != 0) + (qy != 0);
                if (n + need > cap) return n;
                acc_x_ = ax;
                acc_y_ = ay;
                if (qx) put(MOFS_X, qx, e.time);
                if (qy) put(MOFS_Y, qy, e.time);
                // Follow the game's own arithmetic, so an absolute target after this starts from
                // where the game will really be.
                vx = game_axis_after(vx, qx, div, v.thr, v.box_w);
                vy = game_axis_after(vy, qy, div, v.thr, v.box_h);
                break;
            }
            case mouse_raw::button:
                if (n + 1 > cap) return n;
                put((uint32_t)e.a, e.b ? 0x80 : 0, e.time);
                break;
            case mouse_raw::wheel:
                if (n + 1 > cap) return n;
                put(MOFS_Z, e.a, e.time);
                break;
        }
        head_ = (head_ + 1) % CAP;
        --count_;
    }
}

} // namespace mh::input
