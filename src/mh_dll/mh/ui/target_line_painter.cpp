#include "ui/target_line_painter.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <intrin.h>
#include "gfx/wrap_window.h"
#include "include/mh_tile_dirty.h"
#include "state/region_runtime.h"

namespace mh::ui::selected_targets::detail {
namespace {
namespace sv = gfx::stratview;
using sv::point;
using sv::rect;
using sv::world_view;
line_options settings;

struct vec {
    double x, y;
};
vec    operator+(vec a, vec b) { return {a.x + b.x, a.y + b.y}; }
vec    operator-(vec a, vec b) { return {a.x - b.x, a.y - b.y}; }
vec    operator*(vec a, double s) { return {a.x * s, a.y * s}; }
double dot(vec a, vec b) { return a.x * b.x + a.y * b.y; }
double norm(vec a) { return std::sqrt(dot(a, a)); }
double bounded(float value, double lo, double hi, double fallback) {
    return std::isfinite(value) ? std::clamp(static_cast<double>(value), lo, hi) : fallback;
}
double modulo(double value, double period) {
    const double r = std::fmod(value, period);
    return r < 0.0 ? r + period : r;
}
void include(rect &a, int x, int y) {
    if (a.right <= a.left || a.bottom <= a.top) a = {x, y, x + 1, y + 1};
    else {
        a.left   = std::min(a.left, x);
        a.top    = std::min(a.top, y);
        a.right  = std::max(a.right, x + 1);
        a.bottom = std::max(a.bottom, y + 1);
    }
}
struct rgb565 {
    unsigned r, g, b;
};
rgb565 role_color(line_role role) {
    const float *rgb = settings.movement_color;
    switch (role) {
        case line_role::movement: break;
        case line_role::attack: rgb = settings.attack_color; break;
        case line_role::rally: rgb = settings.rally_color; break;
        case line_role::patrol: rgb = settings.patrol_color; break;
    }
    const auto channel = [](float value, unsigned maximum) {
        return static_cast<unsigned>(bounded(value, 0.0, 1.0, 1.0) * maximum + 0.5);
    };
    return {channel(rgb[0], 31), channel(rgb[1], 63), channel(rgb[2], 31)};
}

// A connector is composited once per native sample, after unioning every
// constituent stroke and translated torus copy. Scratch is render-thread-only.
constexpr int raster_limit = 4096;

struct row_scratch {
    uint16_t core[raster_limit]{};
    uint16_t outer[raster_limit]{};
    uint16_t touched[raster_limit]{};
    unsigned count = 0;
};

row_scratch raster_rows[32];

struct stem_geometry {
    vec    unit{}, normal{}, wing{};
    vec    arrow_normal[2]{};
    double length = 0.0;
    double period = 0.0, phase = 0.0;
    double arrow_period = 0.0, arrow_phase = 0.0;
    double arrow_end = 0.0;
};

stem_geometry prepare_stem(point source, point end,
                           double milliseconds, double speed,
                           double dash, double gap, double arc, double phase_period) {
    stem_geometry g;
    const vec     d{static_cast<double>(end.x) - source.x,
                static_cast<double>(end.y) - source.y};
    g.length = norm(d);
    if (g.length <= 0.000001) return g;

    g.unit   = d * (1.0 / g.length);
    g.normal = {-g.unit.y, g.unit.x};
    g.wing   = g.normal * 3.0;

    // Full arrow wings have length sqrt(5*5 + 3*3). Endpoint clipping
    // changes their lengths, not their directions.
    constexpr double inv_arrow_length = 0.17149858514250884;
    for (int i = 0; i < 2; ++i) {
        const double side = i == 0 ? -1.0 : 1.0;
        const vec    edge = g.unit * 5.0 - g.wing * side;
        g.arrow_normal[i] = {-edge.y * inv_arrow_length,
                             edge.x * inv_arrow_length};
    }

    const double nominal = dash + gap;
    g.period             = phase_period > 0.000001 ? phase_period : nominal;
    // One normalized clock across branches with different fitted wavelengths.
    // A shared edge and either cyclic suffix meet at the same phase at EVERY
    // presentation time, not just at the instant their geometry was constructed.
    const double phase = milliseconds * (speed / (1000.0 * nominal)) - arc / g.period;
    g.phase            = modulo(phase, 1.0) * g.period;

    // Independent modulo: never derive this from the wrapped dash phase.
    g.arrow_period = 3.0 * g.period;
    g.arrow_phase =
        modulo(phase + 2.0, 3.0) * g.period;
    g.arrow_end = std::max(0.0, g.length - 8.0);
    return g;
}

uint16_t composite(uint16_t pixel, rgb565 color,
                   unsigned core_alpha, unsigned outer_alpha) {
    // Outline colour is black. Core and outline partition the same coverage:
    // core_alpha + outline_alpha == outer_alpha. Do not blend them sequentially.
    const unsigned inv = 256 - outer_alpha;
    const unsigned r =
        (((pixel >> 11) & 31u) * inv + color.r * core_alpha + 128) >> 8;
    const unsigned g =
        (((pixel >> 5) & 63u) * inv + color.g * core_alpha + 128) >> 8;
    const unsigned b =
        ((pixel & 31u) * inv + color.b * core_alpha + 128) >> 8;
    return static_cast<uint16_t>((r << 11) | (g << 5) | b);
}

struct canvas {
    uint8_t *pixels = nullptr;
    int      pitch  = 0;
    rect     clip{}, damage{};
    int      row   = 0;
    double   width = 1.0, alpha = 0.8;
    uint32_t dirty_columns[4]{};
    int      dirty_tile_row = -1;

