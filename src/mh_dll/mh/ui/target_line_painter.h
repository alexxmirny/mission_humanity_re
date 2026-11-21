// Selected-target artwork in the live RGB565 world surface; no native line primitive.
#pragma once
#include "gfx/strat_world_view.h"
#include "ui/target_line_routes.h"

namespace mh::ui::selected_targets::detail {

enum class grouping_mode { subgroups,
                           shared_command };

struct line_options {
    // Stroke width, dash/gap lengths, endpoint/arrow artwork and speed use native
    // prezoom pixels (the map's 1x pixels); artwork grows with native map zoom.
    // The dark outline extends one native pixel beyond every core stroke.
    // Ground distances and grouping radii are map tiles (32 world fine pixels).
    float         opacity             = 0.8f;
    float         width               = 1.0f;
    float         minimum_tiles       = 20.0f;
    float         speed               = 15.384615f;
    float         dash_length         = 7.0f;
    float         gap_length          = 7.0f;
    float         source_radius_tiles = 2.0f;
    float         goal_radius_tiles   = 1.0f;
    float         movement_color[3]   = {64.0f / 255.0f, 1.0f, 96.0f / 255.0f};
    float         attack_color[3]     = {1.0f, 80.0f / 255.0f, 64.0f / 255.0f};
    float         rally_color[3]      = {1.0f, 224.0f / 255.0f, 64.0f / 255.0f};
    float         patrol_color[3]     = {96.0f / 255.0f, 176.0f / 255.0f, 1.0f};
    grouping_mode grouping            = grouping_mode::shared_command;
};

line_options &options();

// Batch union prevents same-role joins, retraces and wrapped copies from darkening.
// All dimensions are native prezoom pixels. Returns tight changed-pixel bounds,
// dirtying every touched ground tile. Call after terrain, before native objects.
gfx::stratview::rect paint_target_routes(const gfx::stratview::world_view &view,
                                         const route_geometry             &geometry,
                                         double                            presentation_ms);

} // namespace mh::ui::selected_targets::detail
