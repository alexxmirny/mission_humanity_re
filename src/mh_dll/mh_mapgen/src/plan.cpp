// mh_mapgen -- the macro plan: terrain regions and ridge lines on a (N/2)^2 torus lattice, landing spots, clearings, ramps,
// guaranteed paths between the spots, and the repair loop that edits the plan until every adjacent macro pair is tileable.
//
// A lattice point is the shared corner of four macros; a macro (R, C) is the 2x2 tile block whose corners are the points
// (R, C), (R, C+1), (R+1, C), (R+1, C+1). Everything here is integer-only and deterministic.
#include "gen_internal.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <queue>
#include <set>
#include <tuple>

namespace mh_mapgen::detail {

namespace {

using Mask = std::vector<std::uint8_t>;

// Lattice points (1 point = 2 tiles) around a landing spot that stay plain ground: the AI core is 9x9 tiles, stock puts rocks and
// walls 4-7 tiles off the spot, so the keep area is small and the clearing is the macros of the core only.
constexpr int KEEP_M = 3;

// Is class `k` the class `c` itself or one of its ancestors in the nesting tree?
bool is_anc(const Model &mdl, int k, int c) {
    for (int x = c; x >= 0; x = mdl.parent[std::size_t(x)])
        if (x == k) return true;
    return false;
}

constexpr int N8[8][2] = {{-1, -1}, {-1, 0}, {-1, 1}, {0, -1}, {0, 1}, {1, -1}, {1, 0}, {1, 1}};

// numpy.roll over both axes: out[r][c] = a[r - dr][c - dc].
Mask roll(const Mask &a, int M, int dr, int dc) {
    Mask o(a.size());
    for (int r = 0; r < M; ++r)
        for (int c = 0; c < M; ++c) o[std::size_t(r * M + c)] = a[std::size_t(pmod(r - dr, M) * M + pmod(c - dc, M))];
    return o;
}
Mask band_and(const Mask &a, const Mask &b) {
    Mask o(a.size());
    for (std::size_t i = 0; i < a.size(); ++i) o[i] = a[i] & b[i];
    return o;
}
Mask bor(const Mask &a, const Mask &b) {
    Mask o(a.size());
    for (std::size_t i = 0; i < a.size(); ++i) o[i] = a[i] | b[i];
    return o;
}
Mask bnot(const Mask &a) {
    Mask o(a.size());
    for (std::size_t i = 0; i < a.size(); ++i) o[i] = a[i] ? 0 : 1;
    return o;
}
bool any_of_mask(const Mask &a) {
    for (std::uint8_t v : a)
        if (v) return true;
    return false;
}

// ---- value noise (exact port of the prototype; int64 fixed point) -----------------------------------------------------------
std::vector<std::int64_t> noise(int M, std::uint64_t seed, int wl, int octaves) {
    std::vector<std::int64_t> out(std::size_t(M) * M, 0);
    std::int64_t              amp = std::int64_t(1) << 12;
    for (int o = 0; o < octaves; ++o) {
        const int                 s = std::max(1, wl >> o);
        const int                 g = (M + s - 1) / s; // a wavelength that does not divide M leaves a mild seam step instead of reading past the lattice
        std::vector<std::int64_t> lat(std::size_t(g) * g);
        for (int i = 0; i < g; ++i)
            for (int j = 0; j < g; ++j) lat[std::size_t(i * g + j)] = std::int64_t(h64({seed, std::uint64_t(o), std::uint64_t(i), std::uint64_t(j)}) & 0xFFFF);
        const std::size_t         ms = std::size_t(M);
        std::vector<std::int64_t> t(ms);
        std::vector<int>          i0(ms), i1(ms);
        for (int k = 0; k < M; ++k) {
            std::int64_t tt    = ((std::int64_t(k % s)) << 8) / s;
            t[std::size_t(k)]  = (tt * tt * (768 - 2 * tt)) >> 16;
            i0[std::size_t(k)] = k / s;
            i1[std::size_t(k)] = (k / s + 1) % g;
        }
        for (int y = 0; y < M; ++y)
            for (int x = 0; x < M; ++x) {
                const std::int64_t a = lat[std::size_t(i0[std::size_t(y)] * g + i0[std::size_t(x)])], b = lat[std::size_t(i0[std::size_t(y)] * g + i1[std::size_t(x)])];
                const std::int64_t c = lat[std::size_t(i1[std::size_t(y)] * g + i0[std::size_t(x)])], d = lat[std::size_t(i1[std::size_t(y)] * g + i1[std::size_t(x)])];
                const std::int64_t tx = t[std::size_t(x)], ty = t[std::size_t(y)];
                const std::int64_t top = a * (256 - tx) + b * tx, bot = c * (256 - tx) + d * tx;
                const std::int64_t v = (top * (256 - ty) + bot * ty) >> 16; // 0..65535
                out[std::size_t(y * M + x)] += (v * amp) >> 12;
            }
        amp >>= 1;
        if (s == 1) break;
    }
    return out;
}

// Threshold such that `share_ppm` of the points under `mask` are >= it (exact integer quantile). false when nothing to threshold.
bool quant(const std::vector<std::int64_t> &f, const Mask &mask, std::int64_t share_ppm, std::int64_t &thr) {
    std::vector<std::int64_t> v;
    for (std::size_t i = 0; i < f.size(); ++i)
        if (mask[i]) v.push_back(f[i]);
    if (v.empty() || share_ppm <= 0) return false;
    std::sort(v.begin(), v.end());
    const std::int64_t len = std::int64_t(v.size());
    std::int64_t       k   = len - len * share_ppm / 1000000;
    k                      = std::max<std::int64_t>(0, std::min<std::int64_t>(len - 1, k));
    thr                    = v[std::size_t(k)];
    return true;
}

Mask majority(Mask b, int M, int passes) {
    for (int p = 0; p < passes; ++p) {
        std::vector<std::int32_t> s(b.size(), 0);
        for (const auto &d : N8) {
            const Mask rl = roll(b, M, d[0], d[1]);
            for (std::size_t i = 0; i < s.size(); ++i) s[i] += rl[i];
        }
        for (std::size_t i = 0; i < b.size(); ++i) b[i] = s[i] >= 5 ? 1 : (s[i] <= 3 ? 0 : b[i]);
    }
    return b;
}

// Keep points covered by some all-set 2x2 lattice block (minimum feature = 2 points = 4 tiles).
Mask opening(const Mask &b, int M) {
    const Mask r0 = roll(b, M, 0, -1), r1 = roll(b, M, -1, 0), r2 = roll(b, M, -1, -1);
    Mask       blk(b.size());
    for (std::size_t i = 0; i < b.size(); ++i) blk[i] = b[i] & r0[i] & r1[i] & r2[i];
    return bor(bor(blk, roll(blk, M, 0, 1)), bor(roll(blk, M, 1, 0), roll(blk, M, 1, 1)));
}

// Remove 4-connected components (torus) smaller than minsz.
Mask small_comps(const Mask &b, int M, int minsz) {
    Mask                      out = b, seen(b.size(), 0);
    std::vector<std::int32_t> comp;
    for (int r = 0; r < M; ++r)
        for (int c = 0; c < M; ++c) {
            const std::size_t i0 = std::size_t(r * M + c);
            if (!b[i0] || seen[i0]) continue;
            comp.clear();
            comp.push_back(std::int32_t(i0));
            seen[i0] = 1;
            for (std::size_t h = 0; h < comp.size(); ++h) {
                const int y = comp[h] / M, x = comp[h] % M;
                const int ny[4] = {pmod(y + 1, M), pmod(y - 1, M), y, y}, nx[4] = {x, x, pmod(x + 1, M), pmod(x - 1, M)};
                for (int k = 0; k < 4; ++k) {
                    const std::size_t j = std::size_t(ny[k] * M + nx[k]);
                    if (b[j] && !seen[j]) {
                        seen[j] = 1;
                        comp.push_back(std::int32_t(j));
                    }
                }
            }
            if (int(comp.size()) < minsz)
                for (std::int32_t j : comp) out[std::size_t(j)] = 0;
        }
    return out;
}

// Kill 2x2 diagonal patterns (no shipped macro realises them).
Mask fix_saddles(Mask b, int M) {
    for (int it = 0; it < 8; ++it) {
        const Mask a = b, r = roll(a, M, 0, -1), d = roll(a, M, -1, 0), rd = roll(a, M, -1, -1);
        Mask       s1(a.size()), s2(a.size());
        bool       any = false;
        for (std::size_t i = 0; i < a.size(); ++i) {
            s1[i] = a[i] & rd[i] & !r[i] & !d[i];
            s2[i] = r[i] & d[i] & !a[i] & !rd[i];
            any   = any || s1[i] || s2[i];
        }
        if (!any) return b;
        const Mask t1 = roll(s1, M, 1, 1), t2 = roll(s2, M, 0, 1), t3 = roll(s2, M, 1, 0);
        for (std::size_t i = 0; i < b.size(); ++i)
            if (s1[i] || t1[i] || t2[i] || t3[i]) b[i] = 0;
    }
    return b;
}

Mask disc(int M, std::pair<int, int> centre, int r) {
    Mask m(std::size_t(M) * M, 0);
    for (int dr = -r; dr <= r; ++dr)
        for (int dc = -r; dc <= r; ++dc)
            if (dr * dr + dc * dc <= r * r + r) m[std::size_t(pmod(centre.first + dr, M) * M + pmod(centre.second + dc, M))] = 1;
    return m;
}

// Chebyshev square of radius r around a lattice point.
Mask square(int M, std::pair<int, int> centre, int r) {
    Mask m(std::size_t(M) * M, 0);
    for (int dr = -r; dr <= r; ++dr)
        for (int dc = -r; dc <= r; ++dc) m[std::size_t(pmod(centre.first + dr, M) * M + pmod(centre.second + dc, M))] = 1;
    return m;
}

// ---- terrain fields -----------------------------------------------------------------------------------------------
// region classes in nesting order. keep = clearing points: 0 free, else 1 + the class the clearing's core must be.
Mask region_plan(const Model &mdl, int M, std::uint64_t seed, const Mask &keep) {
    const int bg = mdl.bg;
    Mask      cls(std::size_t(M) * M, std::uint8_t(bg));
    const int wl        = std::max(8, std::min(M, 64) / mdl.tune.reg_div);
    int       big_child = -1; // the largest child of the background takes the contour bands
    if (mdl.tune.contours) {
        std::int64_t best = -1;
        for (int j : mdl.order)
            if (mdl.parent[std::size_t(j)] == bg && mdl.sub_ppm[std::size_t(j)] > best) {
                best      = mdl.sub_ppm[std::size_t(j)];
                big_child = j;
            }
    }
    for (std::size_t oi = 1; oi < mdl.order.size(); ++oi) {
        const int k = mdl.order[oi], p = mdl.parent[std::size_t(k)];
        Mask      inside(cls.size()), is_p(cls.size());
        for (std::size_t i = 0; i < cls.size(); ++i) {
            is_p[i]   = cls[i] == p;
            inside[i] = is_p[i] && keep[i] == 0;
        }
        if (p != bg)
            for (const auto &d : N8) {
                const Mask rl = roll(cls, M, d[0], d[1]);
                for (std::size_t i = 0; i < cls.size(); ++i) inside[i] = inside[i] && rl[i] == p;
            }
        const int                       fwl = p == bg ? wl : std::max(8, wl / 2);
        const std::vector<std::int64_t> f   = k == big_child ? std::vector<std::int64_t>() : noise(M, seed * 131 + std::uint64_t(k), fwl, 5);
        std::int64_t                    rel = mdl.sub_ppm[std::size_t(k)] * 1000000 / std::max<std::int64_t>(1, mdl.sub_ppm[std::size_t(p)]);
        if (p == bg) rel = mdl.sub_ppm[std::size_t(k)];
        // threshold at `share_ppm` of the parent, then clean the field up (smooth, drop specks and pinholes, no saddles, keep
        // non-touching siblings apart). The cleanup eats area, so with `fit` the share is re-aimed until the result matches the
        // learned one.
        // the clearing points that belong to k (their core is k or a descendant of k) are part of every cleanup step: the smoothing
        // would shave the corners of the square and leave 1-point stubs that no macro can realise
        Mask kk(cls.size(), 0);
        for (std::size_t i = 0; i < kk.size(); ++i) kk[i] = keep[i] && is_p[i] && is_anc(mdl, k, keep[i] - 1);
        auto finish = [&](Mask &b) {
            b = bor(b, kk);
            if (mdl.tune.maj > 0) b = bor(band_and(majority(b, M, mdl.tune.maj), bor(inside, kk)), kk);
            b          = bor(band_and(opening(b, M), bor(inside, kk)), kk);
            b          = bor(small_comps(b, M, mdl.tune.minc), kk);
            Mask holes = band_and(bnot(b), inside);
            b          = bor(b, band_and(holes, bnot(small_comps(holes, M, 6))));
            b          = bor(fix_saddles(band_and(opening(b, M), bor(inside, kk)), M), kk);
            for (int j : mdl.order) {
                if (j == k || j == p || mdl.parent[std::size_t(j)] != p || (mdl.adjm[std::size_t(k)] >> j & 1)) continue;
                Mask near_j(cls.size(), 0);
                for (const auto &d : N8) {
                    const Mask rl = roll(cls, M, d[0], d[1]);
                    for (std::size_t i = 0; i < near_j.size(); ++i) near_j[i] = near_j[i] | (rl[i] == j);
                }
                for (std::size_t i = 0; i < near_j.size(); ++i) near_j[i] = near_j[i] | (cls[i] == j);
                for (std::size_t i = 0; i < b.size(); ++i) b[i] = (b[i] && !near_j[i]) || kk[i];
            }
            b = bor(fix_saddles(small_comps(opening(b, M), M, mdl.tune.minc), M), kk);
        };
        auto build = [&](std::int64_t share_ppm, Mask &b) {
            std::int64_t thr = 0;
            if (!quant(f, is_p, share_ppm, thr)) return false;
            b.assign(cls.size(), 0);
            for (std::size_t i = 0; i < b.size(); ++i) b[i] = inside[i] && f[i] >= thr;
            finish(b);
            return true;
        };
        Mask b;
        if (k == big_child) {
            // rank-uniform band field: `levels` nested bands whose total area is the learned share (rank noise has no cluster-size luck)
            const std::vector<std::int64_t> g = noise(M, seed * 131 + std::uint64_t(k) + 17, std::max(8, std::min(M, 64) / mdl.tune.contour_div), mdl.tune.contour_oct);
            std::vector<std::int32_t>       idx(g.size()), rank(g.size());
            for (std::size_t i = 0; i < idx.size(); ++i) idx[i] = std::int32_t(i);
            std::sort(idx.begin(), idx.end(), [&](std::int32_t x, std::int32_t y) { return g[std::size_t(x)] != g[std::size_t(y)] ? g[std::size_t(x)] < g[std::size_t(y)] : x < y; });
            for (std::size_t i = 0; i < idx.size(); ++i) rank[std::size_t(idx[i])] = std::int32_t(i);
            const std::int64_t size = std::int64_t(g.size());
            // cshare scales the share (the cleanup, the forced clearings and the repair eat area); the bigger the map the less of it
            // is eaten (the bands stay the same size), so the surplus fades with M past 64. cfit re-aims it like the fit branch does
            const std::int64_t cs   = 100 + std::int64_t(mdl.tune.cshare - 100) * std::min(M, 64) / M;
            const std::int64_t want = std::min<std::int64_t>(950000, rel * cs / 100);
            std::int64_t       q    = want;
            for (int it = 0; it < (mdl.tune.cfit ? 6 : 1); ++it) {
                b.assign(cls.size(), 0);
                for (std::size_t i = 0; i < b.size(); ++i) b[i] = inside[i] && ((std::int64_t(rank[i]) * mdl.tune.levels) % size) * 1000000 >= (1000000 - q) * size;
                finish(b);
                std::int64_t got = 0;
                for (std::uint8_t v : b) got += v;
                const std::int64_t target = size * want / 1000000;
                if (std::abs(got - target) * 20 <= target) break;
                q = std::max<std::int64_t>(20000, std::min<std::int64_t>(950000, got > 0 ? q * target / got : q * 2));
            }
        } else if (!mdl.tune.fit) {
            if (!build(std::min<std::int64_t>(950000, p == bg ? rel : rel * 13 / 10), b)) continue;
        } else {
            std::int64_t points = 0;
            for (std::uint8_t v : is_p) points += v;
            const std::int64_t target = points * std::min<std::int64_t>(950000, rel) / 1000000;
            std::int64_t       share  = std::min<std::int64_t>(950000, rel);
            for (int it = 0; it < 6; ++it) {
                if (!build(share, b)) break;
                std::int64_t got = 0;
                for (std::uint8_t v : b) got += v;
                if (target <= 0 || std::abs(got - target) * 20 <= target) break;
                share = std::max<std::int64_t>(20000, std::min<std::int64_t>(950000, got > 0 ? share * target / got : share * 2));
            }
            if (b.empty()) continue;
        }
        if (b.empty()) b.assign(cls.size(), 0);
        for (std::size_t i = 0; i < b.size(); ++i) // a kept point holds the class of its core: it joins k when k is that class or an ancestor of it
            if (keep[i] && is_p[i]) b[i] = is_anc(mdl, k, keep[i] - 1) ? 1 : 0;
        for (std::size_t i = 0; i < b.size(); ++i)
            if (b[i]) cls[i] = std::uint8_t(k);
    }
    return cls;
}

Mask line_plan(const Model &mdl, const Mask &cls, int M, std::uint64_t seed) {
    const int bg = mdl.bg;
    Mask      ok(cls.size());
    for (std::size_t i = 0; i < ok.size(); ++i) ok[i] = cls[i] == bg;
    for (int dr = -2; dr <= 2; ++dr)
        for (int dc = -2; dc <= 2; ++dc) {
            const Mask rl = roll(cls, M, dr, dc);
            for (std::size_t i = 0; i < ok.size(); ++i) ok[i] = ok[i] && rl[i] == bg;
        }
    const std::vector<std::int64_t> f   = noise(M, seed * 977 + 5, std::max(4, std::min(M, 64) / mdl.tune.line_div), 4);
    std::int64_t                    thr = 0;
    quant(f, Mask(f.size(), 1), std::int64_t(mdl.tune.line_q) * 10000, thr);
    Mask b(f.size());
    for (std::size_t i = 0; i < b.size(); ++i) b[i] = f[i] >= thr;
    b          = majority(b, M, 4);
    b          = band_and(b, ok);
    b          = band_and(opening(b, M), ok);
    b          = small_comps(b, M, 12);
    Mask holes = band_and(bnot(b), ok);
    holes      = bor(opening(holes, M), bnot(ok));
    b          = band_and(bnot(holes), ok);
    return fix_saddles(b, M);
}

// Open points in the (2r+1)^2 square round every lattice point (torus): the room a base has to build in, the 15-tile build-fit radius.
constexpr int             WIDE_R = 7;
std::vector<std::int32_t> box_sum(const Mask &a, int M, int r) {
    std::vector<std::int32_t> h(a.size(), 0), o(a.size(), 0);
    for (int y = 0; y < M; ++y)
        for (int x = 0; x < M; ++x) {
            std::int32_t t = 0;
            for (int d = -r; d <= r; ++d) t += a[std::size_t(y * M + pmod(x + d, M))];
            h[std::size_t(y * M + x)] = t;
        }
    for (int y = 0; y < M; ++y)
        for (int x = 0; x < M; ++x) {
            std::int32_t t = 0;
            for (int d = -r; d <= r; ++d) t += h[std::size_t(pmod(y + d, M) * M + x)];
            o[std::size_t(y * M + x)] = t;
        }
    return o;
}

// ---- landing spots (lattice points) ------------------------------------------------------------------------------------
std::vector<std::pair<int, int>> base_spots(int N, int P, std::uint64_t seed) {
    const int                        M = N / 2, q1 = M / 4, q3 = 3 * M / 4;
    std::vector<std::pair<int, int>> base;
    if (P == 2) base = {{q1, q1}, {q3, q3}};
    else if (P == 4) base = {{q1, q1}, {q3, q3}, {q1, q3}, {q3, q1}};
    else
        for (int i = 0; i < P / 2; ++i) {
            const int a = (M * i) / (P / 2);
            base.push_back({q1, (a + M / 8) % M});
            base.push_back({q3, (a + M / 8 + M / 2) % M});
        }
    const int                        j = std::max(1, M / 16);
    std::vector<std::pair<int, int>> out;
    for (int i = 0; i < P; ++i) {
        const int r = int(h64({seed, 11, std::uint64_t(i)}) % std::uint64_t(2 * j + 1)) - j;
        const int c = int(h64({seed, 12, std::uint64_t(i)}) % std::uint64_t(2 * j + 1)) - j;
        out.push_back({pmod(base[std::size_t(i)].first + r, M), pmod(base[std::size_t(i)].second + c, M)});
    }
    return out;
}

// Terrain-aware: plan regions without clearings, then move each base point (window +-M/8) to the lattice point whose clearing
// disc covers the most open ground. Spots stay >= 3 points (6 tiles) from the map border: the mother footprint must not wrap.
// With tune.spot_mix some spots (whole opposite pairs) sit on the largest child of the background instead (ruins dirt, diuna
// plateau top, metal dark plate); `core` returns each spot's core class.
std::vector<std::pair<int, int>> place_spots(const Model &mdl, int N, int P, std::uint64_t seed, std::vector<std::uint8_t> &core) {
    const int  M    = N / 2;
    const auto base = base_spots(N, P, seed);
    Mask       cls0 = region_plan(mdl, M, seed, Mask(std::size_t(M) * M, 0));
    Mask       lineok(cls0.size(), 1);
    if (mdl.line_ppm > 15000 && !mdl.tune.no_lines) {
        const Mask l0 = line_plan(mdl, cls0, M, seed);
        for (const auto &d : N8) {
            const Mask rl = roll(l0, M, d[0], d[1]);
            for (std::size_t i = 0; i < lineok.size(); ++i) lineok[i] = lineok[i] && rl[i] == l0[i];
        }
    }
    int child = -1; // the largest child of the background
    {
        std::int64_t best = -1;
        for (int j : mdl.order)
            if (mdl.parent[std::size_t(j)] == mdl.bg && mdl.sub_ppm[std::size_t(j)] > best) {
                best  = mdl.sub_ppm[std::size_t(j)];
                child = j;
            }
    }
    std::vector<int> cands{mdl.bg};
    if (mdl.tune.spot_mix > 0 && child >= 0) cands.push_back(child);
    // pairs (2k, 2k+1) are opposite on the map: the first round(P*spot_mix/200) pairs by hash want the child class
    const int                                  npairs = std::min(P / 2, (P * mdl.tune.spot_mix + 100) / 200);
    std::vector<std::pair<std::uint64_t, int>> pord;
    for (int k = 0; k < P / 2; ++k) pord.push_back({h64({seed, 14, std::uint64_t(k)}), k});
    std::sort(pord.begin(), pord.end());
    std::vector<std::uint8_t> pair_child(std::size_t(P / 2 + 1), 0);
    if (cands.size() > 1)
        for (int k = 0; k < npairs; ++k) pair_child[std::size_t(pord[std::size_t(k)].second)] = 1;
    // per candidate class: score = open points inside the keep disc (want all); ring = non-open points 1..3 further out (want
    // some: stock terrain sits 5-7 tiles off a base), capped at 35% of the ring so a spot never trades its clearing for scenery
    const int                              r = KEEP_M;
    std::vector<std::vector<std::int32_t>> score_c, ring_c, wide_c;
    for (int cc : cands) {
        Mask open_(cls0.size());
        for (std::size_t i = 0; i < open_.size(); ++i) open_[i] = cls0[i] == cc && lineok[i];
        std::vector<std::int32_t> score(open_.size(), 0), ring(open_.size(), 0);
        std::int32_t              nring = 0;
        for (int dr = -r - 3; dr <= r + 3; ++dr)
            for (int dc = -r - 3; dc <= r + 3; ++dc) {
                const int d2 = dr * dr + dc * dc;
                if (cc != mdl.bg) { // child ground: the square of the clearing plus its margin must already be child ground, or the plateau is an island with straight walls the tiles cannot realise
                    if (std::abs(dr) <= r + mdl.tune.child_margin && std::abs(dc) <= r + mdl.tune.child_margin) {
                        const Mask rl = roll(open_, M, dr, dc);
                        for (std::size_t i = 0; i < score.size(); ++i) score[i] += rl[i];
                    }
                } else if (d2 <= r * r + r) {
                    const Mask rl = roll(open_, M, dr, dc);
                    for (std::size_t i = 0; i < score.size(); ++i) score[i] += rl[i];
                } else if (d2 <= (r + 3) * (r + 3) + r + 3) {
                    const Mask rl = roll(open_, M, dr, dc);
                    for (std::size_t i = 0; i < ring.size(); ++i) ring[i] += rl[i] ? 0 : 1;
                    ++nring;
                }
            }
        for (auto &v : ring) v = std::min(v, nring * 35 / 100);
        score_c.push_back(std::move(score));
        ring_c.push_back(std::move(ring));
        wide_c.push_back(box_sum(open_, M, WIDE_R));
    }
    const int                        w        = std::max(2, M / 8);
    const std::int64_t               wide_cap = std::int64_t(2 * WIDE_R + 1) * (2 * WIDE_R + 1) * mdl.tune.wide_cap / 100;
    std::vector<std::pair<int, int>> out;
    core.clear();
    for (int i = 0; i < P; ++i) {
        const auto [R0, C0]                                                                                 = base[std::size_t(i)];
        bool                                                                                           have = false;
        std::tuple<std::int64_t, std::int64_t, std::int64_t, std::int64_t, std::int64_t, std::int64_t> best{};
        std::pair<int, int>                                                                            bp{R0, C0};
        int                                                                                            bcls = mdl.bg;
        const int                                                                                      want = pair_child[std::size_t(i / 2)] ? cands[1] : mdl.bg;
        for (int dr = -w; dr <= w; ++dr)
            for (int dc = -w; dc <= w; ++dc) {
                const std::pair<int, int> p{pmod(R0 + dr, M), pmod(C0 + dc, M)};
                if (p.first < 3 || p.first > M - 4 || p.second < 3 || p.second > M - 4) continue;
                bool near_prev = false;
                for (const auto &q : out) {
                    const int a = std::abs(p.first - q.first), b = std::abs(p.second - q.second);
                    if (std::max(std::min(a, M - a), std::min(b, M - b)) < N / 12 + 1) near_prev = true;
                }
                if (near_prev) continue;
                const std::size_t pi = std::size_t(p.first * M + p.second);
                for (std::size_t ci = 0; ci < cands.size(); ++ci) {
                    // a plateau patch cut by the wrap seam is walled in without it (the connectivity gate does not wrap): child spots keep their whole room inside the map
                    if (ci > 0 && (p.first < WIDE_R || p.first > M - 1 - WIDE_R || p.second < WIDE_R || p.second > M - 1 - WIDE_R)) continue;
                    if (ci > 0 && (cls0[pi] != cands[ci] || score_c[ci][pi] * 100 < mdl.tune.child_pct * (2 * (r + mdl.tune.child_margin) + 1) * (2 * (r + mdl.tune.child_margin) + 1))) continue; // a child spot needs child ground all round
                    const std::tuple<std::int64_t, std::int64_t, std::int64_t, std::int64_t, std::int64_t, std::int64_t> k{cands[ci] == want ? 1 : 0, std::min<std::int64_t>(wide_c[ci][pi], wide_cap), score_c[ci][pi], ring_c[ci][pi], -(dr * dr + dc * dc), std::int64_t(h64({seed, 13, std::uint64_t(i), u64(dr), u64(dc)}) % 1000)};
                    if (!have || k > best) {
                        have = true;
                        best = k;
                        bp   = p;
                        bcls = cands[ci];
                    }
                }
            }
        // no child ground in the window: the spot stays on the background (a forced plateau island fails the tile pass)
        out.push_back(bp);
        core.push_back(std::uint8_t(bcls));
    }
    return out;
}

// ---- paths -------------------------------------------------------------------------------------------------------------
enum Act : std::uint8_t { ACT_NONE = 0,
                          ACT_RAMP,
                          ACT_CUT,
                          ACT_CARVE };

void macro_cost(const Model &mdl, std::uint32_t key, std::int32_t &cost, std::uint8_t &act) {
    const int kind = key_tag(key) >> 4, bits = key_tag(key) & 15;
    act  = ACT_NONE;
    cost = 1;
    if (kind == TAG_CLEAR || kind == TAG_RAMP) return;
    if (kind == TAG_MIXED || kind == TAG_CLIFF) {
        if (mdl.ramp_sig[key_sig(key)]) {
            cost = 8;
            act  = ACT_RAMP;
        } else {
            cost = 60;
            act  = ACT_CARVE;
        }
        return;
    }
    if (kind == TAG_ORIENT || bits != 0) {
        cost = 25;
        act  = ACT_CUT;
        return;
    }
    if (key_cls(key, 0) != mdl.bg && !mdl.pass_class[std::size_t(key_cls(key, 0))]) {
        cost = 60;
        act  = ACT_CARVE;
    }
}

void apply_action(const Model &mdl, Plan &pl, const std::vector<std::uint32_t> &keys, std::uint8_t act, int R, int C) {
    const int M = pl.M;
    if (act == ACT_RAMP) {
        for (int dR = -1; dR <= 1; ++dR)
            for (int dC = -1; dC <= 1; ++dC) {
                const std::size_t w    = std::size_t(pmod(R + dR, M) * M + pmod(C + dC, M));
                const int         kind = key_tag(keys[w]) >> 4;
                if ((kind == TAG_MIXED || kind == TAG_CLIFF) && mdl.ramp_sig[key_sig(keys[w])] && !pl.ramp_ban[w]) pl.ramps[w] = 1;
            }
    } else if (act == ACT_CUT) {
        pl.hs[std::size_t(R * M + C)]              = 1;
        pl.hs[std::size_t(pmod(R + 1, M) * M + C)] = 1;
        pl.vs[std::size_t(R * M + C)]              = 1;
        pl.vs[std::size_t(R * M + pmod(C + 1, M))] = 1;
    } else if (act == ACT_CARVE) {
        for (int dR = -1; dR <= 2; ++dR)
            for (int dC = -1; dC <= 2; ++dC) pl.cls[std::size_t(pmod(R + dR, M) * M + pmod(C + dC, M))] = std::uint8_t(mdl.bg);
    }
}

constexpr std::int32_t INF = 1 << 30;
using Heap                 = std::priority_queue<std::tuple<std::int32_t, int, int>, std::vector<std::tuple<std::int32_t, int, int>>, std::greater<>>;

struct Tree {
    std::vector<std::int32_t> dist, prev;
};

Tree dijkstra(const std::vector<std::int32_t> &cost, int M, std::pair<int, int> src) {
    Tree t;
    t.dist.assign(cost.size(), INF);
    t.prev.assign(cost.size(), -1);
    Heap h;
    t.dist[std::size_t(src.first * M + src.second)] = 0;
    h.push({0, src.first, src.second});
    while (!h.empty()) {
        const auto [dd, R, C] = h.top();
        h.pop();
        if (dd > t.dist[std::size_t(R * M + C)]) continue;
        static const int dRC[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
        for (const auto &d : dRC) {
            const int vr = R + d[0], vc = C + d[1];
            if (vr < 0 || vr >= M || vc < 0 || vc >= M) continue;
            const std::int32_t nd = dd + cost[std::size_t(vr * M + vc)];
            if (nd < t.dist[std::size_t(vr * M + vc)]) {
                t.dist[std::size_t(vr * M + vc)] = nd;
                t.prev[std::size_t(vr * M + vc)] = R * M + C;
                h.push({nd, vr, vc});
            }
        }
    }
    return t;
}

void cost_arrays(const Model &mdl, const Plan &pl, std::vector<std::int32_t> &cost, std::vector<std::uint8_t> &act) {
    cost.assign(pl.key.size(), 1);
    act.assign(pl.key.size(), ACT_NONE);
    for (std::size_t i = 0; i < pl.key.size(); ++i) {
        macro_cost(mdl, pl.key[i], cost[i], act[i]);
        if (act[i] == ACT_RAMP && pl.ramp_ban[i]) { // a ramp the tiles refused here: carve the cliff instead
            cost[i] = 60;
            act[i]  = ACT_CARVE;
        }
    }
}

std::uint32_t plan_paths(const Model &mdl, Plan &pl, const std::vector<std::pair<int, int>> &spots) {
    const int                 M = pl.M, P = int(spots.size());
    std::vector<std::int32_t> cost;
    std::vector<std::uint8_t> act;
    cost_arrays(mdl, pl, cost, act);
    const std::vector<std::uint32_t> keys = pl.key; // snapshot: actions are decided on the keys the paths were planned on
    std::vector<Tree>                trees;
    for (const auto &s : spots) trees.push_back(dijkstra(cost, M, s));
    std::vector<std::uint8_t> done(std::size_t(P), 0);
    done[0]               = 1;
    std::uint32_t changed = 0;
    for (int cnt = 1; cnt < P; ++cnt) {
        std::tuple<std::int32_t, int, int> best{INF, 0, 0};
        bool                               have = false;
        for (int i = 0; i < P; ++i) {
            if (!done[std::size_t(i)]) continue;
            for (int j = 0; j < P; ++j) {
                if (done[std::size_t(j)]) continue;
                const std::tuple<std::int32_t, int, int> k{trees[std::size_t(i)].dist[std::size_t(spots[std::size_t(j)].first * M + spots[std::size_t(j)].second)], i, j};
                if (!have || k < best) {
                    have = true;
                    best = k;
                }
            }
        }
        const int i = std::get<1>(best), j = std::get<2>(best);
        done[std::size_t(j)] = 1;
        const Tree        &t = trees[std::size_t(i)];
        std::int32_t       v = spots[std::size_t(j)].first * M + spots[std::size_t(j)].second;
        const std::int32_t s = spots[std::size_t(i)].first * M + spots[std::size_t(i)].second;
        while (v != s && v >= 0) {
            const std::uint8_t a = act[std::size_t(v)];
            if (a != ACT_NONE) {
                apply_action(mdl, pl, keys, a, v / M, v % M);
                ++changed;
            }
            v = t.prev[std::size_t(v)];
        }
    }
    return changed;
}

// Make every walkable macro component of >= min_comp macros reachable from spot 0 (torus).
std::uint32_t plan_connect(const Model &mdl, Plan &pl, const std::vector<std::pair<int, int>> &spots, int min_comp = 6, int rounds = 40) {
    const int     M = pl.M;
    std::uint32_t n = 0;
    for (int round = 0; round < rounds; ++round) {
        pl.refresh_all();
        std::vector<std::int32_t> cost;
        std::vector<std::uint8_t> act;
        cost_arrays(mdl, pl, cost, act);
        std::vector<std::int32_t> comp(cost.size(), -1), sizes;
        for (int R = 0; R < M; ++R)
            for (int C = 0; C < M; ++C) {
                const std::size_t i0 = std::size_t(R * M + C);
                if (cost[i0] != 1 || comp[i0] >= 0) continue;
                const std::int32_t        cid = std::int32_t(sizes.size());
                std::vector<std::int32_t> st{std::int32_t(i0)};
                comp[i0]  = cid;
                int count = 0;
                while (!st.empty()) {
                    const std::int32_t u = st.back();
                    st.pop_back();
                    ++count;
                    const int y = u / M, x = u % M;
                    const int ny[4] = {pmod(y + 1, M), pmod(y - 1, M), y, y}, nx[4] = {x, x, pmod(x + 1, M), pmod(x - 1, M)};
                    for (int k = 0; k < 4; ++k) {
                        const std::size_t j = std::size_t(ny[k] * M + nx[k]);
                        if (cost[j] == 1 && comp[j] < 0) {
                            comp[j] = cid;
                            st.push_back(std::int32_t(j));
                        }
                    }
                }
                sizes.push_back(count);
            }
        const std::int32_t main_c = comp[std::size_t(spots[0].first * M + spots[0].second)];
        std::int32_t       tgt    = -1;
        for (std::size_t i = 0; i < sizes.size(); ++i)
            if (std::int32_t(i) != main_c && sizes[i] >= min_comp) {
                tgt = std::int32_t(i);
                break;
            }
        if (tgt < 0) return n;
        std::vector<std::int32_t> dist(cost.size(), INF), prev(cost.size(), -1);
        Heap                      h;
        for (int R = 0; R < M; ++R)
            for (int C = 0; C < M; ++C)
                if (comp[std::size_t(R * M + C)] == main_c) {
                    dist[std::size_t(R * M + C)] = 0;
                    h.push({0, R, C});
                }
        std::int32_t hit = -1;
        while (!h.empty()) {
            const auto [dd, R, C] = h.top();
            h.pop();
            if (dd > dist[std::size_t(R * M + C)]) continue;
            if (comp[std::size_t(R * M + C)] == tgt) {
                hit = R * M + C;
                break;
            }
            static const int dRC[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
            for (const auto &d : dRC) {
                const int          vr = pmod(R + d[0], M), vc = pmod(C + d[1], M);
                const std::int32_t nd = dd + cost[std::size_t(vr * M + vc)];
                if (nd < dist[std::size_t(vr * M + vc)]) {
                    dist[std::size_t(vr * M + vc)] = nd;
                    prev[std::size_t(vr * M + vc)] = R * M + C;
                    h.push({nd, vr, vc});
                }
            }
        }
        if (hit < 0) return n;
        const std::vector<std::uint32_t> keys = pl.key;
        for (std::int32_t v = hit; v >= 0 && prev[std::size_t(v)] >= 0; v = prev[std::size_t(v)]) {
            if (act[std::size_t(v)] != ACT_NONE) {
                apply_action(mdl, pl, keys, act[std::size_t(v)], v / M, v % M);
                ++n;
            }
        }
    }
    return n;
}

} // namespace

// ---- Plan -------------------------------------------------------------------------------------------------------------
std::uint32_t Plan::compute_key(int R, int C) const {
    const int          R1 = (R + 1) % M, C1 = (C + 1) % M;
    const std::uint8_t c[4] = {cls[std::size_t(R * M + C)], cls[std::size_t(R * M + C1)], cls[std::size_t(R1 * M + C)], cls[std::size_t(R1 * M + C1)]};
    if (!(c[0] == c[1] && c[0] == c[2] && c[0] == c[3])) {
        if (ramps[std::size_t(R * M + C)] && mdl->find_case(make_key(c, make_tag(TAG_RAMP))) >= 0) return make_key(c, make_tag(TAG_RAMP));
        if (mdl->find_case(make_key(c, make_tag(TAG_CLIFF))) >= 0) {
            bool near_ramp = false;
            for (int a = -1; a <= 1; ++a)
                for (int b = -1; b <= 1; ++b) near_ramp = near_ramp || ramps[std::size_t(pmod(R + a, M) * M + pmod(C + b, M))];
            if (!near_ramp) return make_key(c, make_tag(TAG_CLIFF));
        }
        return make_key(c, make_tag(TAG_MIXED));
    }
    int bits = 0;
    if (has_line && c[0] == mdl->bg) {
        const int tl = line[std::size_t(R * M + C)], tr = line[std::size_t(R * M + C1)], bl = line[std::size_t(R1 * M + C)], br = line[std::size_t(R1 * M + C1)];
        const int raw    = (tl != tr ? 1 : 0) | (tr != br ? 2 : 0) | (bl != br ? 4 : 0) | (tl != bl ? 8 : 0);
        bits             = (raw & 1 && !hs[std::size_t(R * M + C)] ? 1 : 0) | (raw & 2 && !vs[std::size_t(R * M + C1)] ? 2 : 0) | (raw & 4 && !hs[std::size_t(R1 * M + C)] ? 4 : 0) | (raw & 8 && !vs[std::size_t(R * M + C)] ? 8 : 0);
        const bool ortho = bits == 5 || bits == 10 || bits == 3 || bits == 6 || bits == 12 || bits == 9;
        if (ortho && bits == raw) {
            const int o  = tl | (tr << 1) | (bl << 2) | (br << 3);
            const int oi = o ^ 15;
            if (mdl->find_case(make_key(c, make_tag(TAG_ORIENT, std::uint8_t(o)))) >= 0) return make_key(c, make_tag(TAG_ORIENT, std::uint8_t(o)));
            if (mdl->find_case(make_key(c, make_tag(TAG_ORIENT, std::uint8_t(oi)))) >= 0) return make_key(c, make_tag(TAG_ORIENT, std::uint8_t(oi)));
        }
    }
    if (c[0] == mdl->bg && bits == 0 && clear[std::size_t(R * M + C)]) return make_key(c, make_tag(TAG_CLEAR));
    return make_key(c, make_tag(TAG_PLAIN, std::uint8_t(bits)));
}

void Plan::refresh_all() {
    key.assign(std::size_t(M) * M, 0);
    info.assign(std::size_t(M) * M, 0);
    for (int R = 0; R < M; ++R)
        for (int C = 0; C < M; ++C) {
            const std::uint32_t k        = compute_key(R, C);
            key[std::size_t(R * M + C)]  = k;
            info[std::size_t(R * M + C)] = mdl->info_of(k);
        }
}

namespace {

// Is the macro pair (a, b) untileable at the quadrant-domain level? dir 0 = b right of a, 1 = b below a.
bool pair_bad(const Model &mdl, std::int32_t a, std::int32_t b, int dir) {
    const int d     = dir == 0 ? 0 : 2;
    const int qa[2] = {dir == 0 ? 1 : 2, 3}, qb[2] = {0, dir == 0 ? 2 : 1};
    for (int k = 0; k < 2; ++k) {
        const std::uint64_t *s   = mdl.info_support(a, qa[k], d);
        const std::uint64_t *dom = mdl.infos[std::size_t(b)].dom[qb[k]].data();
        bool                 hit = false;
        for (std::uint32_t w = 0; w < mdl.W && !hit; ++w) hit = (s[w] & dom[w]) != 0;
        if (!hit) return true;
    }
    return false;
}

} // namespace

bool make_plan(const Model &mdl, std::uint32_t Nu, std::uint32_t players, std::uint64_t seed, bool ensure_paths, bool protect_spots, Plan &pl, std::vector<std::pair<int, int>> &spots, PlanStats &st, std::string &why) {
    const int                 N = int(Nu), M = N / 2, P = int(players), bg = mdl.bg;
    std::vector<std::uint8_t> core;
    spots = place_spots(mdl, N, P, seed, core);
    Mask keep(std::size_t(M) * M, 0), clearm(std::size_t(M) * M, 0);
    for (std::size_t si = 0; si < spots.size(); ++si) {
        const auto &s  = spots[si];
        const Mask  kd = bor(disc(M, s, KEEP_M), square(M, s, 3)); // every corner of a core macro keeps the core's class
        for (std::size_t i = 0; i < kd.size(); ++i)
            if (kd[i]) keep[i] = std::uint8_t(core[si] + 1);
        clearm = bor(clearm, square(M, s, 2)); // the macros that cover the 9x9-tile core
    }
    pl          = Plan{};
    pl.mdl      = &mdl;
    pl.M        = M;
    pl.cls      = region_plan(mdl, M, seed, keep);
    pl.has_line = mdl.line_ppm > 15000 && !mdl.tune.no_lines;
    Mask lflat(std::size_t(M) * M, 0);
    if (pl.has_line) {
        pl.line = line_plan(mdl, pl.cls, M, seed);
        Mask flat(std::size_t(M) * M, 0); // walls stay out of the keep area: the repair loop must not reopen them
        for (const auto &s : spots) {
            const Mask         d = bor(disc(M, s, KEEP_M + 1), square(M, s, 4));
            const std::uint8_t v = pl.line[std::size_t(s.first * M + s.second)];
            for (std::size_t i = 0; i < d.size(); ++i)
                if (d[i]) pl.line[i] = v;
            flat = bor(flat, d);
        }
        lflat   = flat;
        pl.line = fix_saddles(pl.line, M);
    } else pl.line.assign(std::size_t(M) * M, 0);
    pl.clear = clearm;
    pl.ramps.assign(std::size_t(M) * M, 0);
    pl.hs.assign(std::size_t(M) * M, 0);
    pl.vs.assign(std::size_t(M) * M, 0);
    pl.protect.assign(std::size_t(M) * M, 0);
    pl.ramp_ban.assign(std::size_t(M) * M, 0);
    if (protect_spots) pl.protect = lflat;
    else // the fallback attempts give the repair more room but still never wall in the core of a base
        for (const auto &s : spots) pl.protect = bor(pl.protect, square(M, s, 3));
    pl.refresh_all();

    // noise-chosen ramps at the learned share of rampable region edges
    if (mdl.ramp_permille >= 5 && mdl.ramp_permille <= 600) {
        Mask edge(pl.key.size(), 0);
        for (std::size_t i = 0; i < edge.size(); ++i) {
            const int kind = key_tag(pl.key[i]) >> 4;
            edge[i]        = (kind == TAG_MIXED || kind == TAG_CLIFF) && mdl.ramp_sig[key_sig(pl.key[i])];
        }
        const auto   f   = noise(M, seed * 313 + 7, std::max(8, M / 8), 3);
        std::int64_t thr = 0;
        if (quant(f, edge, mdl.ramp_permille * 1000, thr))
            for (std::size_t i = 0; i < edge.size(); ++i)
                if (edge[i] && f[i] >= thr) pl.ramps[i] = 1;
    }
    if (ensure_paths && P > 1) {
        st.path_edits += plan_paths(mdl, pl, spots);
        st.path_edits += plan_connect(mdl, pl, spots);
    }
    pl.refresh_all();
    for (std::uint8_t r : pl.ramps) st.ramps += r;

    // ---- repair: edit the plan until every adjacent macro pair has a tile pair that can sit across the edge, and the
    // whole map survives the initial arc-consistency pass. Only the macros touched by an edit are re-keyed and re-checked.
    std::set<int> bad;
    std::set<int> dirty;
    auto          pair_id = [&](int R, int C, int dir) { return (R * M + C) * 2 + dir; };
    auto          check   = [&](int R, int C, int dir) {
        const int B = dir == 0 ? R * M + (C + 1) % M : ((R + 1) % M) * M + C;
        return pair_bad(mdl, pl.info[std::size_t(R * M + C)], pl.info[std::size_t(B)], dir);
    };
    for (int R = 0; R < M; ++R)
        for (int C = 0; C < M; ++C)
            for (int dir = 0; dir < 2; ++dir)
                if (check(R, C, dir)) bad.insert(pair_id(R, C, dir));
    auto touch_point = [&](int pr, int pc) {
        for (int a = -1; a <= 0; ++a)
            for (int b = -1; b <= 0; ++b) dirty.insert(pmod(pr + a, M) * M + pmod(pc + b, M));
    };
    auto touch_ramp = [&](int R, int C) {
        for (int a = -1; a <= 1; ++a)
            for (int b = -1; b <= 1; ++b) dirty.insert(pmod(R + a, M) * M + pmod(C + b, M));
    };
    std::vector<int> rekeyed;
    auto             flush = [&]() {
        rekeyed.assign(dirty.begin(), dirty.end());
        for (int m : dirty) {
            pl.key[std::size_t(m)]  = pl.compute_key(m / M, m % M);
            pl.info[std::size_t(m)] = mdl.info_of(pl.key[std::size_t(m)]);
        }
        for (int m : dirty) {
            const int R = m / M, C = m % M;
            const int ids[4][3] = {{R, C, 0}, {R, pmod(C - 1, M), 0}, {R, C, 1}, {pmod(R - 1, M), C, 1}};
            for (const auto &e : ids) {
                const int id = pair_id(e[0], e[1], e[2]);
                if (check(e[0], e[1], e[2])) bad.insert(id);
                else bad.erase(id);
            }
        }
        dirty.clear();
    };
    // tile-level arc-consistency, kept up to date across edits
    TileState          ts(mdl, pl, N);
    int                tbr = 0, tbc = 0;
    bool               tile_ok = ts.init(tbr, tbc);
    int                fixes = 0, grow = 0; // grow: extra points rewritten around a bad pair that keeps coming back (an edit that only moves the contradiction)
    std::map<int, int> last_seen;           // first bad macro of a pass -> the pass that saw it last
    for (;;) {
        std::vector<std::pair<int, int>> list; // macro anchors of the pairs to fix: (a, b) flattened as two entries
        if (bad.empty()) {
            if (tile_ok) break;
            const int br = tbr / 2, bc = tbc / 2;
            list.push_back({br, bc});
            list.push_back({br, bc});
        } else {
            int cnt = 0;
            for (int id : bad) {
                if (cnt++ >= 50) break;
                const int m = id / 2, dir = id % 2, R = m / M, C = m % M;
                list.push_back({R, C});
                list.push_back(dir == 0 ? std::make_pair(R, (C + 1) % M) : std::make_pair((R + 1) % M, C));
            }
        }
        ++fixes;
        if (fixes > std::max(512, M * M / 12)) {
            why = "plan repair did not converge";
            return false;
        }
        {
            const int  key = list[0].first * M + list[0].second;
            const auto it  = last_seen.find(key);
            grow           = it != last_seen.end() && fixes - it->second <= 24 ? std::min(grow + 1, 6) : 0;
            last_seen[key] = fixes;
        }
        for (std::size_t li = 0; li < list.size(); li += 2) {
            std::set<std::pair<int, int>> pts;
            for (std::size_t e = li; e < li + 2; ++e) {
                const int R = list[e].first, C = list[e].second;
                if (pl.ramps[std::size_t(R * M + C)]) {
                    pl.ramps[std::size_t(R * M + C)]    = 0;
                    pl.ramp_ban[std::size_t(R * M + C)] = 1;
                    touch_ramp(R, C);
                }
                for (int dR = -1 - grow; dR <= 2 + grow; ++dR)
                    for (int dC = -1 - grow; dC <= 2 + grow; ++dC) pts.insert({pmod(R + dR, M), pmod(C + dC, M)});
            }
            std::set<int> kinds;
            for (const auto &p : pts) kinds.insert(pl.cls[std::size_t(p.first * M + p.second)]);
            const bool mixed = kinds.size() > 1;
            if (pl.has_line && (fixes % 3 != 0 || !mixed)) {
                int ones = 0;
                for (const auto &p : pts) ones += pl.line[std::size_t(p.first * M + p.second)] ? 1 : 0;
                std::uint8_t v = ones * 2 > int(pts.size()) ? 1 : (ones * 2 < int(pts.size()) ? 0 : std::uint8_t(h64({seed, 71, std::uint64_t(fixes), u64(list[li].first), u64(list[li].second)}) & 1)); // a tie would always shift the edge the same way: a cycle
                for (const auto &p : pts)                                                                                                                                                                // a flattened keep area wins: the edit grows it instead of leaving a boundary at its edge
                    if (pl.protect[std::size_t(p.first * M + p.second)]) {
                        v = pl.line[std::size_t(p.first * M + p.second)];
                        break;
                    }
                for (const auto &p : pts) {
                    if (pl.protect[std::size_t(p.first * M + p.second)]) continue; // the flattened keep areas stay as planned
                    pl.line[std::size_t(p.first * M + p.second)] = v;
                    touch_point(p.first, p.second);
                }
            } else if (mixed) {
                for (const auto &p : pts) {
                    std::uint8_t &k = pl.cls[std::size_t(p.first * M + p.second)];
                    if (keep[std::size_t(p.first * M + p.second)]) continue; // a clearing keeps its core class
                    if (k != bg) {
                        k = std::uint8_t(mdl.parent[k] < 0 ? bg : mdl.parent[k]);
                        touch_point(p.first, p.second);
                    }
                }
            }
        }
        flush();
        tile_ok = ts.refresh(rekeyed, tbr, tbc);
    }
    if (ensure_paths && P > 1) {
        // The repair drops the ramps the tiles refuse and can close a plateau in. Every spot that cannot walk to spot 0 gets a ramp on a
        // cliff between its ground and the walkable ground, each candidate tried against the tile pass before it stays.
        for (int tries = 0; tries < 64; ++tries) {
            std::vector<std::int32_t> cost;
            std::vector<std::uint8_t> act;
            cost_arrays(mdl, pl, cost, act);
            std::vector<std::int8_t> comp(std::size_t(M) * M, -1);
            auto                     flood = [&](std::int32_t start, std::int8_t label) {
                std::vector<std::int32_t> stk{start};
                comp[std::size_t(start)] = label;
                while (!stk.empty()) {
                    const std::int32_t u = stk.back();
                    stk.pop_back();
                    const int y = u / M, x = u % M;
                    const int ny[4] = {y + 1, y - 1, y, y}, nx[4] = {x, x, x + 1, x - 1};
                    for (int k = 0; k < 4; ++k) {
                        if (ny[k] < 0 || ny[k] >= M || nx[k] < 0 || nx[k] >= M) continue;
                        const std::size_t j = std::size_t(ny[k] * M + nx[k]);
                        if (cost[j] == 1 && comp[j] < 0) {
                            comp[j] = label;
                            stk.push_back(std::int32_t(j));
                        }
                    }
                }
            };
            flood(spots[0].first * M + spots[0].second, 0);
            int target = -1;
            for (int j = 1; j < P && target < 0; ++j)
                if (comp[std::size_t(spots[std::size_t(j)].first * M + spots[std::size_t(j)].second)] != 0) target = j;
            if (target < 0) break;
            flood(spots[std::size_t(target)].first * M + spots[std::size_t(target)].second, 1);
            std::vector<std::pair<std::uint64_t, int>> cand;
            for (int m = 0; m < M * M; ++m) {
                const int kind = key_tag(pl.key[std::size_t(m)]) >> 4;
                if ((kind != TAG_MIXED && kind != TAG_CLIFF) || !mdl.ramp_sig[key_sig(pl.key[std::size_t(m)])] || pl.ramps[std::size_t(m)] || pl.ramp_ban[std::size_t(m)]) continue;
                const int y = m / M, x = m % M;
                const int ny[4] = {y + 1, y - 1, y, y}, nx[4] = {x, x, x + 1, x - 1};
                bool      has0 = false, has1 = false;
                for (int k = 0; k < 4; ++k) {
                    if (ny[k] < 0 || ny[k] >= M || nx[k] < 0 || nx[k] >= M) continue;
                    const std::size_t j = std::size_t(ny[k] * M + nx[k]);
                    if (cost[j] != 1) continue;
                    has0 = has0 || comp[j] == 0;
                    has1 = has1 || comp[j] == 1;
                }
                if (has0 && has1) cand.push_back({h64({seed, 81, std::uint64_t(tries), std::uint64_t(m)}), m});
            }
            std::sort(cand.begin(), cand.end());
            bool opened = false;
            for (std::size_t k = 0; k < cand.size() && k < 8 && !opened; ++k) {
                const int m = cand[k].second, R = m / M, C = m % M;
                pl.ramps[std::size_t(m)] = 1;
                touch_ramp(R, C);
                flush();
                tile_ok = ts.refresh(rekeyed, tbr, tbc);
                if (tile_ok && bad.empty() && (key_tag(pl.key[std::size_t(m)]) >> 4) == TAG_RAMP) {
                    opened = true;
                    ++st.path_edits;
                    break;
                }
                pl.ramps[std::size_t(m)]    = 0;
                pl.ramp_ban[std::size_t(m)] = 1;
                touch_ramp(R, C);
                flush();
                tile_ok = ts.refresh(rekeyed, tbr, tbc);
                if (!tile_ok || !bad.empty()) {
                    why = "plan repair lost its footing while opening a ramp";
                    return false;
                }
            }
            if (!opened) break;
        }
    }
    st.repair_passes += std::uint32_t(fixes);
    return true;
}

} // namespace mh_mapgen::detail