    vec logical(point p) const {
        // Native integer anchors name pixel centres. No zoom compensation.
        return {p.x + 0.5, p.y + 0.5};
    }

    void accumulate(int x, double core, double outer) {
        auto          &raster_row = raster_rows[static_cast<unsigned>(row) & 31u];
        const unsigned o          = static_cast<unsigned>(
            std::clamp(outer * 256.0 + 0.5, 0.0, 256.0));
        if (!o) return;
        const unsigned k = std::min(
            o, static_cast<unsigned>(
                   std::clamp(core * 256.0 + 0.5, 0.0, 256.0)));

        if (!raster_row.outer[x])
            raster_row.touched[raster_row.count++] =
                static_cast<uint16_t>(x);
        raster_row.core[x] = static_cast<uint16_t>(
            std::max<unsigned>(raster_row.core[x], k));
        raster_row.outer[x] = static_cast<uint16_t>(
            std::max<unsigned>(raster_row.outer[x], o));
    }

    void flush(rgb565 color) {
        auto          &raster_row = raster_rows[static_cast<unsigned>(row) & 31u];
        const unsigned opacity    = static_cast<unsigned>(
            std::clamp(alpha * 256.0 + 0.5, 0.0, 256.0));
        auto *dst = reinterpret_cast<uint16_t *>(
            pixels + static_cast<size_t>(row) * pitch);

        const int tile_row = row >> 5;
        if (tile_row != dirty_tile_row) {
            for (auto &bits : dirty_columns) bits = 0;
            dirty_tile_row = tile_row;
        }

        for (unsigned i = 0; i < raster_row.count; ++i) {
            const int      x = raster_row.touched[i];
            const unsigned core_alpha =
                (raster_row.core[x] * opacity + 128) >> 8;
            const unsigned outer_alpha =
                (raster_row.outer[x] * opacity + 128) >> 8;

            // Clear before any unchanged-pixel exit.
            raster_row.core[x]  = 0;
            raster_row.outer[x] = 0;
            if (!outer_alpha) continue;

            const uint16_t result =
                composite(dst[x], color, core_alpha, outer_alpha);
            if (result == dst[x]) continue;
            dst[x] = result;
            include(damage, x, row);

            // Validated framebuffer width <=4096 means at most128 tile columns.
            const unsigned col  = static_cast<unsigned>(x >> 5);
            uint32_t      &word = dirty_columns[col >> 5];
            const uint32_t bit  = uint32_t{1} << (col & 31u);
            if (!(word & bit)) {
                gfx::mark_ground_tiles_dirty(x, row, 1, 1);
                word |= bit;
            }
        }
        raster_row.count = 0;
    }

