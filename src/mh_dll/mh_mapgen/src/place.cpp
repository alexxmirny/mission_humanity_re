// mh_mapgen -- everything laid on top of the tiles: found-object stamps, passability, trees and ore.
#include "gen_internal.h"
#include "mh_mapgen/sprite_extent.h"
#include <algorithm>
#include <cstdlib>

namespace mh_mapgen::detail {

namespace {

constexpr int DRESS_STAMPS = 3;  // small stamps (rocks, craters) pasted around each landing spot
constexpr int DRESS_SPAN   = 9;  // tiles: their offsets from the spot run -9..9
constexpr int DRESS_FAR    = 10; // tiles: the farthest cell may not lie beyond this Chebyshev distance
constexpr int DRESS_TRIES  = 200;

int near_blocks() { return 7; }
int neutral_blocks(int N) { return N <= 64 ? 2 : (N <= 128 ? 9 : 25); }

int tdist(int a, int b, int n) {
    const int d = std::abs(a - b) % n;
    return std::min(d, n - d);
}

// Would the sprite of object `kind` anchored in tile (r, c) at pixel offset (px, py) be drawn over the 5x3 mother footprint of a
// spot (or the tile ring around it)? Art extends up and left of its anchor by several tiles, so the object tile alone says nothing.
bool sprite_hits_footprint(const SpriteExtent &e, int r, int c, std::int32_t px, std::int32_t py, int sx, int sy, int N) {
    const int dc = pmod(c - sx + N / 2, N) - N / 2, dr = pmod(r - sy + N / 2, N) - N / 2; // tiles from the footprint corner, torus
    const int ax = dc * 32 + px, ay = dr * 32 + py;
    // footprint [0,5) x [0,3) tiles grown by one tile, in pixels
    return ax - e.left < 6 * 32 && ax + e.right > -32 && ay - e.up < 4 * 32 && ay + e.down > -32;
}

} // namespace

void bfs_free(const std::vector<std::uint8_t> &free, int N, int r0, int c0, std::vector<std::int32_t> &dist) {
    dist.assign(free.size(), -1);
    if (r0 < 0 || r0 >= N || c0 < 0 || c0 >= N || !free[std::size_t(r0 * N + c0)]) return;
    std::vector<std::int32_t> q;
    q.push_back(r0 * N + c0);
    dist[std::size_t(r0 * N + c0)] = 0;
    for (std::size_t h = 0; h < q.size(); ++h) {
        const int r = q[h] / N, c = q[h] % N, dd = dist[std::size_t(q[h])] + 1;
        const int nr[4] = {r + 1, r - 1, r, r}, nc[4] = {c, c, c + 1, c - 1};
        for (int k = 0; k < 4; ++k) {
            if (nr[k] < 0 || nr[k] >= N || nc[k] < 0 || nc[k] >= N) continue;
            const std::size_t j = std::size_t(nr[k] * N + nc[k]);
            if (free[j] && dist[j] < 0) {
                dist[j] = dd;
                q.push_back(std::int32_t(j));
            }
        }
    }
}

static void place_stamps_only(const Model &m, const Plan &plan, int N, std::uint64_t seed, const std::vector<std::pair<int, int>> &spots_xy, std::vector<std::int32_t> &fixed, std::vector<std::uint8_t> &used, DecalProbe *probe);

// Ground decals: small groups of rare ground tiles pasted as fixed WFC cells on plain ground of their own class, at the stock rate.
// Every cell must already be in its macro case's quadrant domain (so the macro parity is right) and passable by majority (so
// passability is untouched); the 5x3 mother footprints and the ring round them stay plain. Never fails: a decal that does not fit is skipped.
static std::uint32_t place_decals(const Model &m, const Plan &plan, int N, std::uint64_t seed, const std::vector<std::pair<int, int>> &spots_xy, const std::vector<std::uint8_t> &used, std::vector<std::int32_t> &fixed, DecalProbe &probe) {
    if (m.decals.empty() || m.tune.decal_pct <= 0 || m.synthetic) return 0;
    const int                 M = N / 2;
    std::vector<std::uint8_t> blocked(std::size_t(N) * N, 0), dused(std::size_t(N) * N, 0); // blocked: footprint + ring; dused: decal cells so far
    for (const auto &[x, y] : spots_xy)
        for (int dy = -1; dy <= 3; ++dy)
            for (int dx = -1; dx <= 5; ++dx) blocked[std::size_t(pmod(y + dy, N) * N + pmod(x + dx, N))] = 1;
    std::uint32_t placed = 0;
    for (int k = 0; k < m.ncls; ++k) {
        if (m.decals_of[std::size_t(k)].empty() || m.decal_total[std::size_t(k)] == 0) continue;
        const std::uint8_t        c4[4] = {std::uint8_t(k), std::uint8_t(k), std::uint8_t(k), std::uint8_t(k)};
        const std::uint32_t       pk = make_key(c4, make_tag(TAG_PLAIN, 0)), ck = make_key(c4, make_tag(TAG_CLEAR));
        std::vector<std::uint8_t> okc(std::size_t(M) * M, 0);
        std::int64_t              okcount = 0;
        for (std::size_t i = 0; i < okc.size(); ++i) {
            okc[i] = plan.key[i] == pk || (k == m.bg && plan.key[i] == ck);
            okcount += okc[i];
        }
        if (okcount == 0) continue;
        // the busiest learned density: the hand-made small maps carry far more decoration than the big ones, and the big generated maps should not look bare
        std::int64_t rate = 0;
        for (std::uint32_t sz : {0u, 64u, 128u, 256u}) rate = std::max(rate, m.rules->stat_or(sz, "decal_rate_c" + std::to_string(k) + "_ppb", 0));
        const std::int64_t want = std::int64_t(std::uint64_t(rate) * std::uint64_t(okcount) * 4ull * std::uint64_t(m.tune.decal_pct) / (100ull * 1000000000ull));
        std::int64_t       left = want;
        const auto        &of   = m.decals_of[std::size_t(k)];
        // Stock decals are mostly one or two tiles; the multi-tile ones (cracks, pebble clusters) are what plain ground is missing, so
        // the shapes with 4+ tiles are drawn so that decal_big percent of the placed groups are big (0 = the learned mix).
        std::uint64_t small_n = 0, big_n = 0;
        for (std::uint32_t di : of) (m.decals[di].cells.size() >= 4 ? big_n : small_n) += m.decals[di].n;
        const bool                 mix = m.tune.decal_big > 0 && m.tune.decal_big < 100 && big_n > 0 && small_n > 0;
        std::vector<std::uint64_t> wt(of.size());
        std::uint64_t              wsum = 0;
        for (std::size_t di = 0; di < of.size(); ++di) {
            const DecalModel &d = m.decals[of[di]];
            if (!mix) wt[di] = d.n;
            else if (d.cells.size() >= 4) wt[di] = std::uint64_t(d.n) * small_n * std::uint64_t(m.tune.decal_big);
            else wt[di] = std::uint64_t(d.n) * big_n * std::uint64_t(100 - m.tune.decal_big);
            wsum += wt[di];
        }
        if (wsum == 0) continue;
        for (std::uint64_t i = 1; left > 0 && std::int64_t(i) <= want * 20; ++i) {
            std::uint64_t pick = h64({seed, 71, std::uint64_t(k), i}) % wsum;
            std::size_t   di   = 0;
            for (; di < of.size(); ++di) {
                if (pick < wt[di]) break;
                pick -= wt[di];
            }
            if (di >= of.size()) di = of.size() - 1;
            const DecalModel &d  = m.decals[of[di]];
            const int         r0 = int(h64({seed, 72, std::uint64_t(k), i}) % std::uint64_t(N)), c0 = int(h64({seed, 73, std::uint64_t(k), i}) % std::uint64_t(N));
            bool              ok = true;
            for (int y = -1; y <= d.h && ok; ++y)
                for (int x = -1; x <= d.w; ++x) {
                    const int         rr = pmod(r0 + y, N), cc = pmod(c0 + x, N);
                    const std::size_t at = std::size_t(rr * N + cc);
                    if (!okc[std::size_t((rr / 2) * M + cc / 2)] || blocked[at] || dused[at]) {
                        ok = false;
                        break;
                    }
                }
            if (!ok) continue;
            std::vector<std::pair<std::uint32_t, std::int32_t>> pins;
            for (const auto &cell : d.cells) {
                const std::size_t at = std::size_t(pmod(r0 + cell.first.first, N) * N + pmod(c0 + cell.first.second, N));
                if (used[at] || fixed[at] >= 0) {
                    ok = false;
                    break;
                }
                pins.push_back({std::uint32_t(at), cell.second});
            }
            if (!ok || !probe.pin(pins)) continue; // the probe keeps every domain non-empty, so what it takes cannot sink the WFC
            for (const auto &[at, tile] : pins) {
                fixed[at] = tile;
                dused[at] = 1;
            }
            ++placed;
            --left;
        }
    }
    return placed;
}

std::unique_ptr<Placement> place_stamps(const Model &m, const Plan &plan, int N, std::uint64_t seed, const std::vector<std::pair<int, int>> &spots_xy, bool decals) {
    auto                       out   = std::make_unique<Placement>();
    std::vector<std::int32_t> &fixed = out->fixed;
    fixed.assign(std::size_t(N) * N, -1);
    std::vector<std::uint8_t> used(std::size_t(N) * N, 0);
    const bool                have_stamps = !m.stamps.empty() && m.stamp_ppb > 0, have_decals = decals && !m.decals.empty() && m.tune.decal_pct > 0 && !m.synthetic;
    if (!have_stamps && !have_decals) return out;
    // Every stamp and decal is pinned through the probe, which keeps the whole map arc-consistent: one that would empty a domain is
    // refused, so the WFC's initial propagation never dies on the fixed cells (and no placement is wasted on a doomed WFC try).
    auto probe = std::make_unique<DecalProbe>(m, plan, N, fixed);
    if (!probe->valid()) return out; // the plan itself does not propagate: leave it to the WFC's own reporting
    if (have_stamps) place_stamps_only(m, plan, N, seed, spots_xy, fixed, used, probe.get());
    if (have_decals) out->decals = place_decals(m, plan, N, seed, spots_xy, used, fixed, *probe);
    out->probe = std::move(probe);
    return out;
}

static void place_stamps_only(const Model &m, const Plan &plan, int N, std::uint64_t seed, const std::vector<std::pair<int, int>> &spots_xy, std::vector<std::int32_t> &fixed, std::vector<std::uint8_t> &used, DecalProbe *probe) {
    const int                 M = N / 2;
    std::vector<std::uint8_t> okm(std::size_t(M) * M, 0);
    std::int64_t              okcount  = 0;
    const std::uint8_t        c[4]     = {std::uint8_t(m.bg), std::uint8_t(m.bg), std::uint8_t(m.bg), std::uint8_t(m.bg)};
    const std::uint32_t       want_key = make_key(c, make_tag(TAG_PLAIN, 0)), clear_key = make_key(c, make_tag(TAG_CLEAR));
    for (std::size_t i = 0; i < okm.size(); ++i) {
        okm[i] = plan.key[i] == want_key || plan.key[i] == clear_key;
        okcount += okm[i];
    }
    std::int64_t want = m.stamp_ppb * N * N / 1000000000 * okcount / (std::int64_t(M) * M);
    {
        // keep the precision of the product: ppb * cells * share
        want = std::int64_t(std::uint64_t(m.stamp_ppb) * std::uint64_t(N) * std::uint64_t(N) / 1000000000ull * std::uint64_t(okcount) / (std::uint64_t(M) * std::uint64_t(M)));
    }
    auto paste = [&](const StampModel &s, int r0, int c0) {
        std::vector<std::pair<std::uint32_t, std::int32_t>> pins;
        for (const auto &cell : s.cells) pins.push_back({std::uint32_t(pmod(r0 + cell.first.first, N) * N + pmod(c0 + cell.first.second, N)), cell.second});
        if (probe && !probe->pin(pins)) return false;
        for (const auto &[at, tile] : pins) fixed[at] = tile;
        for (int y = -2; y < s.h + 2; ++y)
            for (int x = -2; x < s.w + 2; ++x) used[std::size_t(pmod(r0 + y, N) * N + pmod(c0 + x, N))] = 1;
        return true;
    };
    // dressing pass: stock puts rocks 4-7 tiles off a base, so every spot gets a few small stamps in the ring around its core
    std::vector<std::size_t> small;
    std::uint64_t            small_total = 0;
    for (std::size_t si = 0; si < m.stamps.size(); ++si)
        if (m.stamps[si].cells.size() <= 4) {
            small.push_back(si);
            small_total += m.stamps[si].n;
        }
    if (small_total > 0)
        for (std::size_t sp = 0; sp < spots_xy.size(); ++sp) {
            const int x = spots_xy[sp].first, y = spots_xy[sp].second;
            int       got = 0;
            for (std::uint64_t j = 1; got < DRESS_STAMPS && j <= DRESS_TRIES; ++j) {
                std::uint64_t pick = h64({seed, 61, sp, j}) % small_total;
                std::size_t   ci   = 0;
                for (; ci < small.size(); ++ci) {
                    if (pick < m.stamps[small[ci]].n) break;
                    pick -= m.stamps[small[ci]].n;
                }
                if (ci >= small.size()) ci = small.size() - 1;
                const StampModel &s  = m.stamps[small[ci]];
                const int         dy = int(h64({seed, 62, sp, j}) % (2 * DRESS_SPAN + 1)) - DRESS_SPAN, dx = int(h64({seed, 63, sp, j}) % (2 * DRESS_SPAN + 1)) - DRESS_SPAN;
                int               nearest = 1 << 20, farthest = 0;
                for (int yy = -1; yy <= s.h; ++yy)
                    for (int xx = -1; xx <= s.w; ++xx) {
                        const int d = std::max(std::abs(dy + yy), std::abs(dx + xx));
                        nearest     = std::min(nearest, d);
                        farthest    = std::max(farthest, d);
                    }
                if (nearest <= CORE_R || farthest > DRESS_FAR) continue;
                const int r0 = pmod(y + dy, N), c0 = pmod(x + dx, N);
                bool      ok = true;
                for (int yy = -1; yy <= s.h && ok; ++yy)
                    for (int xx = -1; xx <= s.w; ++xx) {
                        const int rr = pmod(r0 + yy, N), cc = pmod(c0 + xx, N);
                        if (used[std::size_t(rr * N + cc)] || !okm[std::size_t((rr / 2) * M + cc / 2)]) {
                            ok = false;
                            break;
                        }
                    }
                if (!ok) continue;
                if (!paste(s, r0, c0)) continue;
                ++got;
            }
        }
    std::uint64_t i = 0;
    while (want > 0 && std::int64_t(i) < want * 30) {
        ++i;
        std::uint64_t pick = h64({seed, 51, i}) % m.stamp_total;
        std::size_t   si   = 0;
        for (; si < m.stamps.size(); ++si) {
            if (pick < m.stamps[si].n) break;
            pick -= m.stamps[si].n;
        }
        if (si >= m.stamps.size()) si = m.stamps.size() - 1;
        const StampModel &s  = m.stamps[si];
        const int         r0 = int(h64({seed, 52, i}) % std::uint64_t(N)), c0 = int(h64({seed, 53, i}) % std::uint64_t(N));
        bool              far = true; // the core and one tile beyond stay stamp-free (cells and their ring alike)
        for (const auto &[sx, sy] : spots_xy)
            for (int yy = -1; yy <= s.h && far; ++yy)
                for (int xx = -1; xx <= s.w; ++xx)
                    if (tdist(r0 + yy, sy, N) <= CORE_R + 1 && tdist(c0 + xx, sx, N) <= CORE_R + 1) {
                        far = false;
                        break;
                    }
        if (!far) continue;
        bool ok = true;
        for (int y = -2; y < s.h + 2 && ok; ++y)
            for (int x = -2; x < s.w + 2; ++x) {
                const int rr = pmod(r0 + y, N), cc = pmod(c0 + x, N);
                if (used[std::size_t(rr * N + cc)] || !okm[std::size_t((rr / 2) * M + cc / 2)]) {
                    ok = false;
                    break;
                }
            }
        if (!ok) continue;
        if (!paste(s, r0, c0)) continue;
        --want;
    }
}

void place_passability(const Model &m, const Plan &plan, const std::vector<std::int32_t> &tile, int N, std::vector<std::uint8_t> &pas) {
    pas.assign(std::size_t(N) * N, 0);
    for (std::size_t i = 0; i < pas.size(); ++i) pas[i] = m.pmaj[std::size_t(tile[i])];
    const int M = N / 2;
    for (int R = 0; R < M; ++R)
        for (int C = 0; C < M; ++C)
            if ((key_tag(plan.key[std::size_t(R * M + C)]) >> 4) == TAG_RAMP)
                for (int dr = 0; dr < 2; ++dr)
                    for (int dc = 0; dc < 2; ++dc) {
                        const std::size_t i = std::size_t((2 * R + dr) * N + 2 * C + dc);
                        if (m.pseen_pass[std::size_t(tile[i])]) pas[i] = 1;
                    }
}

void place_trees(const Model &m, const std::vector<std::int32_t> &tile, int N, std::uint64_t seed, bool ensure_paths, int max_trees, const std::vector<std::pair<int, int>> &spots_xy, std::vector<std::uint8_t> &pas, std::vector<std::int32_t> &treeat, std::vector<MpmTree> &out) {
    struct Sample {
        std::int32_t kind = 0, px = 0, py = 0;
        bool         present = false;
    };
    const std::size_t   n2 = std::size_t(N) * N;
    std::vector<Sample> tr(n2);
    std::int64_t        want = std::int64_t(std::uint64_t(m.tree_ppm) * n2 / 1000000ull);
    want                     = std::min<std::int64_t>(want, max_trees);
    std::vector<std::uint8_t> ok(n2, 0);
    for (std::size_t i = 0; i < n2; ++i) ok[i] = !m.trees[std::size_t(tile[i])].empty() && pas[i];
    for (const auto &[x, y] : spots_xy)
        for (int dr = -CORE_R; dr <= CORE_R; ++dr)
            for (int dc = -CORE_R; dc <= CORE_R; ++dc) ok[std::size_t(pmod(y + dr, N) * N + pmod(x + dc, N))] = 0;
    std::vector<std::tuple<std::uint64_t, int, int>> cand;
    for (int r = 0; r < N; ++r)
        for (int c = 0; c < N; ++c)
            if (ok[std::size_t(r * N + c)]) cand.push_back({h64({seed, 21, std::uint64_t(r), std::uint64_t(c)}) & 0xFFFFFFFFull, r, c});
    std::sort(cand.begin(), cand.end());
    std::vector<std::uint8_t> occ(n2, 0);
    std::int64_t              count = 0;
    SpriteExtent              probe{};
    const std::string        &tree_stem = m.rules->tree_profile.empty() ? m.stem : m.rules->tree_profile;
    const bool                have_ext  = sprite_extent(tree_stem, 1, probe);
    for (const auto &[pr, r, c] : cand) {
        (void)pr;
        if (count >= want) break;
        bool near_occ = false;
        for (int a = -1; a <= 1 && !near_occ; ++a)
            for (int b = -1; b <= 1; ++b)
                if (occ[std::size_t(pmod(r + a, N) * N + pmod(c + b, N))]) {
                    near_occ = true;
                    break;
                }
        if (near_occ) continue;
        const auto   &s     = m.trees[std::size_t(tile[std::size_t(r * N + c)])];
        std::uint64_t total = 0;
        for (const auto &e : s) total += e.n;
        std::uint64_t k  = h64({seed, 22, std::uint64_t(r), std::uint64_t(c)}) % total;
        std::size_t   si = 0;
        for (; si < s.size(); ++si) {
            if (k < s[si].n) break;
            k -= s[si].n;
        }
        if (si >= s.size()) si = s.size() - 1;
        if (have_ext) { // no object's art may be drawn over a mother footprint
            SpriteExtent e{};
            sprite_extent(tree_stem, s[si].kind, e);
            bool hit = false;
            for (const auto &[x, y] : spots_xy) hit = hit || sprite_hits_footprint(e, r, c, s[si].px, s[si].py, x, y, N);
            if (hit) continue;
        }
        Sample &t                   = tr[std::size_t(r * N + c)];
        t                           = {s[si].kind, s[si].px, s[si].py, true};
        occ[std::size_t(r * N + c)] = 1;
        ++count;
    }
    if (ensure_paths && !spots_xy.empty()) {
        std::vector<std::int32_t> d0, d1;
        std::vector<std::uint8_t> free(n2), pass_only(n2);
        for (std::size_t i = 0; i < n2; ++i) pass_only[i] = pas[i] != 0;
        for (int it = 0; it < 40; ++it) {
            for (std::size_t i = 0; i < n2; ++i) free[i] = pas[i] && !tr[i].present;
            bfs_free(free, N, spots_xy[0].second, spots_xy[0].first, d0);
            int cut = -1;
            for (std::size_t s = 0; s < spots_xy.size() && cut < 0; ++s)
                if (d0[std::size_t(spots_xy[s].second * N + spots_xy[s].first)] < 0) cut = int(s);
            if (cut < 0) break;
            bfs_free(pass_only, N, spots_xy[std::size_t(cut)].second, spots_xy[std::size_t(cut)].first, d1);
            int r = spots_xy[0].second, c = spots_xy[0].first;
            if (d1[std::size_t(r * N + c)] < 0) break;
            while (d1[std::size_t(r * N + c)] > 0) {
                tr[std::size_t(r * N + c)].present = false;
                const int          nr[4] = {r + 1, r - 1, r, r}, nc[4] = {c, c, c + 1, c - 1};
                const std::int32_t cur = d1[std::size_t(r * N + c)];
                for (int k = 0; k < 4; ++k) {
                    if (nr[k] < 0 || nr[k] >= N || nc[k] < 0 || nc[k] >= N) continue;
                    if (d1[std::size_t(nr[k] * N + nc[k])] == cur - 1) {
                        r = nr[k];
                        c = nc[k];
                        break;
                    }
                }
            }
            tr[std::size_t(r * N + c)].present = false;
        }
    }
    // slots in (col, row) order
    out.clear();
    treeat.assign(n2, -1);
    for (int c = 0; c < N; ++c)
        for (int r = 0; r < N; ++r) {
            const Sample &t = tr[std::size_t(r * N + c)];
            if (!t.present) continue;
            MpmTree mt;
            mt.slot                        = std::uint32_t(out.size() + 1);
            mt.kind                        = t.kind;
            mt.px_x                        = t.px;
            mt.px_y                        = t.py;
            mt.next                        = 0;
            mt.x                           = c;
            mt.y                           = r;
            treeat[std::size_t(r * N + c)] = std::int32_t(out.size());
            out.push_back(mt);
        }
}

void place_ore(const Model &m, const std::vector<std::uint8_t> &pas, const std::vector<std::int32_t> &treeat, int N, std::uint64_t seed, const std::vector<std::pair<int, int>> &spots_xy, std::vector<std::uint16_t> &ore) {
    const int B = N / 4;
    ore.assign(std::size_t(B) * B * 8, 0);
    std::vector<std::uint8_t> okb(std::size_t(B) * B, 0), okw(std::size_t(B) * B, 0); // okw: the weaker test (6 of 16 tiles free) the top-up pass accepts
    for (int br = 0; br < B; ++br)
        for (int bc = 0; bc < B; ++bc) {
            int n = 0;
            for (int a = 0; a < 4; ++a)
                for (int b = 0; b < 4; ++b) {
                    const std::size_t i = std::size_t((br * 4 + a) * N + bc * 4 + b);
                    n += pas[i] && treeat[i] < 0 ? 1 : 0;
                }
            okb[std::size_t(br * B + bc)] = n * 2 >= 16;
            okw[std::size_t(br * B + bc)] = n >= 6;
        }
    auto has = [&](int r, int c) {
        const std::uint16_t *o = &ore[std::size_t(r * B + c) * 8];
        return o[0] || o[1] || o[2] || o[3];
    };
    auto put = [&](int r, int c, const std::array<std::uint16_t, 4> &v) {
        for (int i = 0; i < 4; ++i) ore[std::size_t(r * B + c) * 8 + std::size_t(i)] = v[std::size_t(i)];
    };
    const auto &good = m.ore_good;
    const auto &recs = m.ore_all;
    // shared template (block offsets, amounts); entry 0 is the AI's first mine, one block from the spot
    struct Ent {
        int                          dr, dc;
        std::array<std::uint16_t, 4> v;
    };
    std::vector<Ent> tmpl;
    tmpl.push_back({1, 0, good[std::size_t(h64({seed, 30}) % good.size())]});
    const bool need_rich = !m.ore_rich.empty();
    if (need_rich) { // one rich block in the ring around the first mine's block: the rotation search below keeps it within reach
        static const int ring1[7][2] = {{-1, -1}, {-1, 0}, {-1, 1}, {0, -1}, {0, 1}, {1, -1}, {1, 1}};
        const auto      &o           = ring1[h64({seed, 35}) % 7];
        tmpl.push_back({o[0], o[1], m.ore_rich[std::size_t(h64({seed, 36}) % m.ore_rich.size())]});
    }
    std::uint64_t i = 0;
    while (int(tmpl.size()) < near_blocks() && i < 400) {
        const int dr = int(h64({seed, 31, i}) % 5) - 2, dc = int(h64({seed, 32, i}) % 5) - 2;
        bool      dup = false;
        for (const Ent &e : tmpl) dup = dup || (e.dr == dr && e.dc == dc);
        if (std::max(std::abs(dr), std::abs(dc)) >= 1 && !dup) {
            const auto &pool = (h64({seed, 34, i}) % 2) ? good : recs;
            tmpl.push_back({dr, dc, pool[std::size_t(h64({seed, 33, i}) % pool.size())]});
        }
        ++i;
    }
    // candidate cell offsets around an ideal block, nearest first
    std::vector<std::array<int, 2>> cand;
    for (int a = -3; a <= 3; ++a)
        for (int b = -3; b <= 3; ++b) cand.push_back({a, b});
    std::stable_sort(cand.begin(), cand.end(), [](const std::array<int, 2> &x, const std::array<int, 2> &y) { return x[0] * x[0] + x[1] * x[1] < y[0] * y[0] + y[1] * y[1]; });
    auto isqrt_ceil = [](int v) {
        int r = 0;
        while (r * r < v) ++r;
        return r;
    };
    struct Rot {
        std::int64_t score;
        bool         ok;
    };
    // an ore block must never sit on any spot's 5x3 landing footprint [x,x+4]x[y,y+2]; the first mine also keeps one tile clear of its own
    auto hits = [&](int r, int c, int margin) {
        for (const auto &[x, y] : spots_xy)
            if (4 * c - margin <= x + 4 && 4 * c + 3 + margin >= x && 4 * r - margin <= y + 2 && 4 * r + 3 + margin >= y) return true;
        return false;
    };
    // nearest rich / full-channel block to the footprint centre (torus tiles, squared), over everything now on the map near the spot
    auto reach = [&](std::size_t si, int &rich, int &goodd, int &n8) {
        const int br = spots_xy[si].second / 4, bc = spots_xy[si].first / 4;
        const int cx = spots_xy[si].first + 2, cy = spots_xy[si].second + 1, T = N;
        rich = goodd = 1 << 20;
        n8           = 0;
        for (int a = -4; a <= 4; ++a)
            for (int b = -4; b <= 4; ++b) {
                const int            r = pmod(br + a, B), c = pmod(bc + b, B);
                const std::uint16_t *o = &ore[std::size_t(r * B + c) * 8];
                if (!(o[0] && o[1] && o[2] && o[3])) continue;
                const int wt = 4 * o[0] + 2 * o[1] + 2 * o[2] + o[3];
                if (wt < 500) continue;
                int dx = std::abs(cx - (c * 4 + 2)), dy = std::abs(cy - (r * 4 + 2));
                dx           = std::min(dx, T - dx);
                dy           = std::min(dy, T - dy);
                const int d2 = dx * dx + dy * dy;
                goodd        = std::min(goodd, d2);
                n8 += d2 <= 64;
                if (wt >= int(ORE_RICH_WEIGHT)) rich = std::min(rich, d2);
            }
    };
    // place the whole template for spot si under rotation `rot` (appending the cells it fills to `placed`), then score it
    auto place_rot = [&](std::size_t si, std::size_t rot, std::size_t limit, std::vector<std::pair<int, int>> &placed) {
        const int    br = spots_xy[si].second / 4, bc = spots_xy[si].first / 4;
        std::int64_t disp = 0;
        for (const Ent &e : tmpl) {
            if (std::size_t(&e - tmpl.data()) >= limit) break;
            int dr = e.dr, dc = e.dc;
            for (std::size_t k = 0; k < rot; ++k) {
                const int t = dr;
                dr          = dc;
                dc          = -t;
            }
            bool done = false;
            for (int margin = (&e == &tmpl[0]) ? 1 : 0; margin >= 0 && !done; --margin)
                for (const auto &ab : cand) {
                    const int r = pmod(br + dr + ab[0], B), c = pmod(bc + dc + ab[1], B);
                    if (okb[std::size_t(r * B + c)] && !has(r, c) && !(r == br && c == bc) && !hits(r, c, margin)) {
                        put(r, c, e.v);
                        placed.push_back({r, c});
                        disp += ab[0] * ab[0] + ab[1] * ab[1];
                        done = true;
                        break;
                    }
                }
            if (!done) disp += 60;
        }
        int rich, goodd, n8;
        reach(si, rich, goodd, n8);
        bool ok = n8 >= 2 && goodd <= int(MAX_GOOD_REACH_SQ) && (!need_rich || rich <= int(MAX_RICH_REACH * MAX_RICH_REACH));
        // build room: the ore placed so far (this spot's template included) must leave the base unsealed (DESIGN sec. 14)
        const BuildRoom room    = build_room([&](int c, int r) { return pas[std::size_t(r * N + c)] && treeat[std::size_t(r * N + c)] < 0 && !has(r >> 2, c >> 2); }, N, spots_xy[si].first, spots_xy[si].second);
        const bool      room_ok = room.n16 >= int(MIN_ROOM16) && room.n32 >= int(MIN_ROOM32);
        return Rot{disp * 4 + (need_rich ? isqrt_ceil(std::min(rich, 4096)) : 0) + (ok ? 0 : 10000) + (room_ok ? 0 : 20000 + 500 - std::min(room.n32, 500)), ok && room_ok};
    };
    for (std::size_t si = 0; si < spots_xy.size(); ++si) {
        // all 4 rotations of the whole template; when none leaves the base build room, the essential entries alone (first mine, rich block)
        std::size_t  best_rot = si % 4, best_lim = tmpl.size();
        std::int64_t best = std::int64_t(1) << 40;
        for (std::size_t pass = 0; pass < 2 && (pass == 0 || best >= 20000); ++pass) {
            const std::size_t lim = pass == 0 ? tmpl.size() : std::min<std::size_t>(tmpl.size(), need_rich ? 2 : 1);
            for (std::size_t k = 0; k < 4; ++k) {
                const std::size_t                rot = (si + k) % 4;
                std::vector<std::pair<int, int>> placed;
                const Rot                        sc = place_rot(si, rot, lim, placed);
                for (const auto &[r, c] : placed) put(r, c, {0, 0, 0, 0});
                if (sc.score < best) {
                    best     = sc.score;
                    best_rot = rot;
                    best_lim = lim;
                }
            }
        }
        std::vector<std::pair<int, int>> placed;
        place_rot(si, best_rot, best_lim, placed);
        // no rotation reached: put one more block on the nearest free cell that does (rich first -- it also counts as full-channel)
        auto top_up = [&](int max_d2, const std::vector<std::array<std::uint16_t, 4>> &pool, std::uint64_t salt) {
            const int br = spots_xy[si].second / 4, bc = spots_xy[si].first / 4;
            const int cx = spots_xy[si].first + 2, cy = spots_xy[si].second + 1;
            int       bd = 1 << 20, bar = 0, bac = 0;
            for (int a = -4; a <= 4; ++a)
                for (int b = -4; b <= 4; ++b) {
                    const int r = pmod(br + a, B), c = pmod(bc + b, B);
                    if (!okw[std::size_t(r * B + c)] || has(r, c) || hits(r, c, 0)) continue;
                    int dx = std::abs(cx - (c * 4 + 2)), dy = std::abs(cy - (r * 4 + 2));
                    dx           = std::min(dx, N - dx);
                    dy           = std::min(dy, N - dy);
                    const int d2 = dx * dx + dy * dy;
                    if (d2 <= max_d2 && d2 < bd) {
                        bd  = d2;
                        bar = r;
                        bac = c;
                    }
                }
            if (bd < (1 << 20)) put(bar, bac, pool[std::size_t(h64({seed, salt, std::uint64_t(si)}) % pool.size())]);
        };
        int rich, goodd, n8;
        reach(si, rich, goodd, n8);
        if (need_rich && rich > int(MAX_RICH_REACH * MAX_RICH_REACH)) top_up(int(MAX_RICH_REACH * MAX_RICH_REACH), m.ore_rich, 37);
        reach(si, rich, goodd, n8);
        if (goodd > int(MAX_GOOD_REACH_SQ)) top_up(int(MAX_GOOD_REACH_SQ), good, 38);
        reach(si, rich, goodd, n8);
        if (n8 < 2) top_up(64, good, 39);
    }
    int n = 0;
    i     = 0;
    while (n < neutral_blocks(N) && i < 5000) {
        const int r = int(h64({seed, 41, i}) % std::uint64_t(B)), c = int(h64({seed, 42, i}) % std::uint64_t(B));
        ++i;
        if (!okb[std::size_t(r * B + c)] || has(r, c)) continue;
        int dmin = 1 << 30;
        for (const auto &[x, y] : spots_xy) dmin = std::min(dmin, std::max(tdist(r, y / 4, B), tdist(c, x / 4, B)));
        if (dmin < 5) continue;
        const int cnt = 1 + int(h64({seed, 43, i}) % 4);
        for (int k = 0; k < cnt; ++k) {
            const int rr = (r + k / 2) % B, cc = (c + k % 2) % B;
            if (okb[std::size_t(rr * B + cc)] && !has(rr, cc)) {
                put(rr, cc, recs[std::size_t(h64({seed, 44, i, std::uint64_t(k)}) % recs.size())]);
                ++n;
            }
        }
    }
}

} // namespace mh_mapgen::detail
