//
// gfx/res_list.cpp -- see res_list.h. Pure; no CRT formatting (the DLL's style), no allocation.
//
#include "gfx/res_list.h"

namespace mh::gfx::reslist {

int floor32(int v) {
    return v & ~31;
}
int round32(int v) {
    return (v + 16) & ~31;
}
int ceil32(int v) {
    return (v + 31) & ~31;
}
// The widest 16:9 width for a height, rounded UP: a 1366x768 panel is "16:9" (768 * 16 / 9 = 1365.3).
static int wide169(int h) {
    return (h * 16 + 8) / 9;
}

// RES18: any size is valid; the game RENDERS ceil32 of it and the presenter crops to the size itself. The caps and the tile
// ceiling are properties of the rendered frame, so they are tested on the rounded-up size.
bool valid_size(int w, int h) {
    if (w < MIN_W || h < MIN_H) return false;
    const int rw = ceil32(w), rh = ceil32(h);
    if (rw > CAP_W || rh > CAP_H) return false;
    return (rw / 32) * (rh / 32) <= MAX_TILES;
}

bool view_wraps_smallest_map(int w, int h) {
    return ceil32(w) > NOWRAP_W || ceil32(h) > NOWRAP_H;
}

bool within_16x9(int w, int h) {
    return w <= wide169(h);
}

int aspect_group(int w, int h) {
    const int d43 = 3 * w - 4 * h;
    if (d43 >= -96 && d43 <= 96) return grp_4x3;
    const int d169 = 9 * w - 16 * h; // sizes are exact now (RES18); the slack still covers a hand-typed or legacy 32-snapped size
    if (d169 >= -288 && d169 <= 496) return grp_16x9;
    return grp_other;
}

namespace {

constexpr int K100[] = {100, 125, 150, 200, 250, 300, 400};
constexpr int NK     = (int)(sizeof(K100) / sizeof(K100[0]));

// RES24: common display sizes, offered whenever they fit the desktop and are valid. 4:3 first, then 16:9. 800x600 is a
// real 800x600 here (renders 800x608, the presenter crops); the stock 800x576 entry stays separate.
struct common_size {
    int w, h;
};
constexpr common_size COMMON[] = {
    {800, 600}, {1024, 768}, {1152, 864}, {1280, 960}, {1400, 1050}, {1600, 1200}, {1920, 1440}, {2048, 1536}, // 4:3
    {1280, 720},
    {1366, 768},
    {1600, 900},
    {1920, 1080},
    {2560, 1440},
    {3200, 1800},
    {3840, 2160}, // 16:9
};
constexpr int NCOMMON = (int)(sizeof(COMMON) / sizeof(COMMON[0]));

long area(const entry &e) {
    return (long)e.w * e.h;
}

// Add unless a same-size entry is already there (the first kind wins: stock, then native, then 4:3).
void add(entry *out, int *n, int cap, int w, int h, int kind) {
    for (int i = 0; i < *n; ++i)
        if (out[i].w == w && out[i].h == h) return;
    if (*n >= cap) return;
    out[*n] = entry{w, h, kind};
    ++*n;
}

// The tail order: 4:3 sizes, then 16:9, then the rest; ascending by area within a group (width breaks a tie).
bool tail_less(const entry &a, const entry &b) {
    const int ga = aspect_group(a.w, a.h), gb = aspect_group(b.w, b.h);
    if (ga != gb) return ga < gb;
    if (area(a) != area(b)) return area(a) < area(b);
    return a.w < b.w;
}

void sort_tail(entry *list, int n) { // insertion sort in tail_less order, indices [3, n)
    for (int i = 4; i < n; ++i)
        for (int j = i; j > 3 && tail_less(list[j], list[j - 1]); --j) {
            entry t     = list[j];
            list[j]     = list[j - 1];
            list[j - 1] = t;
        }
}

} // namespace

int generate(int dw, int dh, entry *out, int cap) {
    int n = 0;
    add(out, &n, cap, 640, 480, k_stock);
    add(out, &n, cap, 800, 576, k_stock); // the engine's "800x600" renders 576 rows
    add(out, &n, cap, 1024, 768, k_stock);
    if (dw <= 0 || dh <= 0) return n;
    for (int i = 0; i < NK; ++i) {
        const int h = dh * 100 / K100[i]; // RES18: exact, not snapped to the tile grid -- the presenter crops the rounded-up frame
        int       w = dw * 100 / K100[i];
        if (w > wide169(h)) w = h * 16 / 9; // a wider desktop: 16:9 of its height, side bars
        if (valid_size(w, h) && w <= dw && h <= dh) add(out, &n, cap, w, h, k_native);
    }
    for (int i = 0; i < NK; ++i) {
        const int h = dh * 100 / K100[i];
        const int w = (h * 4 + 1) / 3; // nearest whole pixel
        if (valid_size(w, h) && w <= dw && h <= dh) add(out, &n, cap, w, h, k_4x3);
    }
    for (int i = 0; i < NCOMMON; ++i)
        if (valid_size(COMMON[i].w, COMMON[i].h) && COMMON[i].w <= dw && COMMON[i].h <= dh) add(out, &n, cap, COMMON[i].w, COMMON[i].h, k_common);
    sort_tail(out, n);
    return n;
}

entry auto_size(int dw, int dh, int scale_pct) {
    if (scale_pct <= 0) scale_pct = 100;
    int       w    = (int)((long long)dw * 100 / scale_pct); // RES18: exact -- the frame is rounded up and cropped, not the size down
    int       h    = (int)((long long)dh * 100 / scale_pct);
    const int w169 = wide169(h);
    if (w > w169) w = h * 16 / 9;           // never wider than 16:9 (a wider desktop gets side bars)
    if (w < AUTO_MIN_W || h < AUTO_MIN_H) { // never below the largest stock UI ...
        if (w < AUTO_MIN_W) w = AUTO_MIN_W;
        if (h < AUTO_MIN_H) h = AUTO_MIN_H;
    }
    if (dw > 0 && w > dw) w = dw; // ... and never above the desktop (this one wins)
    if (dh > 0 && h > dh) h = dh;
    if (ceil32(w) > CAP_W || ceil32(h) > CAP_H) { // over the cap (tested on the rendered frame): the largest generated "native" entry (it keeps the desktop's aspect)
        entry     list[MAX_ENTRIES];
        const int n    = generate(dw, dh, list, MAX_ENTRIES);
        int       best = -1;
        for (int i = 3; i < n; ++i)
            if (list[i].kind == k_native && (best < 0 || area(list[i]) > area(list[best]))) best = i;
        if (best >= 0) return list[best];
        w = CAP_W; // no native entry fits (an extreme aspect): the cap itself
        h = CAP_H;
    }
    if (w < MIN_W) w = MIN_W;
    if (h < MIN_H) h = MIN_H;
    return entry{w, h, k_native};
}

bool parse_size(const char *s, int *w, int *h) {
    if (!s) return false;
    long a = 0, b = 0;
    int  digits = 0;
    while (*s >= '0' && *s <= '9') {
        a = a * 10 + (*s++ - '0');
        if (++digits > 5) return false;
    }
    if (!digits || (*s != 'x' && *s != 'X')) return false;
    ++s;
    digits = 0;
    while (*s >= '0' && *s <= '9') {
        b = b * 10 + (*s++ - '0');
        if (++digits > 5) return false;
    }
    if (!digits || *s) return false;
    *w = (int)a;
    *h = (int)b;
    return true;
}

int index_of(const entry *list, int n, int w, int h) {
    for (int i = 0; i < n; ++i)
        if (list[i].w == w && list[i].h == h) return i;
    return -1;
}

int insert_sorted(entry *list, int n, int cap, entry e, int *new_count) {
    const int have = index_of(list, n, e.w, e.h);
    *new_count     = n;
    if (have >= 0) return have;
    if (n >= cap) return -1;
    int at = n;
    while (at > 3 && tail_less(e, list[at - 1])) {
        list[at] = list[at - 1];
        --at;
    }
    if (at < 3) at = 3;
    list[at]   = e;
    *new_count = n + 1;
    return at;
}

bool integer_scale_ok(int dw, int dh, int w, int h) {
    if (dw <= 0 || dh <= 0 || w <= 0 || h <= 0) return false;
    const double sx = (double)dw / w, sy = (double)dh / h;
    const double s = sx < sy ? sx : sy;
    const int    f = (int)(s + 1e-9);
    if (f < 1) return false;
    return (s - f) <= 0.03 * f;
}

namespace {

struct sink {
    char *p;
    int   n, cap;
    void  put(char c) {
        if (n < cap - 1) p[n++] = c;
    }
    void str(const char *s) {
        while (*s) put(*s++);
    }
    void num(int v) {
        char tmp[12];
        int  t = 0;
        if (v <= 0) tmp[t++] = '0';
        while (v > 0) {
            tmp[t++] = (char)('0' + v % 10);
            v /= 10;
        }
        while (t > 0) put(tmp[--t]);
    }
};

// "1:1" | "2x" | "1.5x" | "1.25x"
void scale_text(sink &o, int s100) {
    const int n   = (s100 + 50) / 100;
    const int dev = s100 > n * 100 ? s100 - n * 100 : n * 100 - s100;
    if (n >= 1 && dev <= 3 * n) {
        if (n == 1) {
            o.str("1:1");
        } else {
            o.num(n);
            o.put('x');
        }
        return;
    }
    o.num(s100 / 100);
    o.put('.');
    const int frac = s100 % 100;
    o.put((char)('0' + frac / 10));
    if (frac % 10) o.put((char)('0' + frac % 10));
    o.put('x');
}

} // namespace

int describe(const entry &e, int dw, int dh, char *out, int cap) {
    sink o{out, 0, cap};
    o.num(e.w);
    o.put('x');
    o.num(e.h);
    o.str(" (");
    if (e.kind == k_stock) {
        o.str("classic");
    } else {
        const double sx = dw > 0 ? (double)dw / e.w : 1.0, sy = dh > 0 ? (double)dh / e.h : 1.0;
        const int    s100 = (int)((sx < sy ? sx : sy) * 100.0 + 0.5);
        if (e.kind == k_common) { // RES24: a fixed common size is labelled by its aspect, not by a scale
            o.str(aspect_group(e.w, e.h) == grp_4x3 ? "4:3" : aspect_group(e.w, e.h) == grp_16x9 ? "16:9"
                                                                                                 : "other");
            o.put(')');
            if (cap > 0) out[o.n < cap ? o.n : cap - 1] = 0;
            return o.n;
        }
        if (e.kind == k_4x3) o.str("4:3, ");
        scale_text(o, s100);
        if (e.kind == k_native && s100 >= 97 && s100 <= 103) o.str(", sharpest");
    }
    o.put(')');
    if (cap > 0) out[o.n < cap ? o.n : cap - 1] = 0;
    return o.n;
}

} // namespace mh::gfx::reslist
