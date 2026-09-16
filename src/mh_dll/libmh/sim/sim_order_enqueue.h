//
// sim/sim_order_enqueue.h -- the order-dispatch ENQUEUE handlers (RI-SIM / SIM1C, slices 1/2/4).
//
// The order-container notes (O2) drew the line: the order QUEUE CONTAINER (mh::orders) owns the three
// arrays and the scratch-args channel; the 132 typed wrappers that DECIDE to enqueue an order --
// reading roster/cfg state, picking a weapon, converting a fine coordinate to a tile -- are the SIM
// and belong here. Slices 1-2 (2026-08-08) were the `_enqueue` (immediate-lane) half of a few order
// kinds plus their small building-order siblings; the big dispatch switch (llm_strat_order_queue_
// dispatch) got its own module (sim_order_dispatch.h). This adds the batch's remaining
// plain issue/enqueue wrappers: the AI-tick-driven `_auto` twins of the exit-storage/move family, the
// default-order issuer, the two control-group notify thunks, and the raw order-param setter.
//
// TWO BATCH-C MEMBERS ARE DELIBERATELY NOT HERE, AND NEVER WILL BE: llm_strat_order_dispatch
// (@0x00465fdf) and llm_strat_order_stage_scheduled (@0x00466211) are ALREADY fully reimplemented,
// T1 shadow-verified, and PROMOTED under RI-ORDERS (mh::orders::dispatch / stage_scheduled, O2/O3,
// done 2026-07-28 -- see the order-container notes). The sim closure reaches
// them (batch C carries them as layer 8/9, `not_started` in the raw ledger), but a second C++ body
// for the same game function would violate Law 4 and fork one address's behaviour across two
// modules. This module's `calls.order_dispatch` member calls out to that address exactly like every
// other callee here (mh::call::, independent per-function verification) -- since orders is promoted,
// it already runs mh::orders::dispatch transparently. See SIM1C progress for
// how their ledger rows were closed.
//
// NONE OF THESE WRITE SIM STATE. Every one of them only READS units/buildings/cfg (sim_view) and
// calls OUT to: the order container's scratch/enqueue API, and a handful of already-committed
// original helpers (coordinate lookups, weapon selection, target classification, storage queries).
// So there is no sim_store here -- only a view and a `calls` table.
//
// EVERY CALLEE GOES THROUGH `calls`, NOT `mh::call::` DIRECTLY -- same reason as ai/ai_state.h's
// `ai_calls` and orders/order_queue.h's `game_calls`: a direct `mh::call::` in a `detail::` function
// reaches into the live game image, which makes the body untestable by `net_selftest.exe simtest`
// (no game, no rig) and unusable in the offline fixture. Production binds `calls` to `mh::call::*`
// (live_calls()); simtest binds it to recording stubs.
//
// THE ORDER-CONTAINER CALLS ARE NOT SPECIAL-CASED. `order_scratch_reset` / `_set_field` / `_enqueue`
// go through `calls` exactly like every other callee here, bound to `mh::call::llm_strat_order_*` in
// production -- NOT to `mh::orders::scratch_reset()` etc. directly. That raw-VA call is what lets
// RI-ORDERS' own promotion (`[promote] orders=1`) transparently redirect it to `mh::orders`' C++ once
// that lands, with nothing here needing to change or know. ai/ai_state.h's `live_calls()` binds its
// three order callees the identical way, for the identical reason.
//
// SHADOW SAFETY -- every callee here is either a PURE QUERY (coordinate lookups, weapon/target
// classification, storage queries -- no state write) or writes a region already declared as an
// `extra_regions` claim for the site (the order container's SCRATCH_ARGS/QUEUE, or `units` via
// unit_notify_status). None of them escape observably (no socket, no dialog, no menu teardown), so
// unlike orders/order_queue.h's `game_calls` this module needs no `inert_calls()` -- every call in
// `calls` runs FOR REAL in both arms and the restore between arms undoes the state ones.
//
#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// The order record's kind-nibble tags packed into `owner_and_kind` alongside the low-nibble player id
// (the order-container notes' record accounting) -- ATTACK_TARGET/ATTACK_UNIT tag the TARGET as 0x80
// (unit) or 0x40 (building); the ENQUEUE CALLER itself is always tagged 0x80 (it is always a unit).
inline constexpr uint32_t ORDER_KIND_UNIT     = 0x80u;
inline constexpr uint32_t ORDER_KIND_BUILDING = 0x40u;

