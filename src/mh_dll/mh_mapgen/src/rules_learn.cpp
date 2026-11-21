// mh_mapgen -- rule learner: tile table, adjacency pairs, tree kinds and per-size statistics from
// a set of shipped maps. Integer only; std::map keeps every output ordering independent of input order.
#include "mh_mapgen/rules.h"
#include <algorithm>
#include <map>
#include <set>

namespace mh_mapgen {

// model_learn.cpp: fills r's macro model (classes, cases, stamps, trees, ore) and adds size-0 stats to `stats`.
void learn_macro_model(const std::vector<LearnInput> &inputs, Rules &r, std::map<std::pair<std::uint32_t, std::string>, std::int64_t> &stats);

namespace {

struct TileAcc {
    std::uint32_t freq = 0, pass = 0, block = 0;
    bool          treeok = false;
};

struct Agg {
    std::int64_t n = 0, mn = 0, mx = 0, sum = 0;
    void         add(std::int64_t v) {
        if (n == 0 || v < mn) mn = v;
        if (n == 0 || v > mx) mx = v;
        sum += v;
        ++n;
    }
};

using StatKey = std::pair<std::uint32_t, std::string>;

std::int64_t block_total(const MpmModel &m, std::size_t block) {
    std::int64_t t = 0;
    for (std::size_t c = 0; c < 4; ++c) t += m.ore[block * ORE_WORDS + c];
    return t;
}

constexpr std::int64_t ORE_NEAR_R2 = 20 * 20; // squared tile distance from a landing spot to a block centre

} // namespace

Rules learn_tileset(const std::vector<LearnInput> &inputs_in) {
    // Order- and duplicate-independent: sort by sha256, keep the first of each.
    std::vector<LearnInput> inputs = inputs_in;
    std::stable_sort(inputs.begin(), inputs.end(), [](const LearnInput &a, const LearnInput &b) { return a.sha256 < b.sha256; });
    inputs.erase(std::unique(inputs.begin(), inputs.end(), [](const LearnInput &a, const LearnInput &b) { return a.sha256 == b.sha256; }), inputs.end());

    Rules r;
    if (!inputs.empty()) r.tileset = inputs[0].map->tileset();
    r.maps = std::uint32_t(inputs.size());

    std::string list;
    for (const LearnInput &in : inputs) list += in.sha256 + "\n";
    r.inputs_sha256 = sha256_hex(reinterpret_cast<const std::uint8_t *>(list.data()), list.size());

    std::map<std::uint16_t, TileAcc>       tiles;
    std::map<std::uint32_t, std::uint32_t> hp, vp;
    std::map<std::int32_t, std::uint32_t>  kinds;
    std::map<StatKey, Agg>                 aggs;
    std::map<std::uint32_t, std::int64_t>  map_counts;

    for (const LearnInput &in : inputs) {
        const MpmModel     &m = *in.map;
        const std::uint32_t w = m.width, h = m.height;
        const std::size_t   n    = std::size_t(w) * h;
        auto                stat = [&](const char *key, std::int64_t v) { aggs[{w, key}].add(v); };
        map_counts[w]++;

        std::vector<std::uint8_t> has_tree(n, 0);
        for (const MpmTree &t : m.trees) {
            kinds[t.kind]++;
            if (t.x >= 0 && t.y >= 0 && std::uint32_t(t.x) < w && std::uint32_t(t.y) < h) has_tree[m.cell(std::uint32_t(t.x), std::uint32_t(t.y))] = 1;
        }
        for (std::size_t i = 0; i < n; ++i)
            if (m.building[i]) has_tree[i] = 1;

        std::int64_t blocked = 0;
        for (std::size_t i = 0; i < n; ++i) {
            TileAcc &a = tiles[m.flags[i] & TILE_ID_MASK];
            a.freq++;
            if (has_tree[i]) a.treeok = true;
            else if (m.passable[i]) a.pass++;
            else a.block++;
            if (!m.passable[i]) ++blocked;
        }
        for (std::uint32_t c = 0; c < w; ++c)
            for (std::uint32_t rw = 0; rw < h; ++rw) {
                std::uint16_t a = m.tile_at(c, rw);
                if (c + 1 < w) hp[(std::uint32_t(a) << 16) | m.tile_at(c + 1, rw)]++;
                if (rw + 1 < h) vp[(std::uint32_t(a) << 16) | m.tile_at(c, rw + 1)]++;
            }

        stat("blocked_permille", blocked * 1000 / std::int64_t(n));
        stat("trees", std::int64_t(m.trees.size()));
        stat("trees_per10k", std::int64_t(m.trees.size()) * 10000 / std::int64_t(n));
        stat("landings", std::int64_t(m.landings.size()));

        const std::uint32_t bw = w / 4, bh = h / 4;
        std::int64_t        ore_blocks = 0, ore_total = 0;
        for (std::size_t b = 0; b < std::size_t(bw) * bh; ++b) {
            std::int64_t t = block_total(m, b);
            if (t) ++ore_blocks;
            ore_total += t;
        }
        stat("ore_blocks", ore_blocks);
        stat("ore_total", ore_total);

        for (const MpmLanding &l : m.landings) {
            std::int64_t near_blocks = 0, near_total = 0;
            for (std::uint32_t bc = 0; bc < bw; ++bc)
                for (std::uint32_t br = 0; br < bh; ++br) {
                    std::int64_t dx = std::int64_t(bc) * 4 + 2 - l.x, dy = std::int64_t(br) * 4 + 2 - l.y;
                    if (dx * dx + dy * dy > ORE_NEAR_R2) continue;
                    std::int64_t t = block_total(m, std::size_t(bc) * bh + br);
                    if (!t) continue;
                    ++near_blocks;
                    near_total += t;
                    stat("ore_near_amt", t);
                }
            stat("ore_near_blocks", near_blocks);
            stat("ore_near_total", near_total);
        }
    }

    for (const auto &[id, a] : tiles) {
        TileRule t;
        t.id     = id;
        t.freq   = a.freq;
        t.pass   = (a.pass + a.block == 0) ? 1 : (a.pass > a.block ? 1 : 0);
        t.mixed  = (a.pass && a.block) ? 1 : 0;
        t.treeok = a.treeok ? 1 : 0;
        t.seen   = std::uint8_t((a.block ? 1 : 0) | (a.pass ? 2 : 0));
        r.tiles.push_back(t);
    }
    for (const auto &[k, n] : hp) r.h.push_back({std::uint16_t(k >> 16), std::uint16_t(k & 0xffff), n});
    for (const auto &[k, n] : vp) r.v.push_back({std::uint16_t(k >> 16), std::uint16_t(k & 0xffff), n});
    for (const auto &[k, n] : kinds) r.kinds.push_back({k, n});

    std::map<StatKey, std::int64_t> flat;
    for (const auto &[w, c] : map_counts) flat[{w, "maps"}] = c;
    for (const auto &[k, a] : aggs) {
        flat[{k.first, k.second + "_max"}] = a.mx;
        flat[{k.first, k.second + "_min"}] = a.mn;
        flat[{k.first, k.second + "_n"}]   = a.n;
        flat[{k.first, k.second + "_sum"}] = a.sum;
    }
    // whole-tileset statistics (size 0) of the macro model
    learn_macro_model(inputs, r, flat);
    for (const auto &[k, v] : flat) r.stats.push_back({k.first, k.second, v});
    return r;
}

std::vector<Rules> learn_all(const std::vector<LearnInput> &inputs) {
    std::map<std::string, std::vector<LearnInput>> by_set;
    for (const LearnInput &in : inputs) by_set[in.map->tileset()].push_back(in);
    std::vector<Rules> out;
    for (const auto &[name, v] : by_set) out.push_back(learn_tileset(v));
    return out;
}

} // namespace mh_mapgen
