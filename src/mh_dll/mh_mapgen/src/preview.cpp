// mh_mapgen -- the map preview renderer (see include/mh_mapgen/preview.h).
#include "mh_mapgen/preview.h"
#include <algorithm>
#include <cmath>

namespace mh_mapgen {

namespace {

constexpr std::size_t TLO_PALETTE_BYTES = 0x200;
constexpr std::size_t TLO_TILE_BYTES    = 1024; // 32x32, one palette index per pixel
constexpr int         MAX_DIM           = 4096; // far past the game's 256; keeps w*h in size_t on 32-bit

inline std::uint16_t darken(std::uint16_t v) { // trees: 60% of each channel
    const unsigned r = ((v >> 11) & 31u) * 3u / 5u, g = ((v >> 5) & 63u) * 3u / 5u, b = (v & 31u) * 3u / 5u;
    return std::uint16_t((r << 11) | (g << 5) | b);
}

inline std::uint16_t pack565(unsigned r, unsigned g, unsigned b) { return std::uint16_t((r << 11) | (g << 5) | b); }

} // namespace

bool tlo_tile_means(const std::uint8_t *tlo, std::size_t n, std::vector<std::uint16_t> &out565) {
    out565.clear();
    if (!tlo || n < TLO_PALETTE_BYTES + TLO_TILE_BYTES) return false;
    std::uint16_t pr[256], pg[256], pb[256];
    for (int i = 0; i < 256; ++i) {
        const std::uint16_t v = std::uint16_t(tlo[i * 2] | (tlo[i * 2 + 1] << 8));
        pr[i]                 = std::uint16_t(v >> 11);
        pg[i]                 = std::uint16_t((v >> 5) & 63);
        pb[i]                 = std::uint16_t(v & 31);
    }
    const std::size_t tiles = (n - TLO_PALETTE_BYTES) / TLO_TILE_BYTES;
    out565.resize(tiles);
    for (std::size_t t = 0; t < tiles; ++t) {
        const std::uint8_t *px = tlo + TLO_PALETTE_BYTES + t * TLO_TILE_BYTES;
        unsigned            r = 0, g = 0, b = 0;
        for (std::size_t i = 0; i < TLO_TILE_BYTES; ++i) {
            r += pr[px[i]];
            g += pg[px[i]];
            b += pb[px[i]];
        }
        out565[t] = pack565(r >> 10, g >> 10, b >> 10); // the game's rule: sum >> 10 per channel
    }
    return true;
}

bool render_preview(const MpmModel &m, const std::uint16_t *tile565, std::size_t ntiles, int size, std::uint16_t *out565,
                    PreviewStats *stats) {
    const int w = int(m.width), h = int(m.height);
    if (size < 8 || size > 4096 || !out565 || w <= 0 || h <= 0 || w > MAX_DIM || h > MAX_DIM) return false;
    const std::size_t cells = std::size_t(w) * std::size_t(h);
    if (m.flags.size() != cells) return false;
    PreviewStats st;
    st.w = w;
    st.h = h;

    // content rectangle: the map keeps its aspect inside size x size
    const int dim = std::max(w, h);
    const int cw = std::max(1, int(std::int64_t(w) * size / dim)), ch = std::max(1, int(std::int64_t(h) * size / dim));
    const int ox = (size - cw) / 2, oy = (size - ch) / 2;

    // tree mask, column-major like the planes
    std::vector<std::uint8_t> tree(cells, 0);
    for (const MpmTree &t : m.trees)
        if (t.x >= 0 && t.y >= 0 && t.x < w && t.y < h) {
            std::uint8_t &s = tree[std::size_t(t.x) * std::size_t(h) + std::size_t(t.y)];
            if (!s) ++st.trees;
            s = 1;
        }

    auto tile_col = [&](int c, int r) -> std::uint16_t {
        const std::size_t i  = std::size_t(c) * std::size_t(h) + std::size_t(r);
        const std::size_t id = std::size_t(m.flags[i] & TILE_ID_MASK);
        std::uint16_t     v  = (tile565 && id < ntiles) ? tile565[id] : PREVIEW_UNKNOWN;
        return tree[i] ? darken(v) : v;
    };

    std::vector<std::uint16_t> buf(std::size_t(size) * std::size_t(size), 0);
    const bool                 box_x = w >= cw, box_y = h >= ch;
    for (int py = 0; py < ch; ++py) {
        int y0 = int(std::int64_t(py) * h / ch), y1 = int(std::int64_t(py + 1) * h / ch);
        if (!box_y) y1 = y0 + 1;
        y1 = std::max(y1, y0 + 1);
        for (int px = 0; px < cw; ++px) {
            int x0 = int(std::int64_t(px) * w / cw), x1 = int(std::int64_t(px + 1) * w / cw);
            if (!box_x) x1 = x0 + 1;
            x1         = std::max(x1, x0 + 1);
            unsigned r = 0, g = 0, b = 0, n = 0;
            for (int yy = y0; yy < y1 && yy < h; ++yy)
                for (int xx = x0; xx < x1 && xx < w; ++xx) {
                    const std::uint16_t v = tile_col(xx, yy);
                    r += v >> 11;
                    g += (v >> 5) & 63;
                    b += v & 31;
                    ++n;
                }
            if (!n) continue;
            buf[std::size_t(oy + py) * size + std::size_t(ox + px)] = pack565((r + n / 2) / n, (g + n / 2) / n, (b + n / 2) / n);
        }
    }

    auto put = [&](int x, int y, std::uint16_t v) {
        if (x >= 0 && y >= 0 && x < size && y < size) buf[std::size_t(y) * size + std::size_t(x)] = v;
    };
    const double sx = double(cw) / w, sy = double(ch) / h;

    // ore: any of channels 0-3 set in a 4x4 block -> a gold square, half a block, centred
    const std::size_t bw = std::size_t(w) / 4, bh = std::size_t(h) / 4;
    if (bw && bh && m.ore.size() >= bw * bh * ORE_WORDS) {
        const int bx = std::max(1, int(std::lround(2 * sx))), by = std::max(1, int(std::lround(2 * sy)));
        for (std::size_t bc = 0; bc < bw; ++bc)
            for (std::size_t br = 0; br < bh; ++br) {
                const std::uint16_t *o = &m.ore[(bc * bh + br) * ORE_WORDS];
                if (!(o[0] | o[1] | o[2] | o[3])) continue;
                ++st.ore_blocks;
                const int cx = ox + int(double(bc) * 4 * sx + 2 * sx - bx / 2.0), cy = oy + int(double(br) * 4 * sy + 2 * sy - by / 2.0);
                for (int yy = 0; yy < by; ++yy)
                    for (int xx = 0; xx < bx; ++xx) put(cx + xx, cy + yy, PREVIEW_ORE);
            }
    }

    // landing spots: 7x7, a black outline round a white fill
    for (const MpmLanding &l : m.landings) {
        if (l.x < 0 || l.y < 0 || l.x >= w || l.y >= h) continue;
        ++st.spots;
        const int cx = ox + int((l.x + 0.5) * sx), cy = oy + int((l.y + 0.5) * sy);
        for (int yy = -3; yy <= 3; ++yy)
            for (int xx = -3; xx <= 3; ++xx) put(cx + xx, cy + yy, (xx == -3 || xx == 3 || yy == -3 || yy == 3) ? PREVIEW_SPOT_OUT : PREVIEW_SPOT_IN);
    }

    std::copy(buf.begin(), buf.end(), out565);
    if (stats) *stats = st;
    return true;
}

} // namespace mh_mapgen
