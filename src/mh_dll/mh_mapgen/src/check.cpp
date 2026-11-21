// mh_mapgen -- adjacency checker and the generation pair set.
#include "mh_mapgen/check.h"
#include <algorithm>

namespace mh_mapgen {

bool PairSet::has_h(std::uint16_t a, std::uint16_t b) const { return std::binary_search(h.begin(), h.end(), key(a, b)); }
bool PairSet::has_v(std::uint16_t a, std::uint16_t b) const { return std::binary_search(v.begin(), v.end(), key(a, b)); }

PairSet pair_set(const Rules &r, std::uint32_t min_count) {
    PairSet s;
    for (const PairRule &p : r.h)
        if (p.n >= min_count) s.h.push_back(PairSet::key(p.a, p.b));
    for (const PairRule &p : r.v)
        if (p.n >= min_count) s.v.push_back(PairSet::key(p.a, p.b));
    std::sort(s.h.begin(), s.h.end());
    std::sort(s.v.begin(), s.v.end());
    return s;
}

Report check_map(const MpmModel &m, const Rules &r, bool wrap) {
    Report              rep;
    const PairSet       ps = pair_set(r, 1);
    const std::uint32_t w = m.width, h = m.height;
    for (std::size_t i = 0; i < m.flags.size(); ++i)
        if (!r.find_tile(m.flags[i] & TILE_ID_MASK)) ++rep.unknown_tiles;

    for (std::uint32_t c = 0; c < w; ++c)
        for (std::uint32_t rw = 0; rw < h; ++rw) {
            const std::uint16_t a = m.tile_at(c, rw);
            if (c + 1 < w || wrap) {
                const bool          seam = c + 1 == w;
                const std::uint16_t b    = m.tile_at(seam ? 0 : c + 1, rw);
                ++rep.pairs_checked;
                if (!ps.has_h(a, b)) rep.illegal_pairs.push_back({c, rw, 'h', a, b, seam});
            }
            if (rw + 1 < h || wrap) {
                const bool          seam = rw + 1 == h;
                const std::uint16_t b    = m.tile_at(c, seam ? 0 : rw + 1);
                ++rep.pairs_checked;
                if (!ps.has_v(a, b)) rep.illegal_pairs.push_back({c, rw, 'v', a, b, seam});
            }
        }
    return rep;
}

std::string format_illegal(const IllegalPair &p) {
    std::string s = std::to_string(p.x) + " " + std::to_string(p.y) + " " + p.dir + " " + std::to_string(p.a) + " " + std::to_string(p.b);
    if (p.seam) s += " seam";
    return s;
}

} // namespace mh_mapgen
