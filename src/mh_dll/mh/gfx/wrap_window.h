//
// gfx/wrap_window.h -- the biased torus window for world->screen sprite placement (RES3 / D13c).
//
// Pure arithmetic; no Windows, no game memory.
//
// THE STOCK FUNCTION. llm_strat_pixel_delta_wrapped (0x004944f6) turns a (camera, sprite) pair of world pixel
// coordinates into a screen offset by wrapping the difference the SHORT way round the torus:
//
//     d = to - from;  if (|d| > big/2) d = d - sign(d) * big      // result in [-big/2, big/2]
//
// That is right for a distance and wrong for a screen position once the view reaches past half a map: a
// sprite more than big/2 (1024 px on the 64-tile map, whose big is 2048) to the right of / below the camera
// lands at a NEGATIVE offset and is not drawn. Retail never gets there (its widest view is 864 px); a render
// size of 1536 wide does.
//
// THE BIASED WINDOW. For placement the useful range is not symmetric: a sprite stays visible while up to its own
// width to the LEFT of the viewport edge (its anchor is its top-left), and everything up to the viewport's far
// edge to the right. So the offset is taken from the window [-K, big-K) instead of [-big/2, big/2]:
//
//     d' = ((to - from + K) mod big) - K
//
// d' equals the stock result whenever that lies in [-K, big/2]; it differs only for a stock result left of -K
// (a sprite more than K px off the left edge, which the window files on the far right instead) and for results
// between big/2 and big-K (which stock folded to the left). A sprite is placed correctly while
//   K >= its extent to the right of its anchor        (so it is still shown when its anchor is K px left of the edge)
//   viewport + K <= big                               (so the visible range (-K, viewport) fits in one window)
//
// DERIVING K. The callers pass (world position - sprite origin), so the offset is the sprite's top-left corner and the
// quantity K must cover is how far the sprite's LAST drawn pixel lies right of / below that offset (a sprite is
// still partly on screen while offset + reach > 0). Measured over every sprite the render callers draw (BNK banks x
// INIT.CFG consumers, true decoded pixel extents, equal to the canvas sizes): units' body 178x154, shadow 193x159,
// selection frames (unit) 160x120, damage smoke 34x50, energy-bar frames 192x4, building selection frames 218x222 as
// drawn (up to 205 right / 193 below the building's tile corner after the baked marker offset), and the building
// animation frames the heli mothership / shuttle draws while landing and deploying (S_human_PORT_korpus) 280x222.
// K IS CHOSEN AT DRAW TIME: K = big - viewport on each axis (bias_for), the largest bias whose window still holds the
// whole viewport. K_X / K_Y below are only the floor it never drops under. A sprite whose reach is more than K is,
// while its offset lies in [-reach, -K), filed on the FAR right instead: it vanishes from the left edge and its leftmost
// (reach - K) px appear at the viewport's right edge until the offset leaves the range. With K = big - viewport that
// ghost needs reach > big - viewport, so it exists only where the viewport is within the deepest reach (280) of the map:
// on the 64-tile map (big 2048) at the 2048 px render-width cap (viewport 1888, K = 160, reaches 161..280 ghost). At
// 1920 wide K = 288 and every measured sprite is clean; the 128 and 256-tile maps are clean at every cap size.
// K_Y: the heights are capped far below the map, so K_Y = big - viewport is always above the deepest sprite (222).
//
// RENDER SITES ONLY. The sim also calls the stock function (llm_strat_weapon_pixel_distance_ratio,
// llm_strat_weapon_scatter_offset) and so does the cursor hit-test in llm_strat_render_view; none of those is a
// screen position, so the game's own function is left alone and only the render call sites are re-pointed at
// this window (seams/view_wrap.cpp). The sim result is therefore stock by construction.
//
#pragma once

namespace mh::gfx::wrapwin {

constexpr int K_X = 160; // horizontal bias floor, px (what the 2048 cap on a 64-tile map comes to: 2048 - 1888)
constexpr int K_Y = 256; // vertical bias floor, px (>= the deepest sprite, 222)

// The bias for one axis at draw time: big - viewport (the widest window that still holds the whole viewport),
// never below loor_k. viewport <= 0 (not readable) gives the floor.
int bias_for(int big, int viewport, int floor_k);

// The stock function's axis, exactly: sign * delta with the fold at big/2 (C++ signed /2 truncates like the
// asm's SAR idiom). Result in [-big/2, big/2].
int stock_axis(int from, int to, int big);

// The biased window for one axis: ((to - from + k) mod big) - k, mod being the mathematical (non-negative) one.
// big <= 0 degrades to the plain difference.
int biased_axis(int from, int to, int big, int k);

// The stock modular distance from `from` to `to` going the positive way round a torus of `big`: ((to - from) mod big)
// with the mathematical (non-negative) mod, in [0, big). The drag-select box's own width / height: the game picks the
// box's left edge so that this is the SHORTER of the two ways (<= big/2). big <= 0 degrades to to - from.
int span_axis(int from, int to, int big);

// The drag-select box's near edge on one axis: where the box's left (top) edge is drawn, given the camera and the two
// WORLD edges the game chose (left_w .. right_w, right_w - left_w (mod big) the box's width). The stock short-way
// position when that is left of -k (the anchor scrolled off the left of the view) but the box still reaches into the
// view (stock + width > 0): there the stock outline is the one a retail view draws and the biased window would file the
// edge on the far right. Otherwise the biased window (equal to the stock result wherever that lies in [-k, big/2]).
// The far edge is this + the width, so the box is always ordered.
int drag_near_axis(int cam, int left_w, int right_w, int big, int k);

// ---- the effect / projectile view gate (llm_strat_tile_in_view_and_visible, 0x00453c9f) ------------------------
// One axis of the stock test. The span is [cam-1, cam+tiles+1] on a torus of `map_tiles` (a power of two): left =
// cam-1 (the stock code uses map_tiles when cam is 0), right = (cam+tiles+1) & (map_tiles-1), and a tile is OUT when
// it lies in the gap. The span holds tiles+3 columns, so once tiles+3 >= map_tiles the right bound wraps round to
// land beside the left one and the stock test calls every tile but two or three out of view: the landing footprint,
// explosions and projectiles stop being drawn. That happens only where the window is at least 2016 px wide on a
// 64-tile map (the 2048 cap); a retail view never gets there. Here that case is the whole axis in view.
bool tile_in_span(int tile, int cam, int tiles, int map_tiles);

// The same test with the stock arithmetic kept as it is (for the selftest's equivalence check).
bool tile_in_span_stock(int tile, int cam, int tiles, int map_tiles);

} // namespace mh::gfx::wrapwin
