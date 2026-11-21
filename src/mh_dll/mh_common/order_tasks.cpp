// Destination: src/mh_dll/mh_common/order_tasks.cpp
#include "include/mh_order_tasks.h"
#include "include/mh_air_patrol.h"
#include <cstring>
#include <limits>

namespace mh::order_tasks {
namespace {
constexpr uint32_t REGION_MAGIC   = 0x31524f51u; // QOR1
constexpr uint32_t SAVE_MAGIC     = 0x44524f51u; // QORD
constexpr uint32_t CAMPAIGN_MAGIC = 0x4d414351u; // QCAM
constexpr uint32_t HAS_IMPLICIT = 1, IMPLICIT_ACTIVE = 2, EXCURSION = 4;
constexpr uint32_t RETURN_MODE = 8, APPLIED = 16, EXTERNAL_PENDING = 32;
constexpr uint32_t REPLACE_PENDING = 64, PENDING_CLEANUP = 128, PLANNER_SUCCEEDED = 256, ALL_FLAGS = 511;
enum offset : uint32_t {
    LIFE             = 0,
    ROUTE            = 4,
    NEXT_LEG         = 8,
    ATTEMPT          = 12,
    PHASE            = 16,
    FAILURE          = 20,
    COUNT            = 24,
    CYCLE            = 28,
    CURSOR           = 32,
    ASSIGNED_X       = 36,
    ASSIGNED_Y       = 40,
    PLACEMENT        = 44,
    ASSIGNMENT_VALID = 48,
    PATH_SLOT        = 52,
    IMPLICIT_X       = 56,
    IMPLICIT_Y       = 60,
    FLAGS            = 64,
    EXCURSION_X      = 68,
    EXCURSION_Y      = 72,
    CIRCUIT_CURSOR   = 76,
    CIRCUIT_X        = 80,
    CIRCUIT_Y        = 84,
    PROGRESS         = 88,
    HEAD             = 92,
    FAMILY           = 96,
    TRACKING         = 100,
    EPOCH            = 104,
    EX_TARGET_KIND   = 108,
    EX_TARGET_OWNER  = 112,
    EX_TARGET_SLOT   = 116,
    EX_TARGET_LIFE   = 120,
    EX_OBJECTIVE     = 124,
    RESERVATION      = 128,
    ORIGIN_X         = 176,
    ORIGIN_Y         = 180,
    ORIGIN_STATE     = 184,
    AIR_MODE         = 188,
    JOIN_X           = 192,
    JOIN_Y           = 196,
    JOIN_HEADING     = 200,
    JOIN_PHASE       = 204,
    JOIN_PROGRESS    = 208,
    CURVE_INDEX      = 212,
    CHUNK_ACK        = 216,
    CHUNK_REVISION   = 220,
    SPEC_REVISION    = 224,
    AIR_SLOT         = 228,
    RETURN_HEADING   = 232,
    AIR_SPEC_COUNT   = 236
};
uint32_t get(const uint8_t *p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
void put(uint8_t *p, uint32_t v) {
    for (uint32_t i = 0; i != 4; ++i) p[i] = uint8_t(v >> (8 * i));
}
template <class E>
uint32_t value(E e) { return static_cast<uint32_t>(e); }
uint32_t field(const uint8_t *r, offset o) { return get(r + o); }
void     field(uint8_t *r, offset o, uint32_t v) { put(r + o, v); }
void     status(uint8_t *r, phase p) { field(r, PHASE, value(p)); }
phase    status(const uint8_t *r) { return static_cast<phase>(field(r, PHASE)); }
bool     same(tile a, tile b) { return a.x == b.x && a.y == b.y; }
bool     same(actor_key a, actor_key b) {
    return a.player == b.player && a.slot == b.slot && a.incarnation == b.incarnation;
}
bool same(world_key a, world_key b) {
    return a.system == b.system && a.planet == b.planet && a.incarnation == b.incarnation;
}
bool same(target_key a, target_key b) { return a.kind == b.kind && same(a.object, b.object); }
bool same(dimensions a, dimensions b) {
    return a.players == b.players && a.units == b.units && a.width == b.width &&
           a.height == b.height && a.buildings == b.buildings;
}
bool same(const entry &a, const entry &b) {
    return a.kind == b.kind && same(a.requested, b.requested) && same(a.target, b.target) &&
           a.group == b.group && a.command_sequence == b.command_sequence;
}
bool zero(const uint8_t *p, uint32_t n) {
    for (uint32_t i = 0; i != n; ++i)
        if (p[i]) return false;
    return true;
}
uint32_t crc32(const uint8_t *p, uint32_t n) {
    uint32_t crc = ~0u;
    for (uint32_t i = 0; i != n; ++i) {
        crc ^= p[i];
        for (uint32_t bit = 0; bit != 8; ++bit) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}
dimensions shape_of(const uint8_t *p) { return {get(p + 8), get(p + 12), get(p + 16), get(p + 20), get(p + 72)}; }
world_key  world_of(const uint8_t *p) { return {get(p + 24), get(p + 28), get(p + 32)}; }
bool       world_valid(dimensions d, world_key w) {
    return d.width ? w.incarnation != 0 && w.planet < 32 : !w.system && !w.planet && !w.incarnation;
}
bool tile_valid(tile t, dimensions d) { return t.x < d.width && t.y < d.height; }
bool family_valid(executor e) { return value(e) <= value(executor::direction_plane); }
bool is_attack(action a) { return a == action::attack_unit || a == action::attack_building; }
bool target_valid(target_key t, dimensions d) {
    if (t.kind == object_kind::none) return !t.object.player && !t.object.slot && !t.object.incarnation;
    if (t.kind != object_kind::unit && t.kind != object_kind::building) return false;
    return t.object.incarnation && t.object.player < d.players &&
           t.object.slot < (t.kind == object_kind::unit ? d.units : d.buildings);
}
bool entry_valid(const entry &e, dimensions d, bool require_sequence) {
    if (value(e.kind) < 1 || value(e.kind) > value(action::patrol) ||
        !tile_valid(e.requested, d) || (require_sequence && !e.command_sequence) ||
        !target_valid(e.target, d)) return false;
    if (e.kind == action::attack_unit) return e.target.kind == object_kind::unit;
    if (e.kind == action::attack_building) return e.target.kind == object_kind::building;
    return e.target.kind == object_kind::none;
}
bool cancel_valid(const entry &e) {
    return !value(e.kind) && !e.requested.x && !e.requested.y &&
           e.target.kind == object_kind::none && !e.target.object.player &&
           !e.target.object.slot && !e.target.object.incarnation && !e.group && e.command_sequence;
}
uint8_t *row(uint8_t *p, dimensions d, uint32_t player, uint32_t slot) {
    return p + HEADER_BYTES + (player * d.units + slot) * ROW_BYTES;
}
const uint8_t *row(const uint8_t *p, dimensions d, uint32_t player, uint32_t slot) {
    return p + HEADER_BYTES + (player * d.units + slot) * ROW_BYTES;
}
uint8_t *node(uint8_t *r, uint32_t logical) {
    return r + ACTOR_BYTES + ((field(r, HEAD) + logical) % CAPACITY) * NODE_BYTES;
}
const uint8_t *node(const uint8_t *r, uint32_t logical) {
    return r + ACTOR_BYTES + ((field(r, HEAD) + logical) % CAPACITY) * NODE_BYTES;
}
entry task_of(const uint8_t *n) {
    return {static_cast<action>(get(n)), {get(n + 4), get(n + 8)}, {static_cast<object_kind>(get(n + 12)), {get(n + 16), get(n + 20), get(n + 24)}}, get(n + 28), get(n + 32)};
}
void write_node(uint8_t *n, const entry &e, uint32_t leg, objective obj) {
    put(n, value(e.kind));
    put(n + 4, e.requested.x);
    put(n + 8, e.requested.y);
    put(n + 12, value(e.target.kind));
    put(n + 16, e.target.object.player);
    put(n + 20, e.target.object.slot);
    put(n + 24, e.target.object.incarnation);
    put(n + 28, e.group);
    put(n + 32, e.command_sequence);
    put(n + 36, leg);
    put(n + 40, value(obj));
    put(n + 44, 0);
}
primary_reservation reservation_of(const uint8_t *r) {
    const uint8_t *p = r + RESERVATION;
    return {static_cast<reservation_phase>(get(p)),
            {static_cast<object_kind>(get(p + 4)), {get(p + 8), get(p + 12), get(p + 16)}},
            get(p + 20),
            {static_cast<object_kind>(get(p + 24)), {get(p + 28), get(p + 32), get(p + 36)}},
            get(p + 40),
            get(p + 44)};
}
bool reservation_valid(const primary_reservation &v, dimensions d, uint32_t epoch, bool checkpoint) {
    if (value(v.phase) > value(reservation_phase::handoff) || v.delta_raw16 > 65535 ||
        (v.prior_packed & ~0x8001ffffu) || !target_valid(v.target, d) || !target_valid(v.prior_target, d)) return false;
    if (v.phase == reservation_phase::none)
        return v.target.kind == object_kind::none && !v.delta_raw16 &&
               v.prior_target.kind == object_kind::none && !v.prior_packed && !v.execution_epoch;
    if (v.target.kind == object_kind::none) return false;
    if (v.phase == reservation_phase::bound)
        return v.prior_target.kind == object_kind::none && (v.prior_packed & 65535u) == v.delta_raw16 &&
               v.execution_epoch <= epoch;
    return (!checkpoint || v.phase != reservation_phase::handoff) &&
           v.execution_epoch && v.execution_epoch == epoch;
}
void write_reservation_raw(uint8_t *r, const primary_reservation &v) {
    uint8_t *p = r + RESERVATION;
    put(p, value(v.phase));
    put(p + 4, value(v.target.kind));
    put(p + 8, v.target.object.player);
    put(p + 12, v.target.object.slot);
    put(p + 16, v.target.object.incarnation);
    put(p + 20, v.delta_raw16);
    put(p + 24, value(v.prior_target.kind));
    put(p + 28, v.prior_target.object.player);
    put(p + 32, v.prior_target.object.slot);
    put(p + 36, v.prior_target.object.incarnation);
    put(p + 40, v.prior_packed);
    put(p + 44, v.execution_epoch);
}
target_key physical_binding(const uint8_t *r) {
    const primary_reservation v = reservation_of(r);
    if (v.phase == reservation_phase::bound) return v.target;
    if (v.phase == reservation_phase::pending || v.phase == reservation_phase::handoff) return v.prior_target;
    return {};
}
bool cancel_pending(const backend &b, actor_key a, uint8_t *r) {
    const auto p = static_cast<reservation_phase>(get(r + RESERVATION));
    if (p == reservation_phase::handoff) return false; // No reentrant ownership edits mid-F.
    if (p != reservation_phase::pending) return true;
    return b.cancel_pending_reservation && b.cancel_pending_reservation(b.context, a) &&
           get(r + RESERVATION) != value(reservation_phase::pending) &&
           get(r + RESERVATION) != value(reservation_phase::handoff);
}
void clear_assignment(uint8_t *r) { std::memset(r + ASSIGNED_X, 0, 20); }
void assign(uint8_t *r, tile t, assignment_reason reason, uint32_t path) {
    field(r, ASSIGNED_X, t.x);
    field(r, ASSIGNED_Y, t.y);
    field(r, PLACEMENT, value(reason));
    field(r, ASSIGNMENT_VALID, 1);
    field(r, PATH_SLOT, path);
    field(r, FLAGS, field(r, FLAGS) | PLANNER_SUCCEEDED);
}
void clear_nodes(uint8_t *r) {
    for (uint32_t i = 0; i != field(r, COUNT); ++i) std::memset(node(r, i), 0, NODE_BYTES);
}
void clear_owned(uint8_t *r) {
    // Preserve spent epoch, non-owning primary reservation and unsafe managed cleanup.
    const uint32_t cleanup = field(r, FLAGS) & PENDING_CLEANUP;
    clear_nodes(r);
    std::memset(r + ATTEMPT, 0, EPOCH - ATTEMPT);
    field(r, FLAGS, cleanup);
    std::memset(r + EX_TARGET_KIND, 0, 20);
    std::memset(r + ORIGIN_X, 0, ACTOR_BYTES - ORIGIN_X);
}
void clear_row(uint8_t *r) {
    clear_nodes(r);
    std::memset(r, 0, ACTOR_BYTES);
}
tile intended_destination(const uint8_t *r) {
    const uint32_t flags = field(r, FLAGS);
    if (flags & RETURN_MODE) return {field(r, EXCURSION_X), field(r, EXCURSION_Y)};
    if (flags & IMPLICIT_ACTIVE) return {field(r, IMPLICIT_X), field(r, IMPLICIT_Y)};
    return task_of(node(r, field(r, CURSOR))).requested;
}
target_key excursion_target_of(const uint8_t *r) {
    return {static_cast<object_kind>(field(r, EX_TARGET_KIND)),
            {field(r, EX_TARGET_OWNER), field(r, EX_TARGET_SLOT), field(r, EX_TARGET_LIFE)}};
}
void suspend_flight(uint8_t *);
void block(uint8_t *r, failure why) {
    suspend_flight(r);
    status(r, phase::blocked);
    field(r, FAILURE, value(why));
    clear_assignment(r);
    field(r, FLAGS, field(r, FLAGS) & ~PLANNER_SUCCEEDED);
}
bool identity_available(const uint8_t *r) {
    return field(r, ATTEMPT) != UINT32_MAX && field(r, EPOCH) != UINT32_MAX;
}
void advance_identity(uint8_t *r) {
    field(r, ATTEMPT, field(r, ATTEMPT) + 1);
    field(r, EPOCH, field(r, EPOCH) + 1);
    clear_assignment(r);
    field(r, FLAGS, field(r, FLAGS) & ~PLANNER_SUCCEEDED);
}
bool bump_attempt(uint8_t *r, const backend &b, actor_key a) {
    if (!identity_available(r)) {
        block(r, failure::exhausted_identity);
        return false;
    }
    if (!cancel_pending(b, a, r)) {
        block(r, failure::application);
        return false;
    }
    advance_identity(r);
    return true;
}
bool begin_anchor_return(uint8_t *r, const backend &b, actor_key a) {
    if (!bump_attempt(r, b, a)) return false;
    field(r, FLAGS, (field(r, FLAGS) & ~APPLIED) | RETURN_MODE);
    status(r, phase::returning_anchor);
    return true;
}
void reset_circuit(uint8_t *r, tile position) {
    field(r, TRACKING, 1);
    field(r, CIRCUIT_CURSOR, field(r, CURSOR));
    field(r, CIRCUIT_X, position.x);
    field(r, CIRCUIT_Y, position.y);
    field(r, PROGRESS, 0);
}
ticket ticket_of(const uint8_t *p, const uint8_t *r, actor_key a) {
    return {world_of(p), a, field(r, ROUTE), get(node(r, field(r, CURSOR)) + 36), field(r, ATTEMPT)};
}
bool matches(const uint8_t *p, const uint8_t *r, const ticket &t) {
    return field(r, COUNT) && field(r, LIFE) == t.actor.incarnation &&
           same(world_of(p), t.world) && field(r, ROUTE) == t.route &&
           get(node(r, field(r, CURSOR)) + 36) == t.leg && field(r, ATTEMPT) == t.attempt;
}
bool read_actor(const backend &b, actor_key a, dimensions d, actor_view &out) {
    return a.player < d.players && a.slot < d.units && a.incarnation && b.read_actor &&
           b.read_actor(b.context, a, out) && same(out.key, a) && family_valid(out.family) &&
           tile_valid(out.position, d);
}
bool target_objective(const backend &b, actor_key a, const entry &e, objective &out) {
    out = objective::unresolved;
    if (!is_attack(e.kind)) return true;
    if (!b.read_target) return false;
    switch (b.read_target(b.context, a, e.target)) {
        case target_status::alive_hostile: return true;
        case target_status::destroyed_confirmed: out = objective::destroyed_confirmed; return true;
        case target_status::unavailable: out = objective::unavailable; return true;
    }
    return false;
}
bool capture_valid(const active_capture &c, dimensions d) {
    if (c.count > 2) return false;
    if (c.requires_replan && (c.count != 1 || c.status != phase::executing ||
                              c.provenance != native_provenance::active_move || c.nodes[0].kind != action::move ||
                              c.assignment_valid)) return false;
    if (c.logical_move_valid &&
        (c.count != 1 || c.provenance != native_provenance::active_move ||
         c.nodes[0].kind != action::move || !tile_valid(c.logical_move, d))) return false;
    if (!c.logical_move_valid && (c.logical_move.x || c.logical_move.y)) return false;
    if (!c.count) return c.status == phase::empty && c.provenance == native_provenance::none && !c.assignment_valid;
    if (!tile_valid(c.position, d) || (c.status != phase::awaiting_application &&
                                       c.status != phase::preparation && c.status != phase::executing && c.status != phase::blocked)) return false;
    for (uint32_t i = 0; i != c.count; ++i)
        if (!entry_valid(c.nodes[i], d, false)) return false;
    if (c.count == 2) {
        if (c.status == phase::awaiting_application || c.provenance != native_provenance::genuine_patrol ||
            c.nodes[0].kind != action::patrol || c.nodes[1].kind != action::patrol) return false;
    } else if (c.nodes[0].kind == action::patrol) {
        // A pending NEW patrol has no truthful home until native application. Its known
        // explicit intent is one point; legacy already-executing patrol requires two.
        if (c.status != phase::awaiting_application || c.provenance != native_provenance::genuine_patrol) return false;
    } else if (c.nodes[0].kind == action::move) {
        if (c.provenance != native_provenance::active_move) return false;
    } else if (is_attack(c.nodes[0].kind)) {
        if (c.provenance != native_provenance::active_attack) return false;
    } else return false;
    if (c.assignment_valid) {
        if (c.status == phase::awaiting_application || c.status == phase::blocked ||
            !tile_valid(c.assigned, d) || value(c.placement) < 1 || value(c.placement) > 3 ||
            (c.placement == assignment_reason::requested && !is_attack(c.nodes[0].kind) &&
             !same(c.assigned, c.nodes[0].requested))) return false;
    } else if (c.assigned.x || c.assigned.y || value(c.placement) || c.path_slot) return false;
    return true;
}
void capture_status(uint8_t *r, const active_capture &c) {
    status(r, c.status);
    field(r, FLAGS, c.status == phase::awaiting_application ? EXTERNAL_PENDING : APPLIED);
    if (c.status == phase::blocked) field(r, FAILURE, value(failure::planning));
    if (c.assignment_valid) assign(r, c.assigned, c.placement, c.path_slot);
    if (field(r, CYCLE) == 0) reset_circuit(r, c.position);
}
bool flight_valid(const uint8_t *, dimensions);
bool origins_valid(const uint8_t *, dimensions);
bool header_valid(const uint8_t *p, uint32_t n) {
    if (!p || n < HEADER_BYTES || get(p) != REGION_MAGIC || get(p + 4) != SCHEMA || get(p + 36) != n) return false;
    const dimensions d = shape_of(p);
    if (state::required_bytes(d) != n || !world_valid(d, world_of(p)) || !zero(p + 108, 20)) return false;
    for (uint32_t i = d.players; i != 8; ++i)
        if (get(p + 40 + 4 * i) || get(p + 76 + 4 * i)) return false;
    if (!d.width && (!zero(p + 40, 32) || !zero(p + 76, 32))) return false;
    for (uint32_t i = 0; i != d.players; ++i)
        if (get(p + 76 + 4 * i) > get(p + 40 + 4 * i)) return false;
    return true;
}
bool valid_row(const uint8_t *r, dimensions d, uint32_t highwater, bool legacy = false) {
    if (!field(r, LIFE)) return zero(r, ROW_BYTES);
    if (!d.width || !reservation_valid(reservation_of(r), d, field(r, EPOCH), true)) return false;
    const uint32_t count = field(r, COUNT);
    const uint32_t flags = field(r, FLAGS), cycle = field(r, CYCLE), cursor = field(r, CURSOR);
    if (!count) {
        if (field(r, ROUTE) ? (!field(r, NEXT_LEG) || field(r, EPOCH) < field(r, ROUTE))
                            : (field(r, NEXT_LEG) != 0 || (legacy && field(r, EPOCH) != 0))) return false;
        if ((flags & ~PENDING_CLEANUP) || ((flags & PENDING_CLEANUP) && !field(r, ROUTE))) return false;
        return zero(r + ATTEMPT, FLAGS - ATTEMPT) && zero(r + FLAGS + 4, EPOCH - FLAGS - 4) &&
               zero(r + EX_TARGET_KIND, 20) &&
               (zero(r + ORIGIN_X, AIR_MODE - ORIGIN_X) ||
                (!legacy && field(r, ORIGIN_STATE) == 5 &&
                 tile_valid({field(r, ORIGIN_X), field(r, ORIGIN_Y)}, d))) &&
               zero(r + AIR_MODE, ROW_BYTES - AIR_MODE);
    }
    if (!field(r, ROUTE) || !field(r, NEXT_LEG) || field(r, EPOCH) < field(r, ROUTE)) return false;
    if (count > CAPACITY || !field(r, ATTEMPT) || field(r, HEAD) >= CAPACITY || cursor >= count ||
        uint64_t(field(r, EPOCH)) < uint64_t(field(r, ROUTE)) + field(r, ATTEMPT) - 1 ||
        value(status(r)) < 1 || value(status(r)) > value(phase::ready_next) ||
        (flags & ~ALL_FLAGS) || field(r, FAMILY) > value(executor::direction_plane) ||
        field(r, TRACKING) > 1 || field(r, PROGRESS) > 1 ||
        (legacy && !zero(r + 176, ACTOR_BYTES - 176))) return false;
    if ((!(field(r, AIR_MODE) && !legacy) && (status(r) == phase::blocked) != (field(r, FAILURE) != 0)) ||
        field(r, FAILURE) > value(failure::exhausted_identity)) return false;
    if (cycle != NO_CYCLE && cycle >= count) return false;
    if ((cycle == NO_CYCLE || cycle > 0) && cursor != 0) return false;
    if (cycle == NO_CYCLE && (flags & (HAS_IMPLICIT | IMPLICIT_ACTIVE))) return false;
    if ((flags & IMPLICIT_ACTIVE) && !(flags & HAS_IMPLICIT)) return false;
    if (flags & HAS_IMPLICIT) {
        if (cycle != 0 || (count != 1 && !(flags & IMPLICIT_ACTIVE)) ||
            !tile_valid({field(r, IMPLICIT_X), field(r, IMPLICIT_Y)}, d)) return false;
    } else if (field(r, IMPLICIT_X) || field(r, IMPLICIT_Y)) return false;
    if (flags & EXCURSION) {
        const target_key target = excursion_target_of(r);
        if (!tile_valid({field(r, EXCURSION_X), field(r, EXCURSION_Y)}, d) ||
            target.kind == object_kind::none || !target_valid(target, d) ||
            field(r, EX_OBJECTIVE) > value(objective::unavailable) ||
            static_cast<action>(get(node(r, cursor))) != action::patrol ||
            (status(r) != phase::excursion && status(r) != phase::returning_anchor &&
             status(r) != phase::preparation && status(r) != phase::blocked)) return false;
    } else if (field(r, EXCURSION_X) || field(r, EXCURSION_Y) || (flags & RETURN_MODE) ||
               !zero(r + EX_TARGET_KIND, 20)) return false;
    if (status(r) == phase::excursion && !(flags & EXCURSION)) return false;
    if (status(r) == phase::returning_anchor && !(flags & RETURN_MODE)) return false;
    if ((flags & RETURN_MODE) && (!(flags & EXCURSION) ||
                                  (status(r) != phase::returning_anchor && status(r) != phase::preparation && status(r) != phase::blocked))) return false;
    if (status(r) == phase::awaiting_application && (flags & APPLIED)) return false;
    if ((status(r) == phase::executing || status(r) == phase::preparation || status(r) == phase::excursion) &&
        !(flags & APPLIED)) return false;
    if (flags & EXTERNAL_PENDING) {
        if (status(r) != phase::awaiting_application || count < 2 || cursor ||
            (flags & APPLIED) || field(r, ASSIGNMENT_VALID)) return false;
    }
    if ((flags & REPLACE_PENDING) && ((flags & (APPLIED | EXTERNAL_PENDING)) ||
                                      (status(r) != phase::awaiting_application && status(r) != phase::blocked))) return false;
    if ((flags & PLANNER_SUCCEEDED) && (!(flags & APPLIED) ||
                                        (status(r) != phase::executing && status(r) != phase::preparation &&
                                         status(r) != phase::returning_anchor && status(r) != phase::ready_next))) return false;
    if (field(r, ASSIGNMENT_VALID) > 1 || field(r, PLACEMENT) > 3) return false;
    if (field(r, ASSIGNMENT_VALID)) {
        if (!(flags & APPLIED) || !(flags & PLANNER_SUCCEEDED) || !field(r, PLACEMENT) ||
            !tile_valid({field(r, ASSIGNED_X), field(r, ASSIGNED_Y)}, d) ||
            (status(r) != phase::executing && status(r) != phase::preparation &&
             status(r) != phase::returning_anchor)) return false;
        if (field(r, PLACEMENT) == value(assignment_reason::requested) &&
            !is_attack(static_cast<action>(get(node(r, cursor)))) &&
            !same({field(r, ASSIGNED_X), field(r, ASSIGNED_Y)}, intended_destination(r))) return false;
    } else if (field(r, ASSIGNED_X) || field(r, ASSIGNED_Y) || field(r, PLACEMENT) || field(r, PATH_SLOT)) return false;
    if (field(r, TRACKING)) {
        if (cycle != 0 || field(r, CIRCUIT_CURSOR) >= count ||
            !tile_valid({field(r, CIRCUIT_X), field(r, CIRCUIT_Y)}, d)) return false;
    } else if (field(r, CIRCUIT_CURSOR) || field(r, CIRCUIT_X) || field(r, CIRCUIT_Y) || field(r, PROGRESS)) return false;
    if ((flags & HAS_IMPLICIT) && !field(r, TRACKING)) return false;
    if (cycle == 0 && count == 1 && ((flags & HAS_IMPLICIT) != 0) != (field(r, TRACKING) != 0)) return false;
    if (cycle == 0 && ((flags & APPLIED) || status(r) == phase::ready_next)) {
        if (!field(r, TRACKING) || (count == 1 && !(flags & HAS_IMPLICIT))) return false;
    }
    bool cycle_distinct = false;
    for (uint32_t i = 0; i != count; ++i) {
        const uint8_t *n = node(r, i);
        const entry    e = task_of(n);
        if (!entry_valid(e, d, false) || !get(n + 36) || get(n + 36) > field(r, NEXT_LEG) ||
            get(n + 32) > highwater || get(n + 40) > value(objective::unavailable) || get(n + 44)) return false;
        if (!is_attack(e.kind) && get(n + 40)) return false;
        if ((e.kind == action::patrol) != (cycle != NO_CYCLE && i >= cycle)) return false;
        if (i > cycle && cycle != NO_CYCLE && !same(e.requested, task_of(node(r, cycle)).requested)) cycle_distinct = true;
        for (uint32_t j = 0; j != i; ++j)
            if (get(node(r, j) + 36) == get(n + 36)) return false;
    }
    if ((legacy || field(r, FAMILY) != value(executor::direction_plane)) &&
        cycle != NO_CYCLE && count - cycle >= 2 && !cycle_distinct) return false;
    if ((flags & RETURN_MODE) && task_of(node(r, cursor)).kind != action::patrol) return false;
    if (!legacy && !flight_valid(r, d)) return false;
    for (uint32_t i = count; i != CAPACITY; ++i)
        if (!zero(node(r, i), NODE_BYTES)) return false;
    return true;
}
bool region_valid(const uint8_t *p, uint32_t n) {
    if (!header_valid(p, n)) return false;
    const dimensions d = shape_of(p);
    for (uint32_t player = 0; player != d.players; ++player)
        for (uint32_t slot = 0; slot != d.units; ++slot)
            if (!valid_row(row(p, d, player, slot), d, get(p + 40 + 4 * player))) return false;
    return origins_valid(p, d);
}
bool has_patrol(const uint8_t *);
void publish_origin(uint8_t *, tile, uint32_t);

constexpr uint32_t LEGACY_HEADER_BYTES = 96, LEGACY_ACTOR_BYTES = 192;
constexpr uint32_t LEGACY_ROW_BYTES = LEGACY_ACTOR_BYTES + CAPACITY * NODE_BYTES;
bool               legacy_region_valid(const uint8_t *p, uint32_t n) {
    if (!p || n < LEGACY_HEADER_BYTES || get(p) != REGION_MAGIC || get(p + 4) != 1 ||
        get(p + 36) != n || !zero(p + 76, 20)) return false;
    const dimensions d = shape_of(p);
    if (!state::required_bytes(d) || !world_valid(d, world_of(p)) ||
        uint64_t(n) != LEGACY_HEADER_BYTES + uint64_t(d.players) * d.units * LEGACY_ROW_BYTES) return false;
    for (uint32_t i = d.players; i != 8; ++i)
        if (get(p + 40 + 4 * i)) return false;
    if (!d.width && !zero(p + 40, 32)) return false;
    uint8_t expanded[ROW_BYTES]{};
    for (uint32_t player = 0; player != d.players; ++player)
        for (uint32_t slot = 0; slot != d.units; ++slot) {
            const uint8_t *old = p + LEGACY_HEADER_BYTES + (player * d.units + slot) * LEGACY_ROW_BYTES;
            if (!zero(old + 176, 16)) return false;
            std::memset(expanded, 0, sizeof(expanded));
            std::memcpy(expanded, old, 176);
            std::memcpy(expanded + ACTOR_BYTES, old + LEGACY_ACTOR_BYTES, CAPACITY * NODE_BYTES);
            // Exactly the schema1 semantic checks, including the old finite-cycle rules.
            if (!valid_row(expanded, d, get(p + 40 + 4 * player), true)) return false;
        }
    return true;
}
bool disjoint(const uint8_t *a, uint32_t an, const uint8_t *b, uint32_t bn) {
    const uintptr_t av = reinterpret_cast<uintptr_t>(a), bv = reinterpret_cast<uintptr_t>(b);
    return av <= bv ? bv - av >= an : av - bv >= bn;
}
// Caller validated the complete old envelope/rows before entering this infallible copy.
// No native callback, reservation rewrite, epoch bump, sequence invention or arrival.
void migrate_legacy(uint8_t *out, const uint8_t *old, dimensions d, uint32_t bytes) {
    std::memset(out, 0, bytes);
    std::memcpy(out, old, LEGACY_HEADER_BYTES);
    put(out + 4, SCHEMA);
    put(out + 36, bytes);
    std::memset(out + 76, 0, HEADER_BYTES - 76);
    for (uint32_t player = 0; player != d.players; ++player)
        for (uint32_t slot = 0; slot != d.units; ++slot) {
            const uint8_t *src = old + LEGACY_HEADER_BYTES + (player * d.units + slot) * LEGACY_ROW_BYTES;
            uint8_t       *dst = row(out, d, player, slot);
            std::memcpy(dst, src, 176);
            std::memcpy(dst + ACTOR_BYTES, src + LEGACY_ACTOR_BYTES, CAPACITY * NODE_BYTES);
            if (!has_patrol(dst)) continue;
            // Old history is local. Its real saved implicit/circuit anchor survives,
            // including nonzero sequence keys that once belonged to a shared selection.
            if (field(dst, FLAGS) & HAS_IMPLICIT)
                publish_origin(dst, {field(dst, IMPLICIT_X), field(dst, IMPLICIT_Y)}, 3);
            else if (field(dst, TRACKING))
                publish_origin(dst, {field(dst, CIRCUIT_X), field(dst, CIRCUIT_Y)}, 3);
            else field(dst, ORIGIN_STATE, 4);
            field(dst, SPEC_REVISION, 1);
            // Schema1 did not record an anchor heading. Zero is a future chosen return
            // tangent, not a claim about that historical heading; bytes0..175 stay exact.
        }
}
void record_sequence(uint8_t *p, uint32_t player, uint32_t sequence) {
    if (sequence > get(p + 40 + 4 * player)) put(p + 40 + 4 * player, sequence);
}
void begin_route(uint8_t *r, actor_key a, uint32_t generation, uint32_t epoch, executor family) {
    if (field(r, LIFE) == a.incarnation) clear_owned(r);
    else clear_row(r);
    field(r, LIFE, a.incarnation);
    field(r, ROUTE, generation);
    field(r, NEXT_LEG, 0);
    field(r, ATTEMPT, 1);
    field(r, CYCLE, NO_CYCLE);
    field(r, FAMILY, value(family));
    field(r, EPOCH, epoch);
    status(r, phase::awaiting_application);
}
void append_node(uint8_t *r, const entry &e, objective obj) {
    const uint32_t count = field(r, COUNT), leg = field(r, NEXT_LEG) + 1;
    write_node(node(r, count), e, leg, obj);
    field(r, NEXT_LEG, leg);
    field(r, COUNT, count + 1);
    if (e.kind == action::patrol && field(r, CYCLE) == NO_CYCLE) field(r, CYCLE, count);
}
// Returns false only at a completed zero-progress circuit. Never drains multiple nodes.
bool advance_ready(uint8_t *r, tile position, const backend &b, actor_key a) {
    if (status(r) != phase::ready_next) return true;
    const uint32_t cycle = field(r, CYCLE), count = field(r, COUNT), cursor = field(r, CURSOR);
    if (cycle == NO_CYCLE && count == 1) {
        clear_owned(r);
        return true;
    }
    // Check both counters BEFORE retiring a finite node or changing a cyclic cursor.
    if (!identity_available(r)) {
        block(r, failure::exhausted_identity);
        return false;
    }
    if (!cancel_pending(b, a, r)) {
        block(r, failure::application);
        return false;
    }
    if (cycle == NO_CYCLE || cycle > 0) {
        std::memset(node(r, 0), 0, NODE_BYTES);
        field(r, HEAD, (field(r, HEAD) + 1) % CAPACITY);
        field(r, COUNT, count - 1);
        if (cycle != NO_CYCLE) field(r, CYCLE, cycle - 1);
        field(r, CURSOR, 0);
    } else {
        uint32_t   flags = field(r, FLAGS), next = cursor;
        const bool implicit = (flags & IMPLICIT_ACTIVE) != 0;
        if (implicit) {
            flags &= ~IMPLICIT_ACTIVE;
            if (count > 1) {
                flags &= ~HAS_IMPLICIT;
                field(r, IMPLICIT_X, 0);
                field(r, IMPLICIT_Y, 0);
                next = (cursor + 1) % count;
            }
        } else if (count == 1) flags |= IMPLICIT_ACTIVE;
        else next = (cursor + 1) % count;
        // A one-point circuit closes after returning to the implicit anchor; a multi-point
        // circuit closes when the next explicit cursor equals its saved start cursor.
        const bool completed = count == 1 ? implicit : !implicit && next == field(r, CIRCUIT_CURSOR);
        if (completed && field(r, TRACKING) && !field(r, PROGRESS)) {
            block(r, failure::zero_progress_cycle);
            return false;
        }
        field(r, FLAGS, flags);
        field(r, CURSOR, next);
        if (completed || (implicit && count > 1)) reset_circuit(r, position);
    }
    advance_identity(r);
    field(r, FLAGS, field(r, FLAGS) & ~APPLIED);
    status(r, phase::awaiting_application);
    return true;
}
control complete(uint8_t *r) {
    clear_assignment(r);
    if (field(r, CYCLE) == NO_CYCLE && field(r, COUNT) == 1) {
        clear_owned(r);
        return control::unchanged;
    }
    status(r, phase::ready_next);
    return control::hold;
}
bool detach_for_cleanup(uint8_t *r, const backend &b, actor_key a, target_key fallback) {
    if (field(r, FLAGS) & PENDING_CLEANUP) return true;
    if (!cancel_pending(b, a, r)) return false;
    target_key target = physical_binding(r);
    if (target.kind == object_kind::none) target = fallback;
    if (target.kind != object_kind::none) {
        if (!b.detach_primary) return false;
        b.detach_primary(b.context, a, target);
        const primary_reservation v = reservation_of(r);
        if (v.phase == reservation_phase::bound && same(v.target, target)) std::memset(r + RESERVATION, 0, 48);
    }
    field(r, FLAGS, field(r, FLAGS) | PENDING_CLEANUP);
    return true;
}
control settle_detached(const uint8_t *p, uint8_t *r, const backend &b, actor_key a, const actor_view &actor) {
    if (!actor.safe_to_apply) return control::unchanged;
    if (!b.cleanup_attack || !b.cleanup_attack(b.context, ticket_of(p, r, a))) return control::hold;
    const primary_reservation pending = reservation_of(r);
    if (pending.phase == reservation_phase::pending && (field(r, FLAGS) & EXTERNAL_PENDING) &&
        field(r, COUNT) && pending.execution_epoch == field(r, EPOCH) &&
        pending.prior_target.kind == object_kind::none && !pending.prior_packed &&
        same(task_of(node(r, field(r, CURSOR))).target, pending.target)) {
        // Cleanup stopped only the retired prior activity. The queued QOEX head
        // still owns its original, already-counted new booking and exact epoch.
        field(r, FLAGS, field(r, FLAGS) & ~PENDING_CLEANUP);
        clear_assignment(r);
        return control::hold;
    }
    if (get(r + RESERVATION) != value(reservation_phase::none)) std::memset(r + RESERVATION, 0, 48);
    const phase old = status(r);
    field(r, FLAGS, field(r, FLAGS) & ~(PENDING_CLEANUP | APPLIED | REPLACE_PENDING | PLANNER_SUCCEEDED));
    clear_assignment(r);
    if (!field(r, COUNT)) return control::unchanged;
    if (old == phase::blocked) return control::hold;
    if ((field(r, FLAGS) & EXCURSION) && old == phase::excursion) {
        begin_anchor_return(r, b, a);
        return control::hold;
    }
    const entry task = task_of(node(r, field(r, CURSOR)));
    if (is_attack(task.kind)) {
        const objective obj = static_cast<objective>(get(node(r, field(r, CURSOR)) + 40));
        if (obj == objective::destroyed_confirmed) return complete(r);
        if (obj == objective::unavailable) {
            block(r, failure::invalid_target);
            return control::hold;
        }
    }
    // A stopped owned executor is resumed, never spatially completed by cleanup.
    if (old == phase::executing || old == phase::preparation || old == phase::returning_anchor) {
        suspend_flight(r);
        if (!bump_attempt(r, b, a)) return control::hold;
        status(r, (field(r, FLAGS) & RETURN_MODE) ? phase::returning_anchor : phase::awaiting_application);
    }
    return control::hold;
}
control service_primary(const uint8_t *p, uint8_t *r, const backend &b, actor_key a,
                        const actor_view &actor, bool &may_access) {
    may_access     = true;
    uint32_t flags = field(r, FLAGS);
    if (flags & PENDING_CLEANUP) {
        may_access = false;
        return settle_detached(p, r, b, a, actor);
    }
    if (!field(r, COUNT) || (flags & EXTERNAL_PENDING)) return control::unchanged;
    const phase current = status(r);
    const entry task    = task_of(node(r, field(r, CURSOR)));
    if (current == phase::ready_next) {
        may_access = false;
        return control::hold;
    }
    if (current == phase::blocked) {
        may_access = false;
        if (flags & (APPLIED | REPLACE_PENDING)) {
            const target_key fallback = (flags & EXCURSION) ? excursion_target_of(r) : is_attack(task.kind) ? task.target
                                                                                                            : target_key{};
            if (!detach_for_cleanup(r, b, a, fallback)) return actor.safe_to_apply ? control::hold : control::unchanged;
            return settle_detached(p, r, b, a, actor);
        }
        return actor.safe_to_apply ? control::hold : control::unchanged;
    }
    if ((flags & EXCURSION) && current != phase::excursion) {
        may_access = false;
        if (actor.family == executor::direction_plane && actor.path_slot != UINT32_MAX)
            return control::unchanged;
        return !(flags & APPLIED) && actor.safe_to_apply ? control::hold : control::unchanged;
    }
    objective  obj = objective::unresolved;
    target_key target{};
    if (current == phase::excursion) {
        target = excursion_target_of(r);
        obj    = static_cast<objective>(field(r, EX_OBJECTIVE));
        if (obj == objective::unresolved) {
            const target_status live = b.read_target ? b.read_target(b.context, a, target) : target_status::unavailable;
            if (live != target_status::alive_hostile) {
                obj = live == target_status::destroyed_confirmed ? objective::destroyed_confirmed : objective::unavailable;
                field(r, EX_OBJECTIVE, value(obj));
            }
        }
    } else if (is_attack(task.kind)) {
        target     = task.target;
        uint8_t *n = node(r, field(r, CURSOR));
        obj        = static_cast<objective>(get(n + 40));
        if (obj == objective::unresolved) {
            if (!target_objective(b, a, task, obj)) obj = objective::unavailable;
            put(n + 40, value(obj));
        }
    } else {
        if (current == phase::awaiting_application && !(flags & APPLIED)) {
            may_access = false;
            if (actor.family == executor::direction_plane && task.kind == action::patrol &&
                actor.path_slot != UINT32_MAX) return control::unchanged;
            return actor.safe_to_apply ? control::hold : control::unchanged;
        }
        return control::unchanged;
    }
    if (obj == objective::unresolved) {
        if (!(flags & APPLIED)) {
            may_access = false;
            return actor.safe_to_apply ? control::hold : control::unchanged;
        }
        return control::unchanged;
    }
    may_access = false;
    if (!(flags & (APPLIED | REPLACE_PENDING | EXCURSION)) && physical_binding(r).kind == object_kind::none) {
        if (obj == objective::destroyed_confirmed) return complete(r);
        block(r, failure::invalid_target);
        return control::hold;
    }
    if (!detach_for_cleanup(r, b, a, (flags & APPLIED) ? target : target_key{}))
        return actor.safe_to_apply ? control::hold : control::unchanged;
    if (obj == objective::unavailable && current != phase::excursion) block(r, failure::invalid_target);
    return settle_detached(p, r, b, a, actor);
}

// Origins are cohort metadata, never an implicit-leg flag or a native group id.
uint32_t root_sequence(const uint8_t *r) {
    return field(r, COUNT) && field(r, CYCLE) != NO_CYCLE
               ? get(node(r, field(r, CYCLE)) + 32)
               : 0;
}
bool has_patrol(const uint8_t *r) {
    return field(r, COUNT) && field(r, CYCLE) != NO_CYCLE;
}
bool resolved_origin(const uint8_t *r) {
    const uint32_t s = field(r, ORIGIN_STATE);
    return s == 1 || s == 2 || s == 3 || s == 6;
}
bool legacy_origin(const uint8_t *r) {
    return field(r, ORIGIN_STATE) == 3 || field(r, ORIGIN_STATE) == 4;
}
bool same_root(const uint8_t *a, const uint8_t *b) {
    return has_patrol(a) && has_patrol(b) && root_sequence(a) &&
           root_sequence(a) == root_sequence(b) && !legacy_origin(a) && !legacy_origin(b);
}
bool centroid_member(const uint8_t *r) {
    return field(r, ORIGIN_STATE) == 0 || field(r, ORIGIN_STATE) == 1;
}
void publish_origin(uint8_t *r, tile t, uint32_t state) {
    field(r, ORIGIN_X, t.x);
    field(r, ORIGIN_Y, t.y);
    field(r, ORIGIN_STATE, state);
}
bool origins_valid(const uint8_t *p, dimensions d) {
    for (uint32_t player = 0; player != d.players; ++player)
        for (uint32_t i = 0; i != d.units; ++i) {
            const uint8_t *r = row(p, d, player, i);
            if (!has_patrol(r) || legacy_origin(r)) continue;
            if (field(r, ORIGIN_STATE) == 1 &&
                root_sequence(r) > get(p + 76 + 4 * player)) return false;
            if (!centroid_member(r)) continue;
            for (uint32_t j = 0; j != i; ++j) {
                const uint8_t *other = row(p, d, player, j);
                if (!same_root(r, other) || !centroid_member(other)) continue;
                if (field(r, ORIGIN_STATE) != field(other, ORIGIN_STATE) ||
                    field(r, ORIGIN_X) != field(other, ORIGIN_X) ||
                    field(r, ORIGIN_Y) != field(other, ORIGIN_Y)) return false;
            }
        }
    return true;
}
// Only idle-centroid freezes advance the canonical frontier. A late idle row may
// inherit a surviving idle origin, never replace one whose last donor retired.
bool new_root_allowed(const uint8_t *p, dimensions d, const backend &b, uint32_t player, uint32_t sequence,
                      const uint8_t *replaced, bool fixed_origin, const uint8_t *&donor) {
    donor = nullptr;
    if (!sequence) return true;
    bool present = false;
    for (uint32_t slot = 0; slot != d.units; ++slot) {
        const uint8_t *r = row(p, d, player, slot);
        if (r == replaced || !has_patrol(r) || legacy_origin(r) || root_sequence(r) != sequence) continue;
        if (!fixed_origin && !centroid_member(r)) continue;
        actor_view live{};
        if (!read_actor(b, {player, slot, field(r, LIFE)}, d, live)) continue;
        present = true;
        if (field(r, ORIGIN_STATE) == 1) donor = r;
    }
    return present || sequence > get(p + 76 + 4 * player);
}
// All lifecycle/ready normalization precedes this pass; no native apply has run.
void freeze_origins(uint8_t *p, dimensions d, const backend &b) {
    for (uint32_t player = 0; player != d.players; ++player)
        for (uint32_t slot = 0; slot != d.units; ++slot) {
            uint8_t *r = row(p, d, player, slot);
            if (!has_patrol(r) || resolved_origin(r) || field(r, CYCLE) != 0 ||
                (status(r) != phase::awaiting_application && status(r) != phase::executing) ||
                (field(r, FLAGS) & (EXTERNAL_PENDING | EXCURSION | PENDING_CLEANUP))) continue;
            actor_view      actor{};
            const actor_key a{player, slot, field(r, LIFE)};
            if (!read_actor(b, a, d, actor) || !actor.safe_to_apply) continue;
            if (legacy_origin(r) || !root_sequence(r)) {
                publish_origin(r, actor.position, legacy_origin(r) ? 3u : 2u);
                continue;
            }
            // A late idle lineage inherits the earlier frozen idle origin. Fixed
            // move-prefix members neither donate nor change that centroid.
            const uint8_t *donor = nullptr;
            for (uint32_t j = 0; j != d.units; ++j) {
                const uint8_t *member = row(p, d, player, j);
                if (!same_root(r, member) || field(member, ORIGIN_STATE) != 1) continue;
                actor_view live{};
                if (read_actor(b, {player, j, field(member, LIFE)}, d, live)) {
                    donor = member;
                    break;
                }
            }
            if (donor) {
                publish_origin(r, {field(donor, ORIGIN_X), field(donor, ORIGIN_Y)}, 1);
                continue;
            }
            int64_t  sx = 0, sy = 0;
            uint32_t count = 0;
            tile     reference{};
            for (uint32_t j = 0; j != d.units; ++j) {
                uint8_t *member = row(p, d, player, j);
                if (!same_root(r, member) || !centroid_member(member)) continue;
                actor_view v{};
                if (!read_actor(b, {player, j, field(member, LIFE)}, d, v)) continue;
                // Prefix, unsafe, blocked and differently appended surviving members all count.
                // Existing group-centroid convention: unwrap about the first live
                // slot, two-sided half-extent fold, signed truncation, then true modulo.
                if (!count) reference = v.position;
                int32_t dx = int32_t(v.position.x) - int32_t(reference.x);
                int32_t dy = int32_t(v.position.y) - int32_t(reference.y);
                if (dx > int32_t(d.width / 2)) dx -= int32_t(d.width);
                if (dx < -int32_t(d.width / 2)) dx += int32_t(d.width);
                if (dy > int32_t(d.height / 2)) dy -= int32_t(d.height);
                if (dy < -int32_t(d.height / 2)) dy += int32_t(d.height);
                sx += dx;
                sy += dy;
                ++count;
            }
            if (!count) continue;
            const tile origin{
                uint32_t((int64_t(reference.x) + sx / count + d.width) % d.width),
                uint32_t((int64_t(reference.y) + sy / count + d.height) % d.height)};
            for (uint32_t j = 0; j != d.units; ++j) {
                uint8_t *member = row(p, d, player, j);
                if (same_root(r, member) && centroid_member(member)) publish_origin(member, origin, 1);
            }
            const uint32_t seq = root_sequence(r);
            if (seq > get(p + 76 + 4 * player)) put(p + 76 + 4 * player, seq);
        }
}
// The only process-local state is bounded, fully specified geometry. No row pointer,
// actor identity, phase, native slot or logical cursor is cached here.
struct curve_cache_entry {
    bool         used = false, valid = false;
    dimensions   shape{};
    uint32_t     point_count = 0;
    tile         points[CAPACITY]{};
    air::circuit curve{};
    air::pose    poses[air::CIRCUIT_STEPS + 1]{};
};
struct connector_cache_entry {
    bool       used = false, valid = false;
    dimensions shape{};
    air::pose  from{}, to{};
    air::path  path{};
    air::pose  poses[air::LEG_STEPS + 1]{};
};
bool same_pose(air::pose a, air::pose b) {
    return same(a.position, b.position) && a.heading == b.heading;
}
const curve_cache_entry *curve_for(dimensions d, const uint8_t *r, uint32_t spec_count) {
    if (field(r, CYCLE) || !spec_count || spec_count > field(r, COUNT) || spec_count > CAPACITY ||
        !resolved_origin(r)) return nullptr;
    tile     points[CAPACITY]{};
    uint32_t n = spec_count;
    for (uint32_t i = 0; i != spec_count; ++i) points[i] = task_of(node(r, i)).requested;
    if (spec_count == 1) {
        points[1] = {field(r, ORIGIN_X), field(r, ORIGIN_Y)};
        n         = 2;
    }
    static curve_cache_entry cache[64];
    static uint32_t          victim = 0;
    for (auto &c : cache) {
        if (!c.used || !same(c.shape, d) || c.point_count != n) continue;
        bool equal = true;
        for (uint32_t i = 0; i != n; ++i) equal &= same(c.points[i], points[i]);
        if (equal) return c.valid ? &c : nullptr;
    }
    auto &c       = cache[victim++ % 64];
    c.used        = true;
    c.shape       = d;
    c.point_count = n;
    for (uint32_t i = 0; i != n; ++i) c.points[i] = points[i];
    c.valid = air::build_circuit(d, points, n, c.curve) && c.curve.count &&
              c.curve.count <= air::CIRCUIT_STEPS;
    if (!c.valid) return nullptr;
    c.poses[0] = c.curve.start;
    for (uint32_t i = 0; i != c.curve.count; ++i)
        c.poses[i + 1] = air::stepped(d, c.poses[i], c.curve.headings[i]);
    c.valid = same_pose(c.poses[0], c.poses[c.curve.count]);
    return c.valid ? &c : nullptr;
}
const connector_cache_entry *connector_for(dimensions d, air::pose from, air::pose to) {
    static connector_cache_entry cache[128];
    static uint32_t              victim = 0;
    for (auto &c : cache)
        if (c.used && same(c.shape, d) && same_pose(c.from, from) && same_pose(c.to, to))
            return c.valid ? &c : nullptr;
    auto &c = cache[victim++ % 128];
    c.used  = true;
    c.shape = d;
    c.from  = from;
    c.to    = to;
    c.valid = air::plan(d, from, to, false, c.path) && c.path.count <= air::LEG_STEPS;
    if (!c.valid) return nullptr;
    c.poses[0] = from;
    for (uint32_t i = 0; i != c.path.count; ++i)
        c.poses[i + 1] = air::stepped(d, c.poses[i], c.path.headings[i]);
    c.valid = same_pose(c.poses[c.path.count], to);
    return c.valid ? &c : nullptr;
}
struct flight_cursor {
    uint32_t  mode = 0;
    air::pose start{};
    uint32_t  join_phase = 0, progress = 0, curve = 0, spec_count = 0;
};
flight_cursor flight_of(const uint8_t *r) {
    return {field(r, AIR_MODE), {{field(r, JOIN_X), field(r, JOIN_Y)}, field(r, JOIN_HEADING)}, field(r, JOIN_PHASE), field(r, JOIN_PROGRESS), field(r, CURVE_INDEX), field(r, AIR_SPEC_COUNT)};
}
void write_flight(uint8_t *r, const flight_cursor &f) {
    field(r, AIR_MODE, f.mode);
    field(r, JOIN_X, f.start.position.x);
    field(r, JOIN_Y, f.start.position.y);
    field(r, JOIN_HEADING, f.start.heading);
    field(r, JOIN_PHASE, f.join_phase);
    field(r, JOIN_PROGRESS, f.progress);
    field(r, CURVE_INDEX, f.curve);
    field(r, AIR_SPEC_COUNT, f.spec_count);
}
void suspend_flight(uint8_t *r) {
    std::memset(r + AIR_MODE, 0, SPEC_REVISION - AIR_MODE);
    std::memset(r + AIR_SLOT, 0, RETURN_HEADING - AIR_SLOT);
    if (!(field(r, FLAGS) & EXCURSION)) field(r, RETURN_HEADING, 0);
    field(r, AIR_SPEC_COUNT, 0);
}
// One stream transition corresponds to one native heading, not one logical task.
// Normalization never emits a zero-motion step or declares a waypoint reached.
bool flight_step(dimensions d, const uint8_t *r, const curve_cache_entry *c,
                 flight_cursor &f, air::pose &before, uint8_t &heading, air::pose &after) {
    for (uint32_t normalize = 0; normalize != 3; ++normalize) {
        if (f.mode == 2) {
            if (!c || f.curve >= c->curve.count) return false;
            before  = c->poses[f.curve];
            heading = c->curve.headings[f.curve];
            f.curve = (f.curve + 1) % c->curve.count;
            after   = c->poses[f.curve];
            return true;
        }
        if (f.mode == 4 || f.mode == 5) {
            air::circuit orbit{};
            if (!air::orbit(d, f.start, orbit) || !orbit.count || f.curve >= orbit.count) return false;
            before  = air::at(d, orbit, f.curve);
            heading = orbit.headings[f.curve];
            f.curve = (f.curve + 1) % orbit.count;
            after   = air::stepped(d, before, heading);
            return true;
        }
        if (f.mode != 1 && f.mode != 3) return false;
        const air::pose goal = f.mode == 3
                                   ? air::pose{{field(r, EXCURSION_X), field(r, EXCURSION_Y)}, field(r, RETURN_HEADING)}
                               : c && f.join_phase < c->curve.count ? c->poses[f.join_phase]
                                                                    : air::pose{{}, UINT32_MAX};
        if (goal.heading >= 24) return false;
        const auto *join = connector_for(d, f.start, goal);
        if (!join || f.progress > join->path.count) return false;
        if (f.progress == join->path.count) {
            if (f.mode == 1) {
                f.mode  = 2;
                f.curve = f.join_phase;
            } else {
                if (!c) return false;
                f.mode       = 1;
                f.start      = goal;
                f.join_phase = air::join_index(d, goal, c->curve);
                f.progress   = 0;
            }
            continue;
        }
        before  = join->poses[f.progress];
        heading = join->path.headings[f.progress++];
        after   = join->poses[f.progress];
        // Normalize immediately after the actual last connector segment. This makes
        // the serialized phase exact at a return-anchor passage, even at chunk end.
        if (f.progress == join->path.count && f.mode == 1) {
            f.mode  = 2;
            f.curve = f.join_phase;
        } else if (f.progress == join->path.count && f.mode == 3) {
            if (!c) return false;
            f.mode       = 1;
            f.start      = goal;
            f.join_phase = air::join_index(d, goal, c->curve);
            f.progress   = 0;
        }
        return air::legal(before.heading, heading);
    }
    return false;
}
bool start_flight(dimensions d, const uint8_t *r, const actor_view &actor,
                  const curve_cache_entry *c, uint32_t spec_count, flight_cursor &f) {
    f            = {};
    f.spec_count = spec_count;
    f.start      = {actor.position, actor.heading};
    if (actor.heading >= 24) return false;
    const bool returning = (field(r, FLAGS) & RETURN_MODE) != 0;
    f.mode               = returning ? 3u : 1u;
    if (c) f.join_phase = air::join_index(d, f.start, c->curve);
    flight_cursor probe = f;
    air::pose     before{}, after{};
    uint8_t       heading = 0;
    if (c && flight_step(d, r, c, probe, before, heading, after) &&
        same_pose(before, {actor.position, actor.heading})) return true;
    air::circuit orbit{};
    if (!air::orbit(d, f.start, orbit) || !orbit.count) return false;
    f.mode       = returning ? 5u : 4u;
    f.join_phase = f.progress = f.curve = 0;
    return true;
}
bool fill_chunk(dimensions d, const uint8_t *r, const curve_cache_entry *c,
                const flight_cursor &start, air_chunk &chunk) {
    flight_cursor f = start;
    air::pose     previous{}, before{}, after{};
    for (uint32_t i = 0; i != AIR_CHUNK_STEPS; ++i) {
        if (!flight_step(d, r, c, f, before, chunk.headings[i], after) ||
            (i && !same_pose(previous, before))) return false;
        previous = after;
    }
    chunk.count = AIR_CHUNK_STEPS;
    return true;
}
bool flight_valid(const uint8_t *r, dimensions d) {
    const uint32_t os = field(r, ORIGIN_STATE), mode = field(r, AIR_MODE);
    if (os > 6 || os == 5 || mode > 5) return false;
    if (!has_patrol(r)) return zero(r + ORIGIN_X, ACTOR_BYTES - ORIGIN_X);
    if (!field(r, SPEC_REVISION)) return false;
    if (resolved_origin(r)) {
        if (!tile_valid({field(r, ORIGIN_X), field(r, ORIGIN_Y)}, d)) return false;
        if ((os == 1 || os == 6) && !root_sequence(r)) return false;
        if (os == 2 && root_sequence(r)) return false;
    } else if (field(r, ORIGIN_X) || field(r, ORIGIN_Y)) return false;
    if (!mode) {
        return zero(r + AIR_MODE, SPEC_REVISION - AIR_MODE) && !field(r, AIR_SLOT) &&
               !field(r, AIR_SPEC_COUNT) && field(r, RETURN_HEADING) < 24 &&
               (!field(r, RETURN_HEADING) || (field(r, FLAGS) & EXCURSION));
    }
    if (!resolved_origin(r) || field(r, FAMILY) != value(executor::direction_plane) ||
        field(r, CYCLE) || !field(r, AIR_SPEC_COUNT) || field(r, AIR_SPEC_COUNT) > field(r, COUNT) ||
        !tile_valid({field(r, JOIN_X), field(r, JOIN_Y)}, d) || field(r, JOIN_HEADING) >= 24 ||
        field(r, RETURN_HEADING) >= 24 || field(r, CHUNK_ACK) > AIR_CHUNK_STEPS ||
        field(r, JOIN_PROGRESS) > air::LEG_STEPS || field(r, JOIN_PHASE) >= air::CIRCUIT_STEPS ||
        field(r, CURVE_INDEX) >= air::CIRCUIT_STEPS ||
        !field(r, CHUNK_REVISION) || field(r, CHUNK_REVISION) > field(r, SPEC_REVISION) ||
        !field(r, AIR_SLOT) || !(field(r, FLAGS) & APPLIED) ||
        (status(r) != phase::executing && status(r) != phase::returning_anchor) ||
        field(r, ASSIGNMENT_VALID)) return false;
    if ((mode <= 3 && field(r, FAILURE)) ||
        ((mode == 4 || mode == 5) && field(r, FAILURE) != value(failure::planning))) return false;
    if ((mode == 3 || mode == 5) && !(field(r, FLAGS) & RETURN_MODE)) return false;
    if ((mode == 1 || mode == 2 || mode == 4) && (field(r, FLAGS) & RETURN_MODE)) return false;
    const auto   *c = curve_for(d, r, field(r, AIR_SPEC_COUNT));
    flight_cursor f = flight_of(r);
    air::pose     before{}, after{};
    uint8_t       heading = 0;
    return flight_step(d, r, c, f, before, heading, after);
}
// The route remains flyable on failed allocation/replan. Publishing a successor and
// its exact slot ownership is atomic with the synchronous backend result.
bool attach_flight(uint8_t *p, uint8_t *r, dimensions d, const backend &b,
                   actor_key a, const actor_view &actor, bool refill, bool rejoin = false) {
    if (!b.apply || actor.heading >= 24 || !resolved_origin(r)) return false;
    flight_cursor f                = flight_of(r);
    const bool    recovery         = f.mode == 4 || f.mode == 5;
    const bool    owed_implicit    = (field(r, FLAGS) & IMPLICIT_ACTIVE) && field(r, COUNT) > 1;
    const bool    geometry_changed = field(r, CHUNK_REVISION) != field(r, SPEC_REVISION) ||
                                  f.spec_count != field(r, COUNT);
    const bool     restart = rejoin || !f.mode || recovery || (geometry_changed && !owed_implicit);
    const uint32_t spec    = restart ? (owed_implicit ? 1u : field(r, COUNT)) : f.spec_count;
    const auto    *c       = curve_for(d, r, spec);
    if (restart && !start_flight(d, r, actor, c, spec, f)) return false;
    air_chunk chunk{};
    if (!fill_chunk(d, r, c, f, chunk)) return false;
    if (refill) {
        if (!field(r, AIR_SLOT) || actor.path_slot != field(r, AIR_SLOT) - 1) return false;
        chunk.slot = actor.path_slot;
    } else if (actor.path_slot != UINT32_MAX) {
        chunk.slot = actor.path_slot;
    }
    execution request{ticket_of(p, r, a), task_of(node(r, field(r, CURSOR))), false, {}, &chunk};
    // Air directions already encode physical return/join; never run finite F planning.
    const apply_result result = b.apply(b.context, request);
    if (result != apply_result::applied || chunk.slot == UINT32_MAX) return false;
    write_flight(r, f);
    field(r, CHUNK_ACK, 0);
    if (restart) field(r, CHUNK_REVISION, field(r, SPEC_REVISION));
    field(r, AIR_SLOT, chunk.slot + 1);
    field(r, FLAGS, (field(r, FLAGS) & ~(REPLACE_PENDING | PLANNER_SUCCEEDED)) | APPLIED | ((f.mode == 4 || f.mode == 5) ? 0u : PLANNER_SUCCEEDED));
    clear_assignment(r);
    field(r, FAILURE, f.mode == 4 || f.mode == 5 ? value(failure::planning) : 0);
    status(r, (field(r, FLAGS) & RETURN_MODE) ? phase::returning_anchor : phase::executing);
    if (!field(r, TRACKING)) reset_circuit(r, actor.position);
    if (field(r, COUNT) == 1 && !(field(r, FLAGS) & HAS_IMPLICIT)) {
        field(r, IMPLICIT_X, field(r, ORIGIN_X));
        field(r, IMPLICIT_Y, field(r, ORIGIN_Y));
        field(r, FLAGS, field(r, FLAGS) | HAS_IMPLICIT);
    }
    return true;
}
void acknowledge_visit(uint8_t *r, tile actual) {
    if (field(r, FLAGS) & RETURN_MODE) return;
    if (!same(actual, intended_destination(r))) return;
    const uint32_t count = field(r, COUNT), cursor = field(r, CURSOR);
    uint32_t       flags = field(r, FLAGS);
    if (count == 1) flags ^= IMPLICIT_ACTIVE;
    else {
        // A formerly implicit leg survives an append until this real passage.
        if (flags & IMPLICIT_ACTIVE) flags &= ~IMPLICIT_ACTIVE;
        field(r, CURSOR, (cursor + 1) % count);
        flags &= ~HAS_IMPLICIT;
        field(r, IMPLICIT_X, 0);
        field(r, IMPLICIT_Y, 0);
    }
    field(r, FLAGS, flags);
    field(r, PROGRESS, 1);
}
struct prepared_edit {
    actor_view     actor{};
    objective      task_objective{};
    active_capture capture{};
    objective      captured[2]{};
    const uint8_t *origin_donor = nullptr;
    bool           existing = false, duplicate = false;
    tile           fixed_move{};
    bool           fixed_move_valid = false;
};
refusal prepare_edit(const state &s, const backend &b, const edit &e, prepared_edit &out) {
    if (!s.valid()) return refusal::malformed;
    if (!same(e.world, s.world())) return refusal::stale_world;
    const dimensions d = s.shape();
    if (!read_actor(b, e.actor, d, out.actor)) return refusal::stale_actor;
    if (e.operation != edit_kind::replace && e.operation != edit_kind::append &&
        e.operation != edit_kind::cancel) return refusal::malformed;
    const uint8_t *r = row(s.data(), d, e.actor.player, e.actor.slot);
    if (e.operation == edit_kind::cancel) {
        if (!cancel_valid(e.task)) return refusal::malformed;
        return field(r, LIFE) && field(r, LIFE) != e.actor.incarnation ? refusal::stale_actor : refusal::none;
    }
    if (!entry_valid(e.task, d, true)) return refusal::malformed;
    if (!target_objective(b, e.actor, e.task, out.task_objective)) return refusal::unsupported;
    if (out.task_objective == objective::unavailable) return refusal::unavailable_target;
    out.existing = field(r, LIFE) == e.actor.incarnation && field(r, COUNT);
    if (e.operation == edit_kind::append && !out.existing && field(r, LIFE) == e.actor.incarnation &&
        (field(r, FLAGS) & PENDING_CLEANUP)) return refusal::unsupported;
    if (e.operation == edit_kind::append && out.existing) {
        const uint32_t count = field(r, COUNT), cycle = field(r, CYCLE);
        if (e.task.kind == action::patrol && cycle != NO_CYCLE &&
            out.actor.family == executor::direction_plane &&
            same(task_of(node(r, count - 1)).requested, e.task.requested)) {
            out.duplicate = true;
            return refusal::none;
        }
        if (count == CAPACITY) return refusal::full;
        if (cycle != NO_CYCLE && e.task.kind != action::patrol) return refusal::after_patrol;
        if (field(r, NEXT_LEG) == UINT32_MAX || field(r, EPOCH) == UINT32_MAX)
            return refusal::exhausted_identity;
        if (e.task.kind == action::patrol && cycle != NO_CYCLE &&
            out.actor.family != executor::direction_plane) {
            bool distinct = false;
            for (uint32_t i = cycle; i != count; ++i)
                if (!same(task_of(node(r, i)).requested, e.task.requested)) distinct = true;
            if (!distinct) return refusal::degenerate_cycle;
        }
        if (e.task.kind == action::patrol && field(r, SPEC_REVISION) == UINT32_MAX)
            return refusal::exhausted_identity;
        if (e.task.kind == action::patrol && cycle == NO_CYCLE) {
            const entry predecessor = task_of(node(r, count - 1));
            if (predecessor.kind == action::move && predecessor.command_sequence) {
                out.fixed_move       = predecessor.requested;
                out.fixed_move_valid = true;
            }
            if (out.fixed_move_valid && out.actor.family != executor::direction_plane &&
                same(out.fixed_move, e.task.requested)) return refusal::degenerate_cycle;
        }
        if (!e.legacy_sequence && e.task.kind == action::patrol && cycle == NO_CYCLE &&
            !new_root_allowed(s.data(), d, b, e.actor.player, e.task.command_sequence, r,
                              out.fixed_move_valid, out.origin_donor))
            return refusal::stale_ticket;
        return refusal::none;
    }
    if (e.operation == edit_kind::append) {
        auto &capture = out.capture;
        if (!b.capture_active || !b.capture_active(b.context, e.actor, capture) || !capture_valid(capture, d))
            return refusal::unsupported;
        if (capture.requires_replan && out.actor.family != executor::direction_plane &&
            out.actor.family != executor::direction_nonplane) return refusal::unsupported;
        for (uint32_t i = 0; i != capture.count; ++i)
            if (!target_objective(b, e.actor, capture.nodes[i], out.captured[i])) return refusal::unsupported;
        if (out.actor.family != executor::direction_plane && capture.count == 2 &&
            same(capture.nodes[0].requested, capture.nodes[1].requested))
            return refusal::degenerate_cycle;
        if (capture.count && capture.nodes[0].kind == action::patrol) {
            if (e.task.kind != action::patrol) return refusal::after_patrol;
            if (capture.count == 1 && same(capture.nodes[0].requested, e.task.requested)) {
                if (out.actor.family != executor::direction_plane) return refusal::degenerate_cycle;
                out.duplicate = true;
                return refusal::none;
            }
        }
        if (e.task.kind == action::patrol && capture.count == 1 &&
            capture.nodes[0].kind == action::move && capture.logical_move_valid) {
            out.fixed_move       = capture.logical_move;
            out.fixed_move_valid = true;
        }
    }
    if (out.fixed_move_valid && out.actor.family != executor::direction_plane &&
        same(out.fixed_move, e.task.requested)) return refusal::degenerate_cycle;
    const uint32_t generation = field(r, LIFE) == e.actor.incarnation ? field(r, ROUTE) : 0;
    const uint32_t epoch      = field(r, LIFE) == e.actor.incarnation ? field(r, EPOCH) : 0;
    if (generation == UINT32_MAX || epoch == UINT32_MAX) return refusal::exhausted_identity;
    if (out.actor.family != executor::direction_plane && !out.capture.count &&
        e.task.kind == action::patrol && same(e.task.requested, out.actor.position))
        return refusal::degenerate_cycle;
    uint32_t incoming_root = 0;
    for (uint32_t i = 0; i != out.capture.count; ++i)
        if (out.capture.nodes[i].kind == action::patrol) {
            incoming_root = out.capture.nodes[i].command_sequence;
            break;
        }
    if (!out.capture.count || out.capture.nodes[0].kind != action::patrol)
        if (e.task.kind == action::patrol) incoming_root = e.task.command_sequence;
    if (!e.legacy_sequence && (incoming_root || e.task.kind == action::patrol) &&
        !new_root_allowed(s.data(), d, b, e.actor.player, incoming_root, r,
                          out.fixed_move_valid, out.origin_donor))
        return refusal::stale_ticket;
    return refusal::none;
}
} // namespace

uint32_t state::required_bytes(dimensions d) {
    if (!d.players && !d.units && !d.width && !d.height && !d.buildings) return HEADER_BYTES;
    if (!d.players || d.players > 8 || !d.units || d.units > 65535 || !d.buildings || d.buildings > 65535 ||
        d.width > 256 || d.height > 256 || ((!d.width) != (!d.height))) return 0;
    const uint64_t n = HEADER_BYTES + uint64_t(d.players) * d.units * ROW_BYTES;
    return n <= std::numeric_limits<uint32_t>::max() - ENVELOPE_BYTES ? uint32_t(n) : 0;
}
bool state::initialize(void *memory, uint32_t bytes, dimensions d, world_key w) {
    const uint32_t n = required_bytes(d);
    if (!memory || !n || bytes != n || !world_valid(d, w)) return false;
    auto *p = static_cast<uint8_t *>(memory);
    std::memset(p, 0, n);
    put(p, REGION_MAGIC);
    put(p + 4, SCHEMA);
    put(p + 8, d.players);
    put(p + 12, d.units);
    put(p + 16, d.width);
    put(p + 20, d.height);
    put(p + 24, w.system);
    put(p + 28, w.planet);
    put(p + 32, w.incarnation);
    put(p + 36, n);
    put(p + 72, d.buildings);
    bytes_       = p;
    bytes_count_ = n;
    return true;
}
bool state::bind(void *memory, uint32_t bytes) {
    if (!region_valid(static_cast<const uint8_t *>(memory), bytes)) return false;
    bytes_       = static_cast<uint8_t *>(memory);
    bytes_count_ = bytes;
    return true;
}
bool state::migrate_schema1(void *destination, uint32_t capacity, const void *source, uint32_t bytes) {
    const auto *old = static_cast<const uint8_t *>(source);
    auto       *out = static_cast<uint8_t *>(destination);
    if (!out || !legacy_region_valid(old, bytes)) return false;
    const dimensions d = shape_of(old);
    if (capacity != required_bytes(d) || !disjoint(out, capacity, old, bytes)) return false;
    migrate_legacy(out, old, d, capacity);
    return true;
}
bool           state::valid() const { return header_valid(bytes_, bytes_count_); }
const uint8_t *state::data() const { return bytes_; }
uint8_t       *state::data() { return bytes_; }
uint32_t       state::size() const { return bytes_count_; }
dimensions     state::shape() const { return bytes_ && bytes_count_ >= HEADER_BYTES ? shape_of(bytes_) : dimensions{}; }
world_key      state::world() const { return bytes_ && bytes_count_ >= HEADER_BYTES ? world_of(bytes_) : world_key{}; }
uint32_t       state::command_high_water(uint32_t issuer) const {
    return valid() && issuer < shape().players ? get(bytes_ + 40 + 4 * issuer) : 0;
}
bool state::ordinary_move_witness(actor_key a, tile &out) const {
    if (!valid() || a.player >= shape().players || a.slot >= shape().units || !a.incarnation) return false;
    const uint8_t *r = row(bytes_, shape(), a.player, a.slot);
    if (field(r, LIFE) != a.incarnation || field(r, COUNT) || field(r, ORIGIN_STATE) != 5) return false;
    out = {field(r, ORIGIN_X), field(r, ORIGIN_Y)};
    return tile_valid(out, shape());
}
bool state::observe_command_sequence(uint32_t issuer, uint32_t sequence) {
    if (!valid() || !shape().width || issuer >= shape().players || !sequence) return false;
    record_sequence(bytes_, issuer, sequence);
    return true;
}
refusal state::preview_edit(const backend &b, const edit &e) const {
    prepared_edit prepared;
    return prepare_edit(*this, b, e, prepared);
}
refusal state::apply_edit(const backend &b, const edit &e) {
    prepared_edit prepared;
    const refusal decision = prepare_edit(*this, b, e, prepared);
    if (decision != refusal::none) return decision;
    if (prepared.duplicate) {
        record_sequence(bytes_, e.actor.player, e.task.command_sequence);
        return refusal::none;
    }
    if (e.operation == edit_kind::cancel) {
        const refusal result = cancel_actor(b, e.actor);
        if (result != refusal::none) return result;
        record_sequence(bytes_, e.actor.player, e.task.command_sequence);
        return refusal::none;
    }
    const dimensions d = shape();
    uint8_t         *r = row(bytes_, d, e.actor.player, e.actor.slot);
    if (e.operation == edit_kind::append && prepared.existing) {
        const uint32_t cycle = field(r, CYCLE);
        append_node(r, e.task, prepared.task_objective);
        if (e.task.kind == action::patrol) {
            field(r, SPEC_REVISION, field(r, SPEC_REVISION) + 1);
            if (cycle == NO_CYCLE) {
                if (prepared.fixed_move_valid) publish_origin(r, prepared.fixed_move, 6);
                else if (e.legacy_sequence) field(r, ORIGIN_STATE, 4);
                else if (prepared.origin_donor && resolved_origin(prepared.origin_donor))
                    publish_origin(r, {field(prepared.origin_donor, ORIGIN_X), field(prepared.origin_donor, ORIGIN_Y)}, 1);
            }
        }
        if (cycle != NO_CYCLE && (field(r, FLAGS) & HAS_IMPLICIT) && !(field(r, FLAGS) & IMPLICIT_ACTIVE)) {
            field(r, FLAGS, field(r, FLAGS) & ~HAS_IMPLICIT);
            field(r, IMPLICIT_X, 0);
            field(r, IMPLICIT_Y, 0);
            if (prepared.actor.family != executor::direction_plane) reset_circuit(r, prepared.actor.position);
        }
        record_sequence(bytes_, e.actor.player, e.task.command_sequence);
        return refusal::none;
    }
    const uint32_t generation     = field(r, LIFE) == e.actor.incarnation ? field(r, ROUTE) : 0;
    const uint32_t epoch          = field(r, LIFE) == e.actor.incarnation ? field(r, EPOCH) : 0;
    const bool     donor_resolved = prepared.origin_donor && resolved_origin(prepared.origin_donor);
    tile           donor_origin{};
    if (donor_resolved)
        donor_origin = {field(prepared.origin_donor, ORIGIN_X), field(prepared.origin_donor, ORIGIN_Y)};
    primary_reservation carried{};
    const bool          carry_pending = e.operation == edit_kind::append && !prepared.existing &&
                               prepared.capture.count == 1 && prepared.capture.status == phase::awaiting_application &&
                               field(r, LIFE) == e.actor.incarnation && read_reservation(e.actor, carried) &&
                               carried.phase == reservation_phase::pending && carried.execution_epoch == epoch &&
                               same(carried.target, prepared.capture.nodes[0].target);
    // capture_active has proved the unique earlier retained physical token before
    // returning an awaiting-application attack. This is metadata-only adoption.
    if (!carry_pending && field(r, LIFE) == e.actor.incarnation && !cancel_pending(b, e.actor, r))
        return refusal::unsupported;
    begin_route(r, e.actor, generation + 1, epoch + 1, prepared.actor.family);
    if (carry_pending) {
        carried.execution_epoch = epoch + 1;
        write_reservation_raw(r, carried);
    }
    for (uint32_t i = 0; i != prepared.capture.count; ++i) {
        append_node(r, prepared.capture.nodes[i], prepared.captured[i]);
        record_sequence(bytes_, e.actor.player, prepared.capture.nodes[i].command_sequence);
    }
    append_node(r, e.task, prepared.task_objective);
    if (has_patrol(r)) {
        field(r, SPEC_REVISION, 1);
        if (prepared.fixed_move_valid) publish_origin(r, prepared.fixed_move, 6);
        else if (e.legacy_sequence) field(r, ORIGIN_STATE, 4);
        else if (donor_resolved) publish_origin(r, donor_origin, 1);
    }
    if (prepared.capture.count && !prepared.capture.requires_replan) capture_status(r, prepared.capture);
    else if (e.operation == edit_kind::replace) field(r, FLAGS, field(r, FLAGS) | REPLACE_PENDING);
    record_sequence(bytes_, e.actor.player, e.task.command_sequence);
    return refusal::none;
}
refusal state::adopt_applied(const backend &b, actor_key a, const entry &task, const ticket *external_owner) {
    if (!valid()) return refusal::malformed;
    const dimensions d = shape();
    if (a.player >= d.players || a.slot >= d.units || !a.incarnation) return refusal::stale_actor;
    uint8_t *r = row(bytes_, d, a.player, a.slot);
    // QOEX was checked BEFORE native application by the carrier. Recheck here too:
    // delayed/double delivery must never cancel a different current route.
    if (external_owner && (!same(external_owner->actor, a) || !matches(bytes_, r, *external_owner)))
        return refusal::stale_ticket;
    const bool existing = field(r, LIFE) == a.incarnation && field(r, COUNT);
    // An actual ordinary F remains unmanaged. A valid Move may retain its original
    // requested point as non-owning history; only a later positive active capture uses it.
    if (!external_owner) {
        bool released = true;
        if (field(r, LIFE) == a.incarnation) {
            field(r, FLAGS, field(r, FLAGS) & ~PENDING_CLEANUP);
            released = cancel_pending(b, a, r);
            clear_owned(r);
        }
        actor_view actor{};
        if (released && task.kind == action::move && entry_valid(task, d, false) &&
            read_actor(b, a, d, actor)) {
            if (field(r, LIFE) != a.incarnation) clear_row(r);
            field(r, LIFE, a.incarnation);
            publish_origin(r, task.requested, 5);
        }
        return released ? refusal::none : refusal::unsupported;
    }
    if (!existing) return refusal::none;
    if (!(field(r, FLAGS) & EXTERNAL_PENDING)) return refusal::none; // Duplicate QOEX delivery.
    actor_view actor{};
    if (!read_actor(b, a, d, actor)) {
        cancel_actor(b, a);
        return refusal::stale_actor;
    }
    if (!entry_valid(task, d, false) || !same(task_of(node(r, field(r, CURSOR))), task)) {
        cancel_actor(b, a);
        return refusal::unsupported;
    }
    active_capture capture{};
    objective      obj;
    if (!b.capture_active || !b.capture_active(b.context, a, capture) || !capture_valid(capture, d) ||
        !capture.count || capture.requires_replan || capture.status == phase::awaiting_application ||
        !same(capture.nodes[0], task) || !target_objective(b, a, task, obj)) {
        cancel_actor(b, a);
        return refusal::unsupported;
    }
    // The retained earlier command has FINALLY applied. Do not rebuild its suffix, import
    // a newly written patrol home, or replace the generation. The exact head owns this F.
    capture_status(r, capture);
    clear_assignment(r);
    if (capture.assignment_valid) assign(r, capture.assigned, capture.placement, capture.path_slot);
    field(r, FAMILY, value(actor.family));
    uint8_t *n = node(r, field(r, CURSOR));
    if (get(n + 40) == value(objective::unresolved)) put(n + 40, value(obj));
    return refusal::none;
}
control state::observe(const backend &b, const outcome &o) {
    if (!valid()) return control::unchanged;
    const dimensions d = shape();
    if (o.owner.actor.player >= d.players || o.owner.actor.slot >= d.units || !family_valid(o.family))
        return control::unchanged;
    uint8_t *r = row(bytes_, d, o.owner.actor.player, o.owner.actor.slot);
    if (!matches(bytes_, r, o.owner) || field(r, FAMILY) != value(o.family)) return control::unchanged;
    actor_view actor{};
    if (!read_actor(b, o.owner.actor, d, actor) || actor.family != o.family) return control::unchanged;
    const phase p     = status(r);
    uint32_t    flags = field(r, FLAGS);
    uint8_t    *n     = node(r, field(r, CURSOR));
    const entry task  = task_of(n);
    if (field(r, AIR_MODE) &&
        o.kind != outcome_kind::primary_excursion_started &&
        o.kind != outcome_kind::primary_excursion_ended &&
        o.kind != outcome_kind::attack_cleanup_finished) return control::unchanged;
    if (p == phase::blocked || p == phase::ready_next) return control::unchanged;
    if ((flags & PENDING_CLEANUP) && o.kind != outcome_kind::attack_cleanup_finished)
        return actor.safe_to_apply ? control::hold : control::unchanged;
    if (flags & EXTERNAL_PENDING) {
        // A discard is authoritative only AFTER the retained native command was removed.
        // Target retirement alone cannot release the tail while that old command can apply.
        if (o.kind != outcome_kind::rejected) return control::unchanged;
        field(r, FLAGS, flags & ~EXTERNAL_PENDING);
        if (is_attack(task.kind) && get(n + 40) == value(objective::destroyed_confirmed)) return complete(r);
        block(r, is_attack(task.kind) && get(n + 40) == value(objective::unavailable) ? failure::invalid_target : failure::application);
        return control::hold;
    }
    switch (o.kind) {
        case outcome_kind::deferred: return control::unchanged;
        case outcome_kind::applied:
            if ((p != phase::awaiting_application && p != phase::returning_anchor) || (flags & APPLIED)) break;
            field(r, FLAGS, (flags & ~(REPLACE_PENDING | PLANNER_SUCCEEDED)) | APPLIED);
            if (p != phase::returning_anchor) status(r, phase::executing);
            return control::unchanged;
        case outcome_kind::rejected:
            if ((p == phase::awaiting_application || p == phase::returning_anchor) && !(flags & APPLIED)) {
                block(r, failure::application);
                return control::hold;
            }
            break;
        case outcome_kind::planning_started:
            if (!(flags & APPLIED) || (p != phase::executing && p != phase::preparation && p != phase::returning_anchor)) break;
            if (!bump_attempt(r, b, o.owner.actor)) return control::hold;
            return control::unchanged;
        case outcome_kind::planning_succeeded:
            if (!(flags & APPLIED) || (p != phase::executing && p != phase::preparation && p != phase::returning_anchor)) break;
            field(r, FLAGS, flags | PLANNER_SUCCEEDED);
            return control::unchanged;
        case outcome_kind::preparation_started:
            if (!(flags & APPLIED) || (p != phase::executing && p != phase::returning_anchor)) break;
            if (!bump_attempt(r, b, o.owner.actor)) return control::hold;
            status(r, phase::preparation);
            return control::unchanged;
        case outcome_kind::preparation_finished:
            if (p != phase::preparation) break;
            status(r, (flags & RETURN_MODE) ? phase::returning_anchor : phase::executing);
            return control::unchanged;
        case outcome_kind::assigned:
            if (!(flags & APPLIED) || (p != phase::executing && p != phase::preparation && p != phase::returning_anchor) ||
                !tile_valid(o.assigned, d) || value(o.placement) < 1 || value(o.placement) > 3 ||
                (o.placement == assignment_reason::requested && !is_attack(task.kind) &&
                 !same(o.assigned, intended_destination(r)))) break;
            // A second assignment needs a new planning_started ticket. Never silently
            // overwrite a successful assignment while its old arrival can still be in flight.
            if (field(r, ASSIGNMENT_VALID)) break;
            assign(r, o.assigned, o.placement, o.path_slot);
            return control::unchanged;
        case outcome_kind::reached_exact:
        case outcome_kind::reached_near: {
            if (!(flags & APPLIED) || !field(r, ASSIGNMENT_VALID) ||
                (p != phase::executing && p != phase::returning_anchor) ||
                o.path_slot != field(r, PATH_SLOT) ||
                (task.kind != action::move && task.kind != action::patrol)) break;
            if (o.kind == outcome_kind::reached_exact) {
                if (!same(o.position, {field(r, ASSIGNED_X), field(r, ASSIGNED_Y)})) break;
            } else {
                // Positive native near-enough acceptance corrects the terminal endpoint of
                // this successful assignment; it is NOT another planning attempt. The
                // producer supplies its actual settled tile and saved pre-free path slot.
                if (o.placement != assignment_reason::valid_projection ||
                    !same(o.position, o.assigned) || !same(o.position, actor.position)) break;
            }
            if (p == phase::returning_anchor) {
                if (!bump_attempt(r, b, o.owner.actor)) return control::hold;
                field(r, FLAGS, flags & ~(EXCURSION | RETURN_MODE | APPLIED | PLANNER_SUCCEEDED));
                field(r, EXCURSION_X, 0);
                field(r, EXCURSION_Y, 0);
                std::memset(r + EX_TARGET_KIND, 0, 20);
                status(r, phase::awaiting_application);
                return control::hold;
            }
            if (task.kind == action::patrol && field(r, TRACKING) &&
                !same(o.position, {field(r, CIRCUIT_X), field(r, CIRCUIT_Y)})) field(r, PROGRESS, 1);
            return complete(r);
        }
        case outcome_kind::planning_failed:
        case outcome_kind::pool_exhausted:
        case outcome_kind::give_up:
            if (p == phase::excursion || !(flags & APPLIED)) break;
            block(r, o.kind == outcome_kind::planning_failed ? failure::planning : o.kind == outcome_kind::pool_exhausted ? failure::path_pool
                                                                                                                          : failure::native_give_up);
            return control::hold;
        case outcome_kind::primary_excursion_started:
            if (!(flags & APPLIED) || p != phase::executing || task.kind != action::patrol || !tile_valid(o.position, d) ||
                (actor.family == executor::direction_plane &&
                 (actor.heading >= 24 || !same(o.position, actor.position))) ||
                o.target.kind == object_kind::none || !target_valid(o.target, d) || !b.read_target ||
                b.read_target(b.context, o.owner.actor, o.target) != target_status::alive_hostile) break;
            if (!bump_attempt(r, b, o.owner.actor)) return control::hold;
            suspend_flight(r);
            field(r, RETURN_HEADING, actor.family == executor::direction_plane ? actor.heading : 0);
            field(r, EXCURSION_X, o.position.x);
            field(r, EXCURSION_Y, o.position.y);
            field(r, EX_TARGET_KIND, value(o.target.kind));
            field(r, EX_TARGET_OWNER, o.target.object.player);
            field(r, EX_TARGET_SLOT, o.target.object.slot);
            field(r, EX_TARGET_LIFE, o.target.object.incarnation);
            field(r, EX_OBJECTIVE, value(objective::unresolved));
            field(r, FLAGS, (flags & ~PLANNER_SUCCEEDED) | EXCURSION);
            status(r, phase::excursion);
            return control::unchanged;
        case outcome_kind::primary_excursion_ended:
            if (p != phase::excursion || !(flags & EXCURSION)) break;
            begin_anchor_return(r, b, o.owner.actor);
            return control::hold;
        case outcome_kind::attack_cleanup_finished:
            if (!is_attack(task.kind) || !(flags & APPLIED) ||
                get(n + 40) != value(objective::destroyed_confirmed)) break;
            field(r, FLAGS, flags & ~(PENDING_CLEANUP | APPLIED | REPLACE_PENDING | PLANNER_SUCCEEDED));
            if (get(r + RESERVATION) != value(reservation_phase::none)) std::memset(r + RESERVATION, 0, 48);
            return complete(r);
    }
    return control::unchanged;
}
void state::tick(const backend &b) {
    if (!valid()) return;
    const dimensions d = shape();
    // Pass1: normalize EVERY lifetime, family, cleanup, objective and ready prefix.
    // No backend.apply occurs until the cohort boundary and origin pass are complete.
    for (uint32_t player = 0; player != d.players; ++player)
        for (uint32_t slot = 0; slot != d.units; ++slot) {
            uint8_t *r = row(bytes_, d, player, slot);
            if (!field(r, COUNT) && !(field(r, FLAGS) & PENDING_CLEANUP)) continue;
            const actor_key a{player, slot, field(r, LIFE)};
            actor_view      actor{};
            if (!read_actor(b, a, d, actor)) {
                clear_owned(r);
                continue;
            }
            if (field(r, COUNT) && value(actor.family) != field(r, FAMILY)) {
                suspend_flight(r);
                block(r, failure::unsupported_executor);
                continue;
            }
            bool may_access = true;
            if (legacy_origin(r) && actor.family == executor::direction_plane && field(r, COUNT) &&
                field(r, CYCLE) == 0 && status(r) == phase::blocked && actor.safe_to_apply &&
                !(field(r, FLAGS) & (PENDING_CLEANUP | EXTERNAL_PENDING)) &&
                (!(field(r, FLAGS) & EXCURSION) || (field(r, FLAGS) & RETURN_MODE)) &&
                physical_binding(r).kind == object_kind::none &&
                (field(r, FAILURE) == value(failure::zero_progress_cycle) ||
                 field(r, FAILURE) == value(failure::planning) ||
                 field(r, FAILURE) == value(failure::path_pool) ||
                 field(r, FAILURE) == value(failure::native_give_up))) {
                // A truthful schema1 finite-planner failure is not permanent aircraft
                // physics. Safely acquire a NEW attempt, after actual pending rollback;
                // migration itself never touched the old lease, epoch or failed history.
                if (bump_attempt(r, b, a)) {
                    field(r, FLAGS, field(r, FLAGS) & ~(APPLIED | REPLACE_PENDING | PLANNER_SUCCEEDED));
                    field(r, FAILURE, 0);
                    status(r, (field(r, FLAGS) & RETURN_MODE) ? phase::returning_anchor : phase::awaiting_application);
                }
            }
            if ((field(r, FLAGS) & PENDING_CLEANUP) || status(r) == phase::blocked || status(r) == phase::excursion) {
                service_primary(bytes_, r, b, a, actor, may_access);
                continue;
            }
            if (!advance_ready(r, actor.position, b, a) || !field(r, COUNT)) continue;
            service_primary(bytes_, r, b, a, actor, may_access);
        }
    // Pass2: a common post-dispatch, pre-movement snapshot, no barrier or recenter.
    freeze_origins(bytes_, d, b);
    // Pass3: independent native applications. Slow/unsafe members never delay a peer.
    for (uint32_t player = 0; player != d.players; ++player)
        for (uint32_t slot = 0; slot != d.units; ++slot) {
            uint8_t *r = row(bytes_, d, player, slot);
            if (!field(r, COUNT) || (field(r, FLAGS) & (EXTERNAL_PENDING | PENDING_CLEANUP))) continue;
            const actor_key a{player, slot, field(r, LIFE)};
            actor_view      actor{};
            if (!read_actor(b, a, d, actor) || !actor.safe_to_apply || value(actor.family) != field(r, FAMILY)) continue;
            const entry task  = task_of(node(r, field(r, CURSOR)));
            uint32_t    flags = field(r, FLAGS);
            const phase p     = status(r);
            if (actor.family == executor::direction_plane && task.kind == action::patrol &&
                field(r, CYCLE) == 0 && !field(r, AIR_MODE) &&
                (p == phase::awaiting_application || p == phase::returning_anchor || p == phase::executing)) {
                // Existing safe airborne predecessors are retained if allocation/planning defers.
                attach_flight(bytes_, r, d, b, a, actor, false);
                continue;
            }
            if (p != phase::awaiting_application && p != phase::returning_anchor) continue;
            if (flags & APPLIED) continue;
            if (task.kind == action::patrol && !(flags & RETURN_MODE)) {
                if (field(r, COUNT) - field(r, CYCLE) == 1 && !(flags & HAS_IMPLICIT)) {
                    const tile anchor = field(r, ORIGIN_STATE) == 6
                                            ? tile{field(r, ORIGIN_X), field(r, ORIGIN_Y)}
                                            : actor.position;
                    if (same(task.requested, anchor)) {
                        block(r, failure::zero_progress_cycle);
                        continue;
                    }
                    field(r, IMPLICIT_X, anchor.x);
                    field(r, IMPLICIT_Y, anchor.y);
                    flags |= HAS_IMPLICIT;
                    field(r, FLAGS, flags);
                }
                if (!field(r, TRACKING)) reset_circuit(r, actor.position);
            }
            if (!b.apply) {
                block(r, failure::application);
                continue;
            }
            execution request{ticket_of(bytes_, r, a), task, false, {}};
            if (flags & RETURN_MODE) {
                request.return_to_anchor = true;
                request.anchor           = {field(r, EXCURSION_X), field(r, EXCURSION_Y)};
            } else if (flags & IMPLICIT_ACTIVE) {
                request.task.requested = {field(r, IMPLICIT_X), field(r, IMPLICIT_Y)};
            }
            const apply_result result = b.apply(b.context, request);
            if (result == apply_result::applied) {
                field(r, FLAGS, (field(r, FLAGS) & ~(REPLACE_PENDING | PLANNER_SUCCEEDED)) | APPLIED);
                if (p != phase::returning_anchor) status(r, phase::executing);
            } else if (result == apply_result::rejected) block(r, failure::application);
        }
}
bool state::aircraft_boundary(const backend &b, actor_key a, uint32_t native_slot, uint32_t native_cursor,
                              bool *owned_at_entry) {
    if (owned_at_entry) *owned_at_entry = false;
    if (!valid()) return false;
    const dimensions d = shape();
    if (a.player >= d.players || a.slot >= d.units || !a.incarnation) return false;
    uint8_t *r = row(bytes_, d, a.player, a.slot);
    if (field(r, LIFE) != a.incarnation || !field(r, COUNT) || !field(r, AIR_MODE) ||
        task_of(node(r, field(r, CURSOR))).kind != action::patrol) return false;
    const uint32_t flags = field(r, FLAGS);
    // RETURN_MODE retains EXCURSION until actual anchor passage, unlike pursuit.
    if ((flags & (PENDING_CLEANUP | EXTERNAL_PENDING)) ||
        ((flags & EXCURSION) && !(flags & RETURN_MODE))) return false;
    actor_view actor{};
    if (!read_actor(b, a, d, actor) || actor.family != executor::direction_plane || actor.heading >= 24 ||
        actor.microstep != 31 || actor.state != 17 || actor.order != 16 || actor.path_slot != native_slot || actor.path_cursor != native_cursor ||
        !field(r, AIR_SLOT) || native_slot != field(r, AIR_SLOT) - 1 || native_cursor > AIR_CHUNK_STEPS ||
        (status(r) != phase::executing && status(r) != phase::returning_anchor)) return false;
    if (owned_at_entry) *owned_at_entry = true;
    const uint32_t ack = field(r, CHUNK_ACK);
    if (native_cursor == ack) {
        flight_cursor current = flight_of(r);
        const auto   *curve   = curve_for(d, r, field(r, AIR_SPEC_COUNT));
        air::pose     before{}, after{};
        uint8_t       next = 0;
        if (!flight_step(d, r, curve, current, before, next, after) ||
            !same_pose(before, {actor.position, actor.heading}))
            return attach_flight(bytes_, r, d, b, a, actor, true, true);
        // A duplicate boundary at terminator retries allocation only; never double-credit.
        if (ack == AIR_CHUNK_STEPS) return attach_flight(bytes_, r, d, b, a, actor, true);
        return true;
    }
    if (native_cursor != ack + 1)
        return attach_flight(bytes_, r, d, b, a, actor, true, true);
    const auto    *c             = curve_for(d, r, field(r, AIR_SPEC_COUNT));
    flight_cursor  f             = flight_of(r);
    const uint32_t previous_mode = f.mode;
    air::pose      before{}, after{};
    uint8_t        heading = 0;
    if (!flight_step(d, r, c, f, before, heading, after) ||
        !same_pose(after, {actor.position, actor.heading})) {
        // No claimed arrival. A successor may be installed only at this settled actual
        // pose; until then the caller retains its current flyable native predecessor.
        return attach_flight(bytes_, r, d, b, a, actor, true, true);
    }
    const bool returning = (field(r, FLAGS) & RETURN_MODE) != 0;
    write_flight(r, f);
    field(r, CHUNK_ACK, native_cursor);
    if (returning && previous_mode == 3 && f.mode != 3) {
        field(r, FLAGS, field(r, FLAGS) & ~(EXCURSION | RETURN_MODE));
        field(r, EXCURSION_X, 0);
        field(r, EXCURSION_Y, 0);
        std::memset(r + EX_TARGET_KIND, 0, 20);
        field(r, RETURN_HEADING, 0);
        status(r, phase::executing);
    } else if (!returning && previous_mode != 4 && previous_mode != 5)
        acknowledge_visit(r, actor.position);
    if (native_cursor == AIR_CHUNK_STEPS) return attach_flight(bytes_, r, d, b, a, actor, true);
    return true;
}
void state::retire_actor(actor_key a) {
    if (!valid()) return;
    const dimensions d = shape();
    if (a.player < d.players && a.slot < d.units && a.incarnation) {
        uint8_t *r = row(bytes_, d, a.player, a.slot);
        if (field(r, LIFE) == a.incarnation) clear_row(r);
    }
}
bool state::retire_target(const backend &b, target_key target, bool destroyed) {
    if (!valid() || target.kind == object_kind::none || !target_valid(target, shape())) return false;
    const dimensions d = shape();
    for (uint32_t player = 0; player != d.players; ++player)
        for (uint32_t slot = 0; slot != d.units; ++slot) {
            uint8_t *r = row(bytes_, d, player, slot);
            if (!field(r, LIFE)) continue;
            const actor_key           a{player, slot, field(r, LIFE)};
            const primary_reservation reservation = reservation_of(r);
            if (reservation.phase == reservation_phase::pending &&
                (same(reservation.target, target) || same(reservation.prior_target, target)) &&
                !cancel_pending(b, a, r)) return false;
            if (field(r, COUNT) || (field(r, FLAGS) & PENDING_CLEANUP)) {
                bool current = same(physical_binding(r), target);
                if (field(r, COUNT)) {
                    const entry head = task_of(node(r, field(r, CURSOR)));
                    current |= (field(r, FLAGS) & APPLIED) && is_attack(head.kind) && same(head.target, target);
                    current |= status(r) == phase::excursion && same(excursion_target_of(r), target);
                }
                if (current && !detach_for_cleanup(r, b, a, target)) return false;
            }
            for (uint32_t i = 0; i != field(r, COUNT); ++i) {
                uint8_t *n = node(r, i);
                if (is_attack(static_cast<action>(get(n))) && same(task_of(n).target, target) &&
                    get(n + 40) == value(objective::unresolved))
                    put(n + 40, value(destroyed ? objective::destroyed_confirmed : objective::unavailable));
            }
            if ((field(r, FLAGS) & EXCURSION) && field(r, EX_OBJECTIVE) == value(objective::unresolved) &&
                same(excursion_target_of(r), target))
                field(r, EX_OBJECTIVE, value(destroyed ? objective::destroyed_confirmed : objective::unavailable));
        }
    return true; // Caller may now retire/reuse the canonical target lifetime.
}
refusal state::cancel_actor(const backend &b, actor_key a) {
    if (!valid()) return refusal::malformed;
    const dimensions d = shape();
    if (a.player >= d.players || a.slot >= d.units || !a.incarnation) return refusal::stale_actor;
    uint8_t *r = row(bytes_, d, a.player, a.slot);
    if (!field(r, LIFE)) return refusal::none;
    if (field(r, LIFE) != a.incarnation) return refusal::stale_actor;
    if (!cancel_pending(b, a, r)) return refusal::unsupported;
    clear_owned(r);
    return refusal::none;
}
bool state::owns_ticket(const ticket &owner) const {
    if (!valid()) return false;
    const dimensions d = shape();
    const actor_key  a = owner.actor;
    return a.player < d.players && a.slot < d.units && a.incarnation &&
           matches(bytes_, row(bytes_, d, a.player, a.slot), owner);
}
bool state::pending_application_allowed(const backend &b, const ticket &owner) {
    if (!owns_ticket(owner)) return false;
    const dimensions d = shape();
    const actor_key  a = owner.actor;
    uint8_t         *r = row(bytes_, d, a.player, a.slot);
    if (!(field(r, FLAGS) & EXTERNAL_PENDING) || status(r) != phase::awaiting_application) return false;
    actor_view actor{};
    if (!read_actor(b, a, d, actor) || value(actor.family) != field(r, FAMILY)) return false;
    uint8_t    *n    = node(r, field(r, CURSOR));
    const entry task = task_of(n);
    if (!is_attack(task.kind)) return true;
    if (get(n + 40) == value(objective::unresolved)) {
        objective obj;
        if (!target_objective(b, a, task, obj)) return false;
        put(n + 40, value(obj));
    }
    return get(n + 40) == value(objective::unresolved);
}
refusal state::validate_excursion_application(const backend &b, actor_key a, uint32_t epoch, target_key target) const {
    if (!valid()) return refusal::malformed;
    const dimensions d = shape();
    if (a.player >= d.players || a.slot >= d.units || !a.incarnation) return refusal::stale_actor;
    const uint8_t *r = row(bytes_, d, a.player, a.slot);
    if (!field(r, COUNT) || field(r, LIFE) != a.incarnation || !epoch || field(r, EPOCH) != epoch)
        return refusal::stale_ticket;
    const uint32_t flags = field(r, FLAGS);
    if (status(r) != phase::executing || !(flags & APPLIED) || (flags & (EXCURSION | RETURN_MODE | EXTERNAL_PENDING)) ||
        static_cast<action>(get(node(r, field(r, CURSOR)))) != action::patrol) return refusal::unsupported;
    if (!identity_available(r)) return refusal::exhausted_identity;
    actor_view actor{};
    if (!read_actor(b, a, d, actor) || value(actor.family) != field(r, FAMILY)) return refusal::stale_actor;
    if (target.kind == object_kind::none || !target_valid(target, d)) return refusal::malformed;
    return b.read_target && b.read_target(b.context, a, target) == target_status::alive_hostile ? refusal::none : refusal::unavailable_target;
}
target_status state::excursion_target_status(const backend &b, actor_key a) const {
    if (!valid()) return target_status::unavailable;
    const dimensions d = shape();
    if (a.player >= d.players || a.slot >= d.units || !a.incarnation) return target_status::unavailable;
    const uint8_t *r = row(bytes_, d, a.player, a.slot);
    if (field(r, LIFE) != a.incarnation || !field(r, COUNT) || !(field(r, FLAGS) & EXCURSION) ||
        status(r) != phase::excursion) return target_status::unavailable;
    if (field(r, EX_OBJECTIVE) == value(objective::destroyed_confirmed)) return target_status::destroyed_confirmed;
    if (field(r, EX_OBJECTIVE) == value(objective::unavailable)) return target_status::unavailable;
    actor_view actor{};
    if (!read_actor(b, a, d, actor) || !b.read_target) return target_status::unavailable;
    return b.read_target(b.context, a, excursion_target_of(r));
}
control state::guard_primary(const backend &b, actor_key a, bool &may_access) {
    may_access = true;
    if (!valid()) return control::unchanged;
    const dimensions d = shape();
    if (a.player >= d.players || a.slot >= d.units || !a.incarnation) return control::unchanged;
    uint8_t *r = row(bytes_, d, a.player, a.slot);
    if (field(r, LIFE) != a.incarnation || (!field(r, COUNT) && !(field(r, FLAGS) & PENDING_CLEANUP)))
        return control::unchanged; // Non-owning binding observations never poll/clean ordinary work.
    actor_view actor{};
    if (!read_actor(b, a, d, actor)) {
        may_access = false;
        return control::unchanged;
    }
    return service_primary(bytes_, r, b, a, actor, may_access);
}
void state::note_primary_detached(actor_key a) {
    if (!valid()) return;
    const dimensions d = shape();
    if (a.player >= d.players || a.slot >= d.units || !a.incarnation) return;
    uint8_t *r = row(bytes_, d, a.player, a.slot);
    if (field(r, LIFE) == a.incarnation && (field(r, COUNT) || (field(r, FLAGS) & PENDING_CLEANUP)))
        field(r, FLAGS, field(r, FLAGS) | PENDING_CLEANUP);
}
bool state::primary_cleanup_pending(actor_key a) const {
    if (!valid()) return false;
    const dimensions d = shape();
    if (a.player >= d.players || a.slot >= d.units || !a.incarnation) return false;
    const uint8_t *r = row(bytes_, d, a.player, a.slot);
    return field(r, LIFE) == a.incarnation && (field(r, FLAGS) & PENDING_CLEANUP) != 0;
}
bool state::reserve_physical_epoch(actor_key a, uint32_t &result) {
    result = 0;
    if (!valid()) return false;
    const dimensions d = shape();
    if (!d.width || a.player >= d.players || !a.slot || a.slot >= d.units || !a.incarnation) return false;
    uint8_t *r = row(bytes_, d, a.player, a.slot);
    if ((field(r, LIFE) && field(r, LIFE) != a.incarnation) || field(r, COUNT) ||
        field(r, EPOCH) == UINT32_MAX) return false;
    const auto reservation = reservation_of(r);
    if (reservation.phase == reservation_phase::pending || reservation.phase == reservation_phase::handoff) return false;
    field(r, LIFE, a.incarnation);
    result = field(r, EPOCH) + 1;
    field(r, EPOCH, result);
    return true;
}
uint32_t state::execution_epoch(actor_key a) const {
    if (!valid()) return 0;
    const dimensions d = shape();
    if (a.player >= d.players || a.slot >= d.units || !a.incarnation) return 0;
    const uint8_t *r = row(bytes_, d, a.player, a.slot);
    return field(r, LIFE) == a.incarnation ? field(r, EPOCH) : 0;
}
control state::handler_control(const backend &b, actor_key a) const {
    if (!valid()) return control::unchanged;
    const dimensions d = shape();
    if (a.player >= d.players || a.slot >= d.units || !a.incarnation) return control::unchanged;
    const uint8_t *r = row(bytes_, d, a.player, a.slot);
    if (field(r, LIFE) != a.incarnation) return control::unchanged;
    const phase    p     = status(r);
    const uint32_t flags = field(r, FLAGS);
    if (flags & PENDING_CLEANUP) {
        actor_view actor{};
        return read_actor(b, a, d, actor) && actor.safe_to_apply ? control::hold : control::unchanged;
    }
    if (!field(r, COUNT)) return control::unchanged;
    if (p == phase::blocked) return control::hold;
    if (p == phase::ready_next)
        return field(r, CYCLE) != NO_CYCLE || field(r, COUNT) > 1 ? control::hold : control::unchanged;
    // A retained primary must run to its actual F/dequeue; it is not an engine application.
    if (flags & EXTERNAL_PENDING) return control::unchanged;
    if ((p == phase::awaiting_application || p == phase::returning_anchor) && !(flags & APPLIED)) {
        actor_view actor{};
        if (!read_actor(b, a, d, actor)) return control::unchanged;
        if (actor.family == executor::direction_plane && task_of(node(r, field(r, CURSOR))).kind == action::patrol &&
            actor.path_slot != UINT32_MAX) return control::unchanged;
        return actor.safe_to_apply ? control::hold : control::unchanged;
    }
    return control::unchanged;
}
bool state::read_route(actor_key a, route_view &out) const {
    if (!valid()) return false;
    const dimensions d = shape();
    if (a.player >= d.players || a.slot >= d.units || !a.incarnation) return false;
    const uint8_t *r = row(bytes_, d, a.player, a.slot);
    if (field(r, LIFE) != a.incarnation || !field(r, COUNT)) return false;
    const uint32_t flags    = field(r, FLAGS);
    out                     = {ticket_of(bytes_, r, a), status(r), static_cast<failure>(field(r, FAILURE)), field(r, COUNT), field(r, CYCLE), field(r, CURSOR), {field(r, ASSIGNED_X), field(r, ASSIGNED_Y)}, field(r, ASSIGNMENT_VALID) != 0, static_cast<assignment_reason>(field(r, PLACEMENT)), field(r, CIRCUIT_CURSOR), {field(r, CIRCUIT_X), field(r, CIRCUIT_Y)}, field(r, PROGRESS) != 0, {field(r, IMPLICIT_X), field(r, IMPLICIT_Y)}, {field(r, EXCURSION_X), field(r, EXCURSION_Y)}, (flags & HAS_IMPLICIT) != 0, (flags & EXCURSION) != 0, (flags & IMPLICIT_ACTIVE) != 0, static_cast<executor>(field(r, FAMILY)), (flags & EXTERNAL_PENDING) != 0, field(r, PATH_SLOT), field(r, EPOCH), excursion_target_of(r), static_cast<objective>(field(r, EX_OBJECTIVE)), (flags & PLANNER_SUCCEEDED) != 0};
    out.cohort_origin       = {field(r, ORIGIN_X), field(r, ORIGIN_Y)};
    out.cohort_sequence     = root_sequence(r);
    out.origin_state        = field(r, ORIGIN_STATE);
    out.origin_resolved     = resolved_origin(r);
    out.legacy_local_origin = legacy_origin(r);
    out.guided_flight       = field(r, AIR_MODE) != 0;
    out.flight_mode         = field(r, AIR_MODE);
    out.curve_index         = field(r, CURVE_INDEX);
    out.chunk_ack           = field(r, CHUNK_ACK);
    out.geometry_revision   = field(r, CHUNK_REVISION);
    out.requested_revision  = field(r, SPEC_REVISION);
    out.geometry_count      = field(r, AIR_SPEC_COUNT);
    if (field(r, AIR_MODE) && field(r, AIR_SLOT)) out.path_slot = field(r, AIR_SLOT) - 1;
    return true;
}
bool state::read_entry(actor_key a, uint32_t logical, node_view &out) const {
    if (!valid()) return false;
    const dimensions d = shape();
    if (a.player >= d.players || a.slot >= d.units || !a.incarnation) return false;
    const uint8_t *r = row(bytes_, d, a.player, a.slot);
    if (field(r, LIFE) != a.incarnation || logical >= field(r, COUNT)) return false;
    const uint8_t *n = node(r, logical);
    out              = {task_of(n), get(n + 36), static_cast<objective>(get(n + 40))};
    return true;
}
bool state::read_reservation(actor_key a, primary_reservation &out) const {
    if (!valid()) return false;
    const dimensions d = shape();
    if (a.player >= d.players || a.slot >= d.units || !a.incarnation) return false;
    const uint8_t *r = row(bytes_, d, a.player, a.slot);
    if (field(r, LIFE) != a.incarnation) return false;
    out = reservation_of(r);
    return true;
}
bool state::write_reservation(actor_key a, const primary_reservation &v) {
    if (!valid()) return false;
    const dimensions d = shape();
    if (!d.width || a.player >= d.players || a.slot >= d.units || !a.incarnation) return false;
    uint8_t *r = row(bytes_, d, a.player, a.slot);
    if (field(r, LIFE) && field(r, LIFE) != a.incarnation) return false;
    if (!reservation_valid(v, d, field(r, EPOCH), false)) return false;
    // This records an ACTUAL binding/clear; it never creates route counters or an epoch.
    field(r, LIFE, a.incarnation);
    write_reservation_raw(r, v);
    return true;
}
void state::clear_reservation(actor_key a) {
    if (!valid()) return;
    const dimensions d = shape();
    if (a.player >= d.players || a.slot >= d.units || !a.incarnation) return;
    uint8_t *r = row(bytes_, d, a.player, a.slot);
    if (field(r, LIFE) == a.incarnation) std::memset(r + RESERVATION, 0, 48);
}
bool state::checkpoint_ready() const {
    if (!valid()) return false;
    const dimensions d = shape();
    for (uint32_t p = 0; p != d.players; ++p)
        for (uint32_t s = 0; s != d.units; ++s)
            if (get(row(bytes_, d, p, s) + RESERVATION) == value(reservation_phase::handoff)) return false;
    return true;
}
bool state::validate_lifetimes(void *context, bool (*match)(void *, object_kind, actor_key)) const {
    if (!valid() || !match) return false;
    const dimensions d = shape();
    for (uint32_t player = 0; player != d.players; ++player)
        for (uint32_t slot = 0; slot != d.units; ++slot) {
            const uint8_t *r = row(bytes_, d, player, slot);
            if (field(r, LIFE) && !match(context, object_kind::unit, {player, slot, field(r, LIFE)})) return false;
            for (uint32_t i = 0; i != field(r, COUNT); ++i) {
                const uint8_t *n    = node(r, i);
                const entry    task = task_of(n);
                if (is_attack(task.kind) && get(n + 40) == value(objective::unresolved) &&
                    !match(context, task.target.kind, task.target.object)) return false;
            }
            if ((field(r, FLAGS) & EXCURSION) && field(r, EX_OBJECTIVE) == value(objective::unresolved)) {
                const target_key target = excursion_target_of(r);
                if (!match(context, target.kind, target.object)) return false;
            }
            const primary_reservation v = reservation_of(r);
            // An ordinary non-owning bound annotation may be historically stale until native
            // mode1 runs. It must not create cleanup ownership or make ordinary saves unusable.
            if (v.phase == reservation_phase::pending ||
                (v.phase == reservation_phase::bound && field(r, COUNT) && v.execution_epoch)) {
                if (!match(context, v.target.kind, v.target.object)) return false;
                if (v.prior_target.kind != object_kind::none && !match(context, v.prior_target.kind, v.prior_target.object))
                    return false;
            }
        }
    return true;
}
uint32_t state::encoded_bytes() const { return valid() ? bytes_count_ + ENVELOPE_BYTES : 0; }
bool     state::encode(void *out, uint32_t capacity) const {
    if (!checkpoint_ready() || !out || capacity < encoded_bytes()) return false;
    auto *p = static_cast<uint8_t *>(out);
    // Encoding requires disjoint output: never invalidate the live raw-region binding.
    const uintptr_t dst = reinterpret_cast<uintptr_t>(p), src = reinterpret_cast<uintptr_t>(bytes_);
    if (dst <= src ? src - dst < encoded_bytes() : dst - src < bytes_count_) return false;
    std::memcpy(p + ENVELOPE_BYTES, bytes_, bytes_count_);
    put(p, SAVE_MAGIC);
    put(p + 4, SCHEMA);
    put(p + 8, bytes_count_);
    put(p + 12, crc32(p + ENVELOPE_BYTES, bytes_count_));
    return true;
}
bool state::inspect(const void *encoded, uint32_t n, dimensions &d, world_key &w) {
    const auto *p = static_cast<const uint8_t *>(encoded);
    if (!p || n < ENVELOPE_BYTES + LEGACY_HEADER_BYTES || get(p) != SAVE_MAGIC ||
        (get(p + 4) != 1 && get(p + 4) != SCHEMA) || get(p + 8) != n - ENVELOPE_BYTES ||
        get(p + 12) != crc32(p + ENVELOPE_BYTES, n - ENVELOPE_BYTES)) return false;
    const uint8_t *raw = p + ENVELOPE_BYTES;
    if (get(raw + 4) != get(p + 4) ||
        !(get(p + 4) == 1 ? legacy_region_valid(raw, n - ENVELOPE_BYTES)
                          : region_valid(raw, n - ENVELOPE_BYTES))) return false;
    d = shape_of(raw);
    w = world_of(raw);
    return true;
}
bool state::validate(const void *encoded, uint32_t n, dimensions expected, world_key w) {
    if (!required_bytes(expected)) return false;
    dimensions actual{};
    world_key  actual_world{};
    return inspect(encoded, n, actual, actual_world) && same(actual, expected) && same(actual_world, w);
}
bool state::import(const void *encoded, uint32_t n) {
    if (!checkpoint_ready() || !validate(encoded, n, shape(), world())) return false;
    const auto *p = static_cast<const uint8_t *>(encoded);
    if (get(p + 4) == SCHEMA) {
        std::memmove(bytes_, p + ENVELOPE_BYTES, bytes_count_);
        return true;
    }
    // Expansion is metadata-only and infallible after complete semantic validation.
    // Disjoint source is required because publication grows both header and every row.
    if (!disjoint(bytes_, bytes_count_, p, n)) return false;
    migrate_legacy(bytes_, p + ENVELOPE_BYTES, shape(), bytes_count_);
    return true;
}

bool encode_edit(const edit &e, int32_t (&a)[13]) {
    // Output is untouched on refusal, including out-of-range coordinates (never masked).
    const dimensions max_shape{8, 65535, 256, 256, 65535};
    if (!e.world.incarnation || e.world.planet >= 32 || e.actor.player >= 8 || e.actor.slot >= 65535 ||
        !e.actor.incarnation || (e.operation != edit_kind::replace && e.operation != edit_kind::append && e.operation != edit_kind::cancel)) return false;
    if (e.operation == edit_kind::cancel ? !cancel_valid(e.task) : !entry_valid(e.task, max_shape, true)) return false;
    a[0]  = static_cast<int32_t>((e.legacy_sequence ? LEGACY_SCHEMA : EDIT_SCHEMA) | value(e.operation) << 8 | value(e.task.kind) << 16);
    a[1]  = static_cast<int32_t>(e.actor.incarnation);
    a[2]  = static_cast<int32_t>(e.world.system);
    a[3]  = static_cast<int32_t>(e.world.planet);
    a[4]  = static_cast<int32_t>(e.world.incarnation);
    a[5]  = static_cast<int32_t>(e.task.requested.x | e.task.requested.y << 16);
    a[6]  = static_cast<int32_t>(value(e.task.target.kind) | e.task.target.object.player << 8);
    a[7]  = static_cast<int32_t>(e.task.target.object.slot);
    a[8]  = static_cast<int32_t>(e.task.target.object.incarnation);
    a[9]  = static_cast<int32_t>(e.task.group);
    a[10] = static_cast<int32_t>(e.task.command_sequence);
    a[11] = 0;
    a[12] = 0;
    return true;
}
bool decode_edit(uint16_t owner, uint32_t slot, const int32_t (&a)[13], edit &out) {
    const uint32_t tag = uint32_t(a[0]), coords = uint32_t(a[5]), target = uint32_t(a[6]);
    if ((owner & ~0x7u) || slot >= 65535 || ((tag & 0xffu) != LEGACY_SCHEMA && (tag & 0xffu) != EDIT_SCHEMA) || (tag & 0xff000000u) ||
        (coords & 0xff00ff00u) || (target & 0xfffff800u) || a[11] || a[12]) return false;
    const edit candidate{{uint32_t(a[2]), uint32_t(a[3]), uint32_t(a[4])},
                         {owner, slot, uint32_t(a[1])},
                         static_cast<edit_kind>((tag >> 8) & 255u),
                         {static_cast<action>((tag >> 16) & 255u), {coords & 65535u, coords >> 16}, {static_cast<object_kind>(target & 255u), {(target >> 8) & 7u, uint32_t(a[7]), uint32_t(a[8])}}, uint32_t(a[9]), uint32_t(a[10])},
                         (tag & 0xffu) == LEGACY_SCHEMA};
    int32_t    canonical[13];
    if (!encode_edit(candidate, canonical)) return false;
    for (uint32_t i = 0; i != 13; ++i)
        if (canonical[i] != a[i]) return false;
    out = candidate;
    return true;
}
bool independent(uint16_t ao, uint16_t ac, uint16_t bo, uint16_t bc) {
    return ((ao & 0xf0u) == 0 && ac == COMMAND) || ((bo & 0xf0u) == 0 && bc == COMMAND);
}

bool campaign_state::validate(const void *memory, uint32_t n) {
    const auto *p = static_cast<const uint8_t *>(memory);
    if (!p || n != BYTES || get(p) != CAMPAIGN_MAGIC || get(p + 4) != CAMPAIGN_SCHEMA || get(p + 12) || !zero(p + 144, 16)) return false;
    const uint32_t last = get(p + 8);
    for (uint32_t planet = 0; planet != 32; ++planet) {
        const uint32_t id = get(p + 16 + 4 * planet);
        if (id > last) return false;
        if (id)
            for (uint32_t before = 0; before != planet; ++before)
                if (get(p + 16 + 4 * before) == id) return false;
    }
    return true;
}
bool campaign_state::initialize(void *memory, uint32_t n) {
    if (!memory || n != BYTES) return false;
    auto *p = static_cast<uint8_t *>(memory);
    std::memset(p, 0, BYTES);
    put(p, CAMPAIGN_MAGIC);
    put(p + 4, CAMPAIGN_SCHEMA);
    bytes_ = p;
    return true;
}
bool campaign_state::bind(void *memory, uint32_t n) {
    if (!validate(memory, n)) return false;
    bytes_ = static_cast<uint8_t *>(memory);
    return true;
}
bool           campaign_state::valid() const { return bytes_ && get(bytes_) == CAMPAIGN_MAGIC && get(bytes_ + 4) == CAMPAIGN_SCHEMA; }
const uint8_t *campaign_state::data() const { return bytes_; }
uint8_t       *campaign_state::data() { return bytes_; }
uint32_t       campaign_state::size() const { return bytes_ ? BYTES : 0; }
uint32_t       campaign_state::last_world_inc() const { return valid() ? get(bytes_ + 8) : 0; }
uint32_t       campaign_state::world_inc(uint32_t planet) const { return valid() && planet < 32 ? get(bytes_ + 16 + 4 * planet) : 0; }
bool           campaign_state::fresh_world(uint32_t planet, uint32_t &out) {
    if (!valid() || planet >= 32 || get(bytes_ + 8) == UINT32_MAX) return false;
    const uint32_t next = get(bytes_ + 8) + 1;
    put(bytes_ + 8, next);
    put(bytes_ + 16 + 4 * planet, next);
    out = next;
    return true;
}
bool campaign_state::ensure_known(uint32_t planet, uint32_t &out) {
    if (!valid() || planet >= 32) return false;
    const uint32_t existing = world_inc(planet);
    if (existing) {
        out = existing;
        return true;
    }
    return fresh_world(planet, out);
}
bool campaign_state::seed_legacy(uint32_t mask, uint32_t current, uint32_t &out) {
    if (!valid() || current >= 32 || get(bytes_ + 8) || !zero(bytes_ + 16, 128)) return false;
    mask |= uint32_t(1) << current;
    uint32_t next = 0;
    for (uint32_t planet = 0; planet != 32; ++planet)
        if (mask & (uint32_t(1) << planet)) put(bytes_ + 16 + 4 * planet, ++next);
    put(bytes_ + 8, next);
    out = get(bytes_ + 16 + 4 * current);
    return true;
}
bool campaign_state::import(const void *memory, uint32_t n) {
    if (!valid() || !validate(memory, n)) return false;
    std::memmove(bytes_, memory, BYTES);
    return true;
}
} // namespace mh::order_tasks
