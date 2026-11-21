#include "include/mh_air_patrol.h"

namespace mh::order_tasks::air {
namespace {
struct direction {
    int8_t  dx, dy;
    uint8_t straight, left, right;
};
// Successors: EN 0x0066a224. Physical dx/dy: 0x00ae2d08, initialized
// by 0x0045fac0 and consumed by 0x0048103b. The greedy table's dcol/drow
// are NOT physical offsets for transient headings. Keep the native turn states.
constexpr direction directions[24] = {
    {0, -1, 0, 16, 8},
    {1, 0, 1, 17, 9},
    {0, 1, 2, 18, 10},
    {-1, 0, 3, 19, 11},
    {1, -1, 4, 20, 13},
    {1, 1, 5, 21, 14},
    {-1, 1, 6, 22, 15},
    {-1, -1, 7, 23, 12},
    {0, -1, 4, 20, 13},
    {1, 0, 5, 21, 14},
    {0, 1, 6, 22, 15},
    {-1, 0, 7, 23, 12},
    {-1, -1, 0, 16, 8},
    {1, -1, 1, 17, 9},
    {1, 1, 2, 18, 10},
    {-1, 1, 3, 19, 11},
    {0, -1, 7, 23, 12},
    {1, 0, 4, 20, 13},
    {0, 1, 5, 21, 14},
    {-1, 0, 6, 22, 15},
    {1, -1, 0, 16, 8},
    {1, 1, 1, 17, 9},
    {-1, 1, 2, 18, 10},
    {-1, -1, 3, 19, 11},
};
constexpr uint8_t  bearings[8] = {0, 2, 4, 6, 1, 3, 5, 7};
constexpr uint32_t MAX_STATES  = 8 * 256 * 256;
constexpr uint32_t NOT_OPEN    = UINT32_MAX;

bool valid_dimensions(dimensions d) noexcept {
    return d.width != 0 && d.width <= 256 && d.height != 0 && d.height <= 256;
}
bool valid_tile(dimensions d, tile p) noexcept {
    return p.x < d.width && p.y < d.height;
}
bool same(tile a, tile b) noexcept {
    return a.x == b.x && a.y == b.y;
}
bool same(pose a, pose b) noexcept {
    return a.heading == b.heading && same(a.position, b.position);
}
bool valid_pose(dimensions d, pose p) noexcept {
    return p.heading < 24 && valid_tile(d, p.position);
}
// Every caller translates by at most two tiles. Two corrections also cover
// dimension 1 without division in the search's hot expansion loop.
uint32_t wrapped(int32_t value, uint32_t size) noexcept {
    const int32_t span = static_cast<int32_t>(size);
    if (value < 0) value += span;
    if (value < 0) value += span;
    if (value >= span) value -= span;
    if (value >= span) value -= span;
    return static_cast<uint32_t>(value);
}
tile translated(dimensions d, tile p, int32_t dx, int32_t dy) noexcept {
    return {wrapped(static_cast<int32_t>(p.x) + dx, d.width),
            wrapped(static_cast<int32_t>(p.y) + dy, d.height)};
}
uint32_t axis_distance(uint32_t a, uint32_t b, uint32_t span) noexcept {
    const uint32_t delta = a > b ? a - b : b - a;
    return delta < span - delta ? delta : span - delta;
}
uint32_t distance(dimensions d, tile a, tile b) noexcept {
    const uint32_t dx = axis_distance(a.x, b.x, d.width);
    const uint32_t dy = axis_distance(a.y, b.y, d.height);
    return dx > dy ? dx : dy;
}
int32_t delta(uint32_t from, uint32_t to, uint32_t span) noexcept {
    int32_t       result = static_cast<int32_t>(to) - static_cast<int32_t>(from);
    const int32_t half   = static_cast<int32_t>(span / 2);
    if (result > half) result -= static_cast<int32_t>(span);
    else if (result < -half) result += static_cast<int32_t>(span);
    return result;
}
uint32_t tangent(dimensions d, tile from, tile to) noexcept {
    const int32_t dx = delta(from.x, to.x, d.width);
    const int32_t dy = delta(from.y, to.y, d.height);
    const int32_t ax = dx < 0 ? -dx : dx;
    const int32_t ay = dy < 0 ? -dy : dy;
    if (ax > 2 * ay) return dx > 0 ? 1 : 3;
    if (ay > 2 * ax) return dy > 0 ? 2 : 0;
    if (dx > 0) return dy > 0 ? 5 : 4;
    return dy > 0 ? 6 : 7;
}
uint32_t bearing_change(uint32_t from, uint32_t to) noexcept {
    const uint32_t a          = bearings[directions[from].straight];
    const uint32_t b          = bearings[to];
    const uint32_t difference = a > b ? a - b : b - a;
    return difference < 8 - difference ? difference : 8 - difference;
}
uint32_t encode(dimensions d, pose p) noexcept {
    return ((p.position.y * d.width + p.position.x) * 8) + p.heading;
}
pose decode(dimensions d, uint32_t state) noexcept {
    const uint32_t cell = state / 8;
    return {{cell % d.width, cell / d.width}, state % 8};
}

// A macro is either one steady step or a transient turn followed by its forced
// steady step. This is lossless: transient nodes have exactly one successor.
// It reduces the complete search universe from 24*256*256 to 8*256*256.
struct macro_step {
    int32_t  dx, dy;
    uint32_t count, middle, heading;
};
macro_step macro(uint32_t heading, uint32_t edge) noexcept {
    if (edge == 0) {
        const direction &s = directions[heading];
        return {s.dx, s.dy, 1, heading, heading};
    }
    const uint32_t middle = edge == 1 ? directions[heading].left : directions[heading].right;
    const uint32_t final  = directions[middle].straight;
    return {static_cast<int32_t>(directions[middle].dx) + directions[final].dx,
            static_cast<int32_t>(directions[middle].dy) + directions[final].dy,
            2, middle, final};
}
// Any two steady headings differ by at most four 45-degree turns (8 steps).
// After that turn, a full 16-step heading cycle has zero displacement and
// visits every steady heading. Inserting E/W and N/S straight runs in that
// cycle realizes any toroidal displacement in at most 128+128 steps.
// Thus every valid pair needs at most 8+16+256+2 = 282 native steps, including
// the optional transient prefix/suffix: LEG_STEPS is not a near-goal cutoff.
struct search_record {
    uint32_t heap_index;
    uint16_t cost, score, epoch;
    uint8_t  parent;
};
// 8 MiB at the supported maximum domain, shared rather than allocated per
// aircraft or plan. Stamps avoid clearing this universe on every short leg.
struct search_scratch {
    search_record records[MAX_STATES];
    uint32_t      heap[MAX_STATES];
    uint32_t      count;
    uint16_t      epoch;
};
search_scratch scratch;

void begin_search() noexcept {
    scratch.count = 0;
    ++scratch.epoch;
    if (scratch.epoch == 0) {
        for (uint32_t i = 0; i < MAX_STATES; ++i) scratch.records[i].epoch = 0;
        scratch.epoch = 1;
    }
}
bool precedes(uint32_t a, uint32_t b) noexcept {
    const search_record &left  = scratch.records[a];
    const search_record &right = scratch.records[b];
    if (left.score != right.score) return left.score < right.score;
    if (left.cost != right.cost) return left.cost > right.cost;
    return a < b;
}
void heap_up(uint32_t index) noexcept {
    const uint32_t state = scratch.heap[index];
    while (index != 0) {
        const uint32_t parent = (index - 1) / 2;
        if (!precedes(state, scratch.heap[parent])) break;
        scratch.heap[index]                             = scratch.heap[parent];
        scratch.records[scratch.heap[index]].heap_index = index;
        index                                           = parent;
    }
    scratch.heap[index]               = state;
    scratch.records[state].heap_index = index;
}
bool heap_insert(uint32_t state) noexcept {
    if (scratch.count == MAX_STATES) return false;
    const uint32_t index = scratch.count++;
    scratch.heap[index]  = state;
    heap_up(index);
    return true;
}
uint32_t heap_pop() noexcept {
    const uint32_t result              = scratch.heap[0];
    const uint32_t replacement         = scratch.heap[--scratch.count];
    scratch.records[result].heap_index = NOT_OPEN;
    if (scratch.count == 0) return result;
    uint32_t index = 0;
    while (index * 2 + 1 < scratch.count) {
        uint32_t child = index * 2 + 1;
        if (child + 1 < scratch.count && precedes(scratch.heap[child + 1], scratch.heap[child])) ++child;
        if (!precedes(scratch.heap[child], replacement)) break;
        scratch.heap[index]                             = scratch.heap[child];
        scratch.records[scratch.heap[index]].heap_index = index;
        index                                           = child;
    }
    scratch.heap[index]                     = replacement;
    scratch.records[replacement].heap_index = index;
    return result;
}

bool search(dimensions d, pose from, pose to, uint32_t budget, uint32_t &goal) noexcept {
    begin_search();
    const uint32_t initial_score = distance(d, from.position, to.position);
    if (initial_score > budget) return false;
    const uint32_t start = encode(d, from);
    goal                 = encode(d, to);
    search_record &root  = scratch.records[start];
    root.epoch           = scratch.epoch;
    root.cost            = 0;
    root.score           = static_cast<uint16_t>(initial_score);
    root.parent          = 0;
    if (!heap_insert(start)) return false;
    while (scratch.count != 0) {
        const uint32_t current = heap_pop();
        if (current == goal) return true;
        const pose     here = decode(d, current);
        const uint32_t cost = scratch.records[current].cost;
        for (uint32_t edge = 0; edge < 3; ++edge) {
            const macro_step step      = macro(here.heading, edge);
            const uint32_t   next_cost = cost + step.count;
            if (next_cost > budget) continue;
            const pose     next       = {translated(d, here.position, step.dx, step.dy), step.heading};
            const uint32_t next_score = next_cost + distance(d, next.position, to.position);
            if (next_score > budget) continue;
            const uint32_t state  = encode(d, next);
            search_record &record = scratch.records[state];
            const bool     seen   = record.epoch == scratch.epoch;
            if (seen && next_cost >= record.cost) continue;
            const uint32_t previous_index = seen ? record.heap_index : NOT_OPEN;
            record.epoch                  = scratch.epoch;
            record.cost                   = static_cast<uint16_t>(next_cost);
            record.score                  = static_cast<uint16_t>(next_score);
            record.parent                 = static_cast<uint8_t>(here.heading | (edge << 3));
            if (previous_index == NOT_OPEN) {
                if (!heap_insert(state)) return false;
            } else {
                heap_up(previous_index);
            }
        }
    }
    return false;
}
bool write_orbit(dimensions d, pose from, uint8_t *headings) noexcept {
    // The left 16-cycle contains steady 0..7 and transient 16..23; the right
    // 16-cycle contains steady 0..7 and transient 8..15. Each displacement
    // direction occurs twice, hence zero displacement even before torus wrap.
    const bool left    = from.heading < 8 || from.heading >= 16;
    pose       current = from;
    for (uint32_t i = 0; i < 16; ++i) {
        const uint32_t next   = current.heading >= 8 ? directions[current.heading].straight
                                : left               ? directions[current.heading].left
                                                     : directions[current.heading].right;
        headings[i]           = static_cast<uint8_t>(next);
        const direction &step = directions[next];
        current               = {translated(d, current.position, step.dx, step.dy), next};
    }
    return same(current, from);
}
} // namespace

bool legal(uint32_t previous, uint32_t next) noexcept {
    if (previous >= 24 || next >= 24) return false;
    const direction &p = directions[previous];
    return next == p.straight || (previous < 8 && (next == p.left || next == p.right));
}
pose stepped(dimensions d, pose from, uint32_t next) noexcept {
    if (!valid_dimensions(d) || !valid_tile(d, from.position) || next >= 24) return from;
    const direction &step = directions[next];
    return {translated(d, from.position, step.dx, step.dy), next};
}

bool orbit(dimensions d, pose from, circuit &out) noexcept {
    out.count = 0;
    if (!valid_dimensions(d) || !valid_pose(d, from)) return false;
    out.start = from;
    if (!write_orbit(d, from, out.headings)) return false;
    out.count = 16;
    return true;
}

bool plan(dimensions d, pose from, pose to, bool require_motion, path &out) noexcept {
    out.count = 0;
    if (!valid_dimensions(d) || !valid_pose(d, from) || !valid_pose(d, to)) return false;
    if (same(from, to)) {
        if (!require_motion) return true;
        // Avoid a large temporary circuit and the zero-cost-root ambiguity.
        if (!write_orbit(d, from, out.headings)) return false;
        out.count = 16;
        return true;
    }
    uint32_t prefix = 0;
    if (from.heading >= 8) {
        const uint32_t next    = directions[from.heading].straight;
        out.headings[prefix++] = static_cast<uint8_t>(next);
        from                   = stepped(d, from, next);
    }
    uint32_t suffix = 0, terminal = 0;
    if (to.heading >= 8) {
        terminal             = to.heading;
        uint32_t predecessor = 0;
        while (predecessor < 8 && directions[predecessor].left != terminal &&
               directions[predecessor].right != terminal) ++predecessor;
        if (predecessor == 8) return false;
        to.position = translated(d, to.position, -directions[terminal].dx, -directions[terminal].dy);
        to.heading  = predecessor;
        suffix      = 1;
    }
    uint32_t goal = 0;
    if (!search(d, from, to, LEG_STEPS - prefix - suffix, goal)) return false;
    const uint32_t total = prefix + scratch.records[goal].cost + suffix;
    if (total == 0 || total > LEG_STEPS) return false;
    if (suffix != 0) out.headings[total - 1] = static_cast<uint8_t>(terminal);
    uint32_t       cursor  = total - suffix;
    const uint32_t start   = encode(d, from);
    uint32_t       current = goal;
    while (current != start) {
        const uint32_t   parent  = scratch.records[current].parent;
        const uint32_t   heading = parent & 7;
        const macro_step step    = macro(heading, parent >> 3);
        if (cursor < prefix + step.count) return false;
        out.headings[--cursor] = static_cast<uint8_t>(step.heading);
        if (step.count == 2) out.headings[--cursor] = static_cast<uint8_t>(step.middle);
        const pose here     = decode(d, current);
        const pose previous = {translated(d, here.position, -step.dx, -step.dy), heading};
        current             = encode(d, previous);
    }
    if (cursor != prefix) return false;
    out.count = total;
    return true;
}

bool build_circuit(dimensions d, const tile *points, uint32_t count, circuit &out) noexcept {
    out.count = 0;
    if (!valid_dimensions(d) || points == nullptr || count == 0 || count > CAPACITY) return false;
    tile     vertices[CAPACITY];
    uint32_t used = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (!valid_tile(d, points[i])) return false;
        if (used == 0 || !same(vertices[used - 1], points[i])) vertices[used++] = points[i];
    }
    if (used > 1 && same(vertices[0], vertices[used - 1])) --used;
    if (used == 1) return orbit(d, {vertices[0], 0}, out);
    uint8_t tangents[CAPACITY];
    for (uint32_t i = 0; i < used; ++i)
        tangents[i] = static_cast<uint8_t>(tangent(d, vertices[i], vertices[(i + 1) % used]));
    out.start        = {vertices[0], tangents[0]};
    uint32_t written = 0;
    path     leg;
    for (uint32_t i = 0; i < used; ++i) {
        const uint32_t next = (i + 1) % used;
        if (!plan(d, {vertices[i], tangents[i]}, {vertices[next], tangents[next]}, true, leg)) return false;
        if (leg.count == 0 || leg.count > CIRCUIT_STEPS - written) return false;
        for (uint32_t j = 0; j < leg.count; ++j) out.headings[written++] = leg.headings[j];
    }
    out.count = written;
    return written != 0;
}

