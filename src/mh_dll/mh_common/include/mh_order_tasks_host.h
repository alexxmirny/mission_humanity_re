// Destination: src/mh_dll/mh_common/include/mh_order_tasks_host.h
#pragma once
#include <stdint.h>
typedef struct mh_order_tasks_request {
    uint32_t operation, player, slot, args[32];
} mh_order_tasks_request;
typedef struct mh_order_tasks_result {
    uint32_t values[24];
} mh_order_tasks_result;
typedef int32_t (*mh_order_tasks_callback)(const mh_order_tasks_request *, mh_order_tasks_result *);
#ifdef __cplusplus
#include "mh_order_tasks.h"
#include "mh_order_tasks_reservations.h"
namespace mh::order_tasks::host_protocol {
enum class operation : uint32_t {
    ticket = 1,
    observe,
    due_edit,
    ordinary_applied,
    autonomous_applied,
    suppress_home,
    hold,
    retire_actor,
    retire_target,
    pre_unit_tick,
    cancel_actor,
    command_sequence,
    primary_admission,
    discarded,
    reservation             = 16,
    primary_access          = 17,
    published               = 18,
    begin_load              = 19,
    abort_load              = 20,
    checkpoint_ready        = 21,
    new_campaign            = 22,
    fresh_world             = 23,
    ensure_known            = 24,
    air_boundary            = 25,
    begin_due_player_batch  = 26,
    end_due_player_batch    = 27,
    ground_patrol_placement = 28
};
// begin_load request args0 carries this reason. Only successful wholesale
// snapshot input-ring replacement retires native per-record proof tables.
enum class begin_load_reason : uint32_t {
    preserve_input_ring          = 0,
    snapshot_input_ring_replaced = 1
};
enum class reservation_operation : uint32_t {
    begin_issue = 1,
    finish_issue,
    cancel_pending,
    handoff,
    release_before,
    rebound_before,
    rebound_after,
    release_after,
    adopt_binding,
    invalidate_nonowning_binding,
    generic_begin        = 11,
    physical_application = 12,
    sink_observed        = 13,
    captured_application = 14
};
// reservation args0 subop. Begin args1..4 target(kind,owner,slot,life); life0 resolves
// only at the proven synchronous issue origin. Result0 decision,1..14 exact issue_scope,15..17 world key.
// Finish args1..14 exactscope,15 ACTUAL published,16 actual source sink entered,17 suppression observed there.
// Accept only15||(16&&17); native host broker also adds actual original-sink receipts. Handoff target1..4,epoch5.
// Finish18..20 carry the originating world key; stale-world finish NEVER touches current native/canonical rows.
// generic_begin11 uses the same target/scope packing as begin_issue, but is physical-only
// and MUST NOT stamp QOPA. physical_application12 uses target1..4 at actual untagged F.
// sink_observed13: requestplayer/slot,args1 actual source-sink suppression; native-origin broker only.
// captured_application14: actor only, after actual-F QOEX admission (not inspection).
// Other phases use request player/slot; result0 handled/success. No pointer-valued word.
// air_boundary args0 actual native slot,args1 cursor; result0 continued,result1 owned at entry.
// A failed owned terminal refill is an invariant violation, never permission for finite arrival.
// begin/end_due_player_batch bracket only AI, after HASH injection and before ENTRY injection.
// begin_issue result18 distinguishes a pre-mutation yield to a due valid player edit.
// primary_access result0 may_access_original_target,result1 typed control,result2 safe_to_apply.
// ground_patrol_placement: player/slot live identity; result0 eligible,1 group,2 command sequence.
// Caller also proves native order16 and exact native group; no epoch is route authority.
inline void put_issue_scope(uint32_t *w, uint32_t *world, const reservations::issue_scope &s) {
    w[0]     = s.actor.player;
    w[1]     = s.actor.slot;
    w[2]     = s.actor.incarnation;
    w[3]     = uint32_t(s.target.kind);
    w[4]     = s.target.object.player;
    w[5]     = s.target.object.slot;
    w[6]     = s.target.object.incarnation;
    w[7]     = uint32_t(s.prior_target.kind);
    w[8]     = s.prior_target.object.player;
    w[9]     = s.prior_target.object.slot;
    w[10]    = s.prior_target.object.incarnation;
    w[11]    = s.epoch;
    w[12]    = s.prior_packed;
    w[13]    = s.active ? 1u : 0u;
    world[0] = s.world.system;
    world[1] = s.world.planet;
    world[2] = s.world.incarnation;
}
inline reservations::issue_scope get_issue_scope(const uint32_t *w, const uint32_t *world) {
    return {{w[0], w[1], w[2]}, {object_kind(w[3]), {w[4], w[5], w[6]}}, {object_kind(w[7]), {w[8], w[9], w[10]}}, w[11], w[12], w[13] != 0, {world[0], world[1], world[2]}};
}
// ticket/hold/suppress_home: q.player/slot, args0 actor incarnation (zero resolves live).
// published18: after complete validated world publication+borrowed rebind; no args, result0 success.
// Native adapter clears local unadmitted inputs/broker; standalone clears source origin/budget stacks only.
// begin_load19: quarantine native/source bookkeeping, result0 opaque prior-live token.
// abort_load20: after complete rollback/rebind, args0 exact token returned by begin_load.
// checkpoint_ready21: read-only result0; quarantine or synchronous reservation handoff refuses capture.
// new_campaign22: quarantine both carriers and reset campaign identities; result0 success.
// fresh_world23: after fresh geometry/roster publication, args0 system,1 planet,2 width,3 height.
// Allocate exactly one new world incarnation, reset its routes and publish; result0 success.
// ensure_known24: immediately BEFORE a real nonzero planet-status write, args0 planet.
// result0 existing/new incarnation, zero on failure. Valid before world publication
// during campaign initialization; changes QCAM only, never QORD or runtime authority.
// ticket: result0 present,1..9 ticket,10 family,11 phase,12 hold,13 action,
// 14 assigned-valid,15/16 assignedXY,17 placement,18 pathslot,19 execution_epoch,20 planner_succeeded,
// 21/22 requestedXY,23 group (the current node, including a returning-anchor activity).
// suppress_home: result0 true ONLY an owned excursion/explicit attack requiring no home leg.
// observe: args0..8 ticket,9 kind,10 family,11/12 position,13/14 assigned,
// 15 placement,16 pathslot,17..20 target(kind,owner,slot,life); result0=control.
// args21 producer source line,22 carrier (1 native/2 translated): optional host diagnostics only;
// core dispatch ignores them. No canonical state or serialized layout depends on these words.
// due_edit/ordinary_applied/autonomous_applied: args0 owner_and_kind,args1 code,
// args2 param0,args3..15 exact args13; autonomous_applied16/17=validated preF anchorXY.
// due_edit args17=kept-prefix+1; other unused words are zero.
// due_edit result0 consumed,result1 refusal. Other application results0 accepted.
// retire_actor/cancel_actor: args0 incarnation. retire_target: args0 kind,
// args1 target incarnation,args2 destruction-confirmed. player/slot identify target.
// pre_unit_tick: no args. command_sequence: player issuer,args0 sequence,result0 accepted.
// discarded: same record packing, AFTER actual logical removal/transfer. result0 typed control.
// result1 current tagged actorlife or exact physical-only pending match (safe carrier flag refresh).
// args18 source lane(1QUEUE,2PENDING,3STAGING),19/20 exclude [begin,end) processed compaction hole.
// Retained scans use prefix+future, not stale raw count; exact duplicate-ticket survivors are not loss.
inline void put_ticket(uint32_t *w, const mh::order_tasks::ticket &t) {
    w[0] = t.world.system;
    w[1] = t.world.planet;
    w[2] = t.world.incarnation;
    w[3] = t.actor.player;
    w[4] = t.actor.slot;
    w[5] = t.actor.incarnation;
    w[6] = t.route;
    w[7] = t.leg;
    w[8] = t.attempt;
}
// primary_admission: same record packing; result0=allow BEFORE F/dequeue side effects,
// result2/3=preF actor anchorXY for QOPA (caller-local; only actual application guard consumes).
// Untagged replacement cancels existing ownership only; no ordinary stamping/sequence allocation.
// args16=1 is retained-record inspection ONLY: untagged returns allowed without cancelling.
// Actual admission leaves args16zero. Survivor flag refresh must NEVER submit actual admission.
// QOEX is stamped ONLY after atomic accepted first append to a unique retained native primary.
// PRIMARY args8=QOEX,9=actorlife,10=route,11=leg; never changes0..7/12 or secondary words.
inline constexpr uint32_t      EXPLICIT_PRIMARY = 0x58454f51u;
inline mh::order_tasks::ticket get_ticket(const uint32_t *w) {
    return {{w[0], w[1], w[2]}, {w[3], w[4], w[5]}, w[6], w[7], w[8]};
}
inline void put_outcome(mh_order_tasks_request &q, const mh::order_tasks::outcome &o) {
    q           = {};
    q.operation = uint32_t(operation::observe);
    q.player    = o.owner.actor.player;
    q.slot      = o.owner.actor.slot;
    put_ticket(q.args, o.owner);
    q.args[9]  = uint32_t(o.kind);
    q.args[10] = uint32_t(o.family);
    q.args[11] = o.position.x;
    q.args[12] = o.position.y;
    q.args[13] = o.assigned.x;
    q.args[14] = o.assigned.y;
    q.args[15] = uint32_t(o.placement);
    q.args[16] = o.path_slot;
    q.args[17] = uint32_t(o.target.kind);
    q.args[18] = o.target.object.player;
    q.args[19] = o.target.object.slot;
    q.args[20] = o.target.object.incarnation;
}
inline mh::order_tasks::outcome get_outcome(const mh_order_tasks_request &q) {
    return {get_ticket(q.args), mh::order_tasks::outcome_kind(q.args[9]), mh::order_tasks::executor(q.args[10]), {q.args[11], q.args[12]}, {q.args[13], q.args[14]}, mh::order_tasks::assignment_reason(q.args[15]), q.args[16], {mh::order_tasks::object_kind(q.args[17]), {q.args[18], q.args[19], q.args[20]}}};
}
inline constexpr uint32_t AUTONOMOUS_PRIMARY = 0x41504f51u;
// QOPH: physical bookkeeping only; words9/10/11 actorlife/unique issue epoch/targetlife.
// A canceled or stale QOPH never falls through as an ordinary primary replacement.
inline constexpr uint32_t PHYSICAL_PRIMARY = 0x48504f51u;
inline bool               is_primary_marker(uint32_t marker) {
    return marker == AUTONOMOUS_PRIMARY || marker == EXPLICIT_PRIMARY || marker == PHYSICAL_PRIMARY;
}
// QOPA PRIMARY args8=magic,9=actorlife,10=execution_epoch,11=targetlife.
// Never applies to secondary or unowned ordinary activity. QOEX retains life/route/leg.
static_assert(sizeof(mh_order_tasks_request) == 140);
static_assert(sizeof(mh_order_tasks_result) == 96);
} // namespace mh::order_tasks::host_protocol
#endif // __cplusplus
