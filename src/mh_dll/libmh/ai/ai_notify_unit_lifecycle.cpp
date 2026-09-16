#include "ai/ai_notify_unit_lifecycle.h"

namespace mh::ai {
namespace detail {

namespace {

// ---- declared_needs: no Ghidra enum backs param_4 as a discriminant ------------------------------
//
// The jump-table index dispatched at 0x004dbb7d, bound-checked `> 4 -> caseD_0` at
// 0x004dbb74-0x004dbb77 (UNSIGNED). This is a DERIVED discriminant (translator-brief rule 17a) with
// no underlying Ghidra enum to name it from -- propose one. Confidence differs sharply by member:
// value 4 is confirmed by the AI notes ("the unit-creation sibling (mode `4` = unit just created,
// called from `llm_strat_unit_create` and `llm_strat_bldg_completion_dispatch`'s production-complete
// paths)"); values 0/1/2 are explicitly flagged UNTRACED upstream ("only mode 4 (unit created)
// is traced") -- the names below describe this function's own CODE SHAPE (what each arm actually
// does), not a confirmed caller-side meaning, and should not be read as more certain than that.
// Value 3 has no distinct jump-table target at all (shares caseD_0 with the out-of-range case).
enum class lifecycle_event : int32_t {
    no_op              = 0,   // caseD_0 -- a pure return; nothing read or written
    reset_and_classify = 1,   // caseD_1 -- full roster/group reset, decrements
                              // ai_start_units_pending_spawn, classifies by combat type when armed
                              // (state==0x1f), ALWAYS reconciles the build queue afterward
    queue_reconcile_only = 2, // caseD_2 -- build-queue reconcile scan ONLY
    unit_created         = 4, // caseD_4 -- CONFIRMED (the AI notes): unit just created
};

// ---- declared_needs: map_object_unit::state == 0x1f has no member name --------------------------
//
// Tested at 0x004dbbf2 (`CMP word[..],0x1f`). The field's type `llm_strat_unit_state` IS a real
// 2-byte Ghidra enum applied to map_object_unit.state/.order (the reimpl tracker settled
// this 2026-08-20, overturning an earlier "no such enum" reading four sibling translation units had
// independently converged on) -- but it carries NO member names, so this specific value has none to
// cite. Reaching it is what gates the soldier/ground/plane/heli classify chain below; propose a name
// for Ghidra member 0x1f; naming it needs caller-site evidence this translation does not read.
inline constexpr uint16_t UNIT_STATE_GATE_0X1F = 0x1f;

// The committed train-queue-entry match test (`AND BL,0x8f ; CMP BL,0x80`, both caseD_1's tail and
// caseD_2), and the removal-mark bit -- all three already named on
// `mh_llm_strat_ai_bldg_queue_entry::status`'s own field comment (addr/mh_structs.gen.h): 0x80 =
// finished/committed, the low nibble = entry kind (0 = train/recruit), 0x40 = "entry removed,
// compacted out by the queue_process prologue".
inline constexpr uint8_t BLDG_QUEUE_TRAIN_COMMITTED_MASK  = 0x8f;
inline constexpr uint8_t BLDG_QUEUE_TRAIN_COMMITTED_VALUE = 0x80;
inline constexpr uint8_t BLDG_QUEUE_STATUS_REMOVED        = 0x40;

// The 3-field group-link reset caseD_1's armed path (0x004dbbbc-0x004dbbce) and caseD_4's enabled
// path (0x004dbd49-0x004dbd5b) BOTH perform -- TWO SEPARATE COPIES of the same three stores in the
// original (not a shared jump target the way the queue-scan tail below is), reproduced here as one
// private helper only to avoid a copy-pasted bug; there is no behavioural difference between the two
// call sites to preserve.
void reset_group_links(const ai_store &own, uint32_t player, uint32_t unit_id) {
    own.roster.ai_group_next(player, (int32_t)unit_id)  = 0;
    own.roster.ai_group_prev(player, (int32_t)unit_id)  = 0;
    own.roster.ai_group_index(player, (int32_t)unit_id) = 0xffffu;
}

// The shared build-queue-reconcile scan -- caseD_1's tail (after its classify, run UNCONDITIONALLY
// regardless of whether the classify matched) and caseD_2 (run directly, nothing before it) execute
// the exact same code in the original: caseD_2's `JZ 0x004dbc9c` at 0x004dbd0b jumps INTO caseD_1's
// own `OR byte[..],0x40` instruction. One physical piece of shared code, not two independent
// ports -- reproduced as one helper for that reason, per translator-brief rule 4 (preserve
// code-sharing the original itself has; don't invent code-sharing it doesn't).
//
// Walks player_data::ai_bldg_queue[0 .. ai_bldg_queue_count), RE-READING the count every iteration
// (0x004dbcbf for caseD_1's tail, 0x004dbd24 for caseD_2 -- never cached, translator-brief rule 16)
// and re-reading the scanned unit's own unit_proto_id every iteration too (0x004dbc90 / 0x004dbd01 --
// the original recomputes the address each pass even though the VALUE cannot change mid-scan; kept
// as a genuine re-read here rather than hoisted, for the same reason). Looks for the first entry
// whose `status` reads as committed+train (`(status & 0x8f) == 0x80`) and whose `tick_or_unit_id`
// (the trained unit's cfg proto id, per the struct's own field comment) equals this unit's
// `unit_proto_id`; marks the FIRST match `status |= 0x40` and stops. No match leaves the queue
// untouched. `tick_or_unit_id` is 8-bit and `unit_proto_id` is 16-bit in the original (one MOVZX
// byte, one MOVZX word) -- both zero-extended before the compare, reproduced as an ordinary unsigned
// comparison between the two (a proto id above 255 can therefore never match, which is the
// original's own truncation, not something to "fix").
void bldg_queue_reconcile_trained_unit(const ai_view &v, const ai_store &own, uint32_t player,
                                       uint32_t unit_id, lifecycle_notify_report &rep) {
    for (int32_t i = 0; i < v.players[player].ai_bldg_queue_count; ++i) {
        const auto    &entry    = v.players[player].ai_bldg_queue[i];
        const uint16_t proto_id = unit_of(v, player, (int32_t)unit_id).unit_proto_id;
        if ((entry.status & BLDG_QUEUE_TRAIN_COMMITTED_MASK) != BLDG_QUEUE_TRAIN_COMMITTED_VALUE)
            continue;
        if (entry.tick_or_unit_id != proto_id) continue;
        own.players[player].ai_bldg_queue[i].status |= BLDG_QUEUE_STATUS_REMOVED;
        rep.queue_reconciled = true;
        return;
    }
}

// caseD_1's classify chain (0x004dbc0d-0x004dbc61), reached only when state==0x1f. Soldier OR ground
// -> group 2 (both land on the SAME `LAB_004dbc1a` target -- ground is tested only when soldier is
// false, exact short-circuit reproduced by the else-if chain below); plane -> group 4; heli -> group
// 3. Returns whether a link call was made; the caller runs the queue-reconcile scan EITHER WAY
// (0x004dbc61 is reached both by falling out of the CALL and by the heli-check's JZ-on-no-match).
bool classify_and_link_case1(const ai_calls &gc, uint32_t player, uint32_t unit_id,
                             lifecycle_notify_report &rep) {
    int32_t group;
    if (gc.unit_is_ai_soldier(player, unit_id) != 0) {                 // @0x004dbc11
        group = 2;                                                     // @0x004dbc1a
    } else if (gc.unit_is_ai_ground((uint16_t)player, unit_id) != 0) { // @0x004dbc27
        group = 2;                                                     // shares LAB_004dbc1a
    } else if (gc.unit_is_ai_plane(player, unit_id) != 0) {            // @0x004dbc34
        group = 4;                                                     // @0x004dbc3f
    } else if (gc.unit_is_ai_heli(player, unit_id) != 0) {             // @0x004dbc4a
        group = 3;                                                     // @0x004dbc55
    } else {
        return false; // NONE matched -- no call; caller still runs the queue scan
    }
    gc.group_member_link(player, group, unit_id); // @0x004dbc5c
    rep.group_linked     = true;
    rep.group_index_used = group;
    return true;
}

// caseD_4's classify chain (0x004dbd9e-0x004dbdf1), reached only when ai_invasion_force==0 and the
// heli-mother early-out didn't fire. THE GROUP ASSIGNMENT DIFFERS FROM caseD_1's ABOVE: soldier OR
// ground here link group 0 (via the shared LAB_004dbbfd/LAB_004dbc01 tail -- the SAME target the
// "state != 0x1f" arm of caseD_1 uses), not group 2. Plane still links group 4, heli still links
// group 3. A non-match returns directly with no call, and -- unlike case1's version above -- never
// reaches the queue-reconcile scan.
void classify_and_link_case4(const ai_calls &gc, uint32_t player, uint32_t unit_id,
                             lifecycle_notify_report &rep) {
    if (gc.unit_is_ai_soldier(player, unit_id) != 0 ||          // @0x004dbda2/0x004dbda9
        gc.unit_is_ai_ground((uint16_t)player, unit_id) != 0) { // @0x004dbdb3/0x004dbdba
        gc.group_member_link(player, 0, unit_id);
        rep.group_linked     = true;
        rep.group_index_used = 0;
        return;
    }
    int32_t group;
    if (gc.unit_is_ai_plane(player, unit_id) != 0) {       // @0x004dbdc4
        group = 4;                                         // @0x004dbdcf
    } else if (gc.unit_is_ai_heli(player, unit_id) != 0) { // @0x004dbddd
        group = 3;                                         // @0x004dbdec
    } else {
        return; // NONE matched -- straight return, no call, no queue scan (@0x004dbde4 -> caseD_0)
    }
    gc.group_member_link(player, group, unit_id);
    rep.group_linked     = true;
    rep.group_index_used = group;
}

} // namespace

lifecycle_notify_report notify_unit_lifecycle(const ai_view &v, const ai_store &own, const ai_calls &gc,
                                              uint16_t player_, uint16_t unit_type, uint32_t unit_id,
                                              uint32_t param_4) {
    (void)unit_type; // verified UNUSED by the body -- see the header banner

    lifecycle_notify_report rep{};
    // `AND EAX,0xf` @0x004dbb4e -- masks to 4 bits (0..15), wider than MAX_PLAYERS(8); preserved
    // exactly, not clamped. See the header banner's note on this.
    const uint32_t player = (uint32_t)player_ & 0xfu;
    rep.player            = player;

    if (v.players[player].ai_enabled == 0) { // top gate @0x004dbb67
        rep.ai_disabled_at_entry = true;
        return rep;
    }

    if (param_4 > 4u) { // @0x004dbb74-0x004dbb77, UNSIGNED
        rep.out_of_range_event = true;
        return rep;
    }
    rep.event = (int32_t)param_4;

    switch ((lifecycle_event)param_4) {
        case lifecycle_event::no_op: // caseD_0 -- also the unassigned index 3, via the same table slot
            break;

        case lifecycle_event::reset_and_classify: {                          // caseD_1
            own.roster.incoming_threat_damage(player, (int32_t)unit_id) = 0; // @0x004dbb93
            own.roster.engage_status_word_clear(player, (int32_t)unit_id);   // @0x004dbb9c
            rep.threat_and_engage_cleared = true;

            if (v.players[player].ai_enabled == 0) {                     // re-read @0x004dbba5, rule 16
                own.roster.ai_group_index(player, (int32_t)unit_id) = 0; // @0x004dbbae, LAB_004dbbae
                rep.ai_disabled_recheck                             = true;
                break;
            }

            reset_group_links(own, player, unit_id); // @0x004dbbbc-0x004dbbce
            rep.group_links_reset = true;

            if (v.players[player].ai_start_units_pending_spawn != 0) { // @0x004dbbd7
                --own.players[player].ai_start_units_pending_spawn;    // @0x004dbbe0
                rep.pending_spawn_decremented = true;
            }

            const unit &u = unit_of(v, player, (int32_t)unit_id);
            if (u.state == UNIT_STATE_GATE_0X1F) { // @0x004dbbf2
                rep.classified = true;
                classify_and_link_case1(gc, player, unit_id, rep);               // may or may not link
                bldg_queue_reconcile_trained_unit(v, own, player, unit_id, rep); // ALWAYS runs after
            } else {
                // state != 0x1f -- LAB_004dbbfd/LAB_004dbc01: link group 0 and return, no queue scan.
                gc.group_member_link(player, 0, unit_id);
                rep.group_linked     = true;
                rep.group_index_used = 0;
            }
            break;
        }

        case lifecycle_event::queue_reconcile_only: // caseD_2
            bldg_queue_reconcile_trained_unit(v, own, player, unit_id, rep);
            break;

        case lifecycle_event::unit_created: {                            // caseD_4
            if (v.players[player].ai_enabled == 0) {                     // re-read @0x004dbd3c
                own.roster.ai_group_index(player, (int32_t)unit_id) = 0; // shared LAB_004dbbae
                rep.ai_disabled_recheck                             = true;
                break;
            }

            reset_group_links(own, player, unit_id); // @0x004dbd49-0x004dbd5b, second copy
            rep.group_links_reset = true;

            const unit    &u     = unit_of(v, player, (int32_t)unit_id);
            const uint32_t utype = v.cfg_units[u.unit_proto_id].type; // @0x004dbd64/0x004dbd77, full dword
            if (utype == (uint32_t)UNIT_TYPE_A_HELI_MOTHER || utype == (uint32_t)UNIT_TYPE_H_HELI_MOTHER) {
                rep.heli_mother_early_out = true; // @0x004dbd7e / 0x004dbd8b
                break;
            }

            if (v.players[player].ai_invasion_force != 0) { // @0x004dbd91
                gc.group_member_link(player, 0, unit_id);
                rep.group_linked               = true;
                rep.group_index_used           = 0;
                rep.invasion_force_direct_link = true;
                break;
            }

            rep.classified = true;
            classify_and_link_case4(gc, player, unit_id, rep); // never reaches the queue scan
            break;
        }
    }

    return rep;
}

} // namespace detail

void notify_unit_lifecycle(uint16_t player_, uint16_t unit_type, uint32_t unit_id,
                           uint32_t param_4) {
    const ai_state st = state();
    (void)detail::notify_unit_lifecycle(st.read, st.own, live_calls(), player_, unit_type, unit_id,
                                        param_4);
}

} // namespace mh::ai
