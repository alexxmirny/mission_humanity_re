//
// sim/sim_attack_range_footprint.h -- mods:M1C [net] attack_range_footprint (follows [net] improved_combat).
//
// Retail measures a unit's weapon range to a BUILDING from the footprint's CENTRE: every attack-building issuer, the
// move walker's path-end check, unit_fire_weapon's auto mode and the re-plan paths fetch llm_strat_bldg_get_coords
// (origin*32 + width*16, the centre) and compare the wrapped Manhattan tile distance against range_min/range_max. A
// 3x3 target costs 1 tile per axis (2 at a corner), a 5x5 one 2 (4), so a unit that is in range of the wall it faces
// walks closer or never fires.
//
// With the key on, every such site uses the AIM TILE instead: the OCCUPIED footprint tile (cfg Building area[i][j] != 0,
// tile (origin_x+i, origin_y+j)) nearest the attacker under the game's own wrapped Manhattan metric (the |delta| of
// llm_strat_tile_delta_wrapped, per axis), first in area scan order on a tie; its fine coordinate is the tile centre
// (tile*32 + 16). The order issuers write it into the order's goal + target_fine, so the planner's anchor IS that tile;
// the checks that run later recompute it from the attacker's current tile, and the nearest tile is never farther than
// the anchor (the anchor is itself a footprint tile). So a check is never STRICTER than the plan -- the property that
// keeps a unit out of the 4 x 2 s replan-and-give-up loop.
//
// Two exceptions keep the centre (retail value): an attacker that carries any weapon with range_min > 1 (a nearer
// footprint tile could fall INSIDE its minimum range while the plan's anchor did not -- the plan and the check would
// disagree), and a building type whose area mask is empty.
//
// Integer only, no state: a pure function of the unit/building records, the cfg tables and the map masks. Header-only
// so the retail splices (mh/seams/combat_proto.cpp) and the libmh twins compute the SAME tile from the same inputs; the
// flag is an inline static, one per module (mh.dll's is unused), set by lockstep::set_fixes.
//
#pragma once
#include <cstdint>

