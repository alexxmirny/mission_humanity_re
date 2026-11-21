// mh_mapgen -- private types shared by model.cpp, plan.cpp, wfc.cpp, place.cpp and gen.cpp.
#pragma once
#include "mh_mapgen/gen.h"
#include "mh_mapgen/rules.h"
#include <array>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <string>
#include <vector>

namespace mh_mapgen::detail {

// ---- deterministic hashing (the generator's only source of randomness) -------------------------------------------------------
inline std::uint64_t h64(std::initializer_list<std::uint64_t> xs) {
    std::uint64_t z = 0x9E3779B97F4A7C15ull;
    for (std::uint64_t x : xs) {
        z = z + x + 0x9E3779B97F4A7C15ull;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        z ^= z >> 31;
    }
    return z;
}
inline std::uint64_t u64(std::int64_t v) { return std::uint64_t(v); }

inline int pmod(int a, int m) {
    const int r = a % m;
    return r < 0 ? r + m : r;
}

// ---- build room ---------------------------------------------------------------------------------------------------------
// Buildable tiles reachable from the ring around a spot's 5x3 mother footprint (x..x+4, y..y+2): 4-neighbour flood on the torus over
// tiles `buildable(col, row)` accepts, the ring is step 1, the footprint itself is neither entered nor counted. The sealed-pocket gate
// (DESIGN sec. 14) wants n16 >= MIN_ROOM16 and n32 >= MIN_ROOM32; the flood stops at 32 steps.
struct BuildRoom {
    int n16 = 0, n32 = 0;
};
template <class F>
inline BuildRoom build_room(F buildable, int N, int x, int y) {
    BuildRoom                  out;
    std::vector<std::uint8_t>  step(std::size_t(N) * std::size_t(N), 0);
    std::vector<std::uint32_t> cur, nxt;
    auto                       in_fp = [&](int c, int r) { return pmod(c - x, N) < 5 && pmod(r - y, N) < 3; };
    auto                       visit = [&](int c, int r, int s, std::vector<std::uint32_t> &q) {
        c                   = pmod(c, N);
        r                   = pmod(r, N);
        const std::size_t i = std::size_t(r) * std::size_t(N) + std::size_t(c);
        if (step[i] || in_fp(c, r) || !buildable(c, r)) return;
        step[i] = std::uint8_t(s);
        out.n16 += s <= 16;
        out.n32 += s <= 32;
        q.push_back(std::uint32_t(i));
    };
    for (int dy = -1; dy <= 3; ++dy)
        for (int dx = -1; dx <= 5; ++dx) visit(x + dx, y + dy, 1, cur);
    for (int s = 2; s <= 32 && !cur.empty(); ++s) {
        nxt.clear();
        for (std::uint32_t i : cur) {
            const int c = int(i % std::uint32_t(N)), r = int(i / std::uint32_t(N));
            visit(c + 1, r, s, nxt);
            visit(c - 1, r, s, nxt);
            visit(c, r + 1, s, nxt);
            visit(c, r - 1, s, nxt);
        }
        cur.swap(nxt);
    }
    return out;
}

// ---- macro keys -----------------------------------------------------------------------------------------------------
// key = four corner classes (4 bits each) + tag byte; the same packing the learner uses.
inline std::uint32_t make_key(const std::uint8_t c[4], std::uint8_t tag) { return (std::uint32_t(c[0]) << 20) | (std::uint32_t(c[1]) << 16) | (std::uint32_t(c[2]) << 12) | (std::uint32_t(c[3]) << 8) | tag; }
inline std::uint32_t key_sig(std::uint32_t key) { return key >> 8; }
inline std::uint8_t  key_tag(std::uint32_t key) { return std::uint8_t(key & 255); }
inline int           key_cls(std::uint32_t key, int q) { return int(key >> (20 - 4 * q)) & 15; }
inline bool          key_pure(std::uint32_t key) { return key_cls(key, 0) == key_cls(key, 1) && key_cls(key, 0) == key_cls(key, 2) && key_cls(key, 0) == key_cls(key, 3); }

// Per-tileset generator tuning (the learned model does not carry these: they are taste, keyed by tileset stem).
struct Tune {
    int  reg_div      = 2;     // region noise wavelength = min(M,64) / reg_div lattice points
    int  line_div     = 4;     // ridge-line noise wavelength = min(M,64) / line_div
    int  line_q       = 50;    // ridge-line cut: percent of the noise field that lies on the line side of the threshold
    bool no_lines     = false; // the "lines" of this tileset are stamps, not contours
    bool contours     = false; // the largest child of the background is a rank-uniform band field (long ridges and nested rings)
    int  contour_div  = 1;     // contours: noise wavelength = min(M,64) / contour_div
    int  contour_oct  = 4;     // contours: noise octaves
    int  levels       = 3;     // contours: number of nested bands
    bool fit          = true;  // re-aim each class's threshold until the cleaned-up region matches the learned share
    int  maj          = 3;     // majority-smoothing passes of the region cleanup
    int  minc         = 10;    // smallest region component kept (lattice points)
    int  spot_mix     = 0;     // percent of the landing spots (whole opposite pairs) put on the largest child of the background
    int  cshare       = 100;   // contours: percent scale of the band share (the cleanup and the forced clearings eat area)
    int  child_margin = 2;     // spot_mix: points of child ground wanted round the clearing square
    int  child_pct    = 90;    // spot_mix: percent of that grown square that must already be child ground
    int  wide_cap     = 60;    // landing spots: percent of the 15x15-point room round a spot that is worth chasing (beyond it scenery wins)
    bool cfit         = false; // contours: re-aim the band share until the cleaned-up result matches
    int  decal_big    = 30;    // decals: percent of the placed groups drawn from the 4+ tile shapes (0 = the learned mix, 1-5%); measures as ~12% 4-connected 4+ groups
    int  decal_pct    = 100;   // percent of the learned decal rate that is placed (the plain WFC already makes some rare tiles)
};
// Parse "key=value,key=value" overrides (reg_div, line_div, line_q, no_lines, contours, contour_div, contour_oct, levels, fit, maj, minc, spot_mix, cshare, cfit, decal_pct, wide_cap, child_margin, child_pct) into t. false on a bad item.
bool tune_apply(Tune &t, const std::string &spec, std::string &err);
Tune tune_for(const std::string &stem);

// ---- the runtime model: tile universe, supports, macro cases -------------------------------------------------------------
struct CaseInfo {
    std::vector<std::uint64_t>                           dom[4];  // quadrant domains, W words each
    std::vector<std::pair<std::uint32_t, std::uint32_t>> wt[4];   // (tile index, uses), ascending tile index
    mutable std::vector<std::uint64_t>                   sup[16]; // [q * 4 + d]: tiles allowed next to the quadrant's domain, lazily built
    mutable std::uint16_t                                sup_done = 0;
};

struct StampModel {
    std::uint32_t                                             n = 0;
    int                                                       h = 0, w = 0;
    std::vector<std::pair<std::pair<int, int>, std::int32_t>> cells; // ((y, x), tile index)
};

struct DecalModel {
    std::int32_t                                              cls = 0;
    std::uint32_t                                             n   = 0;
    int                                                       h = 0, w = 0;
    std::vector<std::pair<std::pair<int, int>, std::int32_t>> cells; // ((y, x), tile index)
};

struct TreeSampleIdx {
    std::int32_t  kind = 0, px = 0, py = 0;
    std::uint32_t n = 0;
};

struct Model {
    const Rules *rules = nullptr;
    std::string  stem;
    Tune         tune;
    bool         synthetic = false; // hand-made rules without a macro model: one class, every tile free

