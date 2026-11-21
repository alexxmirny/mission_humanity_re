#pragma once

#include <stddef.h>
#include <stdint.h>

/* Owned observations, shared by native and translated presentation producers. */
#define LIBMH_EVK_UNIT_ORDER_COMMITTED   24u
#define LIBMH_EVK_ATTACK_GROUP_BEGIN     25u
#define LIBMH_EVK_ATTACK_GROUP_END       26u
#define LIBMH_EVK_ORDER_ADMITTED         27u
#define LIBMH_EVK_ATTACK_TARGET_ASSIGNED 28u
#define LIBMH_EVK_ORDER_APPLIED          29u
#define LIBMH_EVK_ORDER_DISCARDED        30u
#define LIBMH_EVK_ORDER_WORLD_RESET      31u
#define LIBMH_EVK_UNIT_PRIMARY_TERMINAL  32u
#define LIBMH_EVK_MOVE_GROUP_REVOKED     33u
#define LIBMH_EVK_ORDER_METADATA_BEFORE  34u
#define LIBMH_EVK_ORDER_METADATA_AFTER   35u

#define LIBMH_ORDER_OBSERVATION_CONTAINER_MASK 3u
#define LIBMH_ORDER_OBSERVATION_QUEUE          0u
#define LIBMH_ORDER_OBSERVATION_PENDING        1u
#define LIBMH_ORDER_OBSERVATION_STAGING        2u
#define LIBMH_ORDER_OBSERVATION_UNKNOWN_ORIGIN 4u
#define LIBMH_ORDER_OBSERVATION_SECONDARY      8u

typedef struct libmh_order_observation {
    uint16_t kind;
    uint16_t flags;
    uint16_t unit_index;
    uint16_t owner_and_kind;
    int16_t  param0;
    uint16_t order_code;
    int32_t  args[13];

    uint64_t admission_id;
    uint64_t command_id;
    uint32_t actor_incarnation;
    uint32_t world_generation;
} libmh_order_observation;

typedef void (*libmh_order_observation_sink)(
    const libmh_order_observation *);

typedef struct libmh_order_witness_store libmh_order_witness_store;

typedef uint32_t (*mh_order_actor_incarnation)(
    void *context, uint32_t player, uint32_t slot);

typedef void (*mh_order_observation_delivery)(
    void *context, const libmh_order_observation *);

#ifdef __cplusplus

#include <cstring>

static_assert(sizeof(libmh_order_observation) == 88);
static_assert(offsetof(libmh_order_observation, unit_index) == 4);
static_assert(offsetof(libmh_order_observation, args) == 12);
static_assert(offsetof(libmh_order_observation, admission_id) == 64);
static_assert(offsetof(libmh_order_observation, command_id) == 72);

