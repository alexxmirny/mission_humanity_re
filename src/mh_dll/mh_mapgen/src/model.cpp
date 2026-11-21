// mh_mapgen -- the generator's runtime model: tile universe, adjacency supports, macro cases, stamps, trees, ore.
#include "gen_internal.h"
#include <algorithm>
#include <array>
#include <bit>
#include <map>

namespace mh_mapgen::detail {

constexpr std::uint32_t MAX_TILE_ID = 0x2000;
constexpr int           ORE_CAP     = 3276; // the game reads ore as signed 16-bit after a x10: above this it overflows

Tune tune_for(const std::string &stem) {
    Tune t;
    if (stem == "diuna256") {
        t.reg_div   = 8;
        t.contours  = true; // the main plateau as rank-uniform nested bands (stock has long ridges and rings, blobs looked nothing like it)
        t.spot_mix  = 70;   // stock: 11 of 30 spots stand on the plateau top; only deep interior spots qualify, so more are asked for
        t.cshare    = 170;  // the cleanup and the repair eat band area (scaled down on bigger maps)
        t.decal_pct = 200;  // stock diuna 64/128 carry 8-16% rare ground on class 0 (the 256 stock map only 2%); the plain WFC makes 2%
        t.decal_big = 35;
    } else if (stem == "jungle") {
        t.reg_div   = 8;
        t.decal_pct = 70;
        t.decal_big = 50;
    } else if (stem == "ruins256") {
        t.reg_div      = 4;
        t.line_div     = 16; // ruined building outlines: sparse small blobs of "line" cut out of a mostly-set field
        t.line_q       = 90;
        t.spot_mix     = 100; // stock: 19 of 24 spots stand on dirt (not every candidate spot has dirt enough round it)
        t.child_margin = 1;   // the dirt patches are small: a spot needs less dirt round it
        t.child_pct    = 70;
        t.decal_pct    = 85; // stock ruins carry 9-18% rare ground
        t.decal_big    = 70;
    } else if (stem == "metal256") {
        t.reg_div      = 2;
        t.line_div     = 2;
        t.spot_mix     = 80; // stock: 12 of 24 spots stand on the dark plate; 4-player maps ask for 2 of 2 pairs here, 8-player for 3 of 4 (70 asked only 1 of 2 and landed ~35%, 100 asks for all and lands ~65%)
        t.child_margin = 1;
        t.child_pct    = 80;
        t.decal_pct    = 60; // the learned density is already above the stock mean here
        t.decal_big    = 60;
    } else if (stem == "skaly256") {
        t.line_div  = 16;
        t.decal_big = 55;
    } else if (stem == "ksie256") {
        t.line_div  = 8;
        t.decal_big = 35;
    } else if (stem == "lod256") {
        t.line_div  = 5; // wavelength 12: 230-250 line macros per 10k tiles (stock 234-266)
        t.decal_big = 20;
    }
    return t;
}

bool tune_apply(Tune &t, const std::string &spec, std::string &err) {
    std::size_t pos = 0;
    while (pos < spec.size()) {
        std::size_t end = spec.find(',', pos);
        if (end == std::string::npos) end = spec.size();
        const std::string item = spec.substr(pos, end - pos);
        pos                    = end + 1;
        if (item.empty()) continue;
        const std::size_t eq = item.find('=');
        if (eq == std::string::npos || eq + 1 >= item.size()) {
            err = "bad tune item '" + item + "' (want key=value)";
            return false;
        }
        const std::string key = item.substr(0, eq);
        int               v   = 0;
        for (std::size_t i = eq + 1; i < item.size(); ++i) {
            if (item[i] < '0' || item[i] > '9') {
                err = "bad tune value in '" + item + "'";
                return false;
            }
            v = v * 10 + (item[i] - '0');
        }
        if (key == "reg_div") t.reg_div = std::max(1, v);
        else if (key == "line_div") t.line_div = std::max(1, v);
        else if (key == "line_q") t.line_q = std::max(1, std::min(99, v));
        else if (key == "no_lines") t.no_lines = v != 0;
        else if (key == "contours") t.contours = v != 0;
        else if (key == "contour_div") t.contour_div = std::max(1, v);
        else if (key == "contour_oct") t.contour_oct = std::max(1, v);
        else if (key == "levels") t.levels = std::max(1, v);
        else if (key == "fit") t.fit = v != 0;
        else if (key == "maj") t.maj = v;
        else if (key == "minc") t.minc = std::max(1, v);
        else if (key == "spot_mix") t.spot_mix = std::min(100, v);
        else if (key == "cshare") t.cshare = std::max(1, v);
        else if (key == "cfit") t.cfit = v != 0;
        else if (key == "decal_pct") t.decal_pct = v;
        else if (key == "decal_big") t.decal_big = v;
        else if (key == "wide_cap") t.wide_cap = std::min(100, v);
        else if (key == "child_margin") t.child_margin = v;
        else if (key == "child_pct") t.child_pct = std::min(100, v);
        else {
            err = "unknown tune key '" + key + "'";
            return false;
        }
    }
    return true;
}

void Model::support(const std::uint64_t *dom, int d, std::uint64_t *out) const {
    for (std::uint32_t w = 0; w < W; ++w) out[w] = 0;
    const std::uint64_t *tab = utab[d].data();
    for (std::uint32_t w = 0; w < W; ++w) {
        std::uint64_t bits = dom[w];
        while (bits) {
            const int           b    = std::countr_zero(bits) >> 3; // lowest set byte
            const std::uint64_t byte = (bits >> (8 * b)) & 255u;
            bits &= ~(std::uint64_t(255) << (8 * b));
            const std::uint64_t *row = tab + (std::size_t(w * 8 + std::uint32_t(b)) * 256 + byte) * W;
            for (std::uint32_t k = 0; k < W; ++k) out[k] |= row[k];
        }
    }
}

const std::uint64_t *Model::support_cached(const std::uint64_t *dom, int d) const {
    constexpr std::size_t SLOTS = std::size_t(1) << 15;
    if (sc_key.empty()) {
        sc_key.assign(SLOTS, 0);
        sc_dom.assign(SLOTS * W, 0);
        sc_sup.assign(SLOTS * W, 0);
    }
    std::uint64_t h = 0x9E3779B97F4A7C15ull * std::uint64_t(d + 1);
    for (std::uint32_t w = 0; w < W; ++w) {
        h ^= dom[w];
        h *= 0x100000001B3ull;
        h ^= h >> 29;
    }
    h |= 1; // 0 marks an empty slot
    const std::size_t slot = std::size_t(h >> 20) & (SLOTS - 1);
    std::uint64_t    *cd   = &sc_dom[slot * W];
    std::uint64_t    *cs   = &sc_sup[slot * W];
    if (sc_key[slot] == h) {
        bool same = true;
        for (std::uint32_t w = 0; w < W && same; ++w) same = cd[w] == dom[w];
        if (same) {
            return cs;
        }
    }
    support(dom, d, cs);
    for (std::uint32_t w = 0; w < W; ++w) cd[w] = dom[w];
    sc_key[slot] = h;
    return cs;
}

std::int32_t Model::find_case(std::uint32_t key) const {
    auto it = std::lower_bound(case_key.begin(), case_key.end(), key);
    return (it != case_key.end() && *it == key) ? case_idx[std::size_t(it - case_key.begin())] : -1;
}

std::int32_t Model::info_of(std::uint32_t key) const {
    const std::int32_t i = find_case(key);
    if (i >= 0) return i;
    const std::uint32_t sig = key_sig(key);
    auto                it  = std::lower_bound(fb_sig.begin(), fb_sig.end(), sig);
    if (it != fb_sig.end() && *it == sig) return fb_idx[std::size_t(it - fb_sig.begin())];
    return empty_info;
}

const std::uint64_t *Model::info_support(std::int32_t info, int q, int d) const {
    const CaseInfo &ci = infos[std::size_t(info)];
    const int       s  = q * 4 + d;
    if (!(ci.sup_done & (1u << s))) {
        ci.sup[s].assign(W, 0);
        support(ci.dom[q].data(), d, ci.sup[s].data());
        ci.sup_done = std::uint16_t(ci.sup_done | (1u << s));
    }
    return ci.sup[s].data();
}

namespace {

using Acc = std::map<std::uint32_t, std::uint32_t>; // tile index -> uses

void finish_info(CaseInfo &ci, const Acc acc[4], std::uint32_t W) {
    for (int q = 0; q < 4; ++q) {
        ci.dom[q].assign(W, 0);
        ci.wt[q].clear();
        for (const auto &[t, n] : acc[q]) {
            ci.dom[q][t / 64] |= std::uint64_t(1) << (t % 64);
            ci.wt[q].push_back({t, n});
        }
    }
}

} // namespace

bool build_model(const Rules &r, Model &m, std::string &err) {
    if (!validate_rules(r, err)) return false;
    m       = Model{};
    m.rules = &r;
    m.stem  = r.tileset;
    if (m.stem.size() > 4 && m.stem.compare(m.stem.size() - 4, 4, ".tlo") == 0) m.stem.resize(m.stem.size() - 4);
    for (char &c : m.stem)
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    m.tune = tune_for(r.gameplay_profile.empty() ? m.stem : r.gameplay_profile);

    const std::size_t nt = r.tiles.size();
    m.T                  = std::uint32_t(nt);
    m.W                  = (m.T + 63) / 64;
    m.idx_of.assign(MAX_TILE_ID, -1);
    for (std::size_t i = 0; i < nt; ++i) {
        const TileRule &t                  = r.tiles[i];
        m.idx_of[t.id & (MAX_TILE_ID - 1)] = std::int32_t(i);
        m.tid.push_back(t.id);
        m.pmaj.push_back(t.pmaj() ? 1 : 0);
        m.pseen_pass.push_back((t.seen & 2) ? 1 : 0);
    }
    const std::size_t W = m.W;
    for (auto &s : m.sup) s.assign(nt * W, 0);
    auto set = [&](int dir, std::int32_t a, std::int32_t b) { m.sup[dir][std::size_t(a) * W + std::size_t(b) / 64] |= std::uint64_t(1) << (b % 64); };
    for (const PairRule &p : r.h) {
        const std::int32_t a = m.idx_of[p.a & (MAX_TILE_ID - 1)], b = m.idx_of[p.b & (MAX_TILE_ID - 1)];
        if (a < 0 || b < 0) continue;
        set(0, a, b);
        set(1, b, a);
    }
    for (const PairRule &p : r.v) {
        const std::int32_t a = m.idx_of[p.a & (MAX_TILE_ID - 1)], b = m.idx_of[p.b & (MAX_TILE_ID - 1)];
        if (a < 0 || b < 0) continue;
        set(2, a, b);
        set(3, b, a);
    }
    {
        const std::size_t nchunk = (nt + 7) / 8;
        for (int d = 0; d < 4; ++d) {
            m.utab[d].assign(nchunk * 256 * W, 0);
            for (std::size_t q = 0; q < nchunk; ++q)
                for (std::size_t pat = 1; pat < 256; ++pat) {
                    std::uint64_t       *o    = &m.utab[d][(q * 256 + pat) * W];
                    const std::uint64_t *prev = &m.utab[d][(q * 256 + (pat & (pat - 1))) * W];
                    const std::size_t    tile = q * 8 + std::size_t(std::countr_zero(pat));
                    for (std::size_t w = 0; w < W; ++w) o[w] = prev[w] | (tile < nt ? m.sup[d][tile * W + w] : 0);
                }
        }
    }

    // ---- classes and macro cases
    std::map<std::uint32_t, std::vector<std::size_t>> by_key;
    m.synthetic = r.classes.empty() || r.cases.empty();
    std::vector<std::uint32_t> keys_sorted;
    if (!m.synthetic) {
        int maxc = 0;
        for (const ClassRule &c : r.classes) maxc = std::max(maxc, c.id);
        m.ncls = maxc + 1;
        m.parent.assign(std::size_t(m.ncls), -1);
        m.share_ppm.assign(std::size_t(m.ncls), 0);
        m.adjm.assign(std::size_t(m.ncls), 0);
        std::vector<std::pair<int, int>> ord;
        for (const ClassRule &c : r.classes) {
            m.parent[std::size_t(c.id)]    = c.parent;
            m.share_ppm[std::size_t(c.id)] = c.share_ppm;
            if (c.order >= 0) ord.push_back({c.order, c.id});
        }
        std::sort(ord.begin(), ord.end());
        for (const auto &o : ord) m.order.push_back(o.second);
        if (m.order.empty()) {
            err = "the rules have no region classes";
            return false;
        }
        m.bg = m.order[0];
        for (const AdjRule &a : r.adj) {
            m.adjm[std::size_t(a.a)] = std::uint16_t(m.adjm[std::size_t(a.a)] | (1u << a.b));
            m.adjm[std::size_t(a.b)] = std::uint16_t(m.adjm[std::size_t(a.b)] | (1u << a.a));
        }
        m.sub_ppm = m.share_ppm;
        for (std::size_t i = m.order.size(); i-- > 0;) {
            const int k = m.order[i];
            if (m.parent[std::size_t(k)] >= 0) m.sub_ppm[std::size_t(m.parent[std::size_t(k)])] += m.sub_ppm[std::size_t(k)];
        }
        for (std::size_t i = 0; i < r.cases.size(); ++i) {
            const CaseRule    &c     = r.cases[i];
            const std::uint8_t cs[4] = {c.cls[0], c.cls[1], c.cls[2], c.cls[3]};
            by_key[make_key(cs, c.tag)].push_back(i);
        }
        std::map<std::uint32_t, Acc[4]> fb;
        m.ramp_sig.assign(65536, 0);
        std::vector<std::int64_t> pas_n(std::size_t(m.ncls), 0), pas_p(std::size_t(m.ncls), 0);
        for (const auto &[key, idxs] : by_key) {
            Acc acc[4];
            for (std::size_t i : idxs) {
                const CaseRule &c = r.cases[i];
                for (int q = 0; q < 4; ++q) {
                    const std::int32_t t = m.idx_of[c.t[q] & (MAX_TILE_ID - 1)];
                    if (t < 0) continue;
                    acc[q][std::uint32_t(t)] += c.n;
                    fb[key_sig(key)][q][std::uint32_t(t)] += c.n;
                }
            }
            CaseInfo ci;
            finish_info(ci, acc, m.W);
            m.case_key.push_back(key);
            m.case_idx.push_back(std::int32_t(m.infos.size()));
            m.infos.push_back(std::move(ci));
            if (key_tag(key) >> 4 == TAG_RAMP) m.ramp_sig[key_sig(key)] = 1;
            if (key_tag(key) == make_tag(TAG_PLAIN, 0) && key_pure(key)) {
                const int k = key_cls(key, 0);
                for (std::size_t i : idxs) {
                    const CaseRule &c   = r.cases[i];
                    bool            all = true;
                    for (int q = 0; q < 4; ++q) {
                        const std::int32_t t = m.idx_of[c.t[q] & (MAX_TILE_ID - 1)];
                        all                  = all && t >= 0 && m.pmaj[std::size_t(t)];
                    }
                    pas_n[std::size_t(k)] += c.n;
                    if (all) pas_p[std::size_t(k)] += c.n;
                }
            }
        }
        for (auto &[sig, acc] : fb) {
            CaseInfo ci;
            finish_info(ci, acc, m.W);
            m.fb_sig.push_back(sig);
            m.fb_idx.push_back(std::int32_t(m.infos.size()));
            m.infos.push_back(std::move(ci));
        }
        m.pass_class.assign(std::size_t(m.ncls), 0);
        for (int k = 0; k < m.ncls; ++k) m.pass_class[std::size_t(k)] = pas_n[std::size_t(k)] > 0 && pas_p[std::size_t(k)] * 2 > pas_n[std::size_t(k)];
    } else {
        // hand-made rules: one class; the plain case may use every tile, the clearing case only passable ones
        m.ncls = 1;
        m.bg   = 0;
        m.order.push_back(0);
        m.parent.assign(1, -1);
        m.share_ppm.assign(1, 1000000);
        m.sub_ppm.assign(1, 1000000);
        m.adjm.assign(1, 0);
        m.pass_class.assign(1, 1);
        m.ramp_sig.assign(65536, 0);
        Acc all[4], pass[4];
        for (std::uint32_t t = 0; t < m.T; ++t)
            for (int q = 0; q < 4; ++q) {
                all[q][t] = std::max<std::uint32_t>(1, std::min<std::uint32_t>(r.tiles[t].freq, 20000));
                if (m.pmaj[t]) pass[q][t] = all[q][t];
            }
        const std::uint8_t z[4] = {0, 0, 0, 0};
        CaseInfo           a, c;
        finish_info(a, all, m.W);
        finish_info(c, pass, m.W);
        m.case_key.push_back(make_key(z, make_tag(TAG_PLAIN, 0)));
        m.case_idx.push_back(0);
        m.infos.push_back(std::move(a));
        m.case_key.push_back(make_key(z, make_tag(TAG_CLEAR)));
        m.case_idx.push_back(1);
        m.infos.push_back(std::move(c));
    }
    {
        Acc      none[4];
        CaseInfo e;
        finish_info(e, none, m.W);
        m.empty_info = std::int32_t(m.infos.size());
        m.infos.push_back(std::move(e));
    }

    // ---- learned scalars
    std::int64_t v  = 0;
    m.line_ppm      = r.stat_or(0, "line_share_ppm", 0);
    m.ramp_permille = r.stat_or(0, "ramp_share_permille", 0);
    m.stamp_ppb     = r.stat_or(0, "stamp_rate_ppb", 0);
    if (r.find_stat(0, "tree_ppm", v)) m.tree_ppm = v;
    else {
        std::int64_t lo = r.stat_or(64, "trees_per10k_min", 0), hi = r.stat_or(64, "trees_per10k_max", 0);
        m.tree_ppm = (lo + hi) * 100 / 2;
    }
    m.trees.assign(nt, {});
    for (const TreeSample &s : r.tree_samples) {
        const std::int32_t t = m.idx_of[s.tile & (MAX_TILE_ID - 1)];
        if (t >= 0) m.trees[std::size_t(t)].push_back({s.kind, s.px, s.py, s.n});
    }
    if (r.tree_samples.empty())
        for (std::size_t t = 0; t < nt; ++t)
            if (r.tiles[t].treeok)
                for (const KindRule &k : r.kinds)
                    if (k.n > 0) m.trees[t].push_back({k.kind, 16, 16, k.n});

    for (const StampRule &s : r.stamps) {
        StampModel sm;
        sm.n    = s.n;
        bool ok = true;
        for (const StampCell &c : s.cells) {
            const std::int32_t t = m.idx_of[c.tile & (MAX_TILE_ID - 1)];
            if (t < 0) ok = false;
            sm.cells.push_back({{c.y, c.x}, t});
            sm.h = std::max(sm.h, c.y + 1);
            sm.w = std::max(sm.w, c.x + 1);
        }
        if (!ok || sm.cells.empty()) continue;
        m.stamp_total += sm.n;
        m.stamps.push_back(std::move(sm));
    }

    m.decals_of.assign(std::size_t(std::max(m.ncls, 1)), {});
    m.decal_total.assign(std::size_t(std::max(m.ncls, 1)), 0);
    for (const DecalRule &d : r.decals) {
        if (d.cls < 0 || d.cls >= m.ncls) continue;
        DecalModel dm;
        dm.cls  = d.cls;
        dm.n    = d.n;
        bool ok = true;
        for (const StampCell &c : d.cells) {
            const std::int32_t t = m.idx_of[c.tile & (MAX_TILE_ID - 1)];
            if (t < 0 || !m.pmaj[std::size_t(t)]) ok = false; // a decal never changes passability
            dm.cells.push_back({{c.y, c.x}, t});
            dm.h = std::max(dm.h, c.y + 1);
            dm.w = std::max(dm.w, c.x + 1);
        }
        if (!ok || dm.cells.empty()) continue;
        m.decal_total[std::size_t(d.cls)] += dm.n;
        m.decals_of[std::size_t(d.cls)].push_back(std::uint32_t(m.decals.size()));
        m.decals.push_back(std::move(dm));
    }

    for (const OreRec &o : r.ore_recs) {
        std::array<std::uint16_t, 4> v4;
        for (int i = 0; i < 4; ++i) v4[std::size_t(i)] = std::uint16_t(std::min<int>(o.ch[i], ORE_CAP));
        for (std::uint32_t k = 0; k < o.n; ++k) {
            m.ore_all.push_back(v4);
            if (v4[0] > 0 && v4[1] > 0 && v4[2] > 0 && v4[3] > 0 && 4 * v4[0] + 2 * v4[1] + 2 * v4[2] + v4[3] >= 500) m.ore_good.push_back(v4);
            if (v4[0] > 0 && v4[1] > 0 && v4[2] > 0 && v4[3] > 0 && 4 * v4[0] + 2 * v4[1] + 2 * v4[2] + v4[3] >= int(ORE_RICH_WEIGHT)) m.ore_rich.push_back(v4);
        }
    }
    if (m.ore_good.empty()) m.ore_good.push_back({500, 300, 300, 300});
    if (m.ore_all.empty()) m.ore_all = m.ore_good;
    return true;
}

} // namespace mh_mapgen::detail