    // tiles
    std::uint32_t              T = 0, W = 0;
    std::vector<std::uint16_t> tid;
    std::vector<std::int32_t>  idx_of; // 0x2000 entries
    std::vector<std::uint8_t>  pmaj, pseen_pass;
    std::vector<std::uint64_t> sup[4];  // T * W words per direction: 0 east, 1 west, 2 south, 3 north
    std::vector<std::uint64_t> utab[4]; // byte-chunk tables: OR of sup rows over the 8 tiles of a chunk selected by the byte
    void                       support(const std::uint64_t *dom, int d, std::uint64_t *out) const;
    // The same, memoised in a direct-mapped cache keyed by the domain's contents (a pure function, so determinism is untouched).
    // The returned row stays valid until the next support_cached call.
    const std::uint64_t               *support_cached(const std::uint64_t *dom, int d) const;
    mutable std::vector<std::uint64_t> sc_key, sc_dom, sc_sup;
    // support of a single tile: points into sup
    const std::uint64_t *sup_row(int d, std::uint32_t t) const { return &sup[d][std::size_t(t) * W]; }

    // classes
    int                        ncls = 1, bg = 0;
    std::vector<int>           order, parent;
    std::vector<std::int64_t>  share_ppm, sub_ppm; // own share and cumulative (self + descendants)
    std::vector<std::uint16_t> adjm;               // adjacency bitmask per class
    std::vector<std::uint8_t>  pass_class;         // class is walkable ground