// llm_strat_unit_state members this file tests. Not emitted as a real enum by the struct generator
// (Ghidra's cfg-enum comment tag is documentation only -- see sim_state.h's cfg_unit_weapon_id note
// on the same limitation), so named locally like ai/ai_state.h's UNIT_TYPE_* constants.
inline constexpr uint16_t UNIT_STATE_PARKED    = 0x1f; // map_object_unit.state -- docked, idle
inline constexpr uint16_t UNIT_STATE_EXIT_WAIT = 0x22; // map_object_unit.state -- mid-exit, waiting

// cfg_enum_E_UNIT_TYPE members the attack-target family branches on (Ghidra enum dump, 2026-08-08).
// Local, not shared with ai/ai_state.h's own UNIT_TYPE_* constants -- sim/ deliberately does not
// depend on ai/ (see sim_state.h's header note on cfg_unit/cfg_building duplication for the reason).
inline constexpr uint32_t UNIT_TYPE_A_HELI       = 0x0f;
inline constexpr uint32_t UNIT_TYPE_H_HELI       = 0x10;
inline constexpr uint32_t UNIT_TYPE_A_PLANE      = 0x11;
inline constexpr uint32_t UNIT_TYPE_H_PLANE      = 0x12;
inline constexpr uint32_t UNIT_TYPE_A_HELI_CARGO = 0x17;
inline constexpr uint32_t UNIT_TYPE_H_HELI_CARGO = 0x18;

// map_object_building.built_flags -- bit 0x1 connected, bit 0x2 staffed (see the field's own comment
// in addr/mh_structs.gen.h). auto_launch_from_storage's dock-ready gate is the "== 3" idiom named
// there: connected AND staffed.
inline constexpr uint8_t BUILT_FLAGS_OPERATIONAL = 0x3;

// llm_strat_unit_state members unit_order_state_is_settled tests, on BOTH the unit's `order` and
// `state` fields (Ghidra applies the same enum to both -- the settled-pair check is cross-field, not
// two independent domains). Ghidra enum dump 2026-08-08 (get-data-type-by-string llm_strat_unit_state);
// PARKED/EXIT_WAIT above are the same enum, already named for auto_launch_from_storage's own state
// checks -- not repeated here.
inline constexpr uint16_t UNIT_STATE_STOP_TO_DEFAULT = 0x01;
inline constexpr uint16_t UNIT_STATE_PATROL_SWAP     = 0x10;
inline constexpr uint16_t UNIT_STATE_MOVE_PATH       = 0x11;
inline constexpr uint16_t UNIT_STATE_IDLE_SCATTER    = 0x13;
inline constexpr uint16_t UNIT_STATE_HOVER_ENGAGE    = 0x2e;

// cfg_enum_E_BUILDING members bldg_order_repair_cycle_start_enqueue's guard excludes (a main base is
// never gated on its own charge cycle this way). Ghidra enum dump 2026-08-08.
inline constexpr uint8_t BUILDING_TYPE_A_MAIN_BASE = 0x0e;
inline constexpr uint8_t BUILDING_TYPE_H_MAIN_BASE = 0x22;

