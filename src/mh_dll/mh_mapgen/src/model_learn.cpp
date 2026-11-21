// mh_mapgen -- the macro-model learner (design sec. 9/10).
//
// The shipped tilesets are Wang-like on a 2x2 MACRO grid: the corner classes of a tile (nw/ne/sw/se) cluster into a handful of
// terrain classes (ground, plateau, water, forest, ...) and every shipped adjacency is explained by the class of the corner
// the two tiles share. This file recovers
//   - the corner classes (agglomerative clustering over the shipped adjacencies, integer ratios),
//   - the macro catalogue: for every 2x2 block seen on the best-fitting macro parity, its four corner classes plus a tag
//     (ridge crossings, ramp / cliff / clear / orientation variants) and the four tiles,
//   - the class nesting tree (ground -> plateau, ground -> shore -> water ...),
//   - found objects ("stamps"), tree samples, four-channel ore records and whole-tileset statistics.
// Integer only; every container is ordered, so the output does not depend on the input order.
#include "mh_mapgen/rules.h"
#include <algorithm>
#include <array>
#include <map>
#include <queue>
#include <set>

namespace mh_mapgen {

namespace {

using Stats = std::map<std::pair<std::uint32_t, std::string>, std::int64_t>;
using Quad  = std::array<std::uint16_t, 4>;

constexpr std::uint32_t CLUSTER_NUM = 1, CLUSTER_DEN = 50; // merge two corner groups when the link >= 2% of the smaller mass
constexpr int           MAX_CLASSES = 15;

// Row-major view of one stock map: ids[row * w + col].
struct View {
    std::uint32_t              w = 0, h = 0;
    std::vector<std::uint16_t> id;
    std::vector<std::uint8_t>  pas, tree;
    std::uint16_t              at(std::uint32_t r, std::uint32_t c) const { return id[std::size_t(r) * w + c]; }
};

View make_view(const MpmModel &m) {
    View v;
    v.w = m.width;
    v.h = m.height;
    v.id.resize(std::size_t(v.w) * v.h);
    v.pas.resize(v.id.size());
    v.tree.resize(v.id.size());
    for (std::uint32_t c = 0; c < v.w; ++c)
        for (std::uint32_t r = 0; r < v.h; ++r) {
            const std::size_t s = m.cell(c, r), d = std::size_t(r) * v.w + c;
            v.id[d]   = m.flags[s] & TILE_ID_MASK;
            v.pas[d]  = m.passable[s] != 0;
            v.tree[d] = m.building[s] != 0;
        }
    for (const MpmTree &t : m.trees)
        if (t.x >= 0 && t.y >= 0 && std::uint32_t(t.x) < v.w && std::uint32_t(t.y) < v.h) v.tree[std::size_t(t.y) * v.w + std::size_t(t.x)] = 1;
    return v;
}

// ---- corner clustering --------------------------------------------------------------------------------------------------
struct Merge {
    std::uint64_t n, m; // link, smaller mass: ratio n / m
    int           x, y;
};
struct MergeLess { // priority_queue is a max-heap: larger ratio first, then smaller (x, y)
    bool operator()(const Merge &a, const Merge &b) const {
        const std::uint64_t l = a.n * b.m, r = b.n * a.m;
        if (l != r) return l < r;
        if (a.x != b.x) return a.x > b.x;
        return a.y > b.y;
    }
};

// lab[tile_index * 4 + corner] = the root key of the merged corner group.
std::vector<int> cluster_corners(const std::vector<std::uint16_t> &tiles, const std::vector<std::uint64_t> &used, const std::vector<std::int32_t> &ix, const Rules &r) {
    const int                                    K  = int(tiles.size()) * 4;
    const std::size_t                            KS = std::size_t(K);
    std::map<std::pair<int, int>, std::uint64_t> links;
    auto                                         add = [&](int x, int y, std::uint64_t n) {
        if (x == y) return;
        if (x > y) std::swap(x, y);
        links[{x, y}] += n;
    };
    for (const PairRule &p : r.h) {
        const int a = ix[p.a], b = ix[p.b];
        add(a * 4 + 1, b * 4 + 0, p.n);
        add(a * 4 + 3, b * 4 + 2, p.n);
    }
    for (const PairRule &p : r.v) {
        const int a = ix[p.a], b = ix[p.b];
        add(a * 4 + 2, b * 4 + 0, p.n);
        add(a * 4 + 3, b * 4 + 1, p.n);
    }
    std::vector<std::uint64_t>                mass(KS);
    std::vector<std::map<int, std::uint64_t>> adj(KS);
    std::vector<int>                          par(KS);
    std::vector<std::uint8_t>                 alive(KS, 1);
    for (int k = 0; k < K; ++k) {
        mass[std::size_t(k)] = used[std::size_t(k / 4)];
        par[std::size_t(k)]  = k;
    }
    std::priority_queue<Merge, std::vector<Merge>, MergeLess> heap;
    for (const auto &[e, n] : links) {
        adj[std::size_t(e.first)][e.second] += n;
        adj[std::size_t(e.second)][e.first] += n;
        heap.push({n, std::min(mass[std::size_t(e.first)], mass[std::size_t(e.second)]), e.first, e.second});
    }
    while (!heap.empty()) {
        const Merge e = heap.top();
        heap.pop();
        if (!alive[std::size_t(e.x)] || !alive[std::size_t(e.y)]) continue;
        auto                it = adj[std::size_t(e.x)].find(e.y);
        const std::uint64_t cn = it == adj[std::size_t(e.x)].end() ? 0 : it->second;
        const std::uint64_t cm = std::min(mass[std::size_t(e.x)], mass[std::size_t(e.y)]);
        if (cn * e.m != e.n * cm) continue; // stale
        if (cn * CLUSTER_DEN < cm * CLUSTER_NUM) break;
        const int x = e.x, y = e.y;
        alive[std::size_t(y)] = 0;
        par[std::size_t(y)]   = x;
        mass[std::size_t(x)] += mass[std::size_t(y)];
        for (const auto &[z, n] : adj[std::size_t(y)]) {
            if (z == x) continue;
            adj[std::size_t(x)][z] += n;
            adj[std::size_t(z)][x] += n;
            adj[std::size_t(z)].erase(y);
        }
        adj[std::size_t(x)].erase(y);
        adj[std::size_t(y)].clear();
        for (const auto &[z, n] : adj[std::size_t(x)]) heap.push({n, std::min(mass[std::size_t(x)], mass[std::size_t(z)]), x, z});
    }
    std::vector<int> lab(KS);
    for (int k = 0; k < K; ++k) {
        int f = k;
        while (par[std::size_t(f)] != f) f = par[std::size_t(f)];
        lab[std::size_t(k)] = f;
    }
    return lab;
}

// ---- the model builder ----------------------------------------------------------------------------------------------------
struct CaseKey {
    std::uint8_t  c[4];
    std::uint8_t  tag;
    std::uint32_t pack() const { return (std::uint32_t(c[0]) << 20) | (std::uint32_t(c[1]) << 16) | (std::uint32_t(c[2]) << 12) | (std::uint32_t(c[3]) << 8) | tag; }
};
std::uint32_t pack_key(const int c[4], std::uint8_t tag) { return CaseKey{{std::uint8_t(c[0]), std::uint8_t(c[1]), std::uint8_t(c[2]), std::uint8_t(c[3])}, tag}.pack(); }

struct Puf {
    std::vector<int>          p;
    std::vector<std::uint8_t> x;
    explicit Puf(std::size_t n) : p(n), x(n, 0) {
        for (std::size_t i = 0; i < n; ++i) p[i] = int(i);
    }
    std::pair<int, int> find(int a) {
        int r = a, par = 0;
        while (p[std::size_t(r)] != r) {
            par ^= x[std::size_t(r)];
            r = p[std::size_t(r)];
        }
        int cur = a, cp = par;
        while (cur != r) {
            const int nxt = p[std::size_t(cur)], px = x[std::size_t(cur)];
            p[std::size_t(cur)] = r;
            x[std::size_t(cur)] = std::uint8_t(cp);
            cp ^= px;
            cur = nxt;
        }
        return {r, par};
    }
    bool unite(int a, int b, int diff) {
        auto [ra, pa] = find(a);
        auto [rb, pb] = find(b);
        if (ra == rb) return (pa ^ pb) == diff;
        p[std::size_t(rb)] = ra;
        x[std::size_t(rb)] = std::uint8_t(pa ^ pb ^ diff);
        return true;
    }
};

} // namespace

void learn_macro_model(const std::vector<LearnInput> &inputs, Rules &r, Stats &stats) {
    if (inputs.empty() || r.tiles.empty()) return;
    // dense tile index
    std::vector<std::uint16_t> tiles;
    std::vector<std::int32_t>  ix(0x2000, -1);
    for (const TileRule &t : r.tiles) {
        ix[t.id & 0x1FFF] = std::int32_t(tiles.size());
        tiles.push_back(t.id);
    }
    std::vector<std::uint64_t> used(tiles.size());
    for (std::size_t i = 0; i < tiles.size(); ++i) used[i] = r.tiles[i].freq;
    std::uint64_t tot4 = 0;
    for (std::uint64_t u : used) tot4 += 4 * u;

    // ---- corner classes
    const std::vector<int>       lab = cluster_corners(tiles, used, ix, r);
    std::map<int, std::uint64_t> cmass;
    for (std::size_t k = 0; k < lab.size(); ++k) cmass[lab[k]] += used[k / 4];
    std::vector<int> big;
    {
        std::vector<std::pair<std::uint64_t, int>> v;
        for (const auto &[root, m] : cmass)
            if (m * 100 > tot4) v.push_back({m, root});
        std::sort(v.begin(), v.end(), [](const auto &a, const auto &b) { return a.first != b.first ? a.first > b.first : a.second < b.second; });
        for (const auto &e : v) big.push_back(e.second);
        if (big.size() > std::size_t(MAX_CLASSES)) big.resize(std::size_t(MAX_CLASSES));
    }
    if (big.empty()) return;
    std::map<int, int> bi;
    for (std::size_t i = 0; i < big.size(); ++i) bi[big[i]] = int(i);
    // class of corner q of the tile with dense index ti, -1 when not a big class
    auto cls_of = [&](std::int32_t ti, int q) {
        auto it = bi.find(lab[std::size_t(ti) * 4 + std::size_t(q)]);
        return it == bi.end() ? -1 : it->second;
    };
    for (std::size_t i = 0; i < tiles.size(); ++i) {
        CornerRule c;
        c.tile = tiles[i];
        for (int q = 0; q < 4; ++q) c.cls[q] = std::int8_t(cls_of(std::int32_t(i), q));
        r.corners.push_back(c);
    }

    // ---- macro catalogue on the best parity of every map
    std::vector<View> views;
    for (const LearnInput &in : inputs) views.push_back(make_view(*in.map));
    struct Par {
        std::uint32_t pr = 0, pc = 0;
    };
    std::vector<Par>              pars(views.size());
    std::map<Quad, std::uint32_t> cat, rampn;
    for (std::size_t vi = 0; vi < views.size(); ++vi) {
        const View   &v        = views[vi];
        std::uint64_t score[4] = {0, 0, 0, 0};
        for (std::uint32_t y = 0; y < v.h; ++y)
            for (std::uint32_t x = 0; x < v.w; ++x)
                if (cls_of(ix[v.at(y, x)], 0) >= 0) ++score[(y & 1) * 2 + (x & 1)];
        int best = 0;
        for (int k = 1; k < 4; ++k)
            if (score[k] > score[best]) best = k;
        pars[vi] = {std::uint32_t(best / 2), std::uint32_t(best % 2)};
        for (std::uint32_t R = 0; R < v.h; R += 2)
            for (std::uint32_t C = 0; C < v.w; C += 2) {
                const std::uint32_t r0 = (R + pars[vi].pr) % v.h, r1 = (R + pars[vi].pr + 1) % v.h, c0 = (C + pars[vi].pc) % v.w, c1 = (C + pars[vi].pc + 1) % v.w;
                const Quad          q = {v.at(r0, c0), v.at(r0, c1), v.at(r1, c0), v.at(r1, c1)};
                cat[q]++;
                const std::size_t i0 = std::size_t(r0) * v.w, i1 = std::size_t(r1) * v.w;
                if (v.pas[i0 + c0] && v.pas[i0 + c1] && v.pas[i1 + c0] && v.pas[i1 + c1]) rampn[q]++;
            }
    }
    std::uint64_t tot = 0;
    for (const auto &[q, n] : cat) tot += n;

    std::map<std::uint32_t, std::map<Quad, std::uint32_t>> cases;
    std::map<int, std::uint64_t>                           pts, pure;
    std::map<std::pair<int, int>, std::uint64_t>           graph;
    auto                                                   classes_of = [&](const Quad &q, int cs[4]) {
        for (int k = 0; k < 4; ++k) cs[k] = cls_of(ix[q[std::size_t(k)]], k);
    };
    for (const auto &[q, n] : cat) {
        int cs[4];
        classes_of(q, cs);
        pts[cs[0]] += n;
        if (cs[0] < 0 || cs[1] < 0 || cs[2] < 0 || cs[3] < 0) continue;
        if (cs[0] == cs[1] && cs[0] == cs[2] && cs[0] == cs[3]) {
            const int          g  = big[std::size_t(cs[0])];
            const std::int32_t tl = ix[q[0]], tr = ix[q[1]], bl = ix[q[2]], br = ix[q[3]];
            (void)bl;
            const int bits = (lab[std::size_t(tl) * 4 + 1] != g ? 1 : 0) | (lab[std::size_t(tr) * 4 + 3] != g ? 2 : 0) | (lab[std::size_t(br) * 4 + 2] != g ? 4 : 0) | (lab[std::size_t(tl) * 4 + 2] != g ? 8 : 0);
            pure[cs[0]] += n;
            cases[pack_key(cs, make_tag(TAG_PLAIN, std::uint8_t(bits)))][q] += n;
        } else {
            std::set<int> s(cs, cs + 4);
            for (auto a = s.begin(); a != s.end(); ++a)
                for (auto b = std::next(a); b != s.end(); ++b) graph[{*a, *b}] += n;
            cases[pack_key(cs, make_tag(TAG_MIXED))][q] += n;
        }
    }

    // regions, background, nesting tree
    std::vector<int> regions;
    for (int k = 0; k < int(big.size()); ++k)
        if (pure[k] * 250 >= tot) regions.push_back(k);
    if (regions.empty()) return;
    int bg = regions[0];
    for (int k : regions)
        if (pure[k] > pure[bg]) bg = k;
    std::map<int, std::set<int>> adj;
    for (int k : regions) adj[k];
    for (const auto &[e, n] : graph)
        if (adj.count(e.first) && adj.count(e.second) && n >= 20) {
            adj[e.first].insert(e.second);
            adj[e.second].insert(e.first);
        }
    std::map<int, int> parent;
    std::vector<int>   order;
    parent[bg] = -1;
    order.push_back(bg);
    for (std::size_t i = 0; i < order.size(); ++i) {
        const int k = order[i];
        for (int j : adj[k])
            if (!parent.count(j)) {
                parent[j] = k;
                order.push_back(j);
            }
    }
    for (int k = 0; k < int(big.size()); ++k) {
        ClassRule c;
        c.id     = k;
        c.parent = parent.count(k) ? parent[k] : -1;
        c.order  = -1;
        for (std::size_t i = 0; i < order.size(); ++i)
            if (order[i] == k) c.order = int(i);
        c.share_ppm = std::uint32_t(tot ? pts[k] * 1000000 / tot : 0);
        c.pure_uses = std::uint32_t(pure[k]);
        r.classes.push_back(c);
    }
    for (const auto &[a, s] : adj)
        for (int b : s)
            if (a < b) r.adj.push_back({a, b});

    // line share: pure background macros crossed by a ridge on exactly two edges
    std::uint64_t lines = 0;
    for (const auto &[k, c] : cases) {
        const std::uint8_t tag = std::uint8_t(k & 255);
        if ((tag >> 4) != TAG_PLAIN) continue;
        const std::uint32_t cl = k >> 8;
        const int           c0 = int(cl >> 12) & 15, c1 = int(cl >> 8) & 15, c2 = int(cl >> 4) & 15, c3 = int(cl) & 15;
        if (c0 != bg || c1 != bg || c2 != bg || c3 != bg) continue;
        const int bits = tag & 15, pop = (bits & 1) + ((bits >> 1) & 1) + ((bits >> 2) & 1) + ((bits >> 3) & 1);
        if (pop != 2) continue;
        for (const auto &[q, n] : c) lines += n;
    }
    const std::int64_t line_share_ppm = std::int64_t(tot ? lines * 1000000 / tot : 0);
    stats[{0, "line_share_ppm"}]      = line_share_ppm;

    // ---- orientation of the ridge pieces (the hidden side field, two-coloured per map)
    if (line_share_ppm >= 15000) {
        const int                                      bgroot = big[std::size_t(bg)];
        std::map<Quad, std::map<int, int>>             votes;
        std::vector<std::vector<std::pair<int, Quad>>> allm;
        for (std::size_t vi = 0; vi < views.size(); ++vi) {
            const View         &v  = views[vi];
            const std::uint32_t pr = pars[vi].pr, pc = pars[vi].pc, Mh = v.h / 2, Mw = v.w / 2;
            auto                a    = [&](std::uint32_t y, std::uint32_t x) { return v.at((y + pr) % v.h, (x + pc) % v.w); };
            auto                pt   = [&](std::uint32_t R, std::uint32_t C) { return int((R % Mh) * Mw + (C % Mw)); };
            auto                isbg = [&](std::uint32_t R, std::uint32_t C) { return lab[std::size_t(ix[a((2 * R) % v.h, (2 * C) % v.w)]) * 4] == bgroot; };
            Puf                 uf(std::size_t(Mh) * Mw);
            for (std::uint32_t R = 0; R < Mh; ++R)
                for (std::uint32_t C = 0; C < Mw; ++C) {
                    if (!isbg(R, C)) continue;
                    const std::int32_t t = ix[a(2 * R, 2 * C)];
                    if (isbg(R, C + 1)) uf.unite(pt(R, C), pt(R, C + 1), lab[std::size_t(t) * 4 + 1] != bgroot ? 1 : 0);
                    if (isbg(R + 1, C)) uf.unite(pt(R, C), pt(R + 1, C), lab[std::size_t(t) * 4 + 2] != bgroot ? 1 : 0);
                }
            std::map<int, std::size_t>                     comp_ix;
            std::vector<std::vector<std::pair<int, Quad>>> comps;
            for (std::uint32_t R = 0; R < Mh; ++R)
                for (std::uint32_t C = 0; C < Mw; ++C) {
                    const std::uint32_t pr_[4] = {R, R, R + 1, R + 1}, pc_[4] = {C, C + 1, C, C + 1};
                    bool                okb = true;
                    for (int k = 0; k < 4; ++k) okb = okb && isbg(pr_[k], pc_[k]);
                    if (!okb) continue;
                    int roots[4], bits = 0;
                    for (int k = 0; k < 4; ++k) {
                        auto f   = uf.find(pt(pr_[k], pc_[k]));
                        roots[k] = f.first;
                        bits |= f.second << k;
                    }
                    if (roots[0] != roots[1] || roots[0] != roots[2] || roots[0] != roots[3]) continue;
                    if (bits == 0 || bits == 15) continue;
                    const Quad mac = {a(2 * R, 2 * C), a(2 * R, 2 * C + 1), a(2 * R + 1, 2 * C), a(2 * R + 1, 2 * C + 1)};
                    auto       it  = comp_ix.find(roots[0]);
                    if (it == comp_ix.end()) {
                        it = comp_ix.emplace(roots[0], comps.size()).first;
                        comps.emplace_back();
                    }
                    comps[it->second].push_back({bits, mac});
                }
            for (auto &c : comps) allm.push_back(std::move(c));
        }
        std::stable_sort(allm.begin(), allm.end(), [](const auto &a, const auto &b) { return a.size() > b.size(); });
        for (const auto &comp : allm) {
            std::int64_t sc = 0;
            for (const auto &[bits, mac] : comp) {
                auto it = votes.find(mac);
                if (it == votes.end()) continue;
                auto b1 = it->second.find(bits), b2 = it->second.find(bits ^ 15);
                sc += (b1 == it->second.end() ? 0 : b1->second) - (b2 == it->second.end() ? 0 : b2->second);
            }
            for (const auto &[bits, mac] : comp) votes[mac][sc >= 0 ? bits : (bits ^ 15)]++;
        }
        const int bgc[4] = {bg, bg, bg, bg};
        for (const auto &[mac, c] : votes) {
            std::int64_t n = 0;
            for (const auto &[bits, k] : c) n += k;
            for (const auto &[bits, k] : c)
                if (std::int64_t(k) * 4 >= n) cases[pack_key(bgc, make_tag(TAG_ORIENT, std::uint8_t(bits)))][mac] += std::uint32_t(k);
        }
    }

    // ---- ramps, clear, cliff
    auto pmaj = [&](std::uint16_t t) {
        const TileRule *tr = r.find_tile(t);
        return tr && tr->pmaj();
    };
    auto          all_pass  = [&](const Quad &q) { return pmaj(q[0]) && pmaj(q[1]) && pmaj(q[2]) && pmaj(q[3]); };
    std::uint64_t ramp_uses = 0;
    for (const auto &[q, n] : rampn) {
        int cs[4];
        classes_of(q, cs);
        if (cs[0] < 0 || cs[1] < 0 || cs[2] < 0 || cs[3] < 0) continue;
        if (cs[0] == cs[1] && cs[0] == cs[2] && cs[0] == cs[3]) continue;
        cases[pack_key(cs, make_tag(TAG_RAMP))][q] += n;
        ramp_uses += n;
    }
    const int bgc[4] = {bg, bg, bg, bg};
    {
        const auto base = cases.find(pack_key(bgc, make_tag(TAG_PLAIN, 0)));
        if (base != cases.end()) {
            std::map<Quad, std::uint32_t> clear;
            for (const auto &[q, n] : base->second)
                if (all_pass(q)) clear[q] = n;
            if (!clear.empty()) cases[pack_key(bgc, make_tag(TAG_CLEAR))] = clear;
        }
    }
    // edge passability share per class set, over the mixed keys (before cliff keys exist)
    std::map<std::set<int>, std::pair<std::uint64_t, std::uint64_t>> share;
    std::uint64_t                                                    mixed_uses = 0;
    for (const auto &[k, c] : cases) {
        const int           kind = (k & 255) >> 4;
        const std::uint32_t cl   = k >> 8;
        const std::set<int> s    = {int(cl >> 12) & 15, int(cl >> 8) & 15, int(cl >> 4) & 15, int(cl) & 15};
        std::uint64_t       sum  = 0;
        for (const auto &[q, n] : c) sum += n;
        if (kind == TAG_RAMP) share[s].first += sum;
        else if (kind == TAG_MIXED) {
            share[s].second += sum;
            mixed_uses += sum;
        }
    }
    {
        std::map<std::uint32_t, std::map<Quad, std::uint32_t>> cliffs;
        for (const auto &[k, c] : cases) {
            if ((k & 255) >> 4 != TAG_MIXED) continue;
            const std::uint32_t cl = k >> 8;
            const std::set<int> s  = {int(cl >> 12) & 15, int(cl >> 8) & 15, int(cl >> 4) & 15, int(cl) & 15};
            const auto          sh = share[s];
            if (sh.second == 0 || sh.first * 20 < sh.second || sh.first * 10 > sh.second * 6) continue; // 5% <= pass share <= 60%
            auto &dst = cliffs[(k & ~255u) | make_tag(TAG_CLIFF)];
            for (const auto &[q, n] : c)
                if (!all_pass(q)) dst[q] += n;
        }
        for (auto &[k, c] : cliffs)
            if (!c.empty()) cases[k] = std::move(c);
    }
    stats[{0, "ramp_share_permille"}] = std::int64_t(mixed_uses ? ramp_uses * 1000 / mixed_uses : 0);

    // open pure ground: keep the all-passable macros of a pure, crossing-free key when they are at least half of its uses
    for (auto &[k, c] : cases) {
        if ((k & 255) != make_tag(TAG_PLAIN, 0)) continue;
        std::uint64_t all = 0, ok = 0;
        for (const auto &[q, n] : c) {
            all += n;
            if (all_pass(q)) ok += n;
        }
        if (ok * 2 < all) continue;
        for (auto it = c.begin(); it != c.end();)
            it = all_pass(it->first) ? std::next(it) : c.erase(it);
    }
    std::set<std::uint16_t> ground;
    {
        const auto base = cases.find(pack_key(bgc, make_tag(TAG_PLAIN, 0)));
        if (base != cases.end())
            for (const auto &[q, n] : base->second)
                for (std::uint16_t t : q) ground.insert(t);
    }
    for (const auto &[k, c] : cases)
        for (const auto &[q, n] : c) {
            CaseRule            cr;
            const std::uint32_t cl = k >> 8;
            cr.cls[0]              = std::uint8_t((cl >> 12) & 15);
            cr.cls[1]              = std::uint8_t((cl >> 8) & 15);
            cr.cls[2]              = std::uint8_t((cl >> 4) & 15);
            cr.cls[3]              = std::uint8_t(cl & 15);
            cr.tag                 = std::uint8_t(k & 255);
            for (int i = 0; i < 4; ++i) cr.t[i] = q[std::size_t(i)];
            cr.n = n;
            r.cases.push_back(cr);
        }

    // ---- stamps: small 8-connected groups of non-ground tiles in a sea of ground
    {
        std::map<std::vector<std::array<std::int32_t, 3>>, std::uint32_t> st;
        std::uint64_t                                                     stamps = 0, area = 0;
        for (const View &v : views) {
            const std::uint32_t N = v.h;
            area += std::uint64_t(v.w) * v.h;
            std::vector<std::uint8_t> g(v.id.size()), seen(v.id.size(), 0);
            for (std::size_t i = 0; i < g.size(); ++i) g[i] = ground.count(v.id[i]) ? 1 : 0;
            for (std::uint32_t y0 = 0; y0 < v.h; ++y0)
                for (std::uint32_t x0 = 0; x0 < v.w; ++x0) {
                    if (g[std::size_t(y0) * v.w + x0] || seen[std::size_t(y0) * v.w + x0]) continue;
                    std::vector<std::pair<std::uint32_t, std::uint32_t>> stack{{y0, x0}}, cells;
                    seen[std::size_t(y0) * v.w + x0] = 1;
                    while (!stack.empty()) {
                        const auto [y, x] = stack.back();
                        stack.pop_back();
                        cells.push_back({y, x});
                        for (int dy = -1; dy <= 1; ++dy)
                            for (int dx = -1; dx <= 1; ++dx) {
                                const std::uint32_t yy = std::uint32_t((std::int32_t(y) + dy + std::int32_t(v.h)) % std::int32_t(v.h)), xx = std::uint32_t((std::int32_t(x) + dx + std::int32_t(v.w)) % std::int32_t(v.w));
                                const std::size_t   o = std::size_t(yy) * v.w + xx;
                                if (!g[o] && !seen[o]) {
                                    seen[o] = 1;
                                    stack.push_back({yy, xx});
                                }
                            }
                    }
                    if (cells.size() > 80) continue;
                    std::uint32_t ymin = ~0u, ymax = 0, xmin = ~0u, xmax = 0;
                    for (const auto &[y, x] : cells) {
                        ymin = std::min(ymin, y);
                        ymax = std::max(ymax, y);
                        xmin = std::min(xmin, x);
                        xmax = std::max(xmax, x);
                    }
                    if (ymax - ymin > 11 || xmax - xmin > 11 || ymax - ymin >= N / 2) continue;
                    std::vector<std::array<std::int32_t, 3>> shape;
                    for (const auto &[y, x] : cells) shape.push_back({std::int32_t(y - ymin), std::int32_t(x - xmin), std::int32_t(v.at(y, x))});
                    std::sort(shape.begin(), shape.end());
                    st[shape]++;
                    ++stamps;
                }
        }
        for (const auto &[shape, n] : st) {
            StampRule s;
            s.n = n;
            for (const auto &c : shape) s.cells.push_back({c[0], c[1], std::uint16_t(c[2])});
            r.stamps.push_back(s);
        }
        stats[{0, "stamp_rate_ppb"}] = std::int64_t(area ? stamps * 1000000000ull / area : 0);

        // ---- decals: small 8-connected groups of RARE ground tiles (< 1% of the passable cells of their pure corner class), isolated
        // in plain tiles of the same class. Stock decorates plain ground this way (cracks, dark marks, pebble clusters).
        {
            const std::size_t         NC = big.size();
            std::vector<int>          pcls(tiles.size(), -1); // pure corner class of a tile, -1 when its corners differ
            std::vector<std::uint8_t> pm(tiles.size(), 0);    // passability majority: a decal must never change passability
            for (std::size_t i = 0; i < tiles.size(); ++i) {
                const int c0 = cls_of(std::int32_t(i), 0);
                if (c0 >= 0 && cls_of(std::int32_t(i), 1) == c0 && cls_of(std::int32_t(i), 2) == c0 && cls_of(std::int32_t(i), 3) == c0) pcls[i] = c0;
                pm[i] = r.tiles[i].pmaj() ? 1 : 0;
            }
            std::vector<std::map<std::uint16_t, std::uint64_t>> cnt(NC);
            std::vector<std::uint64_t>                          total(NC, 0);
            for (const View &v : views)
                for (std::size_t i = 0; i < v.id.size(); ++i) {
                    const int k = pcls[std::size_t(ix[v.id[i]])];
                    if (k < 0 || !v.pas[i]) continue;
                    cnt[std::size_t(k)][v.id[i]]++;
                    total[std::size_t(k)]++;
                }
            auto                                                                              is_rare = [&](int k, std::uint16_t t) { return cnt[std::size_t(k)][t] * 100 < total[std::size_t(k)]; };
            std::map<std::pair<int, std::vector<std::array<std::int32_t, 3>>>, std::uint32_t> dc;
            std::map<std::pair<std::uint32_t, int>, std::uint64_t>                            groups, carea; // (width, class)
            for (const View &v : views) {
                const std::int64_t W = v.w, H = v.h;
                // cls: the class of a rare passable decal-capable cell, -2 plain (non-rare) cell of class k stored as 16 + k, -1 other
                std::vector<std::int16_t> kind(v.id.size(), -1);
                for (std::size_t i = 0; i < kind.size(); ++i) {
                    const int k = pcls[std::size_t(ix[v.id[i]])];
                    if (k < 0 || !v.pas[i]) continue;
                    carea[{v.w, k}]++;
                    carea[{0, k}]++;
                    kind[i] = std::int16_t(is_rare(k, v.id[i]) ? (pm[std::size_t(ix[v.id[i]])] ? k : 32 + k) : 16 + k); // 32+k: rare but blocking-majority tile
                }
                std::vector<std::uint8_t> seen(kind.size(), 0);
                for (std::int64_t y0 = 0; y0 < H; ++y0)
                    for (std::int64_t x0 = 0; x0 < W; ++x0) {
                        const std::size_t i0 = std::size_t(y0 * W + x0);
                        if (seen[i0] || kind[i0] < 0 || kind[i0] >= 16) continue;
                        const int                                          k = kind[i0];
                        std::vector<std::pair<std::int64_t, std::int64_t>> cells, stack{{y0, x0}};
                        seen[i0] = 1;
                        bool ok  = true;
                        while (!stack.empty()) {
                            const auto [y, x] = stack.back();
                            stack.pop_back();
                            cells.push_back({y, x});
                            if (cells.size() > 12) ok = false;
                            const std::int64_t ny[8] = {y + 1, y - 1, y, y, y + 1, y + 1, y - 1, y - 1}, nx[8] = {x, x, x + 1, x - 1, x + 1, x - 1, x + 1, x - 1};
                            for (int d = 0; d < 8; ++d) {
                                if (ny[d] < 0 || ny[d] >= H || nx[d] < 0 || nx[d] >= W) continue;
                                const std::size_t o = std::size_t(ny[d] * W + nx[d]);
                                if (!seen[o] && kind[o] == k) {
                                    seen[o] = 1;
                                    stack.push_back({ny[d], nx[d]});
                                }
                            }
                        }
                        if (!ok) continue;
                        // isolated: every 8-neighbour outside the group is a plain (non-rare) cell of class k, inside the map
                        std::int64_t ymin = H, ymax = -1, xmin = W, xmax = -1;
                        for (const auto &[y, x] : cells) {
                            ymin = std::min(ymin, y);
                            ymax = std::max(ymax, y);
                            xmin = std::min(xmin, x);
                            xmax = std::max(xmax, x);
                        }
                        for (const auto &[y, x] : cells)
                            for (int dy = -1; dy <= 1 && ok; ++dy)
                                for (int dx = -1; dx <= 1; ++dx) {
                                    const std::int64_t yy = y + dy, xx = x + dx;
                                    if (yy < 0 || yy >= H || xx < 0 || xx >= W) {
                                        ok = false;
                                        break;
                                    }
                                    if (std::find(cells.begin(), cells.end(), std::make_pair(yy, xx)) != cells.end()) continue;
                                    if (kind[std::size_t(yy * W + xx)] != 16 + k) {
                                        ok = false;
                                        break;
                                    }
                                }
                        if (!ok || ymax - ymin > 11 || xmax - xmin > 11) continue;
                        std::vector<std::array<std::int32_t, 3>> shape;
                        for (const auto &[y, x] : cells) shape.push_back({std::int32_t(y - ymin), std::int32_t(x - xmin), std::int32_t(v.at(std::uint32_t(y), std::uint32_t(x)))});
                        std::sort(shape.begin(), shape.end());
                        dc[{k, shape}]++;
                        groups[{v.w, k}]++;
                        groups[{0, k}]++;
                    }
            }
            for (const auto &[key, n] : dc) {
                DecalRule d;
                d.cls = key.first;
                d.n   = n;
                for (const auto &c : key.second) d.cells.push_back({c[0], c[1], std::uint16_t(c[2])});
                r.decals.push_back(d);
            }
            for (const auto &[wk, g] : groups) {
                const std::uint64_t a                                                  = carea[wk];
                stats[{wk.first, "decal_rate_c" + std::to_string(wk.second) + "_ppb"}] = std::int64_t(a ? g * 1000000000ull / a : 0);
            }
        }

        // trees and ore
        std::map<std::array<std::int32_t, 4>, std::uint32_t>  ts;
        std::map<std::array<std::uint16_t, 4>, std::uint32_t> ore;
        std::uint64_t                                         trees = 0;
        for (std::size_t vi = 0; vi < views.size(); ++vi) {
            const View     &v = views[vi];
            const MpmModel &m = *inputs[vi].map;
            for (const MpmTree &t : m.trees) {
                const std::uint32_t x = std::uint32_t(t.x) % v.w, y = std::uint32_t(t.y) % v.h;
                ts[{std::int32_t(v.at(y, x)), t.kind, t.px_x, t.px_y}]++;
                ++trees;
            }
            for (std::size_t b = 0; b * ORE_WORDS < m.ore.size(); ++b) {
                std::array<std::uint16_t, 4> rec = {m.ore[b * ORE_WORDS], m.ore[b * ORE_WORDS + 1], m.ore[b * ORE_WORDS + 2], m.ore[b * ORE_WORDS + 3]};
                if (rec[0] || rec[1] || rec[2] || rec[3]) ore[rec]++;
            }
        }
        for (const auto &[k, n] : ts) r.tree_samples.push_back({std::uint16_t(k[0]), k[1], k[2], k[3], n});
        for (const auto &[k, n] : ore) {
            OreRec o;
            for (int i = 0; i < 4; ++i) o.ch[i] = k[std::size_t(i)];
            o.n = n;
            r.ore_recs.push_back(o);
        }
        stats[{0, "tree_ppm"}] = std::int64_t(area ? trees * 1000000 / area : 0);
    }
}

} // namespace mh_mapgen