    // macro cases
    std::vector<std::uint32_t> case_key; // sorted
    std::vector<std::int32_t>  case_idx;
    std::vector<std::uint32_t> fb_sig; // sorted corner signatures with a union-of-tags fallback
    std::vector<std::int32_t>  fb_idx;
    std::vector<CaseInfo>      infos;
    std::int32_t               empty_info = -1;
    std::vector<std::uint8_t>  ramp_sig; // 65536: corner signatures that have ramp variants

    std::int32_t find_case(std::uint32_t key) const; // exact key, else -1
    std::int32_t info_of(std::uint32_t key) const;   // exact, else the union fallback, else the empty info

    // learned scalars
    std::int64_t line_ppm = 0, ramp_permille = 0, stamp_ppb = 0, tree_ppm = 0;

    std::vector<StampModel>                   stamps;
    std::uint64_t                             stamp_total = 0;
    std::vector<DecalModel>                   decals;
    std::vector<std::vector<std::uint32_t>>   decals_of;   // per class: indices into decals
    std::vector<std::uint64_t>                decal_total; // per class: sum of the uses
    std::vector<std::vector<TreeSampleIdx>>   trees;       // per tile index
    std::vector<std::array<std::uint16_t, 4>> ore_good;    // first-mine capable records (capped), one entry per use
    std::vector<std::array<std::uint16_t, 4>> ore_rich;    // all four channels > 0 and 4a+2b+2c+d >= ORE_RICH_WEIGHT (empty: the rules have none)
    std::vector<std::array<std::uint16_t, 4>> ore_all;     // every record (capped), one entry per use
    std::int64_t                              tree_cap = 9999;

    // the per-quadrant support of a case (lazily built); W words
    const std::uint64_t *info_support(std::int32_t info, int q, int d) const;
};

bool build_model(const Rules &r, Model &m, std::string &err);

// Tiles around a landing spot (Chebyshev) that stay passable and object-free: the AI footprint core. Trees keep out of it and stamps keep one tile beyond.
constexpr int CORE_R = 4;

// ---- the macro plan --------------------------------------------------------------------------------------------------
struct Plan {
    const Model *mdl      = nullptr;
    int          M        = 0; // lattice size (N / 2)
    bool         has_line = false;

    // lattice points (M*M, row-major) and macros (M*M, anchored at their top-left point)
    std::vector<std::uint8_t>  ramp_ban;                                 // macros whose ramp the repair had to drop: the path planner does not put one back there
    std::vector<std::uint8_t>  cls, line, clear, ramps, hs, vs, protect; // protect: points whose line value the repair loop must not edit (the flattened keep areas)
    std::vector<std::uint32_t> key;
    std::vector<std::int32_t>  info;

