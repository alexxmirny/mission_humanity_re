// Destination: src/mh_dll/mh_common/include/mh_order_tasks.h
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace mh::order_tasks {
inline constexpr uint32_t SCHEMA = 2, CAPACITY = 32;
inline constexpr uint32_t LEGACY_SCHEMA = 1, EDIT_SCHEMA = 2, CAMPAIGN_SCHEMA = 1;
inline constexpr uint32_t HEADER_BYTES = 128, ACTOR_BYTES = 240, NODE_BYTES = 48;
inline constexpr uint32_t ROW_BYTES      = ACTOR_BYTES + CAPACITY * NODE_BYTES;
inline constexpr uint32_t ENVELOPE_BYTES = 16, NO_CYCLE = UINT32_MAX;
struct world_key {
    uint32_t system, planet, incarnation;
};
struct actor_key {
    uint32_t player, slot, incarnation;
};
enum class object_kind : uint32_t { none,
                                    unit,
                                    building };
struct target_key {
    object_kind kind;
    actor_key   object;
};
struct tile {
    uint32_t x, y;
};
// All row pitches are actual reserved roster sizes, not current live counts.
struct dimensions {
    uint32_t players, units, width, height, buildings;
};
struct ticket {
    world_key world;
    actor_key actor;
    uint32_t  route, leg, attempt;
};
enum class action : uint32_t { move = 1,
                               attack_unit,
                               attack_building,
                               patrol };
enum class edit_kind : uint32_t { replace = 1,
                                  append,
                                  cancel };
struct entry {
    action     kind;
    tile       requested;
    target_key target;
    uint32_t   group, command_sequence;
};
struct edit {
    world_key world;
    actor_key actor;
    edit_kind operation;
    entry     task;
    bool      legacy_sequence = false; // decode tag1 only: preserved saved command history.
};
enum class refusal : uint32_t { none,
                                stale_world,
                                stale_actor,
                                unsupported,
                                full,
                                after_patrol,
                                degenerate_cycle,
                                exhausted_identity,
                                malformed,
                                unavailable_target,
                                stale_ticket };
enum class phase : uint32_t { empty,
                              awaiting_application,
                              preparation,
                              executing,
                              blocked,
                              excursion,
                              returning_anchor,
                              ready_next };
enum class executor : uint32_t { ground_run,
                                 direction_nonplane,
                                 direction_plane };
enum class target_status : uint32_t { alive_hostile,
                                      destroyed_confirmed,
                                      unavailable };
enum class outcome_kind : uint32_t { applied,
                                     deferred,
                                     rejected,
                                     assigned,
                                     reached_exact,
                                     reached_near,
                                     planning_failed,
                                     pool_exhausted,
                                     give_up,
                                     preparation_started,
                                     preparation_finished,
                                     primary_excursion_started,
                                     primary_excursion_ended,
                                     attack_cleanup_finished,
                                     planning_started,
                                     planning_succeeded };
enum class assignment_reason : uint32_t { none,
                                          requested,
                                          formation,
                                          valid_projection };
// reached_near retains the existing successful assignment and path_slot identity. Its
// position==assigned is the actually accepted settled near tile, placement=valid_projection.
// It can differ from the old assigned endpoint without a new attempt. Never emit it for
// a failed-planner current-tile fallback or before any successful assignment.
struct outcome {
    ticket            owner;
    outcome_kind      kind;
    executor          family;
    tile              position, assigned;
    assignment_reason placement;
    uint32_t          path_slot;
    target_key        target;
};
enum class control : uint32_t { unchanged,
                                hold };
struct actor_view {
    actor_key key;
    uint32_t  prototype;
    executor  family;
    tile      position;
    uint32_t  state, order, microstep;
    bool      safe_to_apply;
    uint32_t  heading = 0, path_slot = UINT32_MAX, path_cursor = 0;
};
inline constexpr uint32_t AIR_CHUNK_STEPS = 64;
struct air_chunk {
    uint32_t slot = UINT32_MAX, count = AIR_CHUNK_STEPS;
    uint8_t  headings[AIR_CHUNK_STEPS]{};
};
struct execution {
    ticket     owner;
    entry      task;
    bool       return_to_anchor;
    tile       anchor;
    air_chunk *air = nullptr; // Synchronous borrowed request; backend writes actual attached slot.
};
enum class apply_result : uint32_t { deferred,
                                     applied,
                                     rejected };
enum class native_provenance : uint32_t { none,
                                          active_move,
                                          active_attack,
                                          genuine_patrol,
                                          autonomous_excursion,
                                          ambiguous };
struct active_capture {
    // Pending earlier native replacement MUST take precedence over executing predecessor.
    // count0 genuinely idle; count1 ordinary active/pending OR one pending new patrol point;
    // count2 actual EXECUTING legacy patrol [goal,saved home], cursor0, no guessed anchor.
    uint32_t          count;
    entry             nodes[2];
    phase             status;
    native_provenance provenance;
    tile              assigned, position;
    bool              assignment_valid;
    // An ordinary legacy direction-aircraft intent with no truthful prior assignment is planned once
    // under its new canonical owner, rather than trusting stale solver scratch.
    bool              requires_replan;
    assignment_reason placement;
    uint32_t          path_slot;
    // Original retained/applied move intent, independently of its physical assignment.
    bool logical_move_valid = false;
    tile logical_move{};
};
enum class reservation_phase : uint32_t { none,
                                          pending,
                                          bound,
                                          handoff };
struct primary_reservation {
    reservation_phase phase;
    target_key        target;
    uint32_t          delta_raw16;
    target_key        prior_target;
    uint32_t          prior_packed, execution_epoch;
};
struct backend {
    void *context;
    bool (*read_actor)(void *, actor_key, actor_view &);
    // Read-only. False for ambiguous/incompatible intent, not fabricated idle. A retained
    // pending command may have sequence0; carrier stamps its EXACT QOEX ticket only AFTER
    // successful atomic first-append capture, never during this read-only callback.
    bool (*capture_active)(void *, actor_key, active_capture &);
    // Exact lifetime, hostility AND usable weapon; never returns live coordinates.
    target_status (*read_target)(void *, actor_key, target_key);
    // Safe F-equivalent commit, including group/formation semantics and native prep.
    // No enqueue/transmit/tick/route-or-outcome reentry. Native binding hooks may read/write
    // the primary descriptor ONLY; publish typed executor outcomes after apply returns.
    // With execution.air, validate all64 physical headings before mutation, safely reuse
    // air.slot only if it is this actor's actual slot (UINT32_MAX requests allocation),
    // preserve launch/preparation semantics, attach one slot/reset cursor0/order16/state17,
    // preserve actual heading/settled animation, write the attached slot back synchronously.
    // Failure leaves every old flyable predecessor byte unchanged; no outcome reentry.
    apply_result (*apply)(void *, const execution &);
    // Settled safe boundary only: release native damage references/target, restore native
    // default, NO automatic home movement. Also settles a replacement whose attack objective
    // ended before it ever applied (cancel old predecessor, never bind/fire at dead target).
    // False defers; true is completed exactly once.
    bool (*cleanup_attack)(void *, const ticket &);
    // Pending issue-time tally must be rolled back BEFORE epoch/ownership changes. False
    // leaves native/canonical bytes unchanged. True restores prior bound/none via
    // write_reservation ONLY (descriptor-only reentry, never route/outcome/tick recursion).
    bool (*cancel_pending_reservation)(void *, actor_key);
    // Infallible detach of exact known primary: lifetime-guarded bookkeeping, clear native
    // primary ref/cache/owned bits, preserve position/path/goal/order/state/microstep. No
    // synthetic order_queued. Core clears bound descriptor and owns pending safe cleanup.
    void (*detach_primary)(void *, actor_key, target_key);
};
enum class objective : uint32_t { unresolved,
                                  destroyed_confirmed,
                                  unavailable };
struct node_view {
    entry     task;
    uint32_t  leg;
    objective target;
};
enum class failure : uint32_t { none,
                                planning,
                                path_pool,
                                native_give_up,
                                invalid_target,
                                unsupported_executor,
                                zero_progress_cycle,
                                application,
                                exhausted_identity };
struct route_view {
    ticket            active;
    phase             status;
    failure           blocked_reason;
    uint32_t          count, cycle_begin, cursor;
    tile              assigned;
    bool              assignment_valid;
    assignment_reason placement;
    uint32_t          circuit_start_cursor;
    tile              circuit_start_position;
    bool              circuit_made_progress;
    tile              implicit_anchor, excursion_anchor;
    bool              has_implicit_anchor, has_excursion, implicit_leg_active;
    executor          family;
    bool              externally_pending;
    uint32_t          path_slot;
    uint32_t          execution_epoch;
    target_key        excursion_target;
    objective         excursion_objective;
    bool              planner_succeeded;
    tile              cohort_origin;
    uint32_t          cohort_sequence, origin_state;
    bool              origin_resolved, legacy_local_origin, guided_flight;
    uint32_t          flight_mode, curve_index, chunk_ack, geometry_revision, requested_revision, geometry_count;
};
// LITTLE-ENDIAN bytes, never native structs. Schema2 header128:
// magic0/schema4/players8/units12/width16/height20/system24/planet28/world_inc32/
// totalbytes36/highwater[8]40..71/buildings72/frozen_idle_frontier[8]76..107/
// reserved108..127zero. An old idle root needs a surviving idle donor; fixed origins do not donate.
// Actor240 preserves schema1 bytes0..175. Row nodes begin240, each48, 32 slots.
// life0/route4/next_leg8/attempt12/phase16/failure20/count24/cycle_begin28/cursor32/
// assignedXY36,40/placement44/assignment_valid48/pathslot52/implicitXY56,60/flags64/
// excursionXY68,72/circuit_cursor76/startXY80,84/progress88/ring_head92/family96/
// circuit_tracking100/execution_epoch104/excursion_target108..123/objective124/
// primary_reservation128..175. COUNT0/ROUTE0 may retain a spent physical execution epoch.
// Extension: originXY176,180/origin_state184/air_mode188/join_startXY192,196/
// join_start_heading200/join_phase204/join_progress208/curve_index212/chunk_ack216/
// chunk_revision220/spec_revision224/air_slot228/return_heading232/air_spec_count236.
// origin_state: 0 unresolved new,1 frozen shared,2 frozen unknown-sequence singleton,
// 3 frozen schema1 local history,4 unresolved schema1 local history. 3/4 NEVER donate.
// 5 COUNT0 unmanaged applied-move witness,6 fixed logical move-prefix patrol origin.
// A state5 witness is history, never proof that the actor is currently moving.
// air_mode: 0 inactive,1 joining,2 curve,3 returning anchor,4 recovery orbit,
// 5 recovery orbit while return intent remains. air_slot is slot+1 (0 unattached).
// air_spec_count retains the exact old geometry while an appended route is in flight.
// Node48: action0/requestXY4,8/targetkind12/owner16/slot20/life24/group28/
// command_seq32/leg36/objective40/reserved44zero. Unused ring nodes are zero.
class state {
public:
    static uint32_t required_bytes(dimensions);
    bool            initialize(void *, uint32_t, dimensions, world_key);
    bool            bind(void *, uint32_t); // Full semantic validation, input unchanged.
    // Atomic raw schema1 metadata expansion into disjoint required_bytes(shape) storage.
    // Source and destination remain unchanged on refusal; no native callbacks/epoch edits.
    static bool migrate_schema1(void *destination, uint32_t capacity, const void *source, uint32_t bytes);
    // O(1) binding/header check. External byte mutation invalidates the binding contract;
    // snapshots must rebind/import, never write arbitrary bytes behind a live state.
    bool           valid() const;
    const uint8_t *data() const;
    uint8_t       *data();
    uint32_t       size() const;
    dimensions     shape() const;
    world_key      world() const;
    uint32_t       command_high_water(uint32_t issuer) const;
    // Call ONCE at ordered due admission before actor expansion, including refused selections.
    // Local input allocates max(canonical,pending_local)+1 once/click, never wraps.
    bool observe_command_sequence(uint32_t issuer, uint32_t sequence);
    // Read only after independently proving an active ordinary Move of this lifetime.
    bool ordinary_move_witness(actor_key, tile &) const;
    // Atomic refusal; no native mutation. First append captures active/pending native intent.
    refusal apply_edit(const backend &, const edit &);
    // Identical pure admission checks, without sequence/native/canonical publication.
    // none does not promise that a later mutable pending-lease rollback will succeed.
    refusal preview_edit(const backend &, const edit &) const;
    // Ordinary application with no QOEX owner stays UNMANAGED and cancels any old route.
    // Only exact externally-pending owner adoption retains its tail/imports assignment.
    // Carrier MUST reject stale QOEX/QOPA BEFORE native F/dequeue; this method rechecks the
    // ticket after actual F. Sequence0 is unknown legacy issue identity, never fabricated.
    // Derived backend.apply commands MUST NOT call this ordinary-adoption API.
    refusal adopt_applied(const backend &, actor_key, const entry &, const ticket *external_owner = nullptr);
    // planning_started and preparation_started advance attempt and invalidate old assignments;
    // producer must reread active ticket before reporting later assignment/arrival.
    // planning_succeeded is an actual positive planner receipt for this attempt, not an
    // endpoint or completion. A successful assigned outcome also implies this receipt.
    control observe(const backend &, const outcome &);
    // Authoritative persistent handler-budget barrier. Caller latches observe's immediate
    // result through THIS handler return. Unsafe deferred predecessors MUST continue until
    // read_actor.safe_to_apply becomes true; idle/default state is never a safety witness.
    control handler_control(const backend &, actor_key) const;
    // O(1) shared identity guard for carriers BEFORE any direct/deferred native application.
    bool owns_ticket(const ticket &) const;
    // QOEX pre-F guard: exact retained head plus actor/target lifetime/objective. May mark
    // current attack satisfied/unavailable; it NEVER releases it. False means skip native F
    // and publish rejected only AFTER actual native removal; stale tickets never mutate.
    bool pending_application_allowed(const backend &, const ticket &);
    // QOPA pre-F guard for magic/life/epoch/target-life. No issuer-origin sideband. Checks
    // executing managed PATROL and exact target hostility/weapon/lifetime; no mutations.
    // Unsafe native F still defers normally; anchor is captured only at actual application.
    refusal validate_excursion_application(const backend &, actor_key, uint32_t expected_epoch, target_key) const;
    // Current native pursuit/shot guard. Returning/blocked/retired targets are unavailable;
    // unsafe interpolation may continue, but a reused target slot is NEVER a valid victim.
    target_status excursion_target_status(const backend &, actor_key) const;
    // Before target-consuming handlers: false may_access skips ONLY target dereferences;
    // unsafe movement must continue. Safe cleanup follows the same policy used by tick.
    control  guard_primary(const backend &, actor_key, bool &may_access);
    void     note_primary_detached(actor_key); // actual owned detach observation, no new ownership
    bool     primary_cleanup_pending(actor_key) const;
    uint32_t execution_epoch(actor_key) const; // exact LIFE, including COUNT0 retained high-water
    // Spend a physical issue identity without creating a route, task or executor authority.
    bool reserve_physical_epoch(actor_key, uint32_t &result);
    // One deterministic pre-unit pass, after explicit orders/storage retry. At most one new
    // activation per actor/pass. No idle inference, retry loop, group barrier or random draw.
    void tick(const backend &);
    // Settled microstep31, before consuming the next native direction. Exactly one
    // cursor increment acknowledges one actual segment; duplicate calls are idempotent.
    // owned_at_entry distinguishes a failed owned refill from foreign/native activity.
    bool aircraft_boundary(const backend &, actor_key, uint32_t native_slot, uint32_t native_cursor,
                           bool *owned_at_entry = nullptr);
    void retire_actor(actor_key);
    // Native accounting must be released before actual actor retirement. Target retirement
    // returns false if its required pending rollback cannot complete: NEVER reuse that
    // target lifetime until this idempotent lifecycle fanout returns true.
    bool    retire_target(const backend &, target_key, bool destruction_confirmed);
    refusal cancel_actor(const backend &, actor_key);
    bool    read_route(actor_key, route_view &) const;
    bool    read_entry(actor_key, uint32_t logical_index, node_view &) const;
    // Descriptor-only observations do NOT acquire a route. Actual ordinary binding/clear
    // may initialize LIFE without a route. reserve_physical_epoch alone spends an issue epoch.
    bool read_reservation(actor_key, primary_reservation &) const;
    bool write_reservation(actor_key, const primary_reservation &);
    void clear_reservation(actor_key);
    bool checkpoint_ready() const; // false during synchronous handoff, never serialize it
    // Import-boundary cross-region validation. Only unresolved targets must still be live;
    // satisfied/unavailable nodes deliberately retain old keys across target slot reuse.
    bool     validate_lifetimes(void *, bool (*matches)(void *, object_kind, actor_key)) const;
    uint32_t encoded_bytes() const;
    bool     encode(void *, uint32_t) const;
    // inspect/validate accept schema1 or2; import atomically expands schema1 metadata.
    // Raw schema1 regions are NOT bindable; callers must allocate required_bytes and import.
    static bool inspect(const void *, uint32_t, dimensions &, world_key &);
    static bool validate(const void *, uint32_t, dimensions, world_key);
    bool        import(const void *, uint32_t); // Full validation before exact incoming-byte publication.
private:
    uint8_t *bytes_       = nullptr;
    uint32_t bytes_count_ = 0;
};
inline constexpr uint16_t COMMAND = 0xfe;
// Kind0 code=param0=fe; all13 args defined. args0 packs schema/edit/action in low3 bytes,
// args1 actor-life,2 system,3 planet,4 world-life,5 X|(Y<<16),6 kind|(owner<<8),
// 7 target-slot,8 target-life,9 group,10 command-sequence,11/12zero. Coords<256.
// Owner nibble and unit_index supply actor owner/slot. Cancel has zero task except sequence.
bool encode_edit(const edit &, int32_t (&)[13]);
bool decode_edit(uint16_t owner_and_kind, uint32_t slot, const int32_t (&)[13], edit &);
bool independent(uint16_t, uint16_t, uint16_t, uint16_t);

// One campaign-global allocator; planet members carry QORD, never reset this clock. Canonical
// QCAM LE160: magic0/schema4/last_world_inc8/reserved12/world_inc[32]16..143/reserved144..159.
class campaign_state {
public:
    static constexpr uint32_t BYTES = 160;
    bool                      initialize(void *, uint32_t);
    bool                      bind(void *, uint32_t);
    bool                      valid() const;
    static bool               validate(const void *, uint32_t);
    const uint8_t            *data() const;
    uint8_t                  *data();
    uint32_t                  size() const;
    uint32_t                  last_world_inc() const;
    uint32_t                  world_inc(uint32_t planet) const;
    // Overwrites only the chosen slot with a globally fresh checked ID; no map-seed identity.
    bool fresh_world(uint32_t planet, uint32_t &out);
    // Real nonzero planet-status publication, including off-system progress grants.
    // Retain an existing ID; first knowledge allocates without resetting any route owner.
    bool ensure_known(uint32_t planet, uint32_t &out);
    // Empty allocator only. Known mask plus current planet receive IDs in ascending slot order.
    // Full legacy load calls this once; member loads never seed/reset the global allocator.
    bool seed_legacy(uint32_t known_mask, uint32_t current_planet, uint32_t &out);
    bool import(const void *, uint32_t); // Exact incoming state, never max(local,incoming).
private:
    uint8_t *bytes_ = nullptr;
};
} // namespace mh::order_tasks
