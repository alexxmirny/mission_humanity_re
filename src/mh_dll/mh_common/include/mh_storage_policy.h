// Storage settings and auto-exit transitions. No process-global state or game pointers.
#pragma once
#include <stddef.h>
#include <stdint.h>

// Dedicated synchronous libmh host callback payloads. Fixed-width POD, no game pointers, no
// presentation-event routing. Transient requests/results are not serialized state.
typedef struct mh_storage_policy_request {
    uint32_t operation;
    uint32_t player;
    uint32_t slot;
    uint32_t args[16];
} mh_storage_policy_request;
typedef struct mh_storage_policy_result {
    uint32_t values[5];
} mh_storage_policy_result;
typedef int32_t (*mh_storage_policy_callback)(const mh_storage_policy_request *, mh_storage_policy_result *);
#ifdef __cplusplus
static_assert(sizeof(mh_storage_policy_request) == 76);
static_assert(sizeof(mh_storage_policy_result) == 20);

namespace mh::storage_policy {

inline constexpr uint32_t SCHEMA         = 1;
inline constexpr uint32_t HEADER_BYTES   = 32;
inline constexpr uint32_t BUILDING_BYTES = 16;
inline constexpr uint32_t UNIT_BYTES     = 16;
inline constexpr uint32_t ENVELOPE_BYTES = 16;

struct dimensions {
    uint32_t players, buildings, units, width, height;
};
struct identity {
    uint32_t player, slot, incarnation;
};
struct rally_view {
    uint32_t incarnation = 0;
    uint32_t x = 0, y = 0;
    bool     enabled   = false;
    bool     automatic = false;
};
enum class command_kind : uint32_t { set_rally   = 1,
                                     clear_rally = 2,
                                     set_auto    = 3 };
struct command {
    identity     building;
    command_kind kind;
    uint32_t     x = 0, y = 0; // set_auto: x is desired 0/1, y is zero
};
// A departing rally uses the existing planner state as its temporary order. Only the two
// storage departure completions interpret it, then restore the normal Move order/state pair.
inline bool is_rally_departure(uint16_t order, uint16_t move_state, uint16_t default_order) {
    return order == move_state && move_state != default_order;
}

enum class event_kind : uint32_t { production_inserted = 1,
                                   repair_reached_full = 2 };
enum class launch_result : uint32_t { invalid,
                                      deferred,
                                      accepted };

// A span of canonical LITTLE-ENDIAN bytes, not a packed native struct. Registry snapshots may
// copy these bytes directly. Header: magic, schema, players, buildings, units, width, height, size.
// Then player-major building records: incarnation, flags(alive=1,rally=2,auto=4), x, y.
// Then player-major unit records: incarnation, flags(alive=1,pending=2), receiver slot, receiver incarnation.
// Retired slots retain incarnation. All other retired fields and all nonpending receiver fields are zero.
class state {
public:
    state() = default;
    static uint32_t required_bytes(dimensions d);
    bool            initialize(void *memory, uint32_t bytes, dimensions d);
    bool            bind(void *memory, uint32_t bytes); // validates before binding; never changes input
    const uint8_t  *data() const { return bytes_; }
    uint8_t        *data() { return bytes_; }
    uint32_t        size() const { return size_; }
    dimensions      shape() const;
    bool            valid() const;
    bool            reset();

    // Birth/reassignment must be called exactly once at the allocator's successful slot claim.
    // Incarnation exhaustion refuses reuse, never wraps an old identity back into validity.
    bool       birth_building(uint32_t player, uint32_t slot, identity &out);
    bool       birth_unit(uint32_t player, uint32_t slot, identity &out);
    void       retire_building(identity building);
    void       retire_unit(identity unit);
    identity   building_identity(uint32_t player, uint32_t slot) const;
    identity   unit_identity(uint32_t player, uint32_t slot) const;
    bool       apply_command(uint32_t actor, const command &cmd);
    bool       qualify(identity unit, identity receiver, event_kind event);
    void       cancel_pending(identity unit);
    bool       pending(identity unit, identity &receiver) const;
    rally_view read_rally(uint32_t player, uint32_t building_slot) const;

