// Destination: src/mh_dll/mh/ui/target_line_routes.h
// Read-only presentation contract. The registered reader belongs to the canonical task owner.
#pragma once
#include <cstdint>
#include "gfx/strat_world_view.h"

namespace mh::ui::selected_targets::detail {
inline constexpr unsigned route_selection_capacity = 200;
inline constexpr unsigned route_capacity           = route_selection_capacity * 3 + 2;
inline constexpr unsigned explicit_route_capacity  = 32;
inline constexpr unsigned route_node_capacity      = explicit_route_capacity + 1;
inline constexpr unsigned no_cycle                 = 0xff;
enum class line_role : uint8_t { movement,
                                 rally,
                                 patrol,
                                 attack };

struct route_command_key {
    uint32_t system = 0, planet = 0, world = 0, sequence = 0;
    uint64_t legacy    = 0;
    bool     canonical = false;
};
struct route_snapshot_node {
    // Fixed requests are tiles. Entity coordinates MUST NOT be supplied by the owner:
    // collection resolves visibility, native membership and visual height. The
    // canonical reader certifies target_current against ITS lifetime domain;
    // presentation building lives are deliberately a different namespace.
    uint16_t          x = 0, y = 0, target_ref = 0, target_slot = 0;
    uint32_t          target_life = 0;
    route_command_key command{};
    line_role         role           = line_role::movement;
    bool              target_current = false;
};
struct route_snapshot {
    // Only unconsumed finite entries, then the COMPLETE cyclic suffix. cycle_cursor
    // is an index in that suffix; no_cycle means finite-only. A one-point patrol
    // has an implicit anchor. Appending during its return retains that in-flight
    // anchor once, outside the new explicit cycle.
    route_snapshot_node nodes[explicit_route_capacity]{};
    uint32_t            actor_life = 0, generation = 0;
    // Zero-backed scratch; readers initialize the finite-route sentinel explicitly.
    unsigned count = 0, cycle_begin = 0, cycle_cursor = 0;
    uint16_t implicit_x = 0, implicit_y = 0;
    bool     has_implicit_anchor = false, implicit_leg_active = false;
};
enum class route_read_result { unowned,
                               owned,
                               invalid };
using route_reader = route_read_result (*)(unsigned player, unsigned slot,
                                           uint32_t actor_life, route_snapshot &out);
// Register once during canonical owner installation, before presents. nullptr is
// the complete legacy/native-only configuration, not an empty gameplay queue.
void set_route_reader(route_reader reader);

enum class target_life_domain : uint8_t { presentation,
                                          canonical };
struct route_node {
    gfx::stratview::point visual{}, ground{};
    route_command_key     command{};
    uint32_t              entity = 0, target_life = 0;
    // Shared presentation grouping life is resolved only after the canonical
    // certificate and native visible anchor agree on this exact entity.
    // Never substitute this value for target_life or canonical validity.
    uint32_t           grouping_life = 0;
    target_life_domain life_domain   = target_life_domain::presentation;
    line_role          role          = line_role::movement;
    bool               known         = false;
};
enum class source_family : uint8_t { unit,
                                     building };
struct route_path {
    gfx::stratview::point source{}, origin{}, ground_origin{};
    route_node            nodes[route_node_capacity]{};
    uint32_t              actor_life = 0, generation = 0;
    uint16_t              actor  = 0;
    uint8_t               player = 0, mover = 0;
    source_family         family = source_family::unit;
    // Zero-backed scratch; producers initialize the finite-route sentinel explicitly.
    unsigned count = 0, cycle_begin = 0;
    // Native-pixel arc from the canonical first circuit node to the rotated next
    // node. This keeps animation attached to the circuit when its cursor rotates.
    double cycle_phase = 0;
    bool   canonical = false, source_visible = false;
};
struct route_collection {
    route_path paths[route_capacity]{};
    unsigned   count = 0;
};
// Includes native primaries, genuine two-endpoint patrol, successful independent
// secondary fire and selected-building rally/turret. Owned canonical primaries
// replace only legacy PRIMARY presentation, never its independent secondary.
void collect_routes(const gfx::stratview::world_view &, gfx::stratview::world_units &,
                    unsigned player, const uint16_t *ids, unsigned count,
                    unsigned selected_building, route_collection &out);

// Each edge and marker is an independent primitive. Coverage of ALL primitives
// of one role is unioned before a sample is blended, including torus copies.
inline constexpr unsigned route_primitive_capacity =
    route_selection_capacity * (2 * route_node_capacity + 2) + route_capacity * 2;
struct route_primitive {
    gfx::stratview::point source{}, origin{}, destination{};
    double                arc = 0, cycle_length = 0;
    line_role             role = line_role::movement;
    bool                  stem = false, marker = false;
    // Native-pixel wavelength. All edges share one presentation-time phase rate;
    // wavelength fitting may vary in an unshared cyclic suffix to close its seam.
    double phase_period = 0;
};
struct route_geometry {
    route_primitive items[route_primitive_capacity]{};
    unsigned        count = 0;
};
// Fixed scratch; no gameplay state, allocation, lifetime inference or progression.
// False is invalid input, never silent truncation of accepted nodes.
bool build_route_geometry(const gfx::stratview::world_view &, const route_collection &,
                          bool reveal, route_geometry &out);
} // namespace mh::ui::selected_targets::detail
