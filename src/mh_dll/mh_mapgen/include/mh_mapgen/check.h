// mh_mapgen -- the adjacency checker. A map is legal when every adjacent tile pair (right and down,
// plus the wrap seam with wrap=true) was seen at least once in the learned rules.
#pragma once
#include "mh_mapgen/mpm.h"
#include "mh_mapgen/rules.h"
#include <cstdint>
#include <string>
#include <vector>

namespace mh_mapgen {

struct IllegalPair {
    std::uint32_t x = 0, y = 0; // column/row of tile a; b is at (x+1,y) for 'h', (x,y+1) for 'v' (mod size on a seam)
    char          dir = 'h';
    std::uint16_t a = 0, b = 0;
    bool          seam = false;
};

struct Report {
    std::vector<IllegalPair> illegal_pairs;     // scan order: columns left to right, rows top to bottom, h before v
    std::uint32_t            unknown_tiles = 0; // cells whose tile id is not in the rules' tile table
    std::uint64_t            pairs_checked = 0;
    bool                     ok() const { return illegal_pairs.empty() && unknown_tiles == 0; }
};

// Sorted sets of (a<<16 | b) keys of the pairs seen at least min_count times.
struct PairSet {
    std::vector<std::uint32_t> h, v;
    static std::uint32_t       key(std::uint16_t a, std::uint16_t b) { return (std::uint32_t(a) << 16) | b; }
    bool                       has_h(std::uint16_t a, std::uint16_t b) const;
    bool                       has_v(std::uint16_t a, std::uint16_t b) const;
};

// min_count 1 = the checker's accept set; 2 = the generator's (a pair seen once may be an authoring accident).
PairSet pair_set(const Rules &r, std::uint32_t min_count);

Report check_map(const MpmModel &m, const Rules &r, bool wrap);

// "x y h a b" style line, with " seam" appended for wrap-seam pairs.
std::string format_illegal(const IllegalPair &p);

} // namespace mh_mapgen
