#include "include/mh_storage_policy.h"
#include <cstring>
#include <limits>

namespace mh::storage_policy {
namespace {
constexpr uint32_t REGION_MAGIC = 0x31525053u; // SPR1
constexpr uint32_t SAVE_MAGIC   = 0x4c4f5053u; // SPOL
constexpr uint32_t LIVE = 1, RALLY = 2, AUTO = 4, PENDING = 2;
uint32_t           get(const uint8_t *p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
void put(uint8_t *p, uint32_t x) {
    for (uint32_t i = 0; i != 4; ++i) p[i] = uint8_t(x >> (8 * i));
}
uint32_t crc32(const uint8_t *p, uint32_t n) {
    uint32_t crc = ~0u;
    for (uint32_t i = 0; i != n; ++i) {
        crc ^= p[i];
        for (uint32_t b = 0; b != 8; ++b) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}
bool same(dimensions a, dimensions b) {
    return a.players == b.players && a.buildings == b.buildings && a.units == b.units &&
           a.width == b.width && a.height == b.height;
}
dimensions shape_of(const uint8_t *p) {
    return {get(p + 8), get(p + 12), get(p + 16), get(p + 20), get(p + 24)};
}
bool valid_region(const uint8_t *p, uint32_t n) {
    if (!p || n < HEADER_BYTES || get(p) != REGION_MAGIC || get(p + 4) != SCHEMA) return false;
    const dimensions d = shape_of(p);
    if (state::required_bytes(d) != n || get(p + 28) != n) return false;
    const uint32_t bcount = d.players * d.buildings, ucount = d.players * d.units;
    const uint8_t *buildings = p + HEADER_BYTES;
    for (uint32_t i = 0; i != bcount; ++i) {
        const uint8_t *r     = buildings + i * BUILDING_BYTES;
        const uint32_t flags = get(r + 4);
        if (!d.width && flags) return false;
        if (flags & ~(LIVE | RALLY | AUTO)) return false;
        if (!(flags & LIVE)) {
            if (flags || get(r + 8) || get(r + 12)) return false;
        } else if (!get(r)) return false;
        if (flags & RALLY) {
            if (get(r + 8) >= d.width || get(r + 12) >= d.height) return false;
        } else if (get(r + 8) || get(r + 12)) return false;
    }
    const uint8_t *units = buildings + bcount * BUILDING_BYTES;
    for (uint32_t i = 0; i != ucount; ++i) {
        const uint8_t *r     = units + i * UNIT_BYTES;
        const uint32_t flags = get(r + 4);
        if (flags & ~(LIVE | PENDING)) return false;
        if (!d.width && flags) return false;
        if ((flags & LIVE) && !get(r)) return false;
        if (!(flags & LIVE) && flags) return false;
        if (!(flags & PENDING)) {
            if (get(r + 8) || get(r + 12)) return false;
        } else {
            if (!(flags & LIVE) || get(r + 8) >= d.buildings || !get(r + 12)) return false;
            const uint8_t *b = buildings + ((i / d.units) * d.buildings + get(r + 8)) * BUILDING_BYTES;
            if (get(b) != get(r + 12) || (get(b + 4) & (LIVE | AUTO)) != (LIVE | AUTO)) return false;
        }
    }
    return true;
}
} // namespace

uint32_t state::required_bytes(dimensions d) {
    if (!d.players && !d.buildings && !d.units && !d.width && !d.height) return HEADER_BYTES;
    if (!d.players || d.players > 8 || !d.buildings || d.buildings > 65535 || !d.units ||
        d.units > 65535 || d.width > 256 || d.height > 256 || ((!d.width) != (!d.height))) return 0;
    const uint64_t n = HEADER_BYTES + uint64_t(d.players) *
                                          (uint64_t(d.buildings) * BUILDING_BYTES + uint64_t(d.units) * UNIT_BYTES);
    return n <= std::numeric_limits<uint32_t>::max() - ENVELOPE_BYTES ? uint32_t(n) : 0;
}
bool state::initialize(void *memory, uint32_t bytes, dimensions d) {
    const uint32_t n = required_bytes(d);
    if (!memory || !n || bytes != n) return false;
    bytes_ = static_cast<uint8_t *>(memory);
    size_  = n;
    std::memset(bytes_, 0, n);
    put(bytes_, REGION_MAGIC);
    put(bytes_ + 4, SCHEMA);
    put(bytes_ + 8, d.players);
    put(bytes_ + 12, d.buildings);
    put(bytes_ + 16, d.units);
    put(bytes_ + 20, d.width);
    put(bytes_ + 24, d.height);
    put(bytes_ + 28, n);
    return true;
}
bool state::bind(void *memory, uint32_t bytes) {
    if (!valid_region(static_cast<const uint8_t *>(memory), bytes)) return false;
    bytes_ = static_cast<uint8_t *>(memory);
    size_  = bytes;
    return true;
}
dimensions state::shape() const { return bytes_ && size_ >= HEADER_BYTES ? shape_of(bytes_) : dimensions{}; }
bool       state::valid() const { return valid_region(bytes_, size_); }
bool       state::reset() { return bytes_ && initialize(bytes_, size_, shape()); }
uint8_t   *state::building(uint32_t p, uint32_t s) const {
    const dimensions d = shape();
    return p < d.players && s < d.buildings ? bytes_ + HEADER_BYTES + (p * d.buildings + s) * BUILDING_BYTES : nullptr;
}
uint8_t *state::unit(uint32_t p, uint32_t s) const {
    const dimensions d = shape();
    return p < d.players && s < d.units ? bytes_ + HEADER_BYTES + d.players * d.buildings * BUILDING_BYTES +
                                              (p * d.units + s) * UNIT_BYTES
                                        : nullptr;
}
bool state::matches(uint8_t *r, identity id) const {
    return r && id.incarnation && get(r) == id.incarnation && (get(r + 4) & LIVE);
}
identity state::building_identity(uint32_t p, uint32_t s) const {
    uint8_t *r = building(p, s);
    return {p, s, r && (get(r + 4) & LIVE) ? get(r) : 0};
}
identity state::unit_identity(uint32_t p, uint32_t s) const {
    uint8_t *r = unit(p, s);
    return {p, s, r && (get(r + 4) & LIVE) ? get(r) : 0};
}
bool state::birth_building(uint32_t p, uint32_t s, identity &out) {
    uint8_t *r = building(p, s);
    if (!shape().width || !r) return false;
    if (get(r) == ~0u) {
        retire_building({p, s, ~0u});
        return false;
    }
    cancel_receiver({p, s, get(r)});
    const uint32_t incarnation = get(r) + 1;
    std::memset(r, 0, BUILDING_BYTES);
    put(r, incarnation);
    put(r + 4, LIVE);
    out = {p, s, incarnation};
    return true;
}
bool state::birth_unit(uint32_t p, uint32_t s, identity &out) {
    uint8_t *r = unit(p, s);
    if (!shape().width || !r) return false;
    if (get(r) == ~0u) {
        retire_unit({p, s, ~0u});
        return false;
    }
    const uint32_t incarnation = get(r) + 1;
    std::memset(r, 0, UNIT_BYTES);
    put(r, incarnation);
    put(r + 4, LIVE);
    out = {p, s, incarnation};
    return true;
}
void state::cancel_receiver(identity receiver) {
    const dimensions d = shape();
    if (receiver.player >= d.players) return;
    for (uint32_t s = 0; s != d.units; ++s) {
        uint8_t *r = unit(receiver.player, s);
        if ((get(r + 4) & PENDING) && get(r + 8) == receiver.slot && get(r + 12) == receiver.incarnation) {
            put(r + 4, get(r + 4) & ~PENDING);
            put(r + 8, 0);
            put(r + 12, 0);
        }
    }
}
void state::retire_building(identity b) {
    uint8_t *r = building(b.player, b.slot);
    if (!matches(r, b)) return;
    cancel_receiver(b);
    std::memset(r + 4, 0, BUILDING_BYTES - 4);
}
void state::retire_unit(identity u) {
    uint8_t *r = unit(u.player, u.slot);
    if (matches(r, u)) std::memset(r + 4, 0, UNIT_BYTES - 4);
}
bool state::apply_command(uint32_t actor, const command &c) {
    uint8_t *r = building(c.building.player, c.building.slot);
    if (actor != c.building.player || !matches(r, c.building)) return false;
    uint32_t flags = get(r + 4);
    switch (c.kind) {
        case command_kind::set_rally:
            if (c.x >= shape().width || c.y >= shape().height) return false;
            flags |= RALLY;
            put(r + 8, c.x);
            put(r + 12, c.y);
            break;
        case command_kind::clear_rally:
            if (c.x || c.y) return false;
            flags &= ~RALLY;
            put(r + 8, 0);
            put(r + 12, 0);
            break;
        case command_kind::set_auto:
            if (c.x > 1 || c.y) return false;
            if (c.x) flags |= AUTO;
            else {
                flags &= ~AUTO;
                cancel_receiver(c.building);
            }
            break;
        default: return false;
    }
    put(r + 4, flags);
    return true;
}
bool state::qualify(identity u, identity b, event_kind event) {
    if (event != event_kind::production_inserted && event != event_kind::repair_reached_full) return false;
    uint8_t *ur = unit(u.player, u.slot), *br = building(b.player, b.slot);
    if (u.player != b.player || !matches(ur, u) || !matches(br, b) || !(get(br + 4) & AUTO)) return false;
    put(ur + 4, LIVE | PENDING);
    put(ur + 8, b.slot);
    put(ur + 12, b.incarnation);
    return true;
}
void state::cancel_pending(identity u) {
    uint8_t *r = unit(u.player, u.slot);
    if (!matches(r, u)) return;
    put(r + 4, LIVE);
    put(r + 8, 0);
    put(r + 12, 0);
}
bool state::pending(identity u, identity &b) const {
    uint8_t *r = unit(u.player, u.slot);
    if (!matches(r, u) || !(get(r + 4) & PENDING)) return false;
    b = {u.player, get(r + 8), get(r + 12)};
    return true;
}
rally_view state::read_rally(uint32_t p, uint32_t s) const {
    uint8_t *r = building(p, s);
    if (!r || !(get(r + 4) & LIVE) || !get(r)) return {};
    const uint32_t flags = get(r + 4);
    rally_view     v{get(r), get(r + 8), get(r + 12), (flags & RALLY) != 0, (flags & AUTO) != 0};
    if (flags & ~(LIVE | RALLY | AUTO) || (v.enabled && (v.x >= shape().width || v.y >= shape().height))) return {};
    return v;
}
void state::retry(void *context, launch_fn launch) {
    if (!launch) return;
    const dimensions d = shape();
    for (uint32_t p = 0; p != d.players; ++p) {
        uint8_t *r = bytes_ + HEADER_BYTES + d.players * d.buildings * BUILDING_BYTES + p * d.units * UNIT_BYTES;
        for (uint32_t s = 0; s != d.units; ++s, r += UNIT_BYTES) {
            if ((get(r + 4) & (LIVE | PENDING)) != (LIVE | PENDING) || !get(r)) continue;
            const identity   u{p, s, get(r)}, b{p, get(r + 8), get(r + 12)};
            const rally_view v = read_rally(p, b.slot);
            if (v.incarnation != b.incarnation || !v.automatic) {
                cancel_pending(u);
                continue;
            }
            const launch_result result = launch(context, u, b, v);
            if (result != launch_result::deferred) cancel_pending(u);
        }
    }
}
uint32_t state::encoded_bytes() const { return bytes_ ? size_ + ENVELOPE_BYTES : 0; }
bool     state::encode(void *out, uint32_t capacity) const {
    if (!out || capacity < encoded_bytes() || !valid()) return false;
    auto *p = static_cast<uint8_t *>(out);
    std::memmove(p + ENVELOPE_BYTES, bytes_, size_);
    put(p, SAVE_MAGIC);
    put(p + 4, SCHEMA);
    put(p + 8, size_);
    put(p + 12, crc32(p + ENVELOPE_BYTES, size_));
    return true;
}
bool state::validate(const void *encoded, uint32_t n, dimensions expected) {
    const auto    *p    = static_cast<const uint8_t *>(encoded);
    const uint32_t size = required_bytes(expected);
    return p && size && n == size + ENVELOPE_BYTES && get(p) == SAVE_MAGIC && get(p + 4) == SCHEMA &&
           get(p + 8) == size && get(p + 12) == crc32(p + ENVELOPE_BYTES, size) &&
           valid_region(p + ENVELOPE_BYTES, size) && same(shape_of(p + ENVELOPE_BYTES), expected);
}
bool state::import(const void *encoded, uint32_t n) {
    if (!bytes_ || !validate(encoded, n, shape())) return false;
    std::memmove(bytes_, static_cast<const uint8_t *>(encoded) + ENVELOPE_BYTES, size_);
    return true;
}
void encode_command(const command &c, int32_t (&a)[13]) {
    for (auto &v : a) v = 0;
    a[0] = static_cast<int32_t>(c.kind);
    a[1] = static_cast<int32_t>(c.building.incarnation);
    a[2] = static_cast<int32_t>(c.x);
    a[3] = static_cast<int32_t>(c.y);
}
bool decode_command(uint32_t actor, uint32_t slot, const int32_t (&a)[13], command &out) {
    if (actor >= 8 || !uint32_t(a[1]) || a[0] < 1 || a[0] > 3) return false;
    for (uint32_t i = 4; i != 13; ++i)
        if (a[i]) return false;
    const auto kind = static_cast<command_kind>(a[0]);
    if (kind == command_kind::clear_rally && (a[2] || a[3])) return false;
    if (kind == command_kind::set_auto && (uint32_t(a[2]) > 1 || a[3])) return false;
    out = {{actor, slot, uint32_t(a[1])}, kind, uint32_t(a[2]), uint32_t(a[3])};
    return true;
}
bool independent_settings(uint16_t ao, uint16_t ac, uint16_t bo, uint16_t bc, uint16_t rc, uint16_t tc) {
    const bool a = (ao & 0xf0u) == 0 && (ac == rc || ac == tc);
    const bool b = (bo & 0xf0u) == 0 && (bc == rc || bc == tc);
    return a || b;
}
int32_t dispatch(state &policy, const backend &live, const mh_storage_policy_request &q,
                 mh_storage_policy_result &result) {
    result          = {};
    const auto op   = static_cast<operation>(q.operation);
    uint32_t   used = 0;
    if (op == operation::consume_setting) used = 15;
    else if (op == operation::paid_repair || op == operation::map_extent) used = 2;
    for (uint32_t i = used; i != 16; ++i)
        if (q.args[i]) return 0;
    const uint32_t p = q.player, s = q.slot;
    identity       id{};
    switch (op) {
        case operation::birth_building:
            if (!live.building_exists) return 0;
            if (live.building_exists(live.context, p, s) && policy.birth_building(p, s, id))
                result.values[0] = id.incarnation;
            return 1;
        case operation::birth_unit:
            if (!live.unit_exists) return 0;
            if (live.unit_exists(live.context, p, s) && policy.birth_unit(p, s, id))
                result.values[0] = id.incarnation;
            return 1;
        case operation::retire_building: policy.retire_building(policy.building_identity(p, s)); return 1;
        case operation::retire_unit: policy.retire_unit(policy.unit_identity(p, s)); return 1;
        case operation::cancel: policy.cancel_pending(policy.unit_identity(p, s)); return 1;
        case operation::production:
        case operation::paid_repair: {
            if (!live.resolve_receiver) return 0;
            uint32_t       bslot = 0;
            const uint64_t bits  = uint64_t(q.args[0]) | (uint64_t(q.args[1]) << 32);
            double         before;
            std::memcpy(&before, &bits, sizeof(before));
            const bool repair = op == operation::paid_repair;
            if (live.resolve_receiver(live.context, p, s,
                                      repair ? receiver_reason::paid_repair : receiver_reason::production, before, bslot))
                result.values[0] = policy.qualify(policy.unit_identity(p, s), policy.building_identity(p, bslot),
                                                  repair ? event_kind::repair_reached_full : event_kind::production_inserted)
                                       ? 1u
                                       : 0u;
            return 1;
        }
        case operation::consume_setting: {
            if (p > 0xff || (p & 0xf0u) || (q.args[0] != RALLY_COMMAND && q.args[0] != AUTO_COMMAND)) return 0;
            if (q.args[1] != q.args[0]) return 1;
            if (!live.eligible_building) return 0;
            int32_t args[13];
            std::memcpy(args, q.args + 2, sizeof(args));
            command cmd{};
            if (decode_command(p, s, args, cmd) &&
                ((q.args[0] == AUTO_COMMAND) == (cmd.kind == command_kind::set_auto)) &&
                live.eligible_building(live.context, p, s))
                result.values[0] = policy.apply_command(p, cmd) ? 1u : 0u;
            return 1;
        }
        case operation::retry:
            if (p || s || !live.launch) return 0;
            policy.retry(live.context, live.launch);
            return 1;
        case operation::read_rally: {
            if (!live.eligible_building) return 0;
            if (!live.eligible_building(live.context, p, s)) return 1;
            const rally_view r = policy.read_rally(p, s);
            result             = {{r.incarnation, r.x, r.y, r.enabled ? 1u : 0u, r.automatic ? 1u : 0u}};
            return 1;
        }
        case operation::manual_rally: {
            if (!live.resolve_receiver) return 0;
            uint32_t bslot = 0;
            if (!live.resolve_receiver(live.context, p, s, receiver_reason::manual, 0, bslot)) return 1;
            const rally_view r = policy.read_rally(p, bslot);
            result             = {{r.enabled ? 1u : 0u, r.x, r.y, 0, 0}};
            return 1;
        }
        case operation::reset_loaded_roster: {
            if (p || s || !live.building_exists || !live.unit_exists || !policy.shape().width) return 0;
            if (!policy.reset()) return 0;
            const dimensions d = policy.shape();
            for (uint32_t player = 0; player != d.players; ++player) {
                for (uint32_t slot = 1; slot != d.buildings; ++slot)
                    if (live.building_exists(live.context, player, slot) && !policy.birth_building(player, slot, id)) return 0;
                for (uint32_t slot = 1; slot != d.units; ++slot)
                    if (live.unit_exists(live.context, player, slot) && !policy.birth_unit(player, slot, id)) return 0;
            }
            return 1;
        }
        case operation::map_extent: {
            if (p || s || !policy.data()) return 0;
            dimensions d = policy.shape();
            d.width      = q.args[0];
            d.height     = q.args[1];
            if (!d.players || !state::required_bytes(d) || !d.width) return 0;
            if (d.width == policy.shape().width && d.height == policy.shape().height) return 1;
            return policy.initialize(policy.data(), policy.size(), d) ? 1 : 0;
        }
        default: return 0;
    }
}
} // namespace mh::storage_policy
