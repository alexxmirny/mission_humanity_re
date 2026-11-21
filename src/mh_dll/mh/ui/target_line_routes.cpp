// Destination: src/mh_dll/mh/ui/target_line_routes.cpp
#include "ui/target_line_routes.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <algorithm>
#include <cmath>
#include <cstdint>
#include "gfx/wrap_window.h"
#include "ui/target_line_painter.h"

namespace mh::ui::selected_targets::detail {
namespace {
namespace sv = gfx::stratview;
using sv::point;
using sv::world_view;
int wrap(int x, int span) {
    const int r = x % span;
    return r < 0 ? r + span : r;
}
int    delta(int a, int b, int span) { return gfx::wrapwin::stock_axis(a, b, span); }
double distance(point a, point b, const world_view &v) {
    return std::hypot(double(delta(a.x, b.x, v.span_x)), double(delta(a.y, b.y, v.span_y)));
}
double distance_squared(point a, point b, const world_view &v) {
    const double x = delta(a.x, b.x, v.span_x), y = delta(a.y, b.y, v.span_y);
    return x * x + y * y;
}
double source_join_squared = 0, goal_join_squared = 0;
bool   same_command(const route_command_key &a, const route_command_key &b) {
    if (a.canonical != b.canonical) return false;
    if (!a.canonical) return a.legacy && a.legacy == b.legacy;
    return a.sequence && a.sequence == b.sequence && a.world == b.world &&
           a.system == b.system && a.planet == b.planet;
}
bool known_command(const route_command_key &key) {
    return key.canonical ? key.sequence != 0 : key.legacy != 0;
}
bool same_node(const route_node &a, const route_node &b) {
    if (!a.known || !b.known || a.role != b.role || a.entity != b.entity) return false;
    if (!a.entity) return true;
    if (a.life_domain == b.life_domain) return a.target_life && a.target_life == b.target_life;
    return a.grouping_life && a.grouping_life == b.grouping_life;
}
bool   exact_point(point a, point b) { return a.x == b.x && a.y == b.y; }
double radius(float value, double fallback, double maximum) {
    return (std::isfinite(value) ? std::clamp(double(value), 0.0, maximum) : fallback) * 32.0;
}
double fit_period(double remaining, double traversed_phase, double nominal) {
    if (remaining <= 0.000001) return nominal;
    // Keep every already-shared edge immutable. Fit only the unshared suffix
    // and closing edge to an integral THREE-period winding (arrows + dashes).
    const double winding = std::max(std::floor(traversed_phase / 3.0) + 1.0,
                                    std::floor((remaining / nominal + traversed_phase) / 3.0 + 0.5));
    return remaining / (3.0 * winding - traversed_phase);
}
bool offscreen(const world_view &v, point visual) {
    const point p = sv::project_fine(v, visual.x, visual.y);
    return gfx::wrapwin::span_axis(v.shown.left, p.x, v.span_x) >= v.shown.width ||
           gfx::wrapwin::span_axis(v.shown.top, p.y, v.span_y) >= v.shown.height;
}
// One representative per root. Membership is final before source centroids are
// calculated, and includes every collected off-screen selected actor.
uint16_t roots[route_capacity], order[route_capacity], members[route_capacity];
point    centres[route_capacity], visual_centres[route_capacity], sources[route_capacity];
bool     stems[route_capacity];
int      centroid_axis(const route_collection &routes, unsigned count, int span, bool x_axis) {
    auto coordinate = [&](uint16_t i) {
        const point p = routes.paths[i].ground_origin;
        return wrap(x_axis ? p.x : p.y, span);
    };
    std::sort(members, members + count, [&](uint16_t a, uint16_t b) {
        const int ca = coordinate(a), cb = coordinate(b);
        return ca != cb ? ca < cb : a < b;
    });
    int largest = -1, cut = 0;
    for (unsigned n = 0; n < count; ++n) {
        const int here = coordinate(members[n]);
        const int next = coordinate(members[(n + 1) % count]);
        const int gap  = next + (n + 1 == count ? span : 0) - here;
        if (gap > largest || (gap == largest && next < cut)) {
            largest = gap;
            cut     = next;
        }
    }
    int64_t sum = 0;
    for (unsigned n = 0; n < count; ++n) {
        const int c = coordinate(members[n]);
        sum += c + (c < cut ? span : 0);
    }
    return static_cast<int>((sum / count) % span);
}
struct prefix_node {
    unsigned          parent = 0, next = 0, first_child = 0;
    const route_node *node   = nullptr;
    uint8_t           player = 0, mover = 0;
    bool              cyclic = false;
    unsigned          edge = 0, marker = 0;
    double            end_phase = 0;
};
// Index0 is the absent link; root IDs occupy their own disjoint range. A hidden
// node gets a private prefix ID, so later known points cannot reconverge across it.
inline constexpr unsigned prefix_capacity = route_selection_capacity * route_node_capacity + route_capacity * 2 + 1;
prefix_node               prefixes[prefix_capacity];
unsigned                  root_children[route_capacity];
unsigned                  prefix_count = 1;
unsigned                  path_prefix[route_capacity][route_node_capacity];
uint16_t                  phase_order[route_capacity];

bool root_compatible(const world_view &v, const route_path &a, const route_path &b) {
    if (!a.count || !b.count || a.player != b.player ||
        (a.cycle_begin == 0) != (b.cycle_begin == 0) ||
        !same_node(a.nodes[0], b.nodes[0])) return false;
    if (a.nodes[0].role != line_role::attack && a.mover != b.mover) return false;
    const bool proven = same_command(a.nodes[0].command, b.nodes[0].command);
    if (options().grouping == grouping_mode::shared_command &&
        (known_command(a.nodes[0].command) || known_command(b.nodes[0].command))) {
        // Canonical requests must agree exactly. Legacy native presentation
        // contains planner/formation-adjusted goals; a proven common native
        // command retains its representative goal, as the previous renderer did.
        return proven && (!a.nodes[0].command.canonical ||
                          exact_point(a.nodes[0].ground, b.nodes[0].ground));
    }
    if (options().grouping == grouping_mode::shared_command &&
        (a.nodes[0].command.canonical || b.nodes[0].command.canonical)) return false;
    // Unknown provenance retains spatial subgroup behavior. Distinct proven
    // commands may spatially group only in the explicit Subgroups mode.
    return distance_squared(a.ground_origin, b.ground_origin, v) <= source_join_squared &&
           (a.nodes[0].entity || distance_squared(a.nodes[0].ground, b.nodes[0].ground, v) <= goal_join_squared);
}
unsigned child(const world_view &v, unsigned &head, unsigned parent, const route_path &path,
               const route_node &node, bool cyclic, route_geometry &out) {
    for (unsigned at = head; at; at = prefixes[at].next) {
        const auto &p = prefixes[at];
        if (p.parent != parent || p.player != path.player || p.cyclic != cyclic ||
            !same_node(*p.node, node) ||
            (node.role != line_role::attack && p.mover != path.mover)) continue;
        const bool proven = same_command(p.node->command, node.command);
        if (options().grouping == grouping_mode::shared_command &&
            (known_command(p.node->command) || known_command(node.command))) {
            if (proven && (!node.command.canonical || exact_point(p.node->ground, node.ground))) return at;
        } else if (!(options().grouping == grouping_mode::shared_command &&
                     (p.node->command.canonical || node.command.canonical)) &&
                   (node.entity || distance_squared(p.node->ground, node.ground, v) <= goal_join_squared)) {
            // SAME parent is required even in spatial mode. Coordinates at a later
            // route stage can never reconnect divergent prefixes.
            return at;
        }
    }
    if (prefix_count == prefix_capacity || out.count + 2 > route_primitive_capacity) return 0;
    const unsigned at   = prefix_count++;
    prefixes[at]        = {};
    auto &p             = prefixes[at];
    p.parent            = parent;
    p.next              = head;
    head                = at;
    p.node              = &node;
    p.player            = path.player;
    p.mover             = path.mover;
    p.cyclic            = cyclic;
    p.edge              = out.count++;
    p.marker            = out.count++;
    out.items[p.edge]   = {};
    out.items[p.marker] = {};
    return at;
}
} // namespace

bool build_route_geometry(const world_view &v, const route_collection &routes, bool reveal, route_geometry &out) {
    out.count = 0;
    if (v.span_x <= 0 || v.span_y <= 0 || v.shown.width <= 0 || v.shown.height <= 0 || routes.count > route_capacity) return false;
    unsigned canonical_count = 0, legacy_nodes = 0, legacy_cycles = 0;
    for (unsigned n = 0; n < routes.count; ++n) {
        const auto &p = routes.paths[n];
        if (p.count > route_node_capacity || (!p.canonical && p.count > 2) ||
            (p.cycle_begin != no_cycle && p.cycle_begin >= p.count)) return false;
        canonical_count += p.canonical;
        if (!p.canonical) {
            legacy_nodes += p.count;
            legacy_cycles += p.cycle_begin != no_cycle;
        }
        roots[n]         = static_cast<uint16_t>(n);
        order[n]         = static_cast<uint16_t>(n);
        root_children[n] = 0;
    }
    if (canonical_count > route_selection_capacity || legacy_nodes > route_capacity ||
        legacy_cycles > route_selection_capacity) return false;
    const double source_join = radius(options().source_radius_tiles, 2, 8);
    const double goal_join   = radius(options().goal_radius_tiles, 1, 4);
    const double minimum     = radius(options().minimum_tiles, 20, 64);
    source_join_squared      = source_join * source_join;
    goal_join_squared        = goal_join * goal_join;
    // Stable preference independent of selection ordering; native visible body
    // anchors win, then proximity to the shown crop centre, then actor identity.
    std::sort(order, order + routes.count, [&](uint16_t a, uint16_t b) {
        const auto &x = routes.paths[a], &y = routes.paths[b];
        if (x.source_visible != y.source_visible) return x.source_visible;
        const auto score = [&](const route_path &p) {
            const double dx = p.source.x - (v.shown.left + v.shown.width / 2);
            const double dy = p.source.y - (v.shown.top + v.shown.height / 2);
            return dx * dx + dy * dy;
        };
        const double sx = score(x), sy = score(y);
        if (sx != sy) return sx < sy;
        if (x.player != y.player) return x.player < y.player;
        if (x.actor != y.actor) return x.actor < y.actor;
        return a < b;
    });
    for (unsigned ordinal = 0; ordinal < routes.count; ++ordinal) {
        const unsigned n = order[ordinal];
        for (unsigned earlier = 0; earlier < ordinal; ++earlier) {
            const unsigned r = order[earlier];
            if (roots[r] != r || !root_compatible(v, routes.paths[n], routes.paths[r])) continue;
            roots[n] = static_cast<uint16_t>(r);
            break;
        }
    }
    for (unsigned r = 0; r < routes.count; ++r) {
        if (roots[r] != r) continue;
        unsigned count = 0;
        int64_t  ox = 0, oy = 0;
        for (unsigned n = 0; n < routes.count; ++n) {
            if (roots[n] != r) continue;
            // Primary and independently firing secondary channels may share one
            // source actor. Centroid weights ACTORS, never presentation channels.
            const auto &candidate       = routes.paths[n];
            bool        duplicate_actor = false;
            for (unsigned member = 0; member < count; ++member) {
                const auto &other = routes.paths[members[member]];
                duplicate_actor   = duplicate_actor || (candidate.player == other.player &&
                                                      candidate.family == other.family && candidate.actor == other.actor &&
                                                      candidate.actor_life == other.actor_life);
            }
            if (duplicate_actor) continue;
            members[count++] = static_cast<uint16_t>(n);
            ox += delta(routes.paths[n].ground_origin.x, routes.paths[n].origin.x, v.span_x);
            oy += delta(routes.paths[n].ground_origin.y, routes.paths[n].origin.y, v.span_y);
        }
        const auto &path = routes.paths[r];
        if (count == 1) {
            centres[r]        = path.ground_origin;
            visual_centres[r] = path.origin;
            sources[r]        = path.source;
        } else {
            centres[r]        = {centroid_axis(routes, count, v.span_x, true), centroid_axis(routes, count, v.span_y, false)};
            visual_centres[r] = {wrap(centres[r].x + static_cast<int>(ox / count), v.span_x),
                                 wrap(centres[r].y + static_cast<int>(oy / count), v.span_y)};
            sources[r]        = sv::project_fine(v, visual_centres[r].x, visual_centres[r].y);
        }
    }
    prefix_count = 1;
    // Resolve membership first, then measure exactly the representative geometry
    // that will be shown. Subgroup goal-radius approximation must not measure one
    // member's private goal while painting another member's representative goal.
    for (unsigned ordinal = 0; ordinal < routes.count; ++ordinal) {
        const unsigned n = order[ordinal], root = roots[n];
        const auto    &path   = routes.paths[n];
        unsigned       parent = route_capacity + root + 1, *head = &root_children[root];
        for (unsigned j = 0; j < path.count; ++j) {
            const auto &node = path.nodes[j];
            unsigned    at   = 0;
            if (!node.known) {
                if (prefix_count == prefix_capacity) {
                    out.count = 0;
                    return false;
                }
                at           = prefix_count++;
                prefixes[at] = {};
            } else {
                const bool cyclic = path.cycle_begin != no_cycle && j >= path.cycle_begin;
                at                = child(v, *head, parent, path, node, cyclic, out);
                if (!at) {
                    out.count = 0;
                    return false;
                }
            }
            path_prefix[n][j] = at;
            parent            = at;
            head              = &prefixes[at].first_child;
        }
    }
    const auto displayed = [&](unsigned path, unsigned node) -> const route_node & {
        const auto *representative = prefixes[path_prefix[path][node]].node;
        return representative ? *representative : routes.paths[path].nodes[node];
    };
    for (unsigned n = 0; n < routes.count; ++n) phase_order[n] = static_cast<uint16_t>(n);
    std::sort(phase_order, phase_order + routes.count, [&](uint16_t a, uint16_t b) {
        const auto &x = routes.paths[a], &y = routes.paths[b];
        if (x.player != y.player) return x.player < y.player;
        if (x.actor != y.actor) return x.actor < y.actor;
        return a < b;
    });
    const auto art_dimension = [](float value) {
        return std::isfinite(value) ? std::clamp(double(value), 1.0, 64.0) : 7.0;
    };
    const double nominal_period = art_dimension(options().dash_length) + art_dimension(options().gap_length);
    for (unsigned ordinal = 0; ordinal < routes.count; ++ordinal) {
        const unsigned n = phase_order[ordinal], root = roots[n];
        const auto    &path        = routes.paths[n];
        point          last_ground = centres[root];
        bool           known = true, outside = false;
        double         length = 0, pixel_length = 0, cycle_length = 0;
        point          last_visual = visual_centres[root];
        double         arcs[route_node_capacity]{};
        for (unsigned j = 0; j < path.count; ++j) {
            const auto &node = displayed(n, j);
            if (node.known) {
                if (known) {
                    length += distance(last_ground, node.ground, v);
                    pixel_length += distance(last_visual, node.visual, v);
                }
                outside     = outside || offscreen(v, node.visual);
                last_ground = node.ground;
                last_visual = node.visual;
            }
            arcs[j] = pixel_length;
            known   = node.known;
        }
        // Full circuit consists of every suffix edge plus its closing edge. The
        // traversal above already counted all but the closing edge, plus approach.
        if (path.cycle_begin != no_cycle) {
            const unsigned first = path.cycle_begin, last = path.count - 1;
            if (displayed(n, first).known && displayed(n, last).known) {
                length += distance(displayed(n, last).ground, displayed(n, first).ground, v);
            }
            for (unsigned j = first; j <= last; ++j) {
                const unsigned k = j == last ? first : j + 1;
                if (displayed(n, j).known && displayed(n, k).known)
                    cycle_length += distance(displayed(n, j).visual, displayed(n, k).visual, v);
            }
        }
        stems[n] = reveal || outside || length >= minimum;
        // One time phase rate for the whole graph. Already-shared edge phases
        // are immutable; only a new circuit's unshared suffix is period-fitted.
        const double ideal_cycle_period = fit_period(cycle_length, 0, nominal_period);
        double       phase              = path.cycle_begin == no_cycle ? 0 : path.cycle_phase / ideal_cycle_period - arcs[path.cycle_begin] / nominal_period;
        point        source = sources[root], origin = visual_centres[root];
        bool         previous_known = true;
        unsigned     cycle_first    = 0;
        double       suffix_period  = 0;
        for (unsigned j = 0; j < path.count; ++j) {
            const auto &node = displayed(n, j);
            if (!node.known) {
                previous_known = false;
                continue;
            }
            const unsigned at     = path_prefix[n][j];
            auto          &p      = prefixes[at];
            auto          &edge   = out.items[p.edge];
            auto          &marker = out.items[p.marker];
            // First member fixes a representative edge; later members only union
            // its visibility. Descendants MUST begin at that representative node.
            if (!marker.marker) {
                double period = nominal_period;
                if (cycle_first && previous_known) {
                    if (!suffix_period) {
                        double remaining = 0;
                        for (unsigned k = j; k < path.count; ++k) {
                            if (displayed(n, k - 1).known && displayed(n, k).known)
                                remaining += distance(displayed(n, k - 1).visual, displayed(n, k).visual, v);
                        }
                        const unsigned last = path.count - 1;
                        if (displayed(n, last).known)
                            remaining += distance(displayed(n, last).visual, prefixes[cycle_first].node->visual, v);
                        suffix_period = fit_period(remaining, phase - prefixes[cycle_first].end_phase, nominal_period);
                    }
                    period = suffix_period;
                }
                edge              = {source, origin, node.visual, phase * period,
                                     cycle_length, node.role, previous_known && stems[n], false};
                edge.phase_period = period;
                marker            = {{}, {}, node.visual, 0, 0, node.role, false, true};
                p.end_phase       = phase + (previous_known ? distance(origin, node.visual, v) / period : 0);
            } else edge.stem = edge.stem || (previous_known && stems[n]);
            if (j == path.cycle_begin) cycle_first = at;
            origin         = p.node->visual;
            source         = sv::project_fine(v, origin.x, origin.y);
            phase          = p.end_phase;
            previous_known = true;
        }
        if (cycle_first && path.count && previous_known && displayed(n, path.cycle_begin).known) {
            if (out.count == route_primitive_capacity) {
                out.count = 0;
                return false;
            }
            const auto  &first          = prefixes[cycle_first];
            const double closing_length = distance(origin, first.node->visual, v);
            const double period         = suffix_period ? suffix_period : fit_period(closing_length, phase - first.end_phase, nominal_period);
            // Shared time phase + an integral winding closes both artwork trains.
            auto &closing        = out.items[out.count++];
            closing              = {source, origin, first.node->visual, phase * period, cycle_length,
                                    line_role::patrol, stems[n], false};
            closing.phase_period = period;
        }
    }
    return true;
}
} // namespace mh::ui::selected_targets::detail