// The remaining cfg_enum_E_BUILDING members bldg_instant_construct_find_slot_enqueue's sub-roster
// switch dispatches on (Ghidra enum dump 2026-08-12; A_BIURO/H_BYURO are real members with no case in
// that switch, hence absent here). NOT contiguous per A/H side (H_RELAY skips 0x21->0x23, H_CIVIL
// skips past H_BYURO) -- values are the enum's, not renumbered.
inline constexpr uint8_t BUILDING_TYPE_A_PRODUCTION = 0x01;
inline constexpr uint8_t BUILDING_TYPE_A_MINE       = 0x02;
inline constexpr uint8_t BUILDING_TYPE_A_PLANT      = 0x03;
inline constexpr uint8_t BUILDING_TYPE_A_COLONY     = 0x04;
inline constexpr uint8_t BUILDING_TYPE_A_TURRET     = 0x05;
inline constexpr uint8_t BUILDING_TYPE_A_MOTHER     = 0x06;
inline constexpr uint8_t BUILDING_TYPE_A_BARRAKS    = 0x07;
inline constexpr uint8_t BUILDING_TYPE_A_GARAGE     = 0x08;
inline constexpr uint8_t BUILDING_TYPE_A_AIRFIELD   = 0x09;
inline constexpr uint8_t BUILDING_TYPE_A_HELIPAD    = 0x0a;
inline constexpr uint8_t BUILDING_TYPE_A_LAB        = 0x0b;
inline constexpr uint8_t BUILDING_TYPE_A_PORT       = 0x0c;
inline constexpr uint8_t BUILDING_TYPE_A_SHUTTLE    = 0x0d;
inline constexpr uint8_t BUILDING_TYPE_A_RELAY      = 0x0f;
inline constexpr uint8_t BUILDING_TYPE_A_SILOS      = 0x10;
inline constexpr uint8_t BUILDING_TYPE_A_CIVIL      = 0x12;
inline constexpr uint8_t BUILDING_TYPE_H_PRODUCTION = 0x15;
inline constexpr uint8_t BUILDING_TYPE_H_MINE       = 0x16;
inline constexpr uint8_t BUILDING_TYPE_H_PLANT      = 0x17;
inline constexpr uint8_t BUILDING_TYPE_H_COLONY     = 0x18;
inline constexpr uint8_t BUILDING_TYPE_H_TURRET     = 0x19;
inline constexpr uint8_t BUILDING_TYPE_H_MOTHER     = 0x1a;
inline constexpr uint8_t BUILDING_TYPE_H_BARRACKS   = 0x1b;
inline constexpr uint8_t BUILDING_TYPE_H_GARAGE     = 0x1c;
inline constexpr uint8_t BUILDING_TYPE_H_AIRFIELD   = 0x1d;
inline constexpr uint8_t BUILDING_TYPE_H_HELIPAD    = 0x1e;
inline constexpr uint8_t BUILDING_TYPE_H_LAB        = 0x1f;
inline constexpr uint8_t BUILDING_TYPE_H_PORT       = 0x20;
inline constexpr uint8_t BUILDING_TYPE_H_SHUTTLE    = 0x21;
inline constexpr uint8_t BUILDING_TYPE_H_RELAY      = 0x23;
inline constexpr uint8_t BUILDING_TYPE_H_SILOS      = 0x24;
inline constexpr uint8_t BUILDING_TYPE_H_CIVIL      = 0x26;

// The order-record args[] slot each family stashes its payload in (the order-container notes: the
// scratch channel is a positional protocol per order kind, not a shared shape). Two families appear
// here: the ATTACK_TARGET/_ALT/_BUILDING_REPOSITION_ALT kind uses 0/1/3/4/5/6/7; ATTACK_UNIT/
// ATTACK_BUILDING uses 7/8/9/10/0xb. Named per-family so a call site reads as intent, not a raw index.
namespace scratch_field {
// ATTACK_TARGET / ATTACK_TARGET_ALT / ATTACK_BUILDING_REPOSITION_ALT (orders 0x1a/0x1b/0x1c)
inline constexpr int32_t TARGET_TILE_X = 0;
inline constexpr int32_t TARGET_TILE_Y = 1;
inline constexpr int32_t TARGET_FINE_X = 3;
inline constexpr int32_t TARGET_FINE_Y = 4;
inline constexpr int32_t TARGET_OWNER  = 5;
inline constexpr int32_t TARGET_INDEX  = 6;
inline constexpr int32_t TARGET_WEAPON = 7;
// ATTACK_UNIT / ATTACK_BUILDING (orders 0x1e/0x1d)
inline constexpr int32_t AU_WEAPON       = 7;
inline constexpr int32_t AU_FINE_X       = 8;
inline constexpr int32_t AU_FINE_Y       = 9;
inline constexpr int32_t AU_TARGET_OWNER = 10;
inline constexpr int32_t AU_TARGET_INDEX = 0xb;
} // namespace scratch_field

// Every external callee this closure reaches, indirected for offline testability -- see the header
// note above. Grouped by the order-container triad first (every function here calls all three),
// then the roster/coordinate/classification queries each individual handler needs a subset of.
struct calls {
    // mh::orders' public surface -- see the header note on why these are NOT called directly.
    void (*order_scratch_reset)();
    void (*order_scratch_set_field)(int32_t index, int32_t value);
    int32_t (*order_enqueue)(uint16_t unit_index, uint16_t owner_and_kind, int16_t param0,
                             uint16_t order_code);

