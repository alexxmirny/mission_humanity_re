//
// ui/text_wrap.cpp -- see ui/text_wrap.h.
//
#include "ui/text_wrap.h"

namespace mh {
namespace ui {
namespace {

struct Out {
    wchar_t *p;
    int      cap;
    int      n;
    bool     put(wchar_t c) {
        if (n >= cap - 1) return false;
        p[n++] = c;
        return true;
    }
    void put_run(const wchar_t *s, int k) {
        for (int i = 0; i < k; ++i) put(s[i]);
    }
};

// The largest k in [1, len] with measure(s, k) <= width (1 when not even one unit fits: progress).
int fit_units(const wchar_t *s, int len, int width, measure_fn m, void *ctx) {
    int k = 1;
    while (k < len && m(ctx, s, k + 1) <= width) ++k;
    return k;
}

} // namespace

int wrap_text(const wchar_t *in, wchar_t *out, int cap, int width, int max_lines, measure_fn m, void *ctx,
              WrapReport *rep) {
    WrapReport r = {};
    if (!out || cap <= 0) return 0;
    Out o{out, cap, 0};
    out[0] = 0;
    if (!in) return 0;
    int line_start_out  = 0; // where the current output line begins (for the ellipsis)
    r.lines             = 1;
    bool           stop = false;
    const wchar_t *p    = in;
    while (!stop) {
        const wchar_t *e = p;
        while (*e && *e != L'\n') ++e;
        const int plen = (int)(e - p);
        if (width <= 0 || max_lines <= 0 || m(ctx, p, plen) <= width) {
            o.put_run(p, plen); // fits (or no field to fit): verbatim
        } else {
            const wchar_t *ls = p;
            while (ls < e) {
                // Greedy: extend word by word while the run [ls, we) still fits.
                const wchar_t *le = ls, *we = ls;
                for (;;) {
                    while (we < e && *we == L' ') ++we;
                    while (we < e && *we != L' ') ++we;
                    if (m(ctx, ls, (int)(we - ls)) > width) break;
                    le = we;
                    if (we >= e) break;
                }
                if (le == ls) { // the first word alone is wider than the field: break inside it
                    const wchar_t *wend = ls;
                    while (wend < e && *wend != L' ') ++wend;
                    le = ls + fit_units(ls, (int)(wend - ls), width, m, ctx);
                    ++r.hard;
                }
                o.put_run(ls, (int)(le - ls));
                ls = le;
                while (ls < e && *ls == L' ') ++ls; // the break swallows its spaces
                if (ls < e) {
                    if (r.lines >= max_lines) {
                        r.truncated = true;
                        stop        = true;
                        break;
                    }
                    o.put(L'\n');
                    line_start_out = o.n;
                    ++r.lines;
                    ++r.breaks;
                }
            }
        }
        if (stop || !*e) break;
        if (r.lines >= max_lines && width > 0 && max_lines > 0) { // a kept '\n' that would overflow
            r.truncated = true;
            break;
        }
        o.put(L'\n');
        line_start_out = o.n;
        ++r.lines;
        p = e + 1;
    }
    out[o.n] = 0;
    if (r.truncated) {
        // The last visible line ends in "...": drop units (then trailing spaces) until it fits.
        int n = o.n;
        for (;;) {
            wchar_t tmp[3 + 1024];
            int     k = n - line_start_out;
            if (k > 1024) k = 1024;
            for (int i = 0; i < k; ++i) tmp[i] = out[line_start_out + i];
            tmp[k] = tmp[k + 1] = tmp[k + 2] = L'.';
            if (k == 0 || m(ctx, tmp, k + 3) <= width) break;
            --n;
            while (n > line_start_out && out[n - 1] == L' ') --n;
        }
        o.n = n;
        for (int i = 0; i < 3; ++i) o.put(L'.');
        out[o.n] = 0;
    }
    if (rep) *rep = r;
    return o.n;
}

} // namespace ui
} // namespace mh
