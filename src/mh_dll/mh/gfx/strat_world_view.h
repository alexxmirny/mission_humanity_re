// Read-only strategic placement, shared by selection and world overlays.
#pragma once
#include <cstdint>

namespace mh::gfx::stratview {

struct point {
    int x = 0, y = 0;
};
struct rect {
    int left = 0, top = 0, right = 0, bottom = 0; // half-open, pre-zoom pixels
};
struct crop {
    int left = 0, top = 0, width = 0, height = 0;
};
struct world_view {
    int  width = 0, height = 0;
    int  map_width = 0, map_height = 0; // tiles
    int  span_x = 0, span_y = 0;        // fine pixels
    int  camera_x = 0, camera_y = 0;    // native interpolated scroll origin
    crop shown{};
    bool biased_wrap = false;
    // Native body-bank phase reconstructed once in its tile/chain draw order.
    uint32_t body_phase[8][128]{};
};
struct unit_projection {
    point anchor{}; // interpolated body origin, before sprite metadata offset and zoom
    rect  body{};
    int   sprite       = 0;
    bool  tile_visible = false;
};

// Collection-local membership: walk only queried units' tiles, at most once per tile.
// Never retain across a present/import. The native links, not roster existence, decide membership.
struct world_units {
    uint32_t bits[8][128]{};
    uint32_t checked_tiles[2048]{};
    bool     contains(const world_view &view, unsigned player, unsigned unit_id);
};

// The caller supplies the actual native crop if the wheel-zoom module is unarmed.
// No fallback supplied => false, rather than inventing an unzoomed viewport.
bool read_world_view(world_view &out, const crop *native_fallback = nullptr);
// Reads the stock strategic zoom/shake scales and computes its centred crop.
bool read_native_crop(crop &out);
// Projection uses the same per-site stock/biased wrap choice as view_wrap.cpp.
point project_fine(const world_view &view, int fine_x, int fine_y);
// Does not assert tile-chain presence: query world_units for the same collection.
// A native-culled mapped tile can still supply an anchor with an empty body.
bool  project_unit(const world_view &view, unsigned player, unsigned unit_id, unit_projection &out);
bool  visible_body(const world_view &view, const unit_projection &unit);
point prezoom_to_display(const world_view &view, point prezoom);

} // namespace mh::gfx::stratview