    // One deterministic pass after explicit-order application and before unit ticks. The callback
    // must recheck current receiver/full HP/parked eligibility. accepted means state 0x20 AND goal
    // stores have completed synchronously. It MUST NOT enqueue, transmit, or retain an order.
    using launch_fn = launch_result (*)(void *, identity, identity, rally_view);
    void retry(void *context, launch_fn launch);

    // The save envelope is SPOL,u32 schema,u32 state length,u32 CRC32(state), followed by the
    // canonical region. Import validates everything (including shape) before a single write.
    uint32_t    encoded_bytes() const;
    bool        encode(void *out, uint32_t capacity) const;
    static bool validate(const void *encoded, uint32_t bytes, dimensions expected);
    bool        import(const void *encoded, uint32_t bytes);

private:
    uint8_t *bytes_ = nullptr;
    uint32_t size_  = 0;
    uint8_t *building(uint32_t player, uint32_t slot) const;
    uint8_t *unit(uint32_t player, uint32_t slot) const;
    bool     matches(uint8_t *record, identity id) const;
    void     cancel_receiver(identity receiver);
};

// EN administrative kind-0 allocation: native admin switch's max is 0xfb. Complete measured
// issuer census + whole-image instruction survey recorded in storage_policy_integration.md.
inline constexpr uint16_t RALLY_COMMAND = 0xfc;
inline constexpr uint16_t AUTO_COMMAND  = 0xfd;
// Both native param0 and order_code carry the chosen code. All thirteen words are initialized.
void encode_command(const command &cmd, int32_t (&args)[13]);
bool decode_command(uint32_t actor, uint32_t building_slot, const int32_t (&args)[13], command &out);
// Settings must not participate in native unit-order supersession. Both settings and any other
// order are independent; revisions of settings execute in insertion order, including equal clocks.
bool independent_settings(uint16_t a_owner, uint16_t a_code, uint16_t b_owner, uint16_t b_code,
                          uint16_t rally_code, uint16_t auto_code);

enum class operation : uint32_t {
    birth_building = 1,
    birth_unit,
    retire_building,
    retire_unit,
    cancel,
    production,
    paid_repair,
    consume_setting,
    retry,
    read_rally,
    manual_rally,
    reset_loaded_roster,
    map_extent
};
enum class receiver_reason : uint32_t { production,
                                        paid_repair,
                                        manual };
// Live adapters perform read-only validation against their own current roster. The common
// dispatcher owns all policy mutations; a standalone host binds the region and supplies these
// callbacks, rather than instantiating a hidden second policy owner.
struct backend {
    void *context;
    bool (*eligible_building)(void *, uint32_t player, uint32_t slot);
    bool (*unit_exists)(void *, uint32_t player, uint32_t slot);
    bool (*resolve_receiver)(void *, uint32_t player, uint32_t unit, receiver_reason,
                             double energy_before, uint32_t &building_slot);
    state::launch_fn launch;
    // General lifetime membership, independent of storage/rally/Auto eligibility.
    bool (*building_exists)(void *, uint32_t player, uint32_t slot);
};
// Returns 1 for handled operation, 0 for unsupported/malformed request. result is always cleared.
// consume_setting: player=owner_and_kind; args0=code,args1=param0,args2..14=native args0..12.
// paid_repair: args0/1=raw LE IEEE754 double energy_before bits. map_extent: args0/1=width/height.
// Other operations have all args zero. read_rally result=incarnation,x,y,enabled,auto.
// manual_rally result=enabled,x,y,0,0. Birth result0=incarnation (0 on refusal).
// consume_setting result0=applied (recognized malformed settings are handled but not applied).
int32_t dispatch(state &policy, const backend &live, const mh_storage_policy_request &request,
                 mh_storage_policy_result &result);

} // namespace mh::storage_policy
#endif