    // Pure queries -- see the header note; none of these write state. Out-params carry the committed
    // pointee (TACT1-P C6, 2026-09-04) to match the mh::call:: signatures exactly (function-pointer
    // types must match exactly for the aggregate-initializer binding in live_calls() to compile) --
    // callers pass `&local`.
    void (*unit_get_coords)(uint16_t player, int32_t unit_index, int32_t *out_fine_x,
                            int32_t *out_fine_y);
    void (*bldg_get_coords)(uint16_t player, int32_t building_index, int32_t *out_fine_x,
                            int32_t *out_fine_y);
    uint8_t (*unit_select_weapon)(uint16_t player, int32_t unit_index, uint32_t target_mask);
    int32_t (*target_class)(uint32_t owner_and_kind_flag, int32_t roster_slot);
    uint32_t (*unit_in_weapon_range)(int32_t player, int32_t unit_index, int32_t tile_x,
                                     int32_t tile_y, int32_t target_class_flags);
    int32_t (*unit_state_is_boarding)(int32_t state);
    void (*bldg_footprint_random_offset)(uint32_t player, uint32_t unit_index,
                                         int32_t target_player, int32_t target_bldg_idx,
                                         uint32_t *out_fine_x, uint32_t *out_fine_y);
    int32_t (*storage_type_accepts_unit)(uint32_t building_index, uint16_t unit_proto_id);
    void (*storage_get_approach_tile)(uint16_t player, uint16_t unit_index, uint32_t *out_fine_x,
                                      uint32_t *out_fine_y, uint32_t storage_idx);

    // WRITES `units` (the order-container notes' game_calls note) -- declared as an extra_regions
    // claim wherever it is reached, then run for real; see the header note.
    void (*unit_notify_status)(uint32_t player, int32_t unit_index, uint32_t status_code);

    // llm_strat_unit_order_move_enqueue -- the frontier callee unit_order_exit_storage_enqueue falls
    // back to when its home storage no longer accepts it. TRANSLATED in THIS slice (third); still
    // bound to mh::call:: here rather than to the sibling detail:: function directly -- same reason
    // order_scratch_reset/_enqueue stay bound to mh::call:: even though RI-ORDERS reimplemented them:
    // each function is shadow-verified independently until ITS OWN promotion, so a caller never
    // reaches for a sibling's C++ body early. Pure with respect to sim state (writes only the order
    // container plus `units` via its own unit_notify_status call).
    void (*unit_order_move_enqueue)(uint16_t player, int32_t unit_idx, uint32_t x, uint32_t y,
                                    uint32_t move_flag);

    // llm_strat_order_collect_available_projects_enqueue @0x0046f6c1 -- the frontier callee
    // order_collect_available_projects_thunk tail-jumps to. TRANSLATED in THIS slice (third); bound
    // to mh::call:: for the same independent-verification reason as unit_order_move_enqueue above.
    // Writes only the order container through its own order_enqueue (0xee/0xee, no scratch fields
    // set).
    void (*order_collect_available_projects_enqueue)(uint16_t player);

    // llm_strat_unit_order_move_auto @0x0046a56d -- unit_order_exit_storage_auto's fallback when the
    // storage no longer accepts the unit. Same shape as unit_order_move_enqueue minus the move_flag
    // scratch field; both TRANSLATED in this slice, both bound to mh::call:: for the same reason.
    void (*unit_order_move_auto)(uint16_t player, int32_t unit_idx, uint32_t x, uint32_t y);

    // llm_strat_order_dispatch @0x00465fdf -- the lockstep-aware router the ctrlgrp thunks funnel
    // through. ALREADY REIMPLEMENTED AND PROMOTED under RI-ORDERS (mh::orders::dispatch, O2/O3, done
    // 2026-07-28, T1 shadow-clean 5800 calls both peers) -- NOT part of this or any sim slice, and
    // this module must not gain a second C++ body for it (Law 4). Bound to mh::call:: here for the
    // same independent-per-function-verification reason as every other member; since orders is
    // promoted, this address already runs mh::orders::dispatch transparently.
    int32_t (*order_dispatch)(uint16_t unit_id, uint32_t player, uint16_t op_code, uint16_t arg);
};

const calls &live_calls();