    bool interval(vec a, vec b, double margin,
                  double &lo, double &hi, int top, int bottom) const {
        lo              = 0.0;
        hi              = 1.0;
        const vec  d    = b - a;
        const auto edge = [&](double p, double q) {
            if (p == 0.0) return q >= 0.0;
            const double t = q / p;
            if (p < 0.0) {
                if (t > hi) return false;
                lo = std::max(lo, t);
            } else {
                if (t < lo) return false;
                hi = std::min(hi, t);
            }
            return true;
        };
        return edge(-d.x, a.x - clip.left + margin) &&
               edge(d.x, clip.right + margin - a.x) &&
               edge(-d.y, a.y - top + margin) &&
               edge(d.y, bottom + margin - a.y);
    }

    void stroke(vec a, vec b, rgb565, vec normal) {
        // The expanded outer capsule plus native sample-footprint support.
        const double outer_width      = width + 2.0;
        const double margin           = outer_width * 0.5 + 1.0;
        const double reject_distance2 = margin * margin;
        const vec    d                = b - a;
        const double length2          = dot(d, d);
        const double inverse_length2  = length2 > 0.0 ? 1.0 / length2 : 0.0;
        // Bound conversion by the validated stripe before converting to int.
        const int first_row = static_cast<int>(std::clamp(
            std::floor(std::min(a.y, b.y) - margin),
            static_cast<double>(clip.top), static_cast<double>(clip.bottom)));
        const int last_row  = static_cast<int>(std::clamp(
            std::ceil(std::max(a.y, b.y) + margin),
            static_cast<double>(clip.top), static_cast<double>(clip.bottom)));

        for (row = first_row; row < last_row; ++row) {
            double lo, hi;
            if (!interval(a, b, margin, lo, hi, row, row + 1)) continue;
            const vec ca = a + d * lo, cb = a + d * hi;

            // Retain the original per-row clipping arithmetic.
            const int first = std::max(
                clip.left,
                static_cast<int>(std::floor(std::min(ca.x, cb.x) - margin)));
            const int last = std::min(
                clip.right,
                static_cast<int>(std::ceil(std::max(ca.x, cb.x) + margin)));

            const auto &raster_row = raster_rows[static_cast<unsigned>(row) & 31u];
            for (int x = first; x < last; ++x) {
                // Both channels are max-unions of values bounded by 256.
                if (raster_row.core[x] == 256 && raster_row.outer[x] == 256)
                    continue;
                const vec    p{x + 0.5, row + 0.5};
                const double along =
                    length2 > 0.0
                        ? std::clamp(dot(p - a, d) * inverse_length2, 0.0, 1.0)
                        : 0.0;
                const vec    away      = p - (a + d * along);
                const double distance2 = dot(away, away);
                // The sample footprint is at most sqrt(2), so margin's
                // full extra pixel conservatively encloses all outer coverage.
                if (distance2 >= reject_distance2) continue;
                const double distance = std::sqrt(distance2);
                const vec    n =
                    distance > 0.000001 ? away * (1.0 / distance) : normal;
                const double footprint         = std::abs(n.x) + std::abs(n.y);
                const double inverse_footprint = 1.0 / footprint;

                const auto coverage = [&](double w) {
                    return std::clamp(
                        (w * 0.5 + footprint * 0.5 - distance) * inverse_footprint,
                        // Actual normal footprints are <2, so wide strokes'
                        // upper cap is1; thin cores keep their fractional cap.
                        0.0, w >= 2.0 ? 1.0 : std::min(1.0, w * inverse_footprint));
                };
                const double outer = coverage(outer_width);
                if (outer > 0.0)
                    accumulate(x, coverage(width), outer);
            }
        }
    }
    void endpoint(vec p, rgb565 color, bool rally) {
        constexpr double r = 6.0;
        constexpr vec    diamond_normal{
            0.44721359549995794, 0.8944271909999159};
        constexpr vec flag_normal{
            0.39391929857916763, 0.9191450300180578};
        constexpr vec horizontal_normal{0.0, 1.0};
        constexpr vec vertical_normal{1.0, 0.0};

        // The fallback footprint uses only absolute normal components.
        stroke(p + vec{-r, 0}, p + vec{0, -r * 0.5},
               color, diamond_normal);
        stroke(p + vec{0, -r * 0.5}, p + vec{r, 0},
               color, diamond_normal);
        stroke(p + vec{r, 0}, p + vec{0, r * 0.5},
               color, diamond_normal);
        stroke(p + vec{0, r * 0.5}, p + vec{-r, 0},
               color, diamond_normal);
        stroke(p + vec{-r - 2, 0}, p + vec{r + 2, 0},
               color, horizontal_normal);
        stroke(p + vec{0, -r}, p + vec{0, r},
               color, vertical_normal);

        if (rally) {
            stroke(p + vec{0, -r}, p + vec{0, -3 * r},
                   color, vertical_normal);
            stroke(p + vec{0, -3 * r}, p + vec{r + 1, -2.5 * r},
                   color, flag_normal);
            stroke(p + vec{r + 1, -2.5 * r}, p + vec{0, -2 * r},
                   color, flag_normal);
        }
    }
    void stem(vec a, vec b, rgb565 color,
              const stem_geometry &g, double dash) {
        if (g.length <= 0.000001) return;

        double lo, hi;
        // Include arrow wings, outline and native sample-footprint support.
        if (!interval(a, b, 10.0 + width, lo, hi, clip.top, clip.bottom)) return;

        const double from = std::max(0.0, lo * g.length - 8.0);
        const double to   = std::min(g.length, hi * g.length + 8.0);

        for (double s =
                 std::floor((from - g.phase - dash) / g.period) *
                     g.period +
                 g.phase;
             s <= to; s += g.period) {
            const double first = std::max(0.0, s);
            const double last  = std::min(g.length, s + dash);
            if (last > first)
                stroke(a + g.unit * first, a + g.unit * last,
                       color, g.normal);
        }

        for (double s =
                 std::ceil((from - g.arrow_phase) / g.arrow_period) *
                     g.arrow_period +
                 g.arrow_phase;
             s <= std::min(g.length + 5.0, hi * g.length + 8.0);
             s += g.arrow_period) {
            // Clip wings against source/end planes without popping whole arrows.
            const double first = std::max(0.0, (5.0 - s) / 5.0);
            const double last =
                std::min(1.0, (g.arrow_end + 5.0 - s) / 5.0);
            if (last <= first) continue;

            const vec head = a + g.unit * s;
            const vec tail = head - g.unit * 5.0;
            for (int i = 0; i < 2; ++i) {
                const double side = i == 0 ? -1.0 : 1.0;
                const vec    base = tail + g.wing * side;
                const vec    edge = head - base;
                stroke(base + edge * first, base + edge * last,
                       color, g.arrow_normal[i]);
            }
        }
    }
};

bool open_canvas(const world_view &v, canvas &c) {
    using namespace mh::state;
    const auto *fb    = ptr<const uint32_t>(RID_FRAMEBUFFER);
    const auto *pitch = ptr<const int32_t>(RID_FB_PITCH);
    const auto *fw = ptr<const int32_t>(RID_WINDOWWIDTH), *fh = ptr<const int32_t>(RID_WINDOWHEIGHT);
    if (!fb || !pitch || !fw || !fh || !*fb || *fw <= 0 || *fh <= 0 || *fw > 4096 || *fh > 4096 ||
        *pitch < *fw * 2 || *pitch > 32768 || (*pitch & 1) || v.width <= 0 || v.height <= 0 ||
        v.width > *fw || v.height > *fh || v.shown.width <= 0 || v.shown.height <= 0 ||
        v.shown.left < 0 || v.shown.top < 0 || v.shown.width > v.width || v.shown.height > v.height ||
        v.shown.left > v.width - v.shown.width || v.shown.top > v.height - v.shown.height ||
        v.span_x <= 0 || v.span_y <= 0 || v.span_x > 8192 || v.span_y > 8192) return false;
    // EN0043e6a5/c2 reads these SAME framebuffer/pitch globals before addressing
    // y*pitch+x*2. EN004a810e/128 hands them to the world zoom resampler. They are
    // the locked software world surface, not GFX_DRAW_SURFACE (the sidebar cache).
    c.pixels = reinterpret_cast<uint8_t *>(static_cast<uintptr_t>(*fb));
    c.pitch  = *pitch;
    c.clip   = {v.shown.left, v.shown.top,
                v.shown.left + v.shown.width,
                v.shown.top + v.shown.height};
    c.width  = bounded(settings.width, 0.5, 3.0, 1.0);
    c.alpha  = bounded(settings.opacity, 0.0, 1.0, 0.8);
    return c.alpha > 0.0;
}
} // namespace

line_options &options() { return settings; }

namespace {
inline constexpr unsigned row_blocks      = raster_limit / 32;
inline constexpr unsigned primitive_words = (route_primitive_capacity + 31) / 32;
// All scratch is fixed BSS and render-thread-only. A block membership bit does
// not allocate one list node per segment-row or translated copy.
uint32_t row_bins[4][row_blocks][primitive_words]{};
struct prepared_primitive {
    point         source{}, end{};
    stem_geometry geometry{};
    double        dash = 0, min_x = 0, max_x = 0, min_y = 0, max_y = 0;
};
prepared_primitive prepared[route_primitive_capacity];
static_assert(route_primitive_capacity <= 65536u);
uint16_t marker_indices[route_primitive_capacity]{};
uint32_t duplicate_markers[primitive_words]{};
} // namespace

rect paint_target_routes(const world_view &view, const route_geometry &batch, double presentation_ms) {
    canvas c;
    if (batch.count > route_primitive_capacity || !open_canvas(view, c)) return {};
    if (!std::isfinite(presentation_ms)) presentation_ms = 0;
    const double   speed       = bounded(settings.speed, 0, 128, 15.384615);
    const double   dash        = bounded(settings.dash_length, 1, 64, 7);
    const double   gap         = bounded(settings.gap_length, 1, 64, 7);
    const double   margin      = 21.0 + c.width;
    const rect     crop        = c.clip;
    const unsigned words       = (batch.count + 31) / 32;
    const unsigned first_block = static_cast<unsigned>(crop.top / 32);
    const unsigned last_block  = static_cast<unsigned>((crop.bottom - 1) / 32);
    std::fill_n(duplicate_markers, words, 0u);
    unsigned marker_count = 0;
    for (auto &role : row_bins)
        for (unsigned block = first_block; block <= last_block; ++block)
            std::fill_n(role[block], words, 0u);
    for (unsigned i = 0; i < batch.count; ++i) {
        const auto    &p    = batch.items[i];
        const unsigned role = static_cast<unsigned>(p.role);
        if (role >= 4 || (!p.stem && !p.marker)) continue;
        auto &q    = prepared[i];
        q.source   = p.marker ? sv::project_fine(view, p.destination.x, p.destination.y) : p.source;
        q.end      = p.marker ? q.source : point{p.source.x + gfx::wrapwin::stock_axis(p.origin.x, p.destination.x, view.span_x), p.source.y + gfx::wrapwin::stock_axis(p.origin.y, p.destination.y, view.span_y)};
        q.geometry = prepare_stem(q.source, q.end, presentation_ms, speed, dash, gap, p.arc, p.phase_period);
        q.dash     = q.geometry.period > 0 ? q.geometry.period * dash / (dash + gap) : dash;
        q.min_x    = std::min(q.source.x, q.end.x) - margin;
        q.max_x    = std::max(q.source.x, q.end.x) + margin;
        q.min_y    = std::min(q.source.y, q.end.y) - margin;
        q.max_y    = std::max(q.source.y, q.end.y) + margin;
        if (p.marker && !p.stem)
            marker_indices[marker_count++] = static_cast<uint16_t>(i);
    }
    std::sort(marker_indices, marker_indices + marker_count,
              [&batch](uint16_t left, uint16_t right) {
                  const auto &a = batch.items[left];
                  const auto &b = batch.items[right];
                  if (a.role != b.role) return a.role < b.role;
                  const auto &pa = prepared[left].source;
                  const auto &pb = prepared[right].source;
                  if (pa.x != pb.x) return pa.x < pb.x;
                  return pa.y < pb.y;
              });
    for (unsigned k = 1; k < marker_count; ++k) {
        const unsigned previous = marker_indices[k - 1];
        const unsigned current  = marker_indices[k];
        const auto    &a        = prepared[previous].source;
        const auto    &b        = prepared[current].source;
        if (batch.items[previous].role == batch.items[current].role &&
            a.x == b.x && a.y == b.y)
            duplicate_markers[current >> 5] |= uint32_t{1} << (current & 31u);
    }
    for (unsigned i = 0; i < batch.count; ++i) {
        const auto    &p    = batch.items[i];
        const unsigned role = static_cast<unsigned>(p.role);
        if (role >= 4 || (!p.stem && !p.marker) ||
            (duplicate_markers[i >> 5] & (uint32_t{1} << (i & 31u)))) continue;
        const auto &q  = prepared[i];
        const int   y0 = static_cast<int>(std::ceil((crop.top - q.max_y) / view.span_y));
        const int   y1 = static_cast<int>(std::floor((crop.bottom - q.min_y) / view.span_y));
        for (int y = y0; y <= y1; ++y) {
            const int lo = std::max(crop.top, static_cast<int>(std::floor(q.min_y + y * view.span_y)));
            const int hi = std::min(crop.bottom - 1, static_cast<int>(std::ceil(q.max_y + y * view.span_y)));
            if (hi < lo) continue;
            for (unsigned block = static_cast<unsigned>(lo / 32); block <= static_cast<unsigned>(hi / 32); ++block)
                row_bins[role][block][i >> 5] |= uint32_t{1} << (i & 31);
        }
    }
    // Stable mixed-role priority: movement, rally, patrol, attack. Same-role
    // artwork across the ENTIRE batch is unioned before any sample is blended.
    for (unsigned role = 0; role < 4; ++role) {
        const rgb565 color = role_color(static_cast<line_role>(role));
        for (unsigned block = first_block; block <= last_block; ++block) {
            const int top = std::max(crop.top, static_cast<int>(block * 32u));
            const int bottom =
                std::min(crop.bottom, static_cast<int>((block + 1u) * 32u));
            c.clip           = {crop.left, top, crop.right, bottom};
            const auto *bins = row_bins[role][block];
            for (unsigned word = 0; word < words; ++word) {
                uint32_t bits = bins[word];
                while (bits) {
                    unsigned long bit = 0;
                    _BitScanForward(&bit, bits);
                    bits &= bits - 1;
                    const unsigned i  = word * 32 + bit;
                    const auto    &p  = batch.items[i];
                    const auto    &q  = prepared[i];
                    const int      x0 = static_cast<int>(std::ceil((crop.left - q.max_x) / view.span_x));
                    const int      x1 = static_cast<int>(std::floor((crop.right - q.min_x) / view.span_x));
                    const int      y0 = static_cast<int>(std::ceil((top - q.max_y) / view.span_y));
                    const int      y1 = static_cast<int>(std::floor((bottom - q.min_y) / view.span_y));
                    for (int y = y0; y <= y1; ++y) {
                        for (int x = x0; x <= x1; ++x) {
                            const point a{q.source.x + x * view.span_x, q.source.y + y * view.span_y};
                            const point b{q.end.x + x * view.span_x, q.end.y + y * view.span_y};
                            if (p.stem) c.stem(c.logical(a), c.logical(b), color, q.geometry, q.dash);
                            if (p.marker && bottom >= b.y - margin && top <= b.y + margin)
                                c.endpoint(c.logical(b), color, p.role == line_role::rally);
                        }
                    }
                }
            }
            for (int row = top; row < bottom; ++row) {
                c.row = row;
                c.flush(color);
            }
        }
    }
    return c.damage;
}
} // namespace mh::ui::selected_targets::detail
