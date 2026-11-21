// The only hosted mutable policy owner. Compile this seam into mh.dll ONLY.
#pragma once
#include "include/mh_storage_policy.h"

namespace mh::storage_policy::hosted {
// Before setup, region_data() is a valid 32-byte empty-world header (all dimensions zero).
// Allocate once using final roster ROWS, not toggled limits. Same shape preserves state; a map
// extent change resets in-place. Refresh the host-native registry span after the first setup.
// reset_world reseeds the current loaded roster, default Off, without qualifying any occupant.
bool           reserve_after_caps(); // full row extent, valid unmapped header (width=height=0)
bool           setup(uint32_t width, uint32_t height);
bool           reset_world();
int32_t        invoke(const mh_storage_policy_request *request, mh_storage_policy_result *result);
state         &owner();
const uint8_t *region_data();
uint8_t       *region_data_mut();
uint32_t       region_bytes();
rally_view     read_rally(uint32_t player, uint32_t building_slot);
identity       building_identity(uint32_t player, uint32_t building_slot);
identity       unit_identity(uint32_t player, uint32_t unit_slot);
bool           eligible_building(uint32_t player, uint32_t building_slot);
// Every live building receives a canonical incarnation; storage commands still require eligibility.
bool building_exists(uint32_t player, uint32_t building_slot);
// Consumes only kind-0 administrative settings, validates discriminator/payload/identity.
// Returns true also for malformed settings, so they cannot fall into a native handler.
bool consume_command(uint16_t owner_and_kind, uint16_t code, uint32_t slot, const int32_t (&args)[13]);
bool birth_building(uint32_t player, uint32_t slot);
bool birth_unit(uint32_t player, uint32_t slot);
void retire_building(uint32_t player, uint32_t slot);
void retire_unit(uint32_t player, uint32_t slot);
void cancel_pending(uint32_t player, uint32_t unit_slot);
bool apply_command(uint32_t actor, const command &cmd);
// Production callback only from the production-success arm, never generic dock insertion.
void production_inserted(uint32_t player, uint32_t unit_slot);
// Called only after paid repair success; energy_before is captured before the actual paid pulse.
void repair_paid(uint32_t player, uint32_t unit_slot, double energy_before);
void retry();
// Manual launch executor uses this at state/goal commit, not at issue time. False retains native goal.
bool manual_rally(uint32_t player, uint32_t unit_slot, uint32_t &x, uint32_t &y);
} // namespace mh::storage_policy::hosted