    std::uint32_t compute_key(int R, int C) const;
    void          refresh_all();
};

struct PlanStats {
    std::uint32_t repair_passes = 0, path_edits = 0, ramps = 0;
};

// Run the whole plan (spots, regions, lines, ramps, paths, repair) for one attempt seed. `spots` = lattice points (R, C).
bool make_plan(const Model &m, std::uint32_t N, std::uint32_t players, std::uint64_t seed, bool ensure_paths, bool protect_spots, Plan &plan, std::vector<std::pair<int, int>> &spots, PlanStats &st, std::string &why);

// ---- WFC (wfc.cpp) ----------------------------------------------------------------------------------------------------
struct WfcResult {
    bool                      ok = false;
    std::string               why;
    std::vector<std::int32_t> tile; // N*N tile indices, row-major
    std::uint32_t             resets = 0;
    int                       bad_r = -1, bad_c = -1; // where the initial propagation died
};

// Persistent tile-domain state for the plan repair loop: arc-consistency is kept across plan edits and only the cells around
// the re-keyed macros are re-derived (no whole-map re-propagation per repair pass).
class TileState {
public:
    TileState(const Model &m, const Plan &plan, int N);
    ~TileState();
    TileState(const TileState &)            = delete;
    TileState &operator=(const TileState &) = delete;
    bool       init(int &bad_r, int &bad_c);                                    // whole-map pass; false = a domain emptied at (bad_r, bad_c)
    bool       refresh(const std::vector<int> &macros, int &bad_r, int &bad_c); // macros = re-keyed macro ids (R * M + C)
    struct Impl;

private:
    std::unique_ptr<Impl> p_;
};

// Keeps the whole map arc-consistent while single cells are pinned to a tile (place.cpp's decals): a pin that would empty any
// domain is undone and refused, so what is accepted is guaranteed to survive the WFC's initial propagation.
class DecalProbe {
public:
    DecalProbe(const Model &m, const Plan &plan, int N, const std::vector<std::int32_t> &fixed);
    ~DecalProbe();
    DecalProbe(const DecalProbe &)            = delete;
    DecalProbe &operator=(const DecalProbe &) = delete;
    bool        valid() const;                                                         // false = the fixed cells it started from already contradict (no decal then)
    bool        pin(const std::vector<std::pair<std::uint32_t, std::int32_t>> &cells); // (cell = row * N + col, tile index)
    // wfc_solve over the fixed cells pinned so far, without repeating the initial propagation. The `fixed` vector given to the
    // constructor must outlive the probe and equal the pinned cells.
    WfcResult solve(std::uint64_t seed) const;
    struct Impl;

private:
    std::unique_ptr<Impl> p_;
};

// Initial arc-consistency only: can this plan be tiled at all?
WfcResult wfc_initial(const Model &m, const Plan &plan, int N, const std::vector<std::int32_t> *fixed);
WfcResult wfc_solve(const Model &m, const Plan &plan, int N, std::uint64_t seed, const std::vector<std::int32_t> *fixed);

// ---- place (place.cpp) ------------------------------------------------------------------------------------------------
// spots_xy = tile coordinates (x = col, y = row).
// decals = also place the ground decals (fixed rare-tile groups on plain ground); false = stamps only (the WFC fallback).
struct Placement {
    std::vector<std::int32_t>   fixed;           // N*N tile indices, -1 where the WFC is free
    std::unique_ptr<DecalProbe> probe;           // the pinned state of `fixed` (null: nothing pinned, or the plan itself does not propagate)
    std::uint32_t               decals      = 0; // decal groups pinned
    Placement()                             = default;
    Placement(const Placement &)            = delete; // the probe points at `fixed`
    Placement &operator=(const Placement &) = delete;
};
std::unique_ptr<Placement> place_stamps(const Model &m, const Plan &plan, int N, std::uint64_t seed, const std::vector<std::pair<int, int>> &spots_xy, bool decals);
void                       place_passability(const Model &m, const Plan &plan, const std::vector<std::int32_t> &tile, int N, std::vector<std::uint8_t> &pas);
void                       place_trees(const Model &m, const std::vector<std::int32_t> &tile, int N, std::uint64_t seed, bool ensure_paths, int max_trees, const std::vector<std::pair<int, int>> &spots_xy, std::vector<std::uint8_t> &pas, std::vector<std::int32_t> &treeat, std::vector<MpmTree> &out);
void                       place_ore(const Model &m, const std::vector<std::uint8_t> &pas, const std::vector<std::int32_t> &treeat, int N, std::uint64_t seed, const std::vector<std::pair<int, int>> &spots_xy, std::vector<std::uint16_t> &ore);

// No-wrap 4-neighbour flood fill; dist -1 where unreachable.
void bfs_free(const std::vector<std::uint8_t> &free, int N, int r0, int c0, std::vector<std::int32_t> &dist);

} // namespace mh_mapgen::detail
