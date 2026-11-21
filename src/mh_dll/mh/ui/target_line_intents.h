// Read-only selected-order presentation. No part of this module is simulation authority.
#pragma once
#include <cstdint>
#include "gfx/strat_world_view.h"
#include "ui/target_line_painter.h"

struct libmh_order_observation;
struct libmh_order_witness_store;
namespace mh::order_witness {
struct canonical_order;
}

namespace mh::ui::selected_targets::detail {
enum class kind : uint8_t { move,
                            patrol,
                            patrol_home,
                            attack,
                            secondary,
                            enter,
                            exit,
                            deploy,
                            rally };
inline constexpr unsigned selection_capacity = 200;
inline constexpr unsigned intent_capacity    = selection_capacity * 3 + 2;
struct intent {
    gfx::stratview::point source{}, origin{}, destination{}, group_origin{}, ground_destination{};
    uint64_t              command_token           = 0; // observed common issuer command; zero means unknown, not a singleton command
    uint64_t              secondary_command_token = 0;
    uint32_t              entity = 0, incarnation = 0, target_incarnation = 0;
    kind                  type      = kind::move;
    uint16_t              source_id = 0;
    uint8_t               mover = 0, player = 0;
    bool                  visible = false, also_secondary = false, building_source = false;
};
unsigned collect(const gfx::stratview::world_view &view, gfx::stratview::world_units &mapped,
                 unsigned player, const uint16_t *ids, unsigned number_ids,
                 unsigned selected_building, intent *out, unsigned capacity);
// Setup allocations for unit history and all-building presentation lives. No frame heap or eviction.
bool reserve_intents();
// Mandatory at EVERY world replacement, including same-sized loads, snapshot/resync and replay
// imports. The serialized storage-policy incarnation alone cannot detect a restored old world.
void                       reset_intents();
libmh_order_witness_store *selected_targets_order_witness_store();
void                       observe_order_lifecycle(const libmh_order_observation &observation);
// POST-success exact metadata rekey, scoped to the certified lifecycle pair's admission ID.
void amend_intent_metadata(const mh::order_witness::canonical_order &before,
                           const mh::order_witness::canonical_order &after);
// All buildings, not just storage families. These lives are never serialized or hashed.
uint32_t building_incarnation(unsigned player, unsigned slot);
void     observe_building_birth(unsigned player, unsigned slot);
void     observe_building_retirement(unsigned player, unsigned slot);
// Unconditional PRE-dispatch and POST-dispatch/PRE-unit-tick observation, even with recording OFF.
// POST must run only for the corresponding dispatcher invocation. An abandoned PRE is discarded.
// Reads caps::orders' actual wide/stock queue, never its canonical save mirror.
// Replay replacement revokes transient attack provenance before reading the new queue.
// Presentation-only precedence for the exact still-unapplied ordinary admission.
// Never an ownership, command-application, completion or task-progression query.
bool pending_ordinary_primary(unsigned player, unsigned slot, uint32_t actor_life);
void observe_dispatch(bool replay_replaced = false);
void observe_after_dispatch();
// Actual successful effect only: native fire_weapon's projectile-spawn SUCCESS returns, or its
// direct-damage CALL completion; translated equivalents are the same calls-struct consumers.
// Never call at fire_weapon/fire_at_target2_if_aimed entry or mere target acquisition.
void observe_weapon_fired(unsigned player, unsigned unit_id, uint16_t target_ref, uint16_t target_id);
// Shared monotonically increasing presentation IDs; never written to simulation state.
uint64_t allocate_intent_token();
// Native-only synchronous adapters; queue indices never cross the queued host ABI.
void observe_attack_assignment(unsigned player, unsigned unit_id, unsigned queue_index, bool secondary);
void observe_deferred_commit(unsigned player, unsigned unit_id, unsigned queue_index);
// Owned canonical command; actor comes from its header, channel from flags.
void observe_attack_assignment(const libmh_order_observation &observation);
void observe_deferred_commit(const libmh_order_observation &observation);
} // namespace mh::ui::selected_targets::detail