namespace detail {

// llm_strat_order_recruit_unit_enqueue @0x0046d9a9. Order 0xf3/0xf3, arg[1] = unit_id. Always
// returns 1 (the original never tests the enqueue's own result).
int32_t order_recruit_unit_enqueue(const calls &gc, uint32_t unit_id, uint32_t player_id);

// llm_strat_order_queue_construction_enqueue @0x0046da70. Order 0xf2/0xf2 (dispatch case 0xd ->
// llm_bldg_queue_construction per the plate's own trace). args[4]/[5]/[0] are an opaque payload to
// this container-adjacent layer -- the plate's best guess is a tile position and a building type, but
// that is unverified (sole caller unresolved), so the parameters are named by SLOT, not by guess.
int32_t order_queue_construction_enqueue(const calls &gc, int32_t arg4, int32_t arg5, int32_t arg0,
                                         uint16_t player);

// llm_strat_bldg_order_production_add_enqueue @0x0046db58. Order 0x6d/0x6d -- despite the symbol name
// this is a PRODUCTION QUEUE ADD, not a repair (see the plate: the dispatch handler at 0x0046857c
// bumps productions[].queued_count, no repair semantics anywhere). The family-wide rename is
// deliberately deferred (docs note in the .c plate) -- kept here under its current name. Gated on the
// producer building's online_state; NO scratch_reset (the handler reads only args[0]/[1], so whatever
// an earlier caller left in the other 11 slots is immaterial -- faithful to the original, not cleaned
// up).
void bldg_order_production_add_enqueue(const sim_view &v, const calls &gc, uint16_t player,
                                       int32_t bldg_idx, int32_t unit_type);

// llm_strat_bldg_order_assign_workers_enqueue / _unassign_workers_enqueue @0x0046e005 / 0x0046e0a3.
// Order 0x7e/0x7e and 0x7f/0x7f, arg[1] = worker_count. Also NO scratch_reset (same reasoning).
void bldg_order_assign_workers_enqueue(const calls &gc, uint16_t player, uint16_t bldg_idx,
                                       int32_t worker_count);
void bldg_order_unassign_workers_enqueue(const calls &gc, uint16_t player, uint16_t bldg_idx,
                                         int32_t worker_count);

// llm_strat_unit_order_exit_storage_enqueue @0x0046a918. If the unit's home storage no longer
// accepts it (building offline, or the storage's building type stopped accepting the unit's proto),
// falls back to a plain move order via llm_strat_unit_order_move_enqueue (a frontier callee, still
// original) using the storage's per-slot exit tile and the player's own order-sequence counter --
// the ONE function in this slice that writes sim state directly (the seq-id byte, INC'd with a
// wraparound-skip-zero -- 0 is never revisited), hence the `sim_store &` no sibling here needs.
// Otherwise enqueues order 0x38 (unit can teleport-exit, move_op_arg == 10) or 0x29/0xb (needs an
// approach tile) and notifies the unit. `param_4`/`param_5` (positions 4/5 of the committed
// 5-register prototype) are UNUSED by this body -- param_4 is read nowhere, param_5's incoming value
// is overwritten before any use -- kept in the signature because that is the committed calling
// convention this function shares with its family.
//
// `player` is uint16_t here (unlike the public wrapper's committed uint32_t) to match the original,
// which MOVZX-narrows it to 16 bits before every one of its ~12 memory accesses -- every sibling
// detail:: function in this file already does this; reimpl-verify (2026-08-08) confirmed both real
// callers zero-extend before the call, so the widening was unreachable, not wrong, but there is no
// reason to be the one function in the file that carries it.
void unit_order_exit_storage_enqueue(const sim_view &v, sim_store &own, const calls &gc,
                                     uint16_t player, uint32_t unit_idx, int32_t storage_idx,
                                     uint32_t param_4, uint32_t param_5);

// llm_strat_unit_order_attack_target_enqueue / _attack_target_alt_enqueue @0x0046b271 / 0x0046b8ec.
// Byte-identical bodies except ONE tag (0x1a vs 0x1b) that appears as param0 in the out-of-range arm
// and as order_code in the default arm -- see the .cpp for why that is a single parameter and not two
// near-duplicate functions. `weapon_id == 0xffffffff` means "auto-select" (elevation-gated ground/air
// target mask); any other value is used as-is.
void unit_order_attack_target_enqueue(const sim_view &v, const calls &gc, uint16_t player,
                                      int32_t unit_idx, uint16_t target_player, int32_t target_idx,
                                      uint32_t weapon_id, uint16_t tag);

// llm_strat_unit_order_attack_unit_enqueue @0x0046bcfd. Order 0x1e/0x1e, no branching on unit type or
// weapon range -- always the one enqueue.
void unit_order_attack_unit_enqueue(const sim_view &v, const calls &gc, uint16_t player,
                                    int32_t unit_idx, uint16_t target_player, int32_t target_unit_idx,
                                    uint32_t weapon_id);

// llm_strat_unit_order_attack_building_enqueue @0x0046c7a0. Order 0x1d/0x1d. Target coords come from
// llm_strat_bldg_get_coords + a footprint-random-offset jitter (attacking a building targets a
// point on its footprint, not one fixed tile); weapon auto-select always uses target_mask=1
// (buildings have no elevation).
void unit_order_attack_building_enqueue(const sim_view &v, const calls &gc, uint16_t player,
                                        int32_t unit_idx, uint16_t target_player,
                                        int32_t target_bldg_idx, uint32_t weapon_id);

// llm_strat_unit_order_attack_building_reposition_alt_enqueue @0x0046c21c. Order 0x1c. Unlike the
// attack_target pair there is no plane/heli special case: out of range (or boarding, or not a
// move_op_arg==10 mover) enqueues the same "reposition" order the target family uses; IN range,
// footprint_random_offset is called a SECOND time (re-jittering the aim point) and the tile/fine
// scratch fields are overwritten with the fresh point before the direct-attack enqueue -- the target
// owner/index/weapon fields from the first pass are left as they were.
void unit_order_attack_building_reposition_alt_enqueue(const sim_view &v, const calls &gc,
                                                       uint16_t player, int32_t unit_idx,
                                                       uint16_t target_player,
                                                       int32_t target_bldg_idx, uint32_t weapon_id);

// llm_strat_unit_order_auto_launch_from_storage_enqueue @0x0046cf2c. Two independent gates on the
// unit's OWN state: PARKED and docked in an operational storage -> order default_op_code/0x20 (exit
// tile is `x`/`y` if given, else the storage's own exit_tile_x/y, both torus-masked); EXIT_WAIT ->
// order 0x1f/0x23 unconditionally. Any other state is a no-op (neither branch's guard matches).
void unit_order_auto_launch_from_storage_enqueue(const sim_view &v, const calls &gc, uint16_t player,
                                                 int32_t unit_idx, uint32_t x, uint32_t y);

// llm_strat_bldg_order_activate_enqueue / _upgrade_enqueue / _restart_construction_enqueue
// @0x0046e133 / 0x0046e2ef / 0x0046e627. Byte-identical shape, one order code apiece (0x80/0x82/0x6b,
// same value in both param0 and order_code) -- no scratch fields, no gate, unconditional enqueue.
void bldg_order_activate_enqueue(const calls &gc, uint16_t player, int32_t building_index);
void bldg_order_upgrade_enqueue(const calls &gc, uint16_t player, int32_t building_index);
void bldg_order_restart_construction_enqueue(const calls &gc, uint16_t player, int32_t building_index);

// llm_strat_bldg_order_repair_cycle_start_enqueue @0x0046e409. Despite the name NOTHING IS
// CANCELLED -- see the .cpp and the order-dispatch notes: enqueues order 0x6a (a charge/work-cycle
// START) only while the building's energy (HP/charge stat, NOT the POWER resource) is below its cfg
// max AND its cfg type is neither main-base variant. Pure read of buildings/cfg_buildings.
void bldg_order_repair_cycle_start_enqueue(const sim_view &v, const calls &gc, uint16_t player,
                                           int32_t building_index);

// llm_strat_order_grant_resource_raw @0x0046f592. Order 0xed/0xed, scratch fields 3 (res_type) / 2
// (amount). `__mh_watcall_ebx_volatile` committed convention (amount arrives in EBX) -- irrelevant
// here since the marshalling thunk (addr/mh_calls.gen.h) already hides it from this body.
void order_grant_resource_raw(const calls &gc, uint16_t player, uint32_t res_type, uint32_t amount);

// llm_strat_order_population_delta_enqueue @0x0046f63a. Order 0xec/0xec, scratch field 1
// (population_delta). Real callers always pass positive (see the plate); the body itself branches on
// nothing.
void order_population_delta_enqueue(const calls &gc, uint16_t player, int32_t population_delta);

// llm_strat_unit_order_state_is_settled @0x004d4158. Pure boolean predicate over the unit's own
// (order, state) pair -- no callee at all, not even the order container. See the UNIT_STATE_* constants
// above for the four settled/transition pairs it recognizes.
int32_t unit_order_state_is_settled(const sim_view &v, int32_t player, int32_t unit_idx);

// llm_strat_order_collect_available_projects_thunk @0x004e611f. Stack-probe thunk with no logic of
// its own -- forwards `player` to the (still-original) frontier callee declared in `calls` above.
void order_collect_available_projects_thunk(const calls &gc, int32_t player);

// ---- batch C's remaining plain order-issue wrappers -----------------------------

// llm_strat_order_collect_available_projects_enqueue @0x0046f6c1. Order 0xee/0xee, no scratch
// fields set -- the frontier callee order_collect_available_projects_thunk already
// calls, now translated for real. Sole caller besides the thunk is an unresolved per-player-tick
// tail call.
void order_collect_available_projects_enqueue(const calls &gc, uint16_t player);

// llm_strat_unit_issue_default_order @0x00469e72. Releases a unit from AI-group/combat control by
// re-issuing its cfg type's default/idle order (both op_code and arg = cfg_unit::default_op_code).
void unit_issue_default_order(const sim_view &v, const calls &gc, uint32_t player, int32_t unit_index);

// llm_strat_unit_order_move_enqueue @0x0046a0d5. Raw (non-lockstep) counterpart of
// llm_strat_unit_order_move: stages (x, y, move_flag) into scratch fields 0/1/0xc, then the cfg
// type's own move_op_code/move_op_arg as the order.
void unit_order_move_enqueue(const sim_view &v, const calls &gc, uint16_t player, int32_t unit_idx,
                             int32_t x, int32_t y, int32_t move_flag);

// llm_strat_unit_order_move_auto @0x0046a56d. Same as unit_order_move_enqueue but with no move_flag
// scratch field (only 0/1 are staged) -- the AI-tick-driven twin, called directly from unit
// state-machine handlers rather than through the player group-order API.
void unit_order_move_auto(const sim_view &v, const calls &gc, uint16_t player, int32_t unit_idx,
                          int32_t x, int32_t y);

// llm_strat_unit_order_exit_storage_auto @0x0046ab08. The "_auto" (AI-tick-driven) twin of
// unit_order_exit_storage_enqueue above -- same storage-acceptance gate and teleport/approach-tile
// branch, but its rejected-storage fallback is unit_order_move_auto (no order-sequence stamping: the
// twin does not touch order_seq_id_by_player, unlike the _enqueue original). `param_4` is UNUSED
// (read nowhere) and the incoming `param_5` is overwritten before any use -- same committed-signature
// reasoning as the sibling.
void unit_order_exit_storage_auto(const sim_view &v, const calls &gc, uint16_t player,
                                  uint32_t unit_idx, int32_t storage_idx, uint32_t param_4,
                                  uint32_t param_5);

// llm_strat_order_ctrlgrp_select_member @0x0046f1de. Thin thunk: scratch field 0 = a2, then
// order_dispatch(unit_id, side, 0x34, 0x34) -- one control-group member's "select" notify.
void order_ctrlgrp_select_member(const calls &gc, uint32_t param_1, uint16_t param_2, int32_t a2);

// llm_strat_order_ctrlgrp_flash_member @0x0046f278. Identical shape to ctrlgrp_select_member, order
// 0x35 -- the complementary "flash" notify.
void order_ctrlgrp_flash_member(const calls &gc, uint16_t param_1, uint16_t param_2, int32_t a2);

// llm_unit_set_order_param @0x0048696f. Direct sim_store write, no callees at all: stamps a raw
// order-parameter value into the unit's own `order` field. Called immediately before
// llm_strat_unit_set_state_of when llm_strat_order_queue_dispatch's unit path accepts a new order.
void unit_set_order_param(sim_store &own, int32_t player, int32_t unit_index, int16_t param);

// ---- SIM1B ------------------------------------------------------------------------

// llm_strat_bldg_instant_construct_find_slot_enqueue @0x0046d514. Order 0xea/0xea. Enqueue
// counterpart of llm_strat_bldg_instant_construct_find_slot (a non-enqueue sibling this module does
// not translate); sole caller is an unresolved AI building-queue dispatch function
// (FUN_004e78d7). Two independent free-slot scans, both required to succeed:
//   (1) find an empty slot in the sub-roster `building_type_id`'s CFG type maps to (productions/
//       mines/turrets/unit_storage/labs depending on Building[building_type_id].type -- a plant/
//       colony/mother/main-base/relay/silo/civil type has no sub-roster at all and always resolves
//       to slot 1);
//   (2) find an empty slot in the player's OWN building roster (buildings[player][1..0x5a] --
//       0x5b/91 is the original's literal bound, not BUILDINGS_PER_PLAYER; reproduced as-is).
// On success, stages (arg4, arg5, arg1, building_type_id) into scratch fields (4, 5, 1, 0) and
// enqueues order 0xea tagged ORDER_KIND_BUILDING, returning the building roster slot found in (2).
// arg4/arg5/arg1 are named by SCRATCH SLOT, not by guessed meaning (same convention as
// order_queue_construction_enqueue above) -- the sole caller is unresolved, so their semantics are
// unverified; the plate's best guess is a target tile position (arg4/arg5) and a secondary id
// (arg1), not confirmed.
int32_t bldg_instant_construct_find_slot_enqueue(const sim_view &v, const calls &gc, int32_t arg4,
                                                 int32_t arg5, int32_t building_type_id, int32_t arg1,
                                                 uint16_t player);

} // namespace detail

