// mh_mapgen -- the generator driver: parameter validation, the attempt loop, the hard gates and the description.
#include "mh_mapgen/gen.h"
#include "gen_internal.h"
#include "mh_mapgen/check.h"
#include <algorithm>
#include <chrono>
#include <cstdlib>

namespace mh_mapgen {

namespace {

constexpr std::uint32_t FALLBACK_ATTEMPT = 2; // from this attempt on the landing spots all stay on the background ground

using Clock = std::chrono::steady_clock;
std::uint32_t ms_since(Clock::time_point t0) { return std::uint32_t(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count()); }

std::string lower_ascii(std::string s) {
    for (char &c : s)
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return s;
}

bool tileset_token_valid(const std::string &s) {
    if (s.size() < 5 || s.size() > 12 || s[s.size() - 4] != '.') return false;
    const auto fold = [](char c) { return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c; };
    if (fold(s[s.size() - 3]) != 't' || fold(s[s.size() - 2]) != 'l' || fold(s[s.size() - 1]) != 'o') return false;
    for (std::size_t i = 0; i + 4 < s.size(); ++i) {
        const char c = s[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
    }
    return true;
}

std::string stem_of(const std::string &s) {
    std::string l = lower_ascii(s);
    if (l.size() > 4 && l.compare(l.size() - 4, 4, ".tlo") == 0) l.resize(l.size() - 4);
    return l;
}

std::string clean_line(const std::string &s) {
    std::string o;
    for (char c : s) o += (c >= 32 && c < 127) ? c : '?';
    return o;
}

// name on line 1, then desc word-wrapped to 26 columns; at most 4 lines and MAX_DESC_BYTES bytes in all.
std::string make_description(const GenParams &p, const std::string &tileset) {
    constexpr std::size_t    COLS = 26, LINES = 4;
    std::vector<std::string> lines;
    std::string              name = clean_line(p.name.empty() ? ("gen " + stem_of(tileset) + " " + std::to_string(p.size)) : p.name);
    lines.push_back(name.substr(0, COLS));
    std::string desc = p.desc.empty() ? (std::to_string(p.players) + " players, seed " + std::to_string(p.seed)) : p.desc;
    desc             = clean_line(desc);
    std::string cur;
    std::size_t i = 0;
    while (i < desc.size() && lines.size() < LINES) {
        while (i < desc.size() && desc[i] == ' ') ++i;
        std::size_t j = i;
        while (j < desc.size() && desc[j] != ' ') ++j;
        std::string word = desc.substr(i, j - i);
        i                = j;
        while (!word.empty()) {
            if (!cur.empty() && cur.size() + 1 + word.size() <= COLS) {
                cur += " " + word;
                word.clear();
            } else if (cur.empty() && word.size() <= COLS) {
                cur = word;
                word.clear();
            } else if (cur.empty()) {
                cur = word.substr(0, COLS);
                word.erase(0, COLS);
                lines.push_back(cur);
                cur.clear();
                if (lines.size() >= LINES) break;
            } else {
                lines.push_back(cur);
                cur.clear();
                if (lines.size() >= LINES) break;
            }
        }
    }
    if (!cur.empty() && lines.size() < LINES) lines.push_back(cur);
    auto join = [&]() {
        std::string out;
        for (std::size_t k = 0; k < lines.size(); ++k) out += (k ? "\r\n" : "") + lines[k];
        return out;
    };
    std::string out = join();
    while (out.size() > MAX_DESC_BYTES && lines.size() > 1) {
        lines.pop_back();
        out = join();
    }
    if (out.size() > MAX_DESC_BYTES) out.resize(MAX_DESC_BYTES);
    return out;
}

// the whole-map tree cap for a size: 1.2 x the shipped maximum (area-scaled from the nearest size), never above the slot limit
std::int64_t tree_cap(const Rules &r, std::uint32_t size) {
    static const std::uint32_t order64[3]  = {64, 128, 256};
    static const std::uint32_t order128[3] = {128, 64, 256};
    static const std::uint32_t order256[3] = {256, 128, 64};
    const std::uint32_t       *order       = size <= 64 ? order64 : (size <= 128 ? order128 : order256);
    for (int i = 0; i < 3; ++i) {
        std::int64_t v = 0;
        if (!r.find_stat(order[i], "trees_max", v)) continue;
        if (order[i] != size) v = v * std::int64_t(size) * size / (std::int64_t(order[i]) * order[i]);
        return std::min<std::int64_t>(MAX_TREE_SLOT, std::max<std::int64_t>(16, v * 12 / 10));
    }
    return MAX_TREE_SLOT;
}

} // namespace

std::string gen_validate(const Rules &rules, const GenParams &p) {
    if (rules.tileset.empty() || rules.tiles.empty()) return "empty rules";
    if (!tileset_token_valid(rules.tileset) || !tileset_token_valid(p.tileset)) return "invalid tileset token (expected DOS 8.3 .tlo name)";
    if (stem_of(p.tileset) != stem_of(rules.tileset)) return "tileset " + p.tileset + " does not match the rules (" + rules.tileset + ")";
    if (p.size != 64 && p.size != 128 && p.size != 256) return "size must be 64, 128 or 256";
    if (p.size == 64 && p.players != 2) return "a 64 map takes exactly 2 players";
    if (p.size != 64 && p.players != 4 && p.players != 6 && p.players != 8) return "a 128/256 map takes 4, 6 or 8 players";
    return {};
}

bool landings_connected(const MpmModel &m, std::string &why) {
    const std::uint32_t w = m.width, h = m.height;
    if (m.landings.empty()) {
        why = "no landing spots";
        return false;
    }
    auto free_at = [&](std::int64_t x, std::int64_t y) { return x >= 0 && y >= 0 && x < std::int64_t(w) && y < std::int64_t(h) && m.passable[m.cell(std::uint32_t(x), std::uint32_t(y))] && !m.building[m.cell(std::uint32_t(x), std::uint32_t(y))]; };
    for (std::size_t i = 0; i < m.landings.size(); ++i)
        if (!free_at(m.landings[i].x, m.landings[i].y)) {
            why = "landing spot " + std::to_string(i) + " is not on a passable tree-free tile";
            return false;
        }
    std::vector<std::uint8_t>  seen(m.flags.size(), 0);
    std::vector<std::uint32_t> stack;
    const std::size_t          s0 = m.cell(std::uint32_t(m.landings[0].x), std::uint32_t(m.landings[0].y));
    seen[s0]                      = 1;
    stack.push_back(std::uint32_t(s0));
    static const int dx[4] = {1, -1, 0, 0}, dy[4] = {0, 0, 1, -1};
    while (!stack.empty()) {
        const std::uint32_t c = stack.back();
        stack.pop_back();
        const std::int64_t cx = c / h, cy = c % h;
        for (int d = 0; d < 4; ++d)
            if (free_at(cx + dx[d], cy + dy[d])) {
                const std::size_t nc = m.cell(std::uint32_t(cx + dx[d]), std::uint32_t(cy + dy[d]));
                if (!seen[nc]) {
                    seen[nc] = 1;
                    stack.push_back(std::uint32_t(nc));
                }
            }
    }
    for (std::size_t i = 1; i < m.landings.size(); ++i)
        if (!seen[m.cell(std::uint32_t(m.landings[i].x), std::uint32_t(m.landings[i].y))]) {
            why = "landing spot " + std::to_string(i) + " is not connected to spot 0 (no-wrap BFS)";
            return false;
        }
    return true;
}

bool map_gates(const MpmModel &m, bool ensure_paths, std::string &why, bool need_rich) {
    const std::int64_t w = m.width, h = m.height;
    const std::size_t  ns = m.landings.size();
    if (ns < 2 || ns > 8 || ns % 2) {
        why = "landing spot count " + std::to_string(ns) + " is not even in 2..8";
        return false;
    }
    // no two spots closer than width/6 tiles (torus Chebyshev; stock's closest 8-player 128 pair is 21): bases must not crowd
    const std::int64_t min_dist = std::max(w, h) / MIN_SPOT_DIST_DIV;
    for (std::size_t i = 0; i < ns; ++i)
        for (std::size_t j = i + 1; j < ns; ++j) {
            std::int64_t dx = std::abs(std::int64_t(m.landings[i].x) - m.landings[j].x), dy = std::abs(std::int64_t(m.landings[i].y) - m.landings[j].y);
            dx = std::min(dx, w - dx);
            dy = std::min(dy, h - dy);
            if (std::max(dx, dy) < min_dist) {
                why = "landing spots " + std::to_string(i) + " and " + std::to_string(j) + " are " + std::to_string(std::max(dx, dy)) + " tiles apart (min " + std::to_string(min_dist) + ")";
                return false;
            }
        }
    auto free_tile = [&](std::int64_t x, std::int64_t y) {
        x                   = ((x % w) + w) % w;
        y                   = ((y % h) + h) % h;
        const std::size_t c = m.cell(std::uint32_t(x), std::uint32_t(y));
        return m.passable[c] && !m.building[c];
    };
    for (std::size_t i = 0; i < ns; ++i) {
        const MpmLanding &l   = m.landings[i];
        const std::string tag = "landing spot " + std::to_string(i) + " (" + std::to_string(l.x) + "," + std::to_string(l.y) + ") ";
        if (l.status != 1) {
            why = tag + "status is not 1";
            return false;
        }
        if (l.x < 0 || l.y < 0 || l.x + 4 > w - 1 || l.y + 2 > h - 1) {
            why = tag + "leaves no room for the 5x3 mother footprint inside the map";
            return false;
        }
        for (int dy = 0; dy < 3; ++dy)
            for (int dx = 0; dx < 5; ++dx)
                if (!free_tile(l.x + dx, l.y + dy)) {
                    why = tag + "5x3 mother footprint is blocked";
                    return false;
                }
        int r = 0;
        for (; r <= 12; ++r) {
            bool ring_ok = true;
            for (int dy = -r; dy <= r && ring_ok; ++dy)
                for (int dx = -r; dx <= r; ++dx)
                    if (std::max(std::abs(dx), std::abs(dy)) == r && !free_tile(l.x + dx, l.y + dy)) {
                        ring_ok = false;
                        break;
                    }
            if (!ring_ok) break;
        }
        if (r - 1 < int(MIN_SPOT_CLEAR)) {
            why = tag + "clear radius " + std::to_string(r - 1) + " < " + std::to_string(MIN_SPOT_CLEAR);
            return false;
        }
    }
    // build space: the largest footprint is 6x6 (Baza); each spot needs a share of free 6x6 windows (anchors y-15..y+10, x-15..x+10)
    for (std::size_t i = 0; i < ns; ++i) {
        const MpmLanding &l   = m.landings[i];
        std::int64_t      fit = 0, total = 0;
        for (std::int64_t ay = l.y - 15; ay <= l.y + 10; ++ay)
            for (std::int64_t ax = l.x - 15; ax <= l.x + 10; ++ax) {
                bool ok = true;
                for (std::int64_t dy = 0; dy < 6 && ok; ++dy)
                    for (std::int64_t dx = 0; dx < 6; ++dx)
                        if (!free_tile(ax + dx, ay + dy)) {
                            ok = false;
                            break;
                        }
                fit += ok;
                ++total;
            }
        if (fit * 1000 < std::int64_t(MIN_BUILD_FIT) * total) {
            why = "landing spot " + std::to_string(i) + ": build-fit " + std::to_string(fit * 100 / total) + "% < " + std::to_string(MIN_BUILD_FIT / 10) + "% (free 6x6 windows within 15 tiles)";
            return false;
        }
    }
    // ore: values, channels 4-7, first mine per spot
    const std::int64_t                                 B          = w / 4;
    std::int64_t                                       qualifying = 0, deposits = 0;
    std::vector<std::pair<std::int64_t, std::int64_t>> good; // (br, bc)
    for (std::int64_t bc = 0; bc < B; ++bc)
        for (std::int64_t br = 0; br < B; ++br) {
            const std::uint16_t *o = &m.ore[std::size_t(bc * B + br) * ORE_WORDS];
            for (int k = 0; k < 4; ++k)
                if (o[k] > MAX_ORE_VALUE) {
                    why = "ore value " + std::to_string(o[k]) + " > " + std::to_string(MAX_ORE_VALUE);
                    return false;
                }
            for (int k = 4; k < 8; ++k)
                if (o[k]) {
                    why = "ore channel " + std::to_string(k) + " is not zero";
                    return false;
                }
            if (o[0] || o[1] || o[2] || o[3]) ++deposits;
            if (o[0] && o[1] && o[2] && o[3] && 4 * o[0] + 2 * o[1] + 2 * o[2] + o[3] >= 500) {
                ++qualifying;
                good.push_back({br, bc});
            }
        }
    (void)deposits;
    // no ore block (any channel) on a spot's 5x3 mother footprint: the AI cannot land on or build over it
    for (std::int64_t bc = 0; bc < B; ++bc)
        for (std::int64_t br = 0; br < B; ++br) {
            const std::uint16_t *o = &m.ore[std::size_t(bc * B + br) * ORE_WORDS];
            if (!(o[0] || o[1] || o[2] || o[3])) continue;
            for (std::size_t i = 0; i < ns; ++i) {
                const MpmLanding &l = m.landings[i];
                if (4 * bc <= l.x + 4 && 4 * bc + 3 >= l.x && 4 * br <= l.y + 2 && 4 * br + 3 >= l.y) {
                    why = "landing spot " + std::to_string(i) + ": ore block (" + std::to_string(bc) + ", " + std::to_string(br) + ") overlaps the mother footprint";
                    return false;
                }
            }
        }
    if (qualifying > 1000) {
        why = "more than 1000 first-mine capable ore blocks";
        return false;
    }
    for (std::size_t i = 0; i < ns; ++i) {
        const MpmLanding &l    = m.landings[i];
        std::int64_t      best = 1 << 30;
        for (const auto &[br, bc] : good) {
            std::int64_t dy = 1 << 30, dx = 1 << 30;
            for (int k = 0; k < 4; ++k) {
                std::int64_t a = std::abs(l.y - (br * 4 + k)), b = std::abs(l.x - (bc * 4 + k));
                dy = std::min(dy, std::min(a, h - a));
                dx = std::min(dx, std::min(b, w - b));
            }
            best = std::min(best, std::max(dy, dx));
        }
        if (best > std::int64_t(MAX_FIRST_MINE)) {
            why = "landing spot " + std::to_string(i) + ": first-mine ore is " + std::to_string(best) + " tiles away (max " + std::to_string(MAX_FIRST_MINE) + ")";
            return false;
        }
    }
    // build room (DESIGN sec. 14): a wall ring or ore blocks must not seal the base into a pocket where the AI cannot build a mine
    for (std::size_t i = 0; i < ns; ++i) {
        const MpmLanding &l  = m.landings[i];
        const auto        bt = [&](int c, int r) {
            const std::size_t cell = m.cell(std::uint32_t(c), std::uint32_t(r));
            if (!m.passable[cell] || m.building[cell]) return false;
            const std::uint16_t *o = &m.ore[std::size_t((c >> 2) * int(B) + (r >> 2)) * ORE_WORDS];
            return !(o[0] || o[1] || o[2] || o[3]);
        };
        const detail::BuildRoom br = detail::build_room(bt, int(w), int(l.x), int(l.y));
        if (br.n16 < int(MIN_ROOM16) || br.n32 < int(MIN_ROOM32)) {
            why = "landing spot " + std::to_string(i) + ": build room " + std::to_string(br.n16) + " tiles within 16 steps, " + std::to_string(br.n32) + " within 32 (need " + std::to_string(MIN_ROOM16) + " / " + std::to_string(MIN_ROOM32) + ")";
            return false;
        }
    }
    if (need_rich) { // ore reach: later mines need a rich block near the base, and the first mine a decent one close by
        for (std::size_t i = 0; i < ns; ++i) {
            const MpmLanding  &l  = m.landings[i];
            const std::int64_t cx = l.x + 2, cy = l.y + 1;
            std::int64_t       rich = 1 << 30, goodd = 1 << 30, n8 = 0; // squared tile distances; blocks within 8 tiles
            for (std::int64_t bc = 0; bc < B; ++bc)
                for (std::int64_t br = 0; br < B; ++br) {
                    const std::uint16_t *o = &m.ore[std::size_t(bc * B + br) * ORE_WORDS];
                    if (!(o[0] && o[1] && o[2] && o[3])) continue;
                    const std::int64_t wt = 4 * o[0] + 2 * o[1] + 2 * o[2] + o[3];
                    if (wt < 500) continue;
                    std::int64_t dx = std::abs(cx - (bc * 4 + 2)), dy = std::abs(cy - (br * 4 + 2));
                    dx                    = std::min(dx, w - dx);
                    dy                    = std::min(dy, h - dy);
                    const std::int64_t d2 = dx * dx + dy * dy;
                    goodd                 = std::min(goodd, d2);
                    n8 += d2 <= 64;
                    if (wt >= std::int64_t(ORE_RICH_WEIGHT)) rich = std::min(rich, d2);
                }
            if (rich > std::int64_t(MAX_RICH_REACH * MAX_RICH_REACH)) {
                why = "landing spot " + std::to_string(i) + ": no rich ore block within " + std::to_string(MAX_RICH_REACH) + " tiles";
                return false;
            }
            if (n8 < 2) {
                why = "landing spot " + std::to_string(i) + ": fewer than 2 full-channel ore blocks within 8 tiles";
                return false;
            }
            if (goodd > std::int64_t(MAX_GOOD_REACH_SQ)) {
                why = "landing spot " + std::to_string(i) + ": no full-channel ore block within " + std::to_string(MAX_GOOD_REACH_SQ);
                return false;
            }
        }
    }
    // trees
    if (m.trees.size() > MAX_TREE_SLOT) {
        why = "more than " + std::to_string(MAX_TREE_SLOT) + " trees";
        return false;
    }
    for (std::size_t i = 0; i < m.trees.size(); ++i)
        if (m.trees[i].slot != i + 1 || m.trees[i].slot > MAX_TREE_SLOT) {
            why = "tree slot " + std::to_string(m.trees[i].slot) + " is out of order or above " + std::to_string(MAX_TREE_SLOT);
            return false;
        }
    if (m.tail_desc.size() > MAX_DESC_BYTES) {
        why = "description is " + std::to_string(m.tail_desc.size()) + " bytes (max " + std::to_string(MAX_DESC_BYTES) + ")";
        return false;
    }
    if (ensure_paths && !landings_connected(m, why)) return false;
    return true;
}

GenResult generate(const Rules &rules, const GenParams &p) {
    GenResult res;
    res.reason = gen_validate(rules, p);
    if (!res.reason.empty()) return res;
    detail::Model mdl;
    if (!detail::build_model(rules, mdl, res.reason)) return res;
    if (!p.tune.empty() && !detail::tune_apply(mdl.tune, p.tune, res.reason)) return res;
    const int           N         = int(p.size);
    const std::uint64_t crc_salt  = crc32_ieee(reinterpret_cast<const std::uint8_t *>(mdl.stem.data()), mdl.stem.size()) % 997;
    const std::int64_t  max_trees = tree_cap(rules, p.size);
    const std::string   desc      = make_description(p, rules.tileset);

    for (std::uint32_t att = 0; att < GEN_MAX_ATTEMPTS; ++att) {
        ++res.stats.attempts;
        if (att == FALLBACK_ATTEMPT) mdl.tune.spot_mix = 0; // the late attempts give up child-ground spots: they only need a map that passes
        const std::uint64_t seed = (p.seed * 64 + att) * 131 + crc_salt;
        const std::string   at   = "attempt " + std::to_string(att) + ": ";

        auto                             t0 = Clock::now();
        detail::Plan                     plan;
        std::vector<std::pair<int, int>> lat;
        detail::PlanStats                ps;
        std::string                      why;
        const bool                       planned = detail::make_plan(mdl, p.size, p.players, seed, p.ensure_paths, att < 2, plan, lat, ps, why);
        res.stats.ms_plan += ms_since(t0);
        res.stats.repair_passes += ps.repair_passes;
        res.stats.path_edits += ps.path_edits;
        if (!planned) {
            res.reason = at + "plan: " + why;
            res.log.push_back(res.reason);
            ++res.stats.plan_failures;
            continue;
        }
        std::vector<std::pair<int, int>> spots_xy; // (x = col, y = row)
        for (const auto &s : lat) spots_xy.push_back({2 * s.second + 1, 2 * s.first + 1});

        t0                                                = Clock::now();
        auto                                     solve    = [&](const detail::Placement &pl, std::uint64_t k) { return pl.probe ? pl.probe->solve(seed * 10 + k) : detail::wfc_solve(mdl, plan, N, seed * 10 + k, &pl.fixed); };
        const std::unique_ptr<detail::Placement> placed   = detail::place_stamps(mdl, plan, N, seed, spots_xy, true);
        std::uint32_t                            n_decals = placed->decals;
        detail::WfcResult                        w;
        for (std::uint64_t k = 0; k < 4 && !w.ok; ++k) {
            w = solve(*placed, k);
            res.stats.resets += w.resets;
        }
        if (!w.ok && n_decals > 0) { // the decals do not fit this plan: drop them, never the map
            ++res.stats.decal_drops;
            res.log.push_back(at + "decals dropped: " + w.why + " (resets " + std::to_string(w.resets) + ")");
            n_decals                                             = 0;
            const std::unique_ptr<detail::Placement> stamps_only = detail::place_stamps(mdl, plan, N, seed, spots_xy, false);
            for (std::uint64_t k = 0; k < 4 && !w.ok; ++k) {
                w = solve(*stamps_only, k);
                res.stats.resets += w.resets;
            }
        }
        if (!w.ok) {
            w = detail::wfc_solve(mdl, plan, N, seed * 10 + 9, nullptr);
            res.stats.resets += w.resets;
        }
        res.stats.ms_wfc += ms_since(t0);
        t0 = Clock::now();
        if (!w.ok) {
            res.reason = at + "wfc: " + w.why;
            res.log.push_back(res.reason);
            ++res.stats.wfc_failures;
            continue;
        }

        std::vector<std::uint8_t> pas;
        detail::place_passability(mdl, plan, w.tile, N, pas);
        std::vector<std::int32_t> treeat;
        std::vector<MpmTree>      trees;
        detail::place_trees(mdl, w.tile, N, seed, p.ensure_paths, int(max_trees), spots_xy, pas, treeat, trees);
        std::vector<std::uint16_t> ore;
        detail::place_ore(mdl, pas, treeat, N, seed, spots_xy, ore);

        MpmModel m;
        mpm_init(m, rules.tileset, p.size, p.size);
        for (int r = 0; r < N; ++r)
            for (int c = 0; c < N; ++c) {
                const std::size_t cell = m.cell(std::uint32_t(c), std::uint32_t(r)), i = std::size_t(r * N + c);
                m.flags[cell]    = std::uint16_t(TILE_FLAG_BASE | (mdl.tid[std::size_t(w.tile[i])] & TILE_ID_MASK));
                m.building[cell] = treeat[i] >= 0 ? std::uint16_t(treeat[i] + 1) : 0;
                m.passable[cell] = treeat[i] >= 0 ? 0 : pas[i];
            }
        m.trees     = trees;
        const int B = N / 4;
        for (int br = 0; br < B; ++br)
            for (int bc = 0; bc < B; ++bc)
                for (int k = 0; k < 8; ++k) m.ore[std::size_t(bc * B + br) * ORE_WORDS + std::size_t(k)] = ore[std::size_t(br * B + bc) * 8 + std::size_t(k)];
        for (const auto &s : spots_xy) m.landings.push_back({s.first, s.second, 1});
        mpm_set_tail(m, desc);

        res.stats.ms_place += ms_since(t0);
        // hard gates
        const Report rep = check_map(m, rules, p.wrap);
        if (!rep.ok()) {
            res.reason = at + std::to_string(rep.illegal_pairs.size()) + " illegal pairs";
            res.log.push_back(res.reason);
            ++res.stats.gate_failures;
            continue;
        }
        if (!map_gates(m, p.ensure_paths, why, !mdl.ore_rich.empty())) {
            res.reason = at + why;
            res.log.push_back(res.reason);
            ++res.stats.gate_failures;
            continue;
        }
        std::string err;
        if (!mpm_validate(m, err)) {
            res.reason = "model invalid: " + err;
            return res;
        }
        res.stats.decals     = n_decals;
        res.stats.trees      = std::uint32_t(m.trees.size());
        res.stats.ramps      = ps.ramps;
        std::uint32_t blocks = 0;
        for (std::size_t b = 0; b * ORE_WORDS < m.ore.size(); ++b) blocks += (m.ore[b * ORE_WORDS] | m.ore[b * ORE_WORDS + 1] | m.ore[b * ORE_WORDS + 2] | m.ore[b * ORE_WORDS + 3]) != 0;
        res.stats.ore_blocks = blocks;
        res.ok               = true;
        res.reason           = {};
        res.map              = std::move(m);
        return res;
    }
    res.reason = "gave up after " + std::to_string(GEN_MAX_ATTEMPTS) + " attempts; last: " + res.reason;
    return res;
}

} // namespace mh_mapgen
