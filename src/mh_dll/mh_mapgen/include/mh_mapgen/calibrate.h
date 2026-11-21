// Pixel-only seam calibration; observed pairs are positive/unlabelled, never negative labels.
#pragma once
#include "mh_mapgen/rules.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace mh_mapgen {
struct SeamObservation {
    std::uint32_t score;
    std::uint16_t a, b;
    std::uint32_t weight;
};
struct SeamCalibration {
    std::vector<SeamObservation> pairs;
    std::uint64_t                positive_weight = 0;
};
struct PixelCalibration {
    SeamCalibration horizontal, vertical;
    std::string     art_sha256, rules_sha256;
};

// RGB565 values, not palette indices. The score compares one-pixel boundary and the
// change from the adjacent interior pixel, in coordinate order without mirroring.
inline int rgb565_channel(std::uint16_t p, int channel) {
    return channel == 0 ? int((p >> 11) & 31) * 2 : channel == 1 ? int((p >> 5) & 63)
                                                                 : int(p & 31) * 2;
}
inline std::uint32_t pixel_seam_score(const std::uint16_t *a, const std::uint16_t *b, bool vertical) {
    std::uint32_t s = 0;
    for (int i = 0; i < 32; ++i) {
        const int ea = vertical ? 31 * 32 + i : i * 32 + 31;
        const int ia = vertical ? 30 * 32 + i : i * 32 + 30;
        const int eb = vertical ? i : i * 32;
        const int ib = vertical ? 32 + i : i * 32 + 1;
        for (int c = 0; c < 3; ++c) {
            const int av = rgb565_channel(a[ea], c), bv = rgb565_channel(b[eb], c);
            const int ag = av - rgb565_channel(a[ia], c), bg = rgb565_channel(b[ib], c) - bv;
            const int d = av - bv, g = ag - bg;
            s += std::uint32_t((d < 0 ? -d : d) * 2 + (g < 0 ? -g : g));
        }
    }
    return s;
}
// Decode selected active strategic tiles to emitted RGB565 values. No palette-index comparison.
inline bool decode_tlo565(const std::uint8_t *bytes, std::size_t n, const Rules &r, std::vector<std::uint16_t> &pixels, std::string &error) {
    pixels.clear();
    if (!bytes || n < 512 + 1024 || (n - 512) % 1024 || (n - 512) / 1024 > 8192) {
        error = "invalid strategic TLO length";
        return false;
    }
    if (r.tiles.empty() || r.tiles.size() > 1024) {
        error = "active tile count must be 1..1024";
        return false;
    }
    const std::size_t              nt = (n - 512) / 1024;
    std::array<std::uint16_t, 256> palette{};
    for (std::size_t i = 0; i < 256; ++i) palette[i] = std::uint16_t(bytes[i * 2] | (unsigned(bytes[i * 2 + 1]) << 8));
    for (std::size_t i = 0; i < r.tiles.size(); ++i)
        if (r.tiles[i].id >= nt || (i && r.tiles[i - 1].id >= r.tiles[i].id)) {
            error = "tile ID outside TLO or unsorted/duplicate";
            return false;
        }
    pixels.resize(r.tiles.size() * 1024);
    for (std::size_t i = 0; i < r.tiles.size(); ++i) {
        const auto *src = bytes + 512 + std::size_t(r.tiles[i].id) * 1024;
        for (std::size_t p = 0; p < 1024; ++p) pixels[i * 1024 + p] = palette[src[p]];
    }
    return true;
}
inline bool calibrate_pixels(const std::uint8_t *bytes, std::size_t n, const Rules &r, PixelCalibration &out, std::string &error) {
    out = PixelCalibration{};
    std::vector<std::uint16_t> pixels;
    if (!decode_tlo565(bytes, n, r, pixels, error)) return false;
    const auto collect = [&](bool vertical, const std::vector<PairRule> &known, SeamCalibration &dst) -> bool {
        for (std::size_t i = 0; i < known.size(); ++i) {
            if (!known[i].n || !r.find_tile(known[i].a) || !r.find_tile(known[i].b) ||
                (i && (known[i - 1].a > known[i].a || (known[i - 1].a == known[i].a && known[i - 1].b >= known[i].b)))) return false;
            dst.positive_weight += known[i].n;
        }
        dst.pairs.reserve(r.tiles.size() * r.tiles.size());
        std::size_t k = 0;
        for (std::size_t a = 0; a < r.tiles.size(); ++a)
            for (std::size_t b = 0; b < r.tiles.size(); ++b) {
                const auto    ta = r.tiles[a].id, tb = r.tiles[b].id;
                std::uint32_t weight = 0;
                if (k < known.size() && known[k].a == ta && known[k].b == tb) weight = known[k++].n;
                dst.pairs.push_back({pixel_seam_score(&pixels[a * 1024], &pixels[b * 1024], vertical), ta, tb, weight});
            }
        std::sort(dst.pairs.begin(), dst.pairs.end(), [](const SeamObservation &a, const SeamObservation &b) {
            if (a.score != b.score) return a.score < b.score;
            if (a.a != b.a) return a.a < b.a;
            return a.b < b.b;
        });
        return k == known.size();
    };
    if (!collect(false, r.h, out.horizontal) || !collect(true, r.v, out.vertical)) {
        error = "invalid observed pair references/order";
        return false;
    }
    out.art_sha256         = sha256_hex(bytes, n);
    const std::string text = rules_to_text(r);
    out.rules_sha256       = sha256_hex(reinterpret_cast<const std::uint8_t *>(text.data()), text.size());
    return true;
}
} // namespace mh_mapgen