// ---- the public surface, in original-behaviour terms -----------------------------------------
// Parameter types match the committed prototypes in addr/mh_calls.gen.h exactly (the drift gate
// enforces this on export/shadow installation).

int32_t  order_recruit_unit_enqueue(uint32_t unit_id, uint32_t player_id);
uint32_t order_queue_construction_enqueue(uint32_t param_1, uint32_t param_2, uint32_t a2,
                                          uint16_t param_4);
void     bldg_order_production_add_enqueue(uint16_t player, int32_t bldg_idx,
                                           int32_t unit_type);
void     bldg_order_assign_workers_enqueue(uint16_t player, uint16_t bldg_idx, uint32_t worker_count);
void     bldg_order_unassign_workers_enqueue(uint16_t param_1, uint16_t param_2, uint32_t a2);

void unit_order_exit_storage_enqueue(uint32_t player, uint32_t unit_idx, int32_t storage_idx,
                                     uint32_t param_4, uint32_t param_5);
void unit_order_attack_target_enqueue(uint32_t param_1, int32_t param_2, uint32_t a2, int32_t param_4,
                                      uint32_t param_5);
void unit_order_attack_target_alt_enqueue(uint32_t param_1, int32_t param_2, uint32_t a2,
                                          int32_t param_4, uint32_t param_5);
