// mh_mapgen -- the in-game map preview (mods:MAPGEN2): a square RGB565 thumbnail of a strategic map.
//
// Terrain colour per tile = the game's own minimap colour, `llm_planet_tlo_thumbnail_build` (EN 0x00426f33):
// the mean of the tile's 32x32 pixels per RGB565 channel (sum >> 10). Trees darken their tile, ore blocks are
// small gold squares, landing spots white squares with a black outline. Pure: no file or OS API, so the same
// code runs in the CLI (`mapgen preview`), the offline tests and mh.dll's picker.
#pragma once
#include "mh_mapgen/mpm.h"
#include <cstddef>
#include <cstdint>
#include <vector>

namespace mh_mapgen {

constexpr int PREVIEW_SIZE = 128; // the in-game preview is exactly this square: 256 maps 2:1, 128 maps 1:1, 64 maps 1:2

// A .tlo is `u16 palette[256]` then 1024-byte tiles (one palette index per pixel, 32x32). Writes one RGB565
// colour per tile into out565 (tile count = (n - 0x200) / 1024). False when the image holds no tile.
bool tlo_tile_means(const std::uint8_t *tlo, std::size_t n, std::vector<std::uint16_t> &out565);

struct PreviewStats {
    int spots      = 0; // landing spots drawn (those inside the map)
    int ore_blocks = 0; // 4x4 ore blocks with any of channels 0-3 set
    int trees      = 0;
    int w = 0, h = 0; // the map's tiles
};

// Render `m` into `out565` (size*size RGB565, row-major, row 0 at the top; the map's column is x, its row is y).
// `tile565[id]` is the colour of tile id (flags & TILE_ID_MASK); an id >= ntiles draws a flat mid grey. A map
// larger than `size` is box-filtered, a smaller one is scaled by nearest neighbour; a non-square map keeps its
// aspect, centred on black. False (and the buffer untouched) for size < 8, an empty map or a model whose
// planes do not match its width x height -- a hostile or truncated file must never index out of range.
bool render_preview(const MpmModel &m, const std::uint16_t *tile565, std::size_t ntiles, int size, std::uint16_t *out565,
                    PreviewStats *stats = nullptr);

// The colours the overlay uses (RGB565), shared so tests and the mockups agree.
constexpr std::uint16_t PREVIEW_ORE      = 0xFEA8; // (255,215,64)
constexpr std::uint16_t PREVIEW_SPOT_IN  = 0xFFFF;
constexpr std::uint16_t PREVIEW_SPOT_OUT = 0x0000;
constexpr std::uint16_t PREVIEW_UNKNOWN  = 0x8410; // tile id past the tileset

} // namespace mh_mapgen
