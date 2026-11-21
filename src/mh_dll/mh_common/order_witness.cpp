#include "include/mh_order_witness.h"

#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>

namespace {

constexpr uint32_t magic = 0x5752444fu; // ODRW

struct node {
    mh::order_witness::canonical_order command;
    uint64_t                           admission_id;
    uint64_t                           command_id;
    uint32_t                           actor_incarnation;
    uint32_t                           world_generation;
    uint32_t                           refs;
    uint32_t                           next_free;
    uint16_t                           flags;
    uint8_t                            ever_applied;
    uint8_t                            reserved;
};

struct layout {
    uint32_t map_offset[3]{};
    uint32_t pool_offset   = 0;
    uint32_t pool_capacity = 0;
    uint32_t bytes         = 0;
};

void require(bool condition) {
    if (!condition)
        std::abort();
}

uint64_t align8(uint64_t value) {
    return (value + 7u) & ~UINT64_C(7);
}

} // namespace

struct libmh_order_witness_store {
    uint32_t                      magic;
    uint32_t                      schema;
    uint32_t                      bytes;
    uint32_t                      capacities[3];
    uint32_t                      map_offset[3];
    uint32_t                      mapped_end[3];
    uint32_t                      pool_offset;
    uint32_t                      pool_capacity;
    uint32_t                      free_head;
    uint32_t                      pins;
    uint32_t                      world_generation;
    uint32_t                      group_depth;
    uint64_t                      next_id;
    uint64_t                      group_id;
    uint32_t                      delivering;
    uint32_t                      reserved;
    mh_order_actor_incarnation    actor;
    void                         *actor_context;
    mh_order_observation_delivery deliver;
    void                         *delivery_context;
};
static_assert(
    sizeof(libmh_order_witness_store) <=
    mh::order_witness::store_header_bytes_bound);

static_assert(
    sizeof(node) <= mh::order_witness::node_bytes_bound);

static_assert(alignof(libmh_order_witness_store) <= 8);
static_assert(alignof(node) <= 8);

namespace {

bool make_layout(const uint32_t (&capacities)[3], layout &out) {
    uint64_t cursor = align8(sizeof(libmh_order_witness_store));
    uint64_t slots  = 0;

    for (unsigned i = 0; i != 3; ++i) {
        if (capacities[i] == 0)
            return false;
        slots += capacities[i];
        if (cursor > UINT32_MAX)
            return false;
        out.map_offset[i] = static_cast<uint32_t>(cursor);
        cursor += uint64_t(capacities[i]) * sizeof(uint32_t);
    }

    const uint64_t count =
        slots + mh::order_witness::maximum_pins;

    if (count >= UINT32_MAX)
        return false;

    cursor = align8(cursor);
    if (cursor > UINT32_MAX)
        return false;

    out.pool_offset   = static_cast<uint32_t>(cursor);
    out.pool_capacity = static_cast<uint32_t>(count);

    // Handle zero is permanently invalid.
    cursor += (count + 1u) * sizeof(node);
    cursor = align8(cursor);

    if (cursor > UINT32_MAX ||
        cursor > std::numeric_limits<size_t>::max())
        return false;

    out.bytes = static_cast<uint32_t>(cursor);
    return true;
}

void check(const libmh_order_witness_store &s) {
    require(s.magic == magic &&
            s.schema == mh::order_witness::schema &&
            s.actor != nullptr && s.deliver != nullptr);
}

void mutation(const libmh_order_witness_store &s) {
    check(s);
    require(s.delivering == 0);
}

unsigned ordinal(mh::order_witness::container which) {
    const unsigned result = static_cast<unsigned>(which);
    require(result < 3);
    return result;
}

uint32_t *map(libmh_order_witness_store &s, unsigned which) {
    return reinterpret_cast<uint32_t *>(
        reinterpret_cast<uint8_t *>(&s) + s.map_offset[which]);
}

node *pool(libmh_order_witness_store &s) {
    return reinterpret_cast<node *>(
        reinterpret_cast<uint8_t *>(&s) + s.pool_offset);
}

const uint32_t *map(const libmh_order_witness_store &s, unsigned which) {
    return reinterpret_cast<const uint32_t *>(
        reinterpret_cast<const uint8_t *>(&s) + s.map_offset[which]);
}

const node *pool(const libmh_order_witness_store &s) {
    return reinterpret_cast<const node *>(
        reinterpret_cast<const uint8_t *>(&s) + s.pool_offset);
}

node &entry(libmh_order_witness_store &s, uint32_t value) {
    require(value != 0 && value <= s.pool_capacity);
    node &result = pool(s)[value];
    require(result.refs != 0);
    return result;
}

uint64_t mint(libmh_order_witness_store &s) {
    // The high bit belongs to the renderer's token namespace.
    require(s.next_id <
            mh::order_witness::presentation_token_namespace - 1u);
    return ++s.next_id;
}

void deliver(libmh_order_witness_store     &s,
             const libmh_order_observation &observation) {
    require(s.delivering == 0);
    s.delivering = 1;
    s.deliver(s.delivery_context, &observation);
    s.delivering = 0;
}

libmh_order_observation observation(
    const node &n, uint16_t kind) {
    libmh_order_observation result{};
    result.kind  = kind;
    result.flags = n.flags;
    std::memcpy(&result.unit_index, &n.command, sizeof n.command);
    result.admission_id      = n.admission_id;
    result.command_id        = n.command_id;
    result.actor_incarnation = n.actor_incarnation;
    result.world_generation  = n.world_generation;
    return result;
}

void retain(libmh_order_witness_store &s, uint32_t value) {
    if (value == 0)
        return;
    node &n = entry(s, value);
    require(n.refs != UINT32_MAX);
    ++n.refs;
}

void release(libmh_order_witness_store &s, uint32_t value) {
    if (value == 0)
        return;

    node &n = entry(s, value);
    if (--n.refs != 0)
        return;

    const bool discarded =
        n.world_generation == s.world_generation &&
        n.ever_applied == 0;

    libmh_order_observation event{};
    if (discarded)
        event = observation(n, LIBMH_EVK_ORDER_DISCARDED);

    n           = {};
    n.next_free = s.free_head;
    s.free_head = value;

    if (discarded)
        deliver(s, event);
}

void clear_slot(libmh_order_witness_store &s,
                unsigned which, uint32_t slot) {
    require(slot < s.capacities[which]);
    uint32_t      &destination = map(s, which)[slot];
    const uint32_t old         = destination;
    destination                = 0;
    release(s, old);
}

uint32_t actor_life(libmh_order_witness_store                &s,
                    const mh::order_witness::canonical_order &command) {
    if (command.owner_and_kind > 0xff)
        return 0;

    const unsigned kind = command.owner_and_kind & 0xf0;
    if (kind != 0x20 && kind != 0x80)
        return 0;

    return s.actor(s.actor_context,
                   command.owner_and_kind & 15u,
                   command.unit_index);
}

} // namespace

