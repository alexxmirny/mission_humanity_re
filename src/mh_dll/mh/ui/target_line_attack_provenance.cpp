#include "ui/target_line_attack_provenance.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <utility>

#include "../../libmh/orders/order_index_codec.h"
#include "../../libmh/include/libmh_host_events.h"
#include "../../mh_common/include/mh_order_witness.h"
#include "addr/mh_structs.gen.h"
#include "caps/caps.h"
#include "caps/orders.h"
#include "seams/storage_policy.h"
#include "ui/target_line_intents.h"

// Defined by net_lockstep.cpp beside MH_TeamRel_Enabled. This reads the actual
// configured O1c knob, not the independent orders-cap toggle.
extern "C" int MH_OrderWideIndex_Enabled();

namespace mh::ui::selected_targets::detail {
namespace {

namespace orders = mh::caps::orders;
using order      = mh::game::mh_llm_strat_order;

// All semantic record bytes, excluding exec_time. STAGING's four header fields
// are normalized to the stored QUEUE/PENDING representation before comparison.
struct record_key {
    uint16_t unit_index;
    uint16_t owner_and_kind;
    int16_t  param0;
    uint16_t order_code;
    int32_t  args[13];
};

static_assert(sizeof(record_key) == 60);
static_assert(offsetof(record_key, args) == 8);
static_assert(sizeof(order) == orders::REC_BYTES);
static_assert(offsetof(order, unit_index) == 8);
static_assert(offsetof(order, args) == 16);
static_assert(sizeof(order) - offsetof(order, unit_index) == sizeof(record_key));
static_assert(sizeof(libmh_order_observation) == 88);
static_assert(offsetof(libmh_order_observation, unit_index) == 4);
static_assert(offsetof(libmh_order_observation, args) == 12);
static_assert(offsetof(libmh_order_observation, admission_id) -
                  offsetof(libmh_order_observation, unit_index) ==
              sizeof(record_key));

struct tag {
    record_key key{};
    uint64_t   token              = 0;
    uint32_t   actor_incarnation  = 0;
    uint32_t   target_incarnation = 0;
    uint32_t   entity             = 0;
    bool       secondary          = false;
    bool       poisoned           = true;
    bool       present            = false;
};

struct group_scope {
    uint64_t token              = 0;
    uint32_t entity             = 0;
    uint32_t target_incarnation = 0;
    unsigned depth              = 0;
};

std::unique_ptr<tag[]> tags;
unsigned               tag_capacity = 0;
unsigned               tag_count    = 0;
group_scope            scope;
bool                   codec_known = false;
bool                   codec_wide  = false;

bool primary_attack(uint16_t code) {
    return code >= 0x1a && code <= 0x1c;
}

bool secondary_attack(uint16_t code) {
    return code == 0x1d || code == 0x1e;
}

bool attack_shape(const record_key &key, bool &secondary) {
    const auto param0 = static_cast<uint16_t>(key.param0);
    secondary         = secondary_attack(param0) || secondary_attack(key.order_code);
    return secondary || primary_attack(param0) || primary_attack(key.order_code);
}

bool unit_kind(unsigned kind) {
    return kind == 0x20 || kind == 0x80 || kind == 0xa0;
}

bool valid_slot(mh::caps::arr array, unsigned player, unsigned slot) {
    const auto &geometry = mh::caps::g(array);
    return geometry.base && player < geometry.players &&
           slot != 0 && slot < geometry.limit;
}

uint32_t entity_for(unsigned player, unsigned slot, unsigned kind) {
    if (player > 15 || slot == 0 || slot > 0xffff) return 0;
    if (unit_kind(kind))
        return 0x80000000u | (player << 16) | slot;
    if (kind == 0x40)
        return 0x40000000u | (player << 16) | slot;
    return 0;
}

uint32_t incarnation_for(uint32_t entity) {
    const unsigned player = (entity >> 16) & 15;
    const unsigned slot   = entity & 0xffff;

    if (entity & 0x80000000u) {
        if (!valid_slot(mh::caps::UNITS, player, slot)) return 0;
        return mh::storage_policy::hosted::unit_identity(player, slot).incarnation;
    }
    if (entity & 0x40000000u) {
        if (!valid_slot(mh::caps::BUILDINGS, player, slot)) return 0;
        return building_incarnation(player, slot);
    }
    return 0;
}

uint32_t actor_incarnation_for(const record_key &key) {
    // Preserve and compare the full header, but only ordinary unit references
    // can supply the actor identity used by an attack acceptance.
    if (key.owner_and_kind > 0xff) return 0;
    const unsigned kind   = key.owner_and_kind & 0xf0;
    const unsigned player = key.owner_and_kind & 15;
    if (!unit_kind(kind) ||
        !valid_slot(mh::caps::UNITS, player, key.unit_index))
        return 0;

    return mh::storage_policy::hosted::unit_identity(
               player, key.unit_index)
        .incarnation;
}

attack_assignment assignment_for(const record_key &key) {
    attack_assignment result{};
    if (!attack_shape(key, result.secondary)) return result;

    result.actor_incarnation = actor_incarnation_for(key);

    const unsigned target_arg = result.secondary ? 10u : 5u;
    const int32_t  reference  = key.args[target_arg];
    const int32_t  slot       = key.args[target_arg + 1];

    // -1 means "retain the previous target", not a witnessed target assignment.
    // Reject values whose native narrowing would otherwise hide extra bits.
    if (reference < 0 || reference > 0xff || slot <= 0 || slot > 0xffff)
        return result;

    const unsigned ref        = static_cast<unsigned>(reference);
    result.entity             = entity_for(ref & 15, static_cast<unsigned>(slot), ref & 0xf0);
    result.target_incarnation = incarnation_for(result.entity);
    return result;
}

bool valid_assignment(const attack_assignment &assignment) {
    return assignment.actor_incarnation != 0 &&
           assignment.target_incarnation != 0 &&
           assignment.entity != 0;
}

void poison(tag &entry) {
    entry.token    = 0;
    entry.poisoned = true;
}

bool same_identity(const tag &entry, const attack_assignment &assignment) {
    return entry.actor_incarnation == assignment.actor_incarnation &&
           entry.target_incarnation == assignment.target_incarnation &&
           entry.entity == assignment.entity &&
           entry.secondary == assignment.secondary;
}

bool active_codec() {
    const bool wide = MH_OrderWideIndex_Enabled() != 0;

    if (codec_known && codec_wide != wide) {
        // Reinterpreting already-staged headers is not provenance. Retain the
        // nesting depth so matching end calls still balance, but revoke trust.
        tag_count                = 0;
        scope.token              = 0;
        scope.entity             = 0;
        scope.target_incarnation = 0;
    }

    codec_known = true;
    codec_wide  = wide;
    return wide;
}

void read_key_unchecked(const uint8_t *record, bool staged, bool wide,
                        record_key &key) {
    // No double read or x87 operation: exec_time is deliberately omitted.
    std::memcpy(&key, record + offsetof(order, unit_index), sizeof(key));

    if (!staged) return;

    key.unit_index = wide
                         ? mh::orders::decode_index(key.unit_index)
                         : static_cast<uint16_t>(key.unit_index & 0xff);
    key.owner_and_kind &= 0xff;
    key.param0 = static_cast<int16_t>(
        static_cast<uint16_t>(key.param0) & 0xff);
    key.order_code &= 0xff;
    // args[] are already exact payload bytes and must not be normalized.
}

bool read_key(orders::which container, unsigned slot, bool wide,
              record_key &key) {
    const uint8_t *base = orders::base(container);
    if (!base || slot >= orders::live_count(container)) return false;

    read_key_unchecked(
        base + static_cast<size_t>(slot) * orders::REC_BYTES,
        container == orders::STAGING, wide, key);
    return true;
}

int compare(const record_key &left, const record_key &right) {
    // Padding-free fixed-width key: byte ordering is arbitrary but total.
    // Equality compares every normalized header byte and all thirteen args.
    return std::memcmp(&left, &right, sizeof(left));
}

unsigned lower_bound(const record_key &key) {
    unsigned first = 0;
    unsigned count = tag_count;

    while (count != 0) {
        const unsigned step   = count / 2;
        const unsigned middle = first + step;
        if (compare(tags[middle].key, key) < 0) {
            first = middle + 1;
            count -= step + 1;
        } else {
            count = step;
        }
    }

    return first;
}

tag *find_tag(const record_key &key) {
    if (!tags || tag_count == 0) return nullptr;
    const unsigned index = lower_bound(key);
    return index < tag_count && compare(tags[index].key, key) == 0
               ? &tags[index]
               : nullptr;
}

void invalidate_identity(tag &entry) {
    if (!same_identity(entry, assignment_for(entry.key)))
        poison(entry);
}

void prune_absent(bool wide) {
    if (!tags || tag_count == 0) return;

    for (unsigned i = 0; i < tag_count; ++i)
        tags[i].present = false;

    // Transfers and compaction need no observer. A tag remains live while its
    // full normalized key occurs anywhere in ANY of the three live containers.
    for (unsigned n = 0; n < orders::N; ++n) {
        const auto     container = static_cast<orders::which>(n);
        const uint8_t *base      = orders::base(container);
        const unsigned count     = orders::live_count(container);
        if (!base) continue;

        for (unsigned i = 0; i < count; ++i) {
            record_key key{};
            read_key_unchecked(
                base + static_cast<size_t>(i) * orders::REC_BYTES,
                container == orders::STAGING, wide, key);
            if (tag *entry = find_tag(key))
                entry->present = true;
        }
    }

    unsigned out = 0;
    for (unsigned i = 0; i < tag_count; ++i) {
        if (!tags[i].present) continue;
        invalidate_identity(tags[i]);
        if (out != i) tags[out] = tags[i];
        ++out;
    }
    tag_count = out;
}

unsigned live_occurrences(const record_key &wanted, bool wide) {
    unsigned occurrences = 0;

    for (unsigned n = 0; n < orders::N; ++n) {
        const auto     container = static_cast<orders::which>(n);
        const uint8_t *base      = orders::base(container);
        const unsigned count     = orders::live_count(container);
        if (!base) continue;

        for (unsigned i = 0; i < count; ++i) {
            record_key key{};
            read_key_unchecked(
                base + static_cast<size_t>(i) * orders::REC_BYTES,
                container == orders::STAGING, wide, key);

            if (compare(key, wanted) == 0 && ++occurrences > 1)
                return occurrences;
        }
    }

    return occurrences;
}

bool sole_live_occurrence(const record_key &wanted, bool wide) {
    return live_occurrences(wanted, wide) == 1;
}

tag *insert_tag(const record_key &key, bool wide) {
    if (!tags) return nullptr;

    if (tag_count == tag_capacity) {
        prune_absent(wide);
        if (tag_count == tag_capacity) return nullptr;
    }

    const unsigned index = lower_bound(key);
    for (unsigned i = tag_count; i > index; --i)
        tags[i] = tags[i - 1];

    tags[index]     = {};
    tags[index].key = key;
    ++tag_count;
    return &tags[index];
}

void invalidate_scope_token() {
    if (scope.token != 0) {
        for (unsigned i = 0; i < tag_count; ++i) {
            if (tags[i].token == scope.token)
                poison(tags[i]);
        }
    }
    scope.token              = 0;
    scope.entity             = 0;
    scope.target_incarnation = 0;
}

void trust(tag &entry, const attack_assignment &assignment) {
    entry.token              = scope.token;
    entry.actor_incarnation  = assignment.actor_incarnation;
    entry.target_incarnation = assignment.target_incarnation;
    entry.entity             = assignment.entity;
    entry.secondary          = assignment.secondary;
    entry.poisoned           = false;
}

void admit_attack_key(unsigned container, bool unknown_origin, bool wide, const record_key &key) {
    bool secondary = false;
    if (!attack_shape(key, secondary)) return;
    tag                    *entry      = find_tag(key);
    const attack_assignment assignment = assignment_for(key);
    const bool              witnessed  = !unknown_origin && container != orders::PENDING &&
                           scope.depth == 1 && scope.token != 0 &&
                           valid_assignment(assignment) && assignment.entity == scope.entity &&
                           assignment.target_incarnation == scope.target_incarnation;
    if (!witnessed) {
        if (!entry) entry = insert_tag(key, wide);
        if (entry) poison(*entry);
        return;
    }
    if (entry) {
        invalidate_identity(*entry);
        if (!entry->poisoned && entry->token == scope.token && same_identity(*entry, assignment))
            return;
    }
    // Only a fresh witnessed sole origin may replace a poisoned or different token.
    const bool sole = sole_live_occurrence(key, wide);
    if (!entry) entry = insert_tag(key, wide);
    if (!entry) return;
    if (sole) trust(*entry, assignment);
    else poison(*entry);
}

attack_assignment assignment_for_key(const record_key &key) {
    attack_assignment result = assignment_for(key);
    if (tag *entry = find_tag(key)) {
        invalidate_identity(*entry);
        if (valid_assignment(result) && !entry->poisoned && same_identity(*entry, result))
            result.token = entry->token;
    }
    return result;
}

} // namespace

bool reserve_attack_provenance() {
    if (tags) return true;

    size_t capacity = 0;
    for (unsigned n = 0; n < orders::N; ++n)
        capacity += orders::wide_cap(static_cast<orders::which>(n));

    if (capacity == 0 ||
        capacity > std::numeric_limits<unsigned>::max() ||
        capacity > std::numeric_limits<size_t>::max() / sizeof(tag))
        return false;

    std::unique_ptr<tag[]> allocated(new (std::nothrow) tag[capacity]);
    if (!allocated) return false;

    tags         = std::move(allocated);
    tag_capacity = static_cast<unsigned>(capacity);
    reset_attack_provenance();
    return true;
}

void reset_attack_provenance() {
    tag_count   = 0;
    scope       = {};
    codec_known = false;
    codec_wide  = false;
}

void begin_attack_group(unsigned target_slot, unsigned target_player,
                        uint16_t selector) {
    const bool wide = active_codec();

    if (scope.depth != 0) {
        // Neither the inner scope nor the interrupted outer scope is trusted.
        // Revoke tags already admitted by the outer scope as well.
        invalidate_scope_token();
        if (scope.depth != std::numeric_limits<unsigned>::max())
            ++scope.depth;
        return;
    }

    prune_absent(wide);
    scope       = {};
    scope.depth = 1;

    // These are group selectors, not an arbitrary packed reference.
    if (selector != 0x20 && selector != 0x80 && selector != 0x40)
        return;

    scope.entity             = entity_for(target_player, target_slot, selector);
    scope.target_incarnation = incarnation_for(scope.entity);
    if (scope.target_incarnation != 0 && tags)
        scope.token = allocate_intent_token();
}

void end_attack_group() {
    if (scope.depth == 0) return;

    // Saturation is permanently conservative until the explicit world reset.
    if (scope.depth == std::numeric_limits<unsigned>::max()) return;
    if (--scope.depth != 0) return;

    scope = {};
    prune_absent(active_codec());
}

void admit_attack_order(unsigned container, unsigned slot, bool unknown_origin) {
    if (container >= orders::N) return;
    const bool wide = active_codec();
    record_key key{};
    if (!read_key(static_cast<orders::which>(container), slot, wide, key)) return;
    admit_attack_key(container, unknown_origin, wide, key);
}

void admit_attack_order(const libmh_order_observation &observation) {
    if (observation.kind != LIBMH_EVK_ORDER_ADMITTED) return;
    const unsigned container = observation.flags & LIBMH_ORDER_OBSERVATION_CONTAINER_MASK;
    if (container >= orders::N) return;
    const bool wide = active_codec();
    record_key key{};
    std::memcpy(&key, &observation.unit_index, sizeof key);
    admit_attack_key(container,
                     (observation.flags & LIBMH_ORDER_OBSERVATION_UNKNOWN_ORIGIN) != 0,
                     wide, key);
}

void amend_attack_metadata(
    const mh::order_witness::canonical_order &before,
    const mh::order_witness::canonical_order &after) {
    // canonical60 is exact: only args8..11 may change, never the header,
    // args0..7 or args12. Equal values require no cache operation.
    constexpr size_t prefix = offsetof(mh::order_witness::canonical_order, args) +
                              8 * sizeof(before.args[0]);
    if (std::memcmp(&before, &after, prefix) != 0 ||
        before.args[12] != after.args[12] ||
        std::memcmp(&before, &after, sizeof before) == 0)
        return;

    const bool wide = active_codec();
    record_key old_key{}, new_key{};
    std::memcpy(&old_key, &before, sizeof old_key);
    std::memcpy(&new_key, &after, sizeof new_key);

    // An unknown source must not acquire an already-known destination's token.
    // Keep the old tag while any unamended physical copy still uses its key.
    if (tag *destination = find_tag(new_key)) {
        poison(*destination);
        return;
    }

    tag *source = find_tag(old_key);
    if (!source) return;
    invalidate_identity(*source);
    // insert_tag may prune and shuffle every element. Snapshot before calling it.
    tag amended = *source;
    amended.key = new_key;
    invalidate_identity(amended);
    if (!sole_live_occurrence(new_key, wide)) poison(amended);

    if (live_occurrences(old_key, wide) == 0) {
        const unsigned index = static_cast<unsigned>(source - tags.get());
        for (unsigned i = index + 1; i < tag_count; ++i)
            tags[i - 1] = tags[i];
        --tag_count;
    }
    // This only transfers/clones an existing witnessed (possibly poisoned) tag.
    // Capacity exhaustion leaves the amended key unknown, never fabricated.
    if (tag *destination = insert_tag(new_key, wide))
        *destination = amended;
}

attack_assignment attack_assignment_for(const order &record) {
    active_codec();
    record_key key{};
    std::memcpy(&key, &record.unit_index, sizeof key);
    return assignment_for_key(key);
}

attack_assignment attack_assignment_for(const libmh_order_observation &observation) {
    active_codec();
    record_key key{};
    std::memcpy(&key, &observation.unit_index, sizeof key);
    return assignment_for_key(key);
}

} // namespace mh::ui::selected_targets::detail
