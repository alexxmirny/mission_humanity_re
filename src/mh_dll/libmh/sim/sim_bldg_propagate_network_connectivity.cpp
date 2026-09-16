//
// sim/sim_bldg_propagate_network_connectivity.cpp -- see sim_bldg_propagate_network_connectivity.h.
// Translated from the DISASSEMBLY (tmp/decomp/llm_strat_bldg_propagate_network_connectivity_0049209b.asm),
// not from the Ghidra .c draft (whose window-center variable naming reads backwards from the actual
// struct-offset pairing -- see the header banner for the re-derivation).
//
#include "sim/sim_bldg_propagate_network_connectivity.h"

#include "addr/mh_calls.gen.h"     // typed callables for the original functions we still call OUT to
#include "sim/sim_order_enqueue.h" // BUILDING_TYPE_H_TURRET/A_TURRET, ORDER_KIND_BUILDING
#include "addr/mh_rebind.gen.h"    // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const propagate_network_connectivity_calls &live_propagate_network_connectivity_calls() {
    static const propagate_network_connectivity_calls gc = {
        MH_LIBMH_BIND(llm_bldg_set_connected_flag),
    };
    return gc;
}

namespace detail {

void propagate_network_connectivity(const sim_view &v, const propagate_network_connectivity_calls &gc,
                                    uint16_t player, int32_t b_index) {
    // 0x004920bf: unconditional -- the building marks itself connected regardless of what follows.
    gc.set_connected_flag(player, b_index);

    const building &self = building_of(v, player, b_index);

    // 0x004920d7-0x00492131: online AND not a turret (turrets are power sinks, not conduits) --
    // either failing condition returns without scanning/recursing.
    const cfg_building &self_cfg = v.cfg_buildings[self.building_id];
    if (self.online_state == 0 || self_cfg.type == BUILDING_TYPE_H_TURRET ||
        self_cfg.type == BUILDING_TYPE_A_TURRET) {
        return;
    }

    // 0x00492138-0x0049213d: (player | ORDER_KIND_BUILDING), the tile_object::class_owner value a
    // same-owner BUILDING-class tile carries -- reused from sim_order_enqueue.h rather than a fresh
    // 0x40u literal (same encoding tile_object's own field comment documents).
    const uint32_t owner_building_tag = (uint32_t)player | ORDER_KIND_BUILDING;

    // 0x0049214e-0x0049219a: x_center = (cfg width/2 + self.x) & width_mask. Width is a uint8_t
    // (0-255), so the asm's sign-corrected SAR-based "/2" is equivalent to a plain unsigned halving --
    // reproduced as ordinary integer division for the same reason.
    const uint32_t half_width = (uint32_t)self_cfg.width / 2u;
    const uint32_t x_center   = (half_width + (uint32_t)self.x) & v.geom->width_mask;
    // 0x004921fc-0x0049220a: x_start, computed ONCE before the outer loop (unlike y_start below).
    const uint32_t x_start = (x_center - 0xfu) & v.geom->width_mask;

    // 0x004921a5-0x004921f9: y_center = (cfg height/2 + self.y) & height_mask, same reasoning.
    const uint32_t half_height = (uint32_t)self_cfg.height / 2u;
    const uint32_t y_center    = (half_height + (uint32_t)self.y) & v.geom->height_mask;

    // 0x0049220d: outer (x) loop, 31 iterations (0x1f), tile_x cursor re-wrapped by width_mask after
    // each pass (0x00492223-0x0049222f).
    uint32_t tile_x = x_start;
    for (int32_t i = 0; i < 0x1f; ++i) {
        // 0x0049223a-0x0049224b: y_start is RECOMPUTED fresh at the top of every outer iteration (the
        // same fixed y_center/0xf/height_mask each time, but re-derived rather than reused across
        // iterations -- matches the asm exactly, not an optimisation opportunity).
        uint32_t tile_y = (y_center - 0xfu) & v.geom->height_mask;

        // 0x0049224b: inner (y) loop, 31 iterations, tile_y cursor re-wrapped by height_mask after each
        // pass (0x00492261-0x0049226d).
        for (int32_t j = 0; j < 0x1f; ++j) {
            // 0x00492278-0x00492291: same-owner, building-class tile.
            const tile_object &t = tile_at(v, (int32_t)tile_x, (int32_t)tile_y);
            if ((uint32_t)t.class_owner == owner_building_tag) {
                const building &target = building_of(v, player, t.building);
                // 0x004922bc-0x004922c3: unbuilt/unconnected (built_flags bit0 clear).
                if ((target.built_flags & 0x1u) == 0) {
                    // 0x004922ee-0x004922f9: energized. x87 FLDZ/FCOMP/FNSTSW/SAHF/JC -- CONFIRMED A
                    // REAL DIVERGENCE by reimpl-verify (2026-08-13), NOT the "no precision hazard"
                    // call an earlier pass at this comment made. JC (CF=1) fires for target.energy >
                    // 0.0 OR an UNORDERED (NaN) compare (C0=1 on unordered FCOM loads into CF via
                    // SAHF) -- the same idiom sim_bldg_power_network_recompute.cpp's walk #2 had
                    // backwards too. A plain `target.energy > 0.0` is false for NaN per IEEE 754 and
                    // wrongly skips the recursive call the original takes.
                    if (!(target.energy <= 0.0)) {
                        // 0x00492316: SELF-RECURSION, a genuine local C++ call (NOT mh::call::) --
                        // see the header banner. CORRECTED 2026-08-13 (G21): this originally routed
                        // through mh::call:: at the function's own original address, following this
                        // batch's general "callees stay original" rule -- but that rule is for calls
                        // to OTHER, un-promoted sibling functions, which production genuinely
                        // reaches via the original pre-promotion. A function's OWN recursive edge is
                        // different: mh::call::'s raw address call lands on THIS SITE's shadow hook
                        // once one is installed, so every recursion depth re-triggers a nested
                        // arm/compare/restore cycle and corrupts the outer comparison (a harness
                        // artifact, not a body bug -- see G21's full derivation). A direct call is
                        // also the semantically correct one for AFTER promotion: the promoted body
                        // should recurse into itself, not back out to the retired original. Matches
                        // sim_unit_create_soldier.cpp's established precedent for its own recursion.
                        propagate_network_connectivity(v, gc, player, t.building);
                    }
                }
            }

            tile_y = (tile_y + 1u) & v.geom->height_mask;
        }

        tile_x = (tile_x + 1u) & v.geom->width_mask;
    }
}

} // namespace detail

// ---- the public surface --------------------------------------------------------------------------

void propagate_network_connectivity(uint16_t player, int32_t b_index) {
    const sim_view v = state().read;
    detail::propagate_network_connectivity(v, live_propagate_network_connectivity_calls(), player, b_index);
}


} // namespace mh::sim


namespace mh::sim {
} // namespace mh::sim