namespace mh::sim {

namespace attack_range_footprint_detail {
inline bool &flag() {
    static bool on = false;
    return on;
}
} // namespace attack_range_footprint_detail
inline void set_attack_range_footprint(bool on) { attack_range_footprint_detail::flag() = on; }
inline bool attack_range_footprint() { return attack_range_footprint_detail::flag(); }

struct footprint_aim_in {
    int32_t unit_x, unit_y;           // the attacker's tile
    int32_t origin_x, origin_y;       // the building record's x/y (top-left tile)
    uint8_t width, height;            // cfg Building width/height (the centre formula's operands)
    const uint8_t (*area)[10];        // cfg Building area[10][10]; area[i][j] covers tile (origin_x+i, origin_y+j)
    bool     keep_centre;             // the attacker has a weapon with range_min > 1
    uint32_t width_mask, height_mask; // tile wrap masks (map dims - 1, powers of two)
    uint32_t bw_mask, bh_mask;        // fine (pixel) wrap masks
};

// |llm_strat_tile_delta_wrapped| on one axis: the shorter way round, folded exactly where the original folds (dim/2 < d).
inline int32_t footprint_axis_dist(int32_t a, int32_t b, int32_t dim) {
    int32_t d = a > b ? a - b : b - a;
    if ((dim >> 1) < d) d = dim - d;
    return d;
}

// The fine coordinate the range/plan sites use for this building: the nearest occupied footprint tile's centre, or the
// retail centre (llm_strat_bldg_get_coords's formula) when keep_centre is set or the area mask is empty.
inline void footprint_aim_fine(const footprint_aim_in &in, int32_t *out_x, int32_t *out_y) {
    int32_t best = -1, bx = 0, by = 0;
    if (!in.keep_centre && in.area) {
        const int32_t w = static_cast<int32_t>(in.width_mask) + 1, h = static_cast<int32_t>(in.height_mask) + 1;
        for (int32_t i = 0; i < 10; ++i) {
            for (int32_t j = 0; j < 10; ++j) {
                if (in.area[i][j] == 0) continue;
                const int32_t tx = (in.origin_x + i) & static_cast<int32_t>(in.width_mask);
                const int32_t ty = (in.origin_y + j) & static_cast<int32_t>(in.height_mask);
                const int32_t d  = footprint_axis_dist(in.unit_x, tx, w) + footprint_axis_dist(in.unit_y, ty, h);
                if (best < 0 || d < best) {
                    best = d;
                    bx   = tx;
                    by   = ty;
                }
            }
        }
    }
    if (best < 0) {
        *out_x = static_cast<int32_t>((static_cast<uint32_t>(in.origin_x) * 32u + static_cast<uint32_t>(in.width) * 16u) & in.bw_mask);
        *out_y = static_cast<int32_t>((static_cast<uint32_t>(in.origin_y) * 32u + static_cast<uint32_t>(in.height) * 16u) & in.bh_mask);
        return;
    }
    *out_x = static_cast<int32_t>((static_cast<uint32_t>(bx) * 32u + 16u) & in.bw_mask);
    *out_y = static_cast<int32_t>((static_cast<uint32_t>(by) * 32u + 16u) & in.bh_mask);
}

// The keep_centre input: any of the attacker's four weapon slots that is mounted (enabled_2) with range_min[player] > 1.
// Templated over the record types so mh.dll (mh::game structs) and libmh (its aliases of the same structs) share it.
template <class Unit, class Weapon>
inline bool footprint_keep_centre(const Unit &u, const Weapon *cfg_weapons, uint32_t player) {
    if (!cfg_weapons || player >= 9) return true;
    for (int32_t s = 0; s < 4; ++s) {
        if (u.weapons[s].enabled_2 == 0) continue;
        if (cfg_weapons[u.weapons[s].weapon_id].range_min[player] > 1) return true;
    }
    return false;
}

// Gather + compute in one step from records (any struct set with the mh::game field names).
template <class Unit, class Building, class CfgBuilding, class Weapon, class Geom>
inline void footprint_aim_fine_of(const Unit &attacker, uint32_t attacker_player, const Building &b, const CfgBuilding *cfg_buildings,
                                  const Weapon *cfg_weapons, const Geom &g, int32_t *out_x, int32_t *out_y) {
    const auto      &cb = cfg_buildings[b.building_id];
    footprint_aim_in in{};
    in.unit_x      = attacker.x;
    in.unit_y      = attacker.y;
    in.origin_x    = b.x;
    in.origin_y    = b.y;
    in.width       = cb.width;
    in.height      = cb.height;
    in.area        = cb.area;
    in.keep_centre = footprint_keep_centre(attacker, cfg_weapons, attacker_player);
    in.width_mask  = g.width_mask;
    in.height_mask = g.height_mask;
    in.bw_mask     = g.bw_mask;
    in.bh_mask     = g.bh_mask;
    footprint_aim_fine(in, out_x, out_y);
}

// The libmh twins' form: building (tp, ti) read through a view with buildings / caps.buildings / cfg_buildings /
// cfg_weapons / geom (sim_view and orders' issue_view both have them). false = no live record there (the caller then
// keeps its retail path).
template <class View, class Unit>
inline bool footprint_aim_of_view(const View &v, const Unit &attacker, uint32_t attacker_player, uint32_t tp, int32_t ti, int32_t *out_x,
                                  int32_t *out_y) {
    if (tp >= 8 || ti < 0 || ti >= v.caps.buildings || !v.geom || !v.cfg_buildings) return false;
    const auto &b = v.buildings[tp * v.caps.buildings + ti];
    if (b.building_id >= 100) return false;
    footprint_aim_fine_of(attacker, attacker_player, b, v.cfg_buildings, v.cfg_weapons, *v.geom, out_x, out_y);
    return true;
}

} // namespace mh::sim
