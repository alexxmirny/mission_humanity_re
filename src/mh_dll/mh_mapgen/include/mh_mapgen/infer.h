// Infer directional tile compatibility from decoded RGB565 art and explicit author semantics.
#pragma once
#include "mh_mapgen/calibrate.h"
#include <algorithm>
#include <array>
#include <string>
#include <utility>
#include <vector>

namespace mh_mapgen {
struct SeamRefinement {
    char          dir;
    std::uint16_t a, b;
    bool          allow;
};
struct InferenceOptions {
    std::uint32_t               horizontal_limit, vertical_limit;
    std::vector<SeamRefinement> refinements;
};
struct InferenceReport {
    std::size_t horizontal = 0, vertical = 0, added = 0, removed = 0;
};

// The template is authored semantic data, not a learned adjacency source: h/v must be empty.
// Corners are NW, NE, SW, SE, completely assigned by the author. Cases, walkability,
// tuning, ore and decoration are also explicit; color never decides gameplay behavior.
inline bool infer_rules(const std::uint8_t *art, std::size_t size, const Rules &semantic,
                        const InferenceOptions &options, Rules &out, InferenceReport &report, std::string &err) {
    report    = InferenceReport{};
    auto fail = [&](const std::string &s) { err = s; return false; };
    if (!semantic.h.empty() || !semantic.v.empty()) return fail("semantic template must not contain h/v adjacency");
    if (semantic.gameplay_profile.empty() || semantic.tree_profile.empty()) return fail("explicit gameplay and tree profiles required");
    if (!validate_rules(semantic, err)) return false;
    const std::size_t n = semantic.tiles.size();
    if (n < 2 || semantic.corners.size() != n) return fail("at least two active tiles and one complete corner record per tile required");
    if (semantic.ore_recs.empty()) return fail("author ore records required; implicit ore fallback refused");
    bool clear = false, plain = false;
    for (const auto &c : semantic.cases) {
        clear |= c.tag == make_tag(TAG_CLEAR);
        plain |= c.tag == make_tag(TAG_PLAIN);
    }
    if (!clear || !plain) return fail("author macro cases require plain x0 and clear entries");
    std::array<int, 4096> index;
    index.fill(-1);
    for (std::size_t i = 0; i < n; ++i) {
        if (semantic.corners[i].tile != semantic.tiles[i].id) return fail("every active tile needs an ordered corner record");
        index[semantic.tiles[i].id] = int(i);
        for (auto c : semantic.corners[i].cls)
            if (c < 0) return fail("ambiguous corner class requires an explicit author refinement");
    }
    std::vector<std::uint16_t> pixels;
    if (!decode_tlo565(art, size, semantic, pixels, err)) return false;
    std::vector<std::uint8_t> h(n * n, 0), v(n * n, 0);
    auto                      compatible = [&](std::size_t a, std::size_t b, bool down) {
        const auto &x = semantic.corners[a], &y = semantic.corners[b];
        return down ? x.cls[2] == y.cls[0] && x.cls[3] == y.cls[1]
                                         : x.cls[1] == y.cls[0] && x.cls[3] == y.cls[2];
    };
    for (std::size_t a = 0; a < n; ++a)
        for (std::size_t b = 0; b < n; ++b) {
            const auto *ap = pixels.data() + a * 1024, *bp = pixels.data() + b * 1024;
            if (compatible(a, b, false) && pixel_seam_score(ap, bp, false) <= options.horizontal_limit) h[a * n + b] = 1;
            if (compatible(a, b, true) && pixel_seam_score(ap, bp, true) <= options.vertical_limit) v[a * n + b] = 1;
        }
    const std::size_t initial_h = std::size_t(std::count(h.begin(), h.end(), std::uint8_t(1)));
    const std::size_t initial_v = std::size_t(std::count(v.begin(), v.end(), std::uint8_t(1)));
    if (!initial_h || !initial_v || initial_h == n * n || initial_v == n * n) return fail("cutoffs produce an empty or all-pairs direction; refine thresholds or semantics");
    if (options.refinements.size() > 256 || options.refinements.size() > std::size_t(initial_h + initial_v) / 4 + 1) return fail("too many seam refinements; refine pixel thresholds or semantics");
    std::array<std::uint32_t, 256> keys{};
    for (std::size_t i = 0; i < options.refinements.size(); ++i) {
        const auto &ref = options.refinements[i];
        if ((ref.dir != 'h' && ref.dir != 'v') || ref.a >= 4096 || ref.b >= 4096 || index[ref.a] < 0 || index[ref.b] < 0) return fail("invalid seam refinement reference");
        keys[i] = (std::uint32_t(ref.dir == 'v') << 24) | (std::uint32_t(ref.a) << 12) | ref.b;
    }
    std::sort(keys.begin(), keys.begin() + options.refinements.size());
    if (std::adjacent_find(keys.begin(), keys.begin() + options.refinements.size()) != keys.begin() + options.refinements.size()) return fail("duplicate seam refinement");
    for (const auto &ref : options.refinements) {
        const std::size_t a = std::size_t(index[ref.a]), b = std::size_t(index[ref.b]), k = a * n + b;
        auto             &dst = ref.dir == 'h' ? h : v;
        if (!compatible(a, b, ref.dir == 'v')) return fail("incompatible semantic corners");
        if (dst[k] == std::uint8_t(ref.allow)) return fail("redundant seam refinement");
        dst[k] = std::uint8_t(ref.allow);
        if (ref.allow) ++report.added;
        else ++report.removed;
    }
    // An authored stamp is an indivisible foreground composite in inferred rules.
    // CaseInfo domains are per-quadrant; without these directional constraints the
    // same tiles could otherwise escape from a stamp and appear as orphan fragments.
    std::array<int, 4096> composite_owner;
    composite_owner.fill(-1);
    for (std::size_t si = 0; si < semantic.stamps.size(); ++si) {
        for (const auto &c : semantic.stamps[si].cells) {
            if (composite_owner[c.tile] >= 0) return fail("ambiguous composite: tile reused at multiple stamp positions");
            composite_owner[c.tile] = int(si);
        }
    }
    for (const auto &stamp : semantic.stamps) {
        std::vector<std::pair<int, std::size_t>> at;
        at.reserve(stamp.cells.size());
        for (std::size_t i = 0; i < stamp.cells.size(); ++i)
            at.push_back({stamp.cells[i].y * 256 + stamp.cells[i].x, i});
        std::sort(at.begin(), at.end());
        const auto neighbor = [&](int y, int x) -> int {
            if (y < 0 || y >= 256 || x < 0 || x >= 256) return -1;
            const int  k  = y * 256 + x;
            const auto it = std::lower_bound(at.begin(), at.end(), k, [](const auto &entry, int key) { return entry.first < key; });
            return it != at.end() && it->first == k ? int(it->second) : -1;
        };
        // Disconnected components cannot be enforced atomically through local edges.
        std::vector<std::uint8_t> visited(stamp.cells.size(), 0);
        std::vector<std::size_t>  todo;
        todo.reserve(stamp.cells.size());
        todo.push_back(0);
        visited[0] = 1;
        for (std::size_t i = 0; i < todo.size(); ++i) {
            const auto &c     = stamp.cells[todo[i]];
            const int   yy[4] = {c.y, c.y, c.y + 1, c.y - 1};
            const int   xx[4] = {c.x + 1, c.x - 1, c.x, c.x};
            for (int d = 0; d < 4; ++d) {
                const int j = neighbor(yy[d], xx[d]);
                if (j >= 0 && !visited[std::size_t(j)]) {
                    visited[std::size_t(j)] = 1;
                    todo.push_back(std::size_t(j));
                }
            }
        }
        if (todo.size() != stamp.cells.size()) return fail("disconnected composite stamp cannot be enforced");
        for (const auto &c : stamp.cells) {
            const std::size_t a     = std::size_t(index[c.tile]);
            const int         yy[4] = {c.y, c.y, c.y + 1, c.y - 1};
            const int         xx[4] = {c.x + 1, c.x - 1, c.x, c.x};
            for (int d = 0; d < 4; ++d) {
                const int j        = neighbor(yy[d], xx[d]);
                const int required = j < 0 ? -1 : index[stamp.cells[std::size_t(j)].tile];
                bool      any      = false;
                for (std::size_t b = 0; b < n; ++b) {
                    std::uint8_t &pair      = d == 0 ? h[a * n + b] : d == 1 ? h[b * n + a]
                                                                  : d == 2   ? v[a * n + b]
                                                                             : v[b * n + a];
                    const bool    permitted = required >= 0 ? b == std::size_t(required) : composite_owner[semantic.tiles[b].id] < 0;
                    if (!permitted) pair = 0;
                    any = any || pair != 0;
                }
                if (!any) return fail("composite edge has no pixel-supported neighbor: tile " + std::to_string(c.tile) + " direction " + std::to_string(d));
            }
        }
    }
    out = semantic;
    for (std::size_t a = 0; a < n; ++a)
        for (std::size_t b = 0; b < n; ++b) {
            if (h[a * n + b]) out.h.push_back({semantic.tiles[a].id, semantic.tiles[b].id, 1});
            if (v[a * n + b]) out.v.push_back({semantic.tiles[a].id, semantic.tiles[b].id, 1});
        }
    report.horizontal = out.h.size();
    report.vertical   = out.v.size();
    if (!report.horizontal || !report.vertical || report.horizontal == n * n || report.vertical == n * n) return fail("refinements produce empty or all-pairs direction");
    for (const auto &c : out.cases) {
        const int a = index[c.t[0]], b = index[c.t[1]], d = index[c.t[2]], e = index[c.t[3]];
        if (!h[std::size_t(a) * n + b] || !h[std::size_t(d) * n + e] || !v[std::size_t(a) * n + d] || !v[std::size_t(b) * n + e])
            return fail("macro case contains a rejected seam; adjust author cutoff/semantics or give a bounded seam refinement");
    }
    const std::string art_hash      = sha256_hex(art, size);
    const std::string semantic_text = rules_to_text(semantic);
    const std::string semantic_hash = sha256_hex(reinterpret_cast<const std::uint8_t *>(semantic_text.data()), semantic_text.size());
    const std::string provenance    = art_hash + "\n" + semantic_hash + "\n" + std::to_string(options.horizontal_limit) + " " + std::to_string(options.vertical_limit) + "\n" + rules_to_text(out);
    out.inputs_sha256               = sha256_hex(reinterpret_cast<const std::uint8_t *>(provenance.data()), provenance.size());
    if (!validate_rules(out, err)) return false;
    err.clear();
    return true;
}
} // namespace mh_mapgen
