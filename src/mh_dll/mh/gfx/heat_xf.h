//
// heat_xf.h -- mods:M1WL v6: the heat map's tile -> screen geometry, free of ImGui and of the game's memory so a hosted selftest (mapzoomtest) can drive it with the
// user's real layout numbers. gfx/overlay_imgui.cpp fills one from the live state (camera, map size, zoom crop, the presenter's mapping) and draws with it.
//
// A tile at column offset di from the camera starts at viewport pixel (di * 32 - zl) * vw / zcw (the zoom crop stretched over the viewport); game pixel g -> client
// pixel ox + g * sx. The camera is a torus, and the camera tile is the view's TOP-LEFT one: the visible offsets are [floor(zl / 32), floor((zl + zcw - 1) / 32)], i.e.
// 0 .. view-width - 1 (NOT centred on 0), so the offsets are folded from the first visible tile on (first_i / first_j) and a view wider than the map shows the
// same tile again (each_copy). rc11 folded into [-mw/2, mw/2): tiles 32 or more right / below the camera wrapped to negative and were skipped -- the holes.
//
#pragma once

namespace mh::gfx::heat {

constexpr int HEAT_TILE_PX = 32;

struct heat_xf {
    bool  ok   = false;
    int   camc = 0, camr = 0, mw = 0, mh = 0;
    int   zl = 0, zt = 0, zcw = 1, zch = 1, vw = 0, vh = 0;
    float ox = 0, oy = 0, sx = 1, sy = 1; // game pixel -> client pixel: ox + g * sx
    float cx1 = 0, cy1 = 0;               // the visible map's far corner in client pixels (the clip: the bottom strip may hide the viewport's last rows)
    // The game -> client map from the presenter's answers (owned_game_to_client = the CENTRE of the game pixel) at the origin, the viewport's middle and its
    // far corner. The slope comes from the first two, both INSIDE the visible map: at ui_scale the viewport is taller than the map quad, so the corner can sit
    // under the bottom strip, where the presenter answers the nearest visible pixel and a slope taken to it drifts. False for a degenerate map.
    bool set_map_points(int p0x, int p0y, int p1x, int p1y, int pcx, int pcy) {
        const int hx = vw / 2, hy = vh / 2;
        if (hx <= 0 || hy <= 0) return false;
        sx = (float)(p1x - p0x) / (float)hx;
        sy = (float)(p1y - p0y) / (float)hy;
        if (sx <= 0.0f || sy <= 0.0f) return false;
        ox  = (float)p0x - 0.5f * sx;
        oy  = (float)p0y - 0.5f * sy;
        cx1 = (float)pcx + 0.5f * sx; // the last visible pixel's far edge (== ox + vw * sx when nothing is hidden)
        cy1 = (float)pcy + 0.5f * sy;
        return true;
    }
    // First column / row offset a tile can be seen at: floor(zl / 32) - 1, so [base, base + mw) holds the whole crop. A view WIDER than the map shows
    // further copies (+ mw, + 2 mw ...) of the same tile.
    static int floor_tiles(int px) { return px >= 0 ? px / HEAT_TILE_PX : -((-px + HEAT_TILE_PX - 1) / HEAT_TILE_PX); }
    int        base_i() const { return floor_tiles(zl) - 1; }
    int        base_j() const { return floor_tiles(zt) - 1; }
    int        first_i(int c) const { return ((c - camc - base_i()) & (mw - 1)) + base_i(); }
    int        first_j(int r) const { return ((r - camr - base_j()) & (mh - 1)) + base_j(); }
    template <class F>
    void each_copy(int c, int r, F f) const { // f(di, dj) for every visible copy of tile (c, r)
        const int imax = (zl + zcw) / HEAT_TILE_PX + 1, jmax = (zt + zch) / HEAT_TILE_PX + 1;
        for (int dj = first_j(r); dj <= jmax; dj += mh)
            for (int di = first_i(c); di <= imax; di += mw) f(di, dj);
    }
    // client rect of a tile at offsets (di, dj) from the camera; false when it lies outside the crop
    template <class V>
    bool tile_rect(int di, int dj, V *a, V *b) const {
        const float gx0 = (float)((di * HEAT_TILE_PX - zl)) * (float)vw / (float)zcw, gx1 = (float)((di + 1) * HEAT_TILE_PX - zl) * (float)vw / (float)zcw;
        const float gy0 = (float)((dj * HEAT_TILE_PX - zt)) * (float)vh / (float)zch, gy1 = (float)((dj + 1) * HEAT_TILE_PX - zt) * (float)vh / (float)zch;
        if (gx1 <= 0 || gy1 <= 0 || gx0 >= (float)vw || gy0 >= (float)vh) return false;
        *a = V(ox + gx0 * sx, oy + gy0 * sy);
        *b = V(ox + gx1 * sx, oy + gy1 * sy);
        return true;
    }
    int fold(int d, int m) const { // [-m/2, m/2)
        d &= (m - 1);
        return d >= m / 2 ? d - m : d;
    }
    // the offsets of tile (c, r) from the camera
    void offsets(int c, int r, int *di, int *dj) const {
        *di = fold(c - camc, mw);
        *dj = fold(r - camr, mh);
    }
};

} // namespace mh::gfx::heat