namespace mh::order_witness {

using store  = ::libmh_order_witness_store;
using handle = uint32_t;

enum class container : uint32_t {
    queue   = 0,
    pending = 1,
    staging = 2
};

struct canonical_order {
    uint16_t unit_index;
    uint16_t owner_and_kind;
    int16_t  param0;
    uint16_t order_code;
    int32_t  args[13];
};

static_assert(sizeof(canonical_order) == 60);
static_assert(offsetof(canonical_order, args) == 8);

inline constexpr uint32_t schema       = 1;
inline constexpr uint32_t maximum_pins = 8;
inline constexpr uint64_t presentation_token_namespace =
    UINT64_C(0x8000000000000000);
constexpr size_t slab_align8(size_t value) {
    return (value + size_t(7)) & ~size_t(7);
}

/*
 * Conservative bounds for schema1's private POD layouts.
 * order_witness.cpp asserts its real layouts fit these bounds.
 *
 * Store:
 *   twenty uint32_t fields, two uint64_t fields,
 *   two callback pointers and two context pointers.
 *
 * Node:
 *   canonical command, two uint64_t IDs, four uint32_t fields,
 *   flags plus ever_applied/reserved bytes.
 */
inline constexpr size_t store_header_bytes_bound =
    slab_align8(20u * sizeof(uint32_t)) +
    slab_align8(2u * sizeof(uint64_t)) +
    slab_align8(sizeof(mh_order_actor_incarnation)) +
    slab_align8(sizeof(void *)) +
    slab_align8(sizeof(mh_order_observation_delivery)) +
    slab_align8(sizeof(void *));

inline constexpr size_t node_bytes_bound =
    slab_align8(sizeof(canonical_order)) +
    slab_align8(2u * sizeof(uint64_t)) +
    slab_align8(4u * sizeof(uint32_t)) +
    slab_align8(sizeof(uint16_t) + 2u * sizeof(uint8_t));

constexpr size_t fixed_slab_bytes(
    uint32_t queue_capacity,
    uint32_t pending_capacity,
    uint32_t staging_capacity) {
    return store_header_bytes_bound +
           slab_align8(size_t(queue_capacity) * sizeof(uint32_t)) +
           slab_align8(size_t(pending_capacity) * sizeof(uint32_t)) +
           slab_align8(size_t(staging_capacity) * sizeof(uint32_t)) +
           slab_align8(
               (size_t(queue_capacity) +
                size_t(pending_capacity) +
                size_t(staging_capacity) +
                maximum_pins + 1u) *
               node_bytes_bound);
}

template <typename Record>
canonical_order canonical(const Record &record) {
    canonical_order result{};
    result.unit_index     = record.unit_index;
    result.owner_and_kind = record.owner_and_kind;
    result.param0         = record.param0;
    result.order_code     = record.order_code;
    std::memcpy(result.args, record.args, sizeof result.args);
    return result;
}

/* One setup slab. No allocations occur in the operations below.
 *
 * Callbacks are synchronous and must not recursively mutate this store.
 * Actor identity comes from the existing canonical storage-policy authority.
 */
size_t required_bytes(const uint32_t (&capacities)[3]);

store *initialize(
    void *slab, size_t bytes, const uint32_t (&capacities)[3],
    mh_order_actor_incarnation actor, void *actor_context,
    mh_order_observation_delivery deliver, void *delivery_context);

bool     valid(const store *value);
bool     quiescent(const store &value);
bool     unused(const store &value);
uint32_t generation(const store &value);

bool set_delivery(
    store &value, mh_order_observation_delivery deliver, void *context);

handle pin(store &, container, uint32_t slot);
void   unpin(store &, handle);

handle admit(
    store &, container, uint32_t slot,
    const canonical_order &, bool unknown_origin);

struct metadata_binding {
    handle   value;
    uint32_t world_generation;
    uint64_t admission_id;
    uint64_t command_id;
};

metadata_binding metadata_at(const store &, container, uint32_t slot);

/* Presentation-only rekey of an existing physical handle. Only args[8..11]
 * may change. No admission or identity changes occur. Successful amendments
 * synchronously deliver the exact before/after pair with unchanged lineage.
 * Returns false without changing the store if the payload or actor is stale.
 */
bool amend_metadata(
    store &, container, uint32_t slot,
    metadata_binding       expected,
    const canonical_order &before, const canonical_order &after);

void copy(store &, container destination, uint32_t destination_slot,
          handle source);
void clear(store &, container, uint32_t slot);

/* Clears [first, highest mapped slot), not the entire configured capacity. */
void clear_from(store &, container, uint32_t first);

void applied(store &, handle);
void primary_terminal(store &, uint32_t player, uint32_t slot);
void begin_move_group(store &);
void end_move_group(store &);
void reset_world(store &);

class pinned {
public:
    pinned(store &owner, container source, uint32_t slot)
        : owner_(owner), generation_(generation(owner)),
          value_(pin(owner, source, slot)) {}

    ~pinned() { unpin(owner_, value_); }

    pinned(const pinned &)            = delete;
    pinned &operator=(const pinned &) = delete;

    handle get() const { return value_; }

    bool current() const {
        return generation_ == generation(owner_);
    }

private:
    store   &owner_;
    uint32_t generation_;
    handle   value_;
};

class move_group {
public:
    explicit move_group(store &owner)
        : owner_(owner), generation_(generation(owner)) {
        begin_move_group(owner_);
    }

    ~move_group() {
        if (generation_ == generation(owner_))
            end_move_group(owner_);
    }

    move_group(const move_group &)            = delete;
    move_group &operator=(const move_group &) = delete;

private:
    store   &owner_;
    uint32_t generation_;
};

} // namespace mh::order_witness
#endif
