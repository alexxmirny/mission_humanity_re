// Destination: src/mh_dll/mh/seams/order_tasks_hosted.h
#pragma once
#include "include/mh_order_tasks_host.h"
namespace mh::order_tasks::hosted {
state                      &owner();
campaign_state             &campaign_owner();
const backend              &live_backend();
const reservations::access &live_accounting();
void                        retained_view(uint32_t lane, uint32_t first_dead, uint32_t end_dead);
bool                        live();
// Optional bounded-watch diagnostics only; never used by a gameplay decision.
bool diagnostics_enabled();
// Capture/checkpoint boundary gate, including lifecycle quarantine; raw validity is insufficient.
bool checkpoint_ready();
// Exact pre-mutation disk/fresh boundary, or post-atomic-copy/pre-rederive snapshot boundary.
// Preserves QORD/QCAM bytes. Result is an opaque prior-state token; UINT32_MAX is failure.
uint32_t begin_load(host_protocol::begin_load_reason reason);
// Only before any publication mutation, or after complete byte rollback. Partial native
// file-load failure stays quarantined and MUST NOT call this.
void abort_load(uint32_t prior);
// Called at quiescent setup; reserve maximum supported rows once, retain same-shape bytes.
bool reserve_after_caps();
bool bind_regions(); // both native reset/bind paths, before any hash/capture/host-in consumer
bool setup(uint32_t width, uint32_t height, world_key);
void prepare_fresh_continuation(uint32_t player, uint32_t slot);
// Restore/rebind only after complete composed save/resync validation. No identity allocation here.
bool           bind_published();
bool           new_campaign();
uint32_t       ensure_known_planet(uint32_t planet);
bool           fresh_world(uint32_t system, uint32_t planet, uint32_t width, uint32_t height);
bool           reset_world();
const uint8_t *region_data();
uint8_t       *region_data_mut();
uint32_t       region_bytes();
const uint8_t *campaign_data();
uint8_t       *campaign_data_mut();
uint32_t       campaign_bytes();
int32_t        invoke(const mh_order_tasks_request *, mh_order_tasks_result *);
actor_key      identity(void *, object_kind, uint32_t player, uint32_t slot);
bool           read_route(uint32_t player, uint32_t slot, route_view &);
bool           read_entry(uint32_t player, uint32_t slot, uint32_t logical_index, node_view &);
// Exact application record, not a presentation witness. Called from existing applied hooks.
void applied(uint32_t player, uint32_t slot, uint16_t owner_and_kind, uint16_t code,
             int16_t param0, const int32_t (&)[13], tile pre_application_anchor);
bool consume_command(uint16_t owner_and_kind, uint16_t code, uint16_t param0,
                     uint32_t slot, const int32_t (&)[13], uint32_t retained_prefix);
void tick();
void retire_unit(uint32_t player, uint32_t slot, bool destruction_confirmed);
void retire_building(uint32_t player, uint32_t slot, bool destruction_confirmed);
void cancel(uint32_t player, uint32_t slot);
// Snapshots are per native planner/handler invocation. Old tokens cannot match replacement.
bool    snapshot(uint32_t player, uint32_t slot, ticket &, executor *family = nullptr);
control outcome_for(const ticket &, outcome_kind, tile assigned = {},
                    assignment_reason = assignment_reason::none, uint32_t path_slot = UINT32_MAX,
                    uint32_t producer_line = 0);
bool    should_hold(uint32_t player, uint32_t slot);
bool    suppress_home(uint32_t player, uint32_t slot);
// Pure encoded-path validation. Ground runs and direction bytes use different grammars.
bool path_endpoint(uint32_t player, uint32_t slot, executor, tile &endpoint);
bool decode_applied(void *, actor_key, uint16_t code, int16_t param0,
                    const int32_t (&args)[13], entry &);
} // namespace mh::order_tasks::hosted
