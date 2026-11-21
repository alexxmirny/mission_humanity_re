// mh_mapgen -- the procedural strategic-map generator (design sec. 3, 9, 10).
//
//   plan    regions (terrain classes) and ridge lines on a 2x2-macro torus lattice, landing spots with clearings, ramps,
//           paths between the spots, repair of every macro pair the learned catalogue cannot tile
//   wfc     tile-level wave-function collapse over the learned adjacency pairs inside the plan's macro cases
//   place   found-object stamps, passability, trees, ore laid out for the AI's first mine
//   gen     hard gates (legal pairs, landing clearing, 5x3 mother footprint, first mine, connectivity, loader bounds), retries
//
// Deterministic by construction: integer-only decisions, hashes from SplitMix64-style mixing of the seed, sorted containers,
// no unordered containers, no std distributions. Same rules + same params -> the same bytes on every build.
#pragma once
#include "mh_mapgen/mpm.h"
#include "mh_mapgen/rules.h"
#include <cstdint>
#include <string>
#include <vector>

namespace mh_mapgen {

constexpr std::uint32_t GEN_MAX_ATTEMPTS = 8;

// Adjacency pairs must have been seen at least once (the checker's accept set); the macro catalogue does the shaping.
constexpr std::uint32_t GEN_MIN_PAIR = 1;

struct GenParams {
    std::string   tileset;            // exact spelling as in Rules::tileset (the CLI normalises user input)
    std::uint32_t size         = 128; // 64, 128 or 256 (square)
    std::uint32_t players      = 4;   // 64 -> 2; 128 / 256 -> 4, 6 or 8
    std::uint64_t seed         = 1;
    bool          wrap         = true; // seam pairs must be legal too (the planet wraps)
    bool          ensure_paths = true; // every landing spot reachable from every other by a no-wrap walk (--no-paths allows islands)
    std::string   tune;                // experimental per-tileset overrides, "key=value,..." (reg_div line_div line_q no_lines contours contour_div contour_oct levels fit maj minc)
    std::string   name;                // first description line (<= 26 chars); default "gen <tileset> <size>"
    std::string   desc;                // appended, word-wrapped; the whole description is <= 4 lines x 26 chars
};

struct GenStats {
    std::uint32_t attempts      = 0;                     // attempts used (the successful one included)
    std::uint32_t resets        = 0;                     // WFC window resets, summed over attempts
    std::uint32_t repair_passes = 0;                     // plan repair passes, summed over attempts
    std::uint32_t path_edits    = 0;                     // plan edits made to connect the landing spots
    std::uint32_t ramps         = 0;                     // ramp macros in the returned map's plan
    std::uint32_t trees         = 0;                     // of the returned map
    std::uint32_t ore_blocks    = 0;                     // of the returned map
    std::uint32_t plan_failures = 0;                     // attempts whose plan could not be repaired
    std::uint32_t wfc_failures  = 0;                     // attempts that died in the WFC
    std::uint32_t gate_failures = 0;                     // attempts that died on the hard gates
    std::uint32_t decals        = 0;                     // ground decals placed in the returned map's last WFC try (0 after a fallback)
    std::uint32_t decal_drops   = 0;                     // attempts whose decals were dropped because the WFC could not fit them
    std::uint32_t ms_plan = 0, ms_wfc = 0, ms_place = 0; // wall time per stage, summed over attempts (informational)
};

struct GenResult {
    bool                     ok = false;
    MpmModel                 map;    // an .mpm model (has_tail, id = CRC-32) when ok
    std::string              reason; // why not, when !ok
    std::vector<std::string> log;    // one line per failed attempt (the retries that were needed)
    GenStats                 stats;
};

// Check the parameters against the envelope (size, players, tileset == rules.tileset); "" when fine.
std::string gen_validate(const Rules &rules, const GenParams &p);

GenResult generate(const Rules &rules, const GenParams &p);

// The no-wrap 4-neighbour BFS gate: every landing spot sits on a passable tree-free tile and all spots are
// mutually connected. Stricter than the stock maps (half of which fail it) -- that is intended.
bool landings_connected(const MpmModel &m, std::string &why);

// The generator's other hard gates, on a finished map: even spot count 2..8, mother footprint (x..x+4, y..y+2) inside the map
// and free, passable tree-free clear radius >= 4 around every spot, first-mine ore (all four channels > 0 and
// 4a+2b+2c+d >= 500) within 8 tiles of every spot, ore <= 3276 with channels 4-7 zero, tree slots 1..9999, description <= 79 bytes.
// With ensure_paths also landings_connected. "" in `why` when all hold.
// need_rich adds the ore reach gates: a rich block (full channels, weight >= ORE_RICH_WEIGHT) within MAX_RICH_REACH tiles and a
// full-channel weight >= 500 block within sqrt(MAX_GOOD_REACH_SQ) tiles of every spot (centre of the 5x3 footprint to the block centre).
bool map_gates(const MpmModel &m, bool ensure_paths, std::string &why, bool need_rich = false);

constexpr std::uint32_t MIN_SPOT_CLEAR    = 4;    // Chebyshev radius of free ground around a landing spot
constexpr std::uint32_t MIN_SPOT_DIST_DIV = 6;    // spots are >= map width / this many tiles apart (torus Chebyshev)
constexpr std::uint32_t MIN_BUILD_FIT     = 200;  // permille of the 6x6 windows within 15 tiles of a spot that are fully free (stock median 230-600, p10 60-210)
constexpr std::uint32_t MIN_ROOM16        = 150;  // build room: buildable (passable, tree-free, ore-free) tiles reachable within 16 steps of the 5x3 footprint (stock min 151, p5 183)
constexpr std::uint32_t MIN_ROOM32        = 300;  // ... within 32 steps (stock min 159, p1 330, p5 495)
constexpr std::uint32_t MAX_FIRST_MINE    = 8;    // tiles from a spot to the nearest AI-first-mine ore block
constexpr std::uint32_t ORE_RICH_WEIGHT   = 8000; // 4a+2b+2c+d of a block the AI's later mines need
constexpr std::uint32_t MAX_RICH_REACH    = 7;    // tiles (Euclid) from the footprint centre to the nearest rich block centre
constexpr std::uint32_t MAX_GOOD_REACH_SQ = 30;   // ... to the nearest full-channel weight >= 500 block centre, squared tiles (5.5: the nearest block clear of the 5x3 footprint is 5.4 away at worst)
constexpr std::uint32_t MAX_ORE_VALUE     = 3276; // the game reads ore as a signed 16-bit after a x10
constexpr std::uint32_t MAX_DESC_BYTES    = 79;   // the loader's description buffer
constexpr std::uint32_t MAX_TREE_SLOT     = 9999; // tree slots index a 10000-entry object table

} // namespace mh_mapgen