void unit_order_attack_unit_enqueue(uint32_t player, int32_t unit_idx, uint32_t target_player,
                                    int32_t target_unit_idx, uint32_t weapon_id);
void unit_order_attack_building_enqueue(uint32_t param_1, int32_t param_2, uint32_t a2,
                                        int32_t param_4, uint32_t param_5);
void unit_order_attack_building_reposition_alt_enqueue(uint32_t player, int32_t unit_idx,
                                                       uint32_t target_player,
                                                       int32_t target_bldg_idx, uint32_t weapon_idx);
void unit_order_auto_launch_from_storage_enqueue(uint32_t player, int32_t unit_idx, uint32_t x,
                                                 uint32_t y);

void    bldg_order_activate_enqueue(uint32_t player_id, int32_t building_index);
void    bldg_order_upgrade_enqueue(uint32_t player_id, int32_t building_index);
void    bldg_order_repair_cycle_start_enqueue(uint32_t player_id, int32_t building_index);
void    bldg_order_restart_construction_enqueue(uint32_t player_id, int32_t building_index);
void    order_grant_resource_raw(uint16_t player, uint32_t res_type, uint32_t amount);
void    order_population_delta_enqueue(uint32_t player_id, int32_t population_delta);
int32_t unit_order_state_is_settled(int32_t player, int32_t unit_idx);
void    order_collect_available_projects_thunk(int32_t player);

// ---------------------------------------------------------------------------------
void order_collect_available_projects_enqueue(uint16_t player);
void unit_issue_default_order(uint32_t player, int32_t unit_index);
void unit_order_move_enqueue(uint16_t player, int32_t unit_idx, uint32_t x, uint32_t y,
                             uint32_t move_flag);
void unit_order_move_auto(uint16_t player, int32_t unit_idx, uint32_t x, uint32_t y);
void unit_order_exit_storage_auto(uint32_t param_1, uint32_t param_2, int32_t a2, uint32_t param_4,
                                  uint32_t param_5);
void order_ctrlgrp_select_member(uint32_t side, uint16_t unit_id, int32_t group_index);
void order_ctrlgrp_flash_member(uint16_t side, uint16_t unit_id, int32_t group_index);
void unit_set_order_param(int32_t player, int32_t unit_index, int16_t param);

// ----------------------------------------------------------------------------------
int32_t bldg_instant_construct_find_slot_enqueue(uint32_t param_1, uint32_t param_2,
                                                 int32_t building_type_id, uint32_t param_4,
                                                 uint16_t player);


} // namespace mh::sim