namespace mh::order_witness {

size_t required_bytes(const uint32_t (&capacities)[3]) {
    layout value{};
    return make_layout(capacities, value) ? value.bytes : 0;
}

store *initialize(
    void *slab, size_t bytes, const uint32_t (&capacities)[3],
    mh_order_actor_incarnation actor, void *actor_context,
    mh_order_observation_delivery delivery, void *delivery_context) {
    layout l{};
    if (!slab || !actor || !delivery ||
        (reinterpret_cast<uintptr_t>(slab) & 7u) != 0 ||
        !make_layout(capacities, l) || bytes < l.bytes)
        return nullptr;

    auto *s             = new (slab) store{};
    s->magic            = magic;
    s->schema           = schema;
    s->bytes            = l.bytes;
    s->pool_offset      = l.pool_offset;
    s->pool_capacity    = l.pool_capacity;
    s->free_head        = 1;
    s->world_generation = 1;
    s->actor            = actor;
    s->actor_context    = actor_context;
    s->deliver          = delivery;
    s->delivery_context = delivery_context;

    for (unsigned which = 0; which != 3; ++which) {
        s->capacities[which] = capacities[which];
        s->map_offset[which] = l.map_offset[which];
        uint32_t *slots      = map(*s, which);
        for (uint32_t i = 0; i != capacities[which]; ++i)
            new (slots + i) uint32_t(0);
    }

    node *nodes = pool(*s);
    for (uint32_t i = 0; i <= s->pool_capacity; ++i)
        new (nodes + i) node{};

    for (uint32_t i = 1; i <= s->pool_capacity; ++i)
        nodes[i].next_free =
            i == s->pool_capacity ? 0 : i + 1;

    return s;
}

bool valid(const store *s) {
    if (!s || s->magic != magic || s->schema != schema ||
        !s->actor || !s->deliver ||
        s->pins > maximum_pins || s->world_generation == 0)
        return false;

    layout l{};
    if (!make_layout(s->capacities, l) ||
        s->bytes != l.bytes ||
        s->pool_offset != l.pool_offset ||
        s->pool_capacity != l.pool_capacity ||
        s->free_head > s->pool_capacity)
        return false;

    for (unsigned i = 0; i != 3; ++i)
        if (s->map_offset[i] != l.map_offset[i] ||
            s->mapped_end[i] > s->capacities[i])
            return false;

    return true;
}

bool quiescent(const store &s) {
    check(s);
    return s.pins == 0 &&
           s.group_depth == 0 &&
           s.delivering == 0;
}

bool unused(const store &s) {
    check(s);
    return s.next_id == 0 && quiescent(s);
}

uint32_t generation(const store &s) {
    check(s);
    return s.world_generation;
}

bool set_delivery(store                        &s,
                  mh_order_observation_delivery delivery,
                  void                         *context) {
    if (!delivery || !valid(&s) || !quiescent(s))
        return false;
    s.deliver          = delivery;
    s.delivery_context = context;
    return true;
}

handle pin(store &s, container source, uint32_t slot) {
    mutation(s);
    const unsigned which = ordinal(source);
    require(slot < s.capacities[which]);

    const handle value = map(s, which)[slot];
    if (value != 0) {
        require(s.pins < maximum_pins);
        retain(s, value);
        ++s.pins;
    }
    return value;
}

void unpin(store &s, handle value) {
    mutation(s);
    if (value == 0)
        return;
    require(s.pins != 0);
    --s.pins;
    release(s, value);
}

handle admit(
    store &s, container destination, uint32_t slot,
    const canonical_order &command, bool unknown_origin) {
    mutation(s);
    const unsigned which = ordinal(destination);
    require(slot < s.capacities[which]);

    // Release stale physical ownership before requesting a pool node.
    clear_slot(s, which, slot);

    const handle value = s.free_head;
    require(value != 0);

    node &n             = pool(s)[value];
    s.free_head         = n.next_free;
    n                   = {};
    n.command           = command;
    n.admission_id      = mint(s);
    n.command_id        = unknown_origin ? 0 : s.group_id;
    n.actor_incarnation = actor_life(s, command);
    n.world_generation  = s.world_generation;
    n.refs              = 1;
    n.flags             = static_cast<uint16_t>(which);
    if (unknown_origin)
        n.flags |= LIBMH_ORDER_OBSERVATION_UNKNOWN_ORIGIN;

    map(s, which)[slot] = value;
    if (slot >= s.mapped_end[which])
        s.mapped_end[which] = slot + 1;

    const auto event = observation(n, LIBMH_EVK_ORDER_ADMITTED);
    deliver(s, event);
    return value;
}

metadata_binding metadata_at(
    const store &s, container source, uint32_t slot) {
    if (!valid(&s) || s.delivering != 0)
        return {};
    const unsigned which = static_cast<unsigned>(source);
    if (which >= 3 || slot >= s.capacities[which] ||
        slot >= s.mapped_end[which])
        return {};
    const handle value = map(s, which)[slot];
    if (value == 0 || value > s.pool_capacity)
        return {};
    const node &n = pool(s)[value];
    if (n.refs == 0 || n.world_generation != s.world_generation ||
        n.admission_id == 0)
        return {};
    return {value, s.world_generation, n.admission_id, n.command_id};
}

bool amend_metadata(
    store &s, container source, uint32_t slot,
    metadata_binding       expected,
    const canonical_order &before, const canonical_order &after) {
    // This optional presentation operation must not turn a stale observation
    // into either a gameplay failure or a new admission.
    if (!valid(&s) || s.delivering != 0 ||
        expected.value == 0 ||
        expected.admission_id == 0 ||
        expected.world_generation != s.world_generation)
        return false;

    const unsigned which = static_cast<unsigned>(source);
    if (which >= 3 || slot >= s.capacities[which] ||
        slot >= s.mapped_end[which])
        return false;

    // Compare the exact canonical bytes, not a hash or a normalized command.
    // The header and args[0..7] form the first 40 bytes; args[12] is the tail.
    if (std::memcmp(&before, &after,
                    offsetof(canonical_order, args) +
                        8u * sizeof(before.args[0])) != 0 ||
        before.args[12] != after.args[12])
        return false;

    const handle value = map(s, which)[slot];
    if (value == 0 || value > s.pool_capacity ||
        value != expected.value)
        return false;

    // Do not use entry(): a missing/free physical handle is an ordinary miss
    // here, rather than a violated gameplay/store mutation precondition.
    node &n = pool(s)[value];
    if (n.refs == 0 || n.world_generation != s.world_generation ||
        n.admission_id != expected.admission_id ||
        n.command_id != expected.command_id ||
        n.actor_incarnation == 0 ||
        std::memcmp(&n.command, &before, sizeof before) != 0 ||
        actor_life(s, n.command) != n.actor_incarnation)
        return false;

    const auto previous =
        observation(n, LIBMH_EVK_ORDER_METADATA_BEFORE);

    // All aliases and pins keep the same node and its IDs, refs and flags.
    // Copy only the authorized metadata, leaving every other byte untouched.
    std::memmove(n.command.args + 8, after.args + 8,
                 4u * sizeof(n.command.args[0]));
    const auto amended =
        observation(n, LIBMH_EVK_ORDER_METADATA_AFTER);
    deliver(s, previous);
    deliver(s, amended);
    return true;
}

void copy(store &s, container destination,
          uint32_t slot, handle source) {
    mutation(s);
    const unsigned which = ordinal(destination);
    require(slot < s.capacities[which]);

    // A world replaced during a producer call cannot republish its old pin.
    if (source != 0 &&
        entry(s, source).world_generation != s.world_generation)
        return;

    uint32_t &target = map(s, which)[slot];
    if (target == source)
        return;

    const handle old = target;
    retain(s, source);
    target = source;
    if (slot >= s.mapped_end[which])
        s.mapped_end[which] = slot + 1;
    release(s, old);
}

void clear(store &s, container which, uint32_t slot) {
    mutation(s);
    clear_slot(s, ordinal(which), slot);
}

void clear_from(store &s, container container_value, uint32_t first) {
    mutation(s);
    const unsigned which = ordinal(container_value);
    require(first <= s.capacities[which]);

    const uint32_t end = s.mapped_end[which];
    for (uint32_t i = first; i < end; ++i)
        clear_slot(s, which, i);

    if (first < end)
        s.mapped_end[which] = first;
}

void applied(store &s, handle value) {
    mutation(s);
    if (value == 0)
        return;

    node &n = entry(s, value);
    if (n.world_generation != s.world_generation)
        return;

    n.ever_applied = 1;
    auto event     = observation(n, LIBMH_EVK_ORDER_APPLIED);
    event.flags =
        static_cast<uint16_t>(
            (event.flags &
             ~LIBMH_ORDER_OBSERVATION_CONTAINER_MASK) |
            LIBMH_ORDER_OBSERVATION_QUEUE);
    deliver(s, event);
}

void primary_terminal(store &s, uint32_t player, uint32_t slot) {
    mutation(s);
    if (player > 15 || slot == 0 || slot > 0xffff)
        return;

    const uint32_t life = s.actor(s.actor_context, player, slot);
    if (life == 0)
        return;

    libmh_order_observation event{};
    event.kind              = LIBMH_EVK_UNIT_PRIMARY_TERMINAL;
    event.unit_index        = static_cast<uint16_t>(slot);
    event.owner_and_kind    = static_cast<uint16_t>(0x80u | player);
    event.actor_incarnation = life;
    event.world_generation  = s.world_generation;
    deliver(s, event);
}

void begin_move_group(store &s) {
    mutation(s);
    require(s.group_depth != UINT32_MAX);

    if (s.group_depth == 0) {
        s.group_depth = 1;
        s.group_id    = mint(s);
        return;
    }

    if (s.group_id != 0) {
        libmh_order_observation event{};
        event.kind             = LIBMH_EVK_MOVE_GROUP_REVOKED;
        event.command_id       = s.group_id;
        event.world_generation = s.world_generation;
        s.group_id             = 0;
        deliver(s, event);
    }

    ++s.group_depth;
}

void end_move_group(store &s) {
    mutation(s);
    if (s.group_depth == 0)
        return;
    if (--s.group_depth == 0)
        s.group_id = 0;
}

void reset_world(store &s) {
    mutation(s);
    require(s.world_generation != UINT32_MAX);

    // Invalidate before dropping physical references: no old-world DISCARD.
    ++s.world_generation;
    s.group_depth = 0;
    s.group_id    = 0;

    for (unsigned which = 0; which != 3; ++which) {
        const uint32_t end  = s.mapped_end[which];
        s.mapped_end[which] = 0;
        for (uint32_t i = 0; i < end; ++i)
            clear_slot(s, which, i);
    }

    // Pinned old nodes survive until unpin. Their handles cannot be reused.
    libmh_order_observation event{};
    event.kind             = LIBMH_EVK_ORDER_WORLD_RESET;
    event.world_generation = s.world_generation;
    deliver(s, event);
}

} // namespace mh::order_witness