pose at(dimensions d, const circuit &curve, uint32_t next_index) noexcept {
    if (!valid_dimensions(d) || !valid_pose(d, curve.start) || curve.count == 0 ||
        curve.count > CIRCUIT_STEPS) return curve.start;
    pose           result = curve.start;
    const uint32_t end    = next_index % curve.count;
    for (uint32_t i = 0; i < end; ++i) {
        if (!legal(result.heading, curve.headings[i])) return curve.start;
        result = stepped(d, result, curve.headings[i]);
    }
    return result;
}
uint32_t join_index(dimensions d, pose from, const circuit &curve) noexcept {
    if (!valid_dimensions(d) || !valid_pose(d, from) || !valid_pose(d, curve.start) ||
        curve.count == 0 || curve.count > CIRCUIT_STEPS) return UINT32_MAX;
    pose     current = curve.start;
    uint32_t best = UINT32_MAX, best_distance = UINT32_MAX, best_bearing = UINT32_MAX;
    for (uint32_t i = 0; i < curve.count; ++i) {
        if (current.heading < 8) {
            const uint32_t candidate_distance = distance(d, from.position, current.position);
            const uint32_t candidate_bearing  = bearing_change(from.heading, current.heading);
            if (candidate_distance < best_distance ||
                (candidate_distance == best_distance && candidate_bearing < best_bearing)) {
                best          = i;
                best_distance = candidate_distance;
                best_bearing  = candidate_bearing;
            }
        }
        if (!legal(current.heading, curve.headings[i])) return UINT32_MAX;
        current = stepped(d, current, curve.headings[i]);
    }
    return same(current, curve.start) ? best : UINT32_MAX;
}
} // namespace mh::order_tasks::air
