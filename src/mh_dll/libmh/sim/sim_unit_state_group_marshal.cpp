#include "sim/sim_unit_state_group_marshal.h"

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"        // ai_say / trace_budget -- the shared trace sink, not AI state
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const unit_state_group_marshal_calls &live_unit_state_group_marshal_calls() {
    static const unit_state_group_marshal_calls c = {
        MH_LIBMH_BIND(llm_strat_target_class),
        MH_LIBMH_BIND(llm_strat_target_release_ref),
        MH_LIBMH_BIND(llm_strat_unit_get_coords),
        MH_LIBMH_BIND(llm_strat_unit_set_state_order),
        MH_LIBMH_BIND(llm_strat_unit_notify_status),
        MH_LIBMH_BIND(llm_strat_group_move_register_member),
        MH_LIBMH_BIND(llm_strat_pathfind_route_leg_group_and_sort),
        MH_LIBMH_BIND(llm_strat_group_move_order_commit),
        MH_LIBMH_BIND(llm_strat_unit_path_step_blocked),
        MH_LIBMH_BIND(llm_strat_path_free_slot),
        MH_LIBMH_BIND(llm_strat_unit_in_weapon_range),
        MH_LIBMH_BIND(llm_strat_unit_set_state_order_of),
        MH_LIBMH_BIND(llm_strat_unit_order_move_auto),
        MH_LIBMH_BIND(llm_strat_unit_set_state_of),
        MH_LIBMH_BIND(llm_unit_set_order_param),
    };
    return c;
}

namespace {

// ---- the order/state numeric domain this function compares against --------------------------------
//
// unit.order and unit.state share ONE number line in this binary (a peer-scan gate compares a peer's
// STATE to GROUP_MARSHAL while every other comparison here is against the ORDER field) -- same domain
// sim_unit_update_rotation.cpp's own local STATE_* block already established and value-checked; this
// TU re-derives its own copy per translator-brief rule 4 ("no new shared helpers" -- every sim TU that
// needs this domain keeps its own anonymous-namespace copy rather than importing one, so no two TUs'
// copies can ever collide if linked together). Values are Ghidra's own decompiler-applied symbolic
// names on this function's own order/state field comparisons (llm_strat_unit_state_group_marshal_
// 00482451.c shows ATTACK_BUILDING/ATTACK_UNIT/ATTACK_UNIT_RETURN/ENTER_STORAGE_BEGIN/GROUP_MARSHAL
// directly, i.e. an enum is already applied to these fields), cross-checked against the raw CMP
// immediates at 0x00482475/0x004824a9/0x004824be/0x004826c9/0x0048272d/0x00482747/0x0048276f and
// against sim_unit_update_rotation.cpp's STATE_ATTACK_UNIT=0x1a/_UNIT_RETURN=0x1b/_BUILDING=0x1c and
// sim_dock_slot_is_busy.cpp's "0x24 ENTER_STORAGE_BEGIN" comment.
constexpr uint16_t GM_STATE_GROUP_MARSHAL       = 0x0a; // unit.state peer-eligibility gate
constexpr uint16_t GM_ORDER_ATTACK_UNIT         = 0x1a; // unit.order
constexpr uint16_t GM_ORDER_ATTACK_UNIT_RETURN  = 0x1b; // unit.order
constexpr uint16_t GM_ORDER_ATTACK_BUILDING     = 0x1c; // unit.order
constexpr uint16_t GM_ORDER_ENTER_STORAGE_BEGIN = 0x24; // unit.order
constexpr uint16_t GM_STATE_STOP_TO_DEFAULT     = 0x01; // the retry-escalation's set_state_order_of target
constexpr uint16_t GM_STATE_MOVE_PATH_12        = 0x0c; // set_state_of's "not the ticking unit" state

// llm_strat_unit_notify_status status codes this function passes through verbatim -- no Ghidra enum
// backs these (see sim_unit_notify.h's dispatch-table comment for what each numeric value does inside
// the callee); named locally per sim_order_dispatch.cpp's own NOTIFY_STATUS_* precedent.
constexpr uint32_t GM_NOTIFY_STATUS_TARGET_LOST = 0x66; // 102 -> callee sets order_notify_status=5
constexpr uint32_t GM_NOTIFY_STATUS_WAVE_READY  = 100;  // 0x64 -> callee sets order_notify_status=3

// 0x0048257e-0x00482586 (goal_x/goal_y from the current unit's own target) and 0x00482aaa-0x00482ae1
// (the peer-side twin, tile_x/tile_y feeding unit_in_weapon_range): the SAR 0x1f; SHL 0x5; SBB; SAR
// 0x5 idiom this function uses TWICE to convert a signed fine (sub-tile) coordinate to a tile
// coordinate. Transcribed literally per the G1 batch context rather than simplified to `fine / 32`
// (unlike sim_unit_state_die_explode.cpp's own `/32` equivalence, established there for an
// always-non-negative fine_x/fine_y from get_coords): target_fine_x/y here can be negative (a target
// across the torus wrap), so the two forms are not obviously the same at a glance even though a
// bit-level derivation of the SBB's borrow (sourced from the preceding SHL's carry-out, which is 1 iff
// the input was negative) shows this sequence computes the same truncating-toward-zero division by 32
// that IDIV/`/` would for every int32_t input -- kept as the shift form anyway per the explicit
// instruction, so a future reader does not have to redo that derivation to trust it.
int32_t gm_fine_to_tile_signed(int32_t fine) {
    const int32_t sign      = fine >> 31;                     // SAR EDX,0x1f  (0 or -1)
    const int32_t sbb_carry = sign & 1;                       // CF out of SHL EDX,0x5 (1 iff sign<0)
    const int32_t biased    = fine - (sign << 5) - sbb_carry; // SBB EAX,EDX
    return biased >> 5;                                       // SAR EAX,0x5
}

} // namespace

namespace detail {

void unit_state_group_marshal(const sim_view &v, sim_store &own,
                              const unit_state_group_marshal_calls &c) {
    const uint16_t player = *v.cur_player;
    const uint16_t index  = *v.cur_index;
    unit          &u      = own.cur_unit(); // _G_LLM_STRAT_CUR_UNIT dereferenced; SAME record as
                                            // v.units[player][index] -- see sim_view::cur_unit's
                                            // comment.

    // ---- 0x00482469-0x004824ce: is_move flag + ref_x/ref_y setup, UNCONDITIONAL -------------------
    // The order==0x1a / order==0x1b CMPs at 0x00482475-0x00482481 are followed by NO conditional jump
    // that tests their result before falling into the SAME code (LAB_00482486, ref_x/ref_y from
    // goal_x/goal_y) regardless of order's value -- a dead comparison the compiler left behind, not a
    // real branch. See uncertainties: omitted here as inert (same category as the
    // utils_assert_stack_capacity opening call every sim TU already omits).
    uint32_t is_move_flag = 1; // uStack_20 in the Ghidra draft: 1 = plain move, 0 = attack-move
    uint32_t attack_class = 0; // uStack_24: ONLY assigned in the attack arm below. The original
                               // leaves this UNINITIALIZED (Watcom stack garbage) on the plain-move
                               // path and passes it as group_move_order_commit's 7th param -- but the
                               // deterministic 0 here is PROVABLY inert: reimpl-verify (2026-08-19)
                               // traced the callee chain via ReVA (group_move_order_commit @0x0041c86c
                               // -> group_move_order_pathfind @0x0041ca37), whose every read of the
                               // target_mask param is gated by `if (mode == 0)`; on the plain-move
                               // path mode=is_move_flag=1, so this value is never read. CONFIRMED DEAD.
    const uint32_t ref_x = u.goal_x;
    const uint32_t ref_y = u.goal_y;

    if (u.order == GM_ORDER_ATTACK_BUILDING || u.order == GM_ORDER_ATTACK_UNIT ||
        u.order == GM_ORDER_ATTACK_UNIT_RETURN) {
        is_move_flag = 0;
        attack_class = static_cast<uint32_t>(
            c.target_class(static_cast<uint16_t>(u.target_ref), static_cast<uint16_t>(u.target_index)));

        if (u.order != GM_ORDER_ATTACK_BUILDING) {
            // ---- 0x004824fa-0x004825be: unit-target dead-check + goal recompute ------------------
            const uint16_t target_owner = static_cast<uint16_t>(static_cast<uint16_t>(u.target_ref) & 0xfu);
            // (ushort) reinterpretation, matching the asm's zero-extending MOVZX word[...] reads --
            // target_index is committed int16_t, and a raw int16_t->uint32_t widen would sign-extend a
            // negative value instead of reproducing the original's unsigned 16-bit index.
            const uint16_t target_index_u = static_cast<uint16_t>(u.target_index);
            const unit    &target         = v.units[static_cast<uint32_t>(target_owner) * v.caps.units +
                                         static_cast<uint32_t>(target_index_u)];

            if (target.energy <= 0.0) {
                // 0x004825c3-0x00482640: target already dead -- release, revert to the unit's default
                // order/state, notify, and return without gathering any peers.
                c.target_release_ref(player, index, 1u);
                u.target_ref          = 0;
                u.target_index        = 0;
                const cfg_unit &proto = v.cfg_units[u.unit_proto_id];
                c.unit_set_state_order(proto.default_op_code, proto.default_op_code);
                c.unit_notify_status(player, index, GM_NOTIFY_STATUS_TARGET_LOST);
                return;
            }

            // 0x00482548-0x004825be: re-fetch the target's fine coords, recompute goal_x/goal_y from
            // them. ref_x/ref_y ABOVE keep their earlier (now stale) goal_x/goal_y snapshot -- the asm
            // never re-reads them, so this is intentional, not a missed update.
            c.unit_get_coords(target_owner, static_cast<int32_t>(target_index_u), &u.target_fine_x,
                              &u.target_fine_y);
            u.goal_x = static_cast<uint8_t>(gm_fine_to_tile_signed(u.target_fine_x));
            u.goal_y = static_cast<uint8_t>(gm_fine_to_tile_signed(u.target_fine_y));
        }
    }

    // ---- 0x00482645-0x0048283d: register self, then gather peers -----------------------------------
    // int32_t: group_move_register_member's committed out-param is int32_t * (TACT1-P C6, 2026-09-04).
    int32_t member_count = 0;
    c.group_move_register_member(player, index, &member_count);

    for (uint32_t i = static_cast<uint32_t>(index) + 1; i < v.caps.units && member_count < 100; ++i) {
        const unit &peer = v.units[static_cast<uint32_t>(player) * v.caps.units + i];

        if (peer.state != GM_STATE_GROUP_MARSHAL) continue;
        if (peer.order != u.order) continue;
        if (peer.order == GM_ORDER_ENTER_STORAGE_BEGIN) continue;
        if (peer.move_group_id != u.move_group_id) continue;

        if (peer.order == GM_ORDER_ATTACK_BUILDING || peer.order == GM_ORDER_ATTACK_UNIT_RETURN ||
            peer.order == GM_ORDER_ATTACK_UNIT) {
            if (peer.target_ref != u.target_ref || peer.target_index != u.target_index) continue;
        } else {
            if (peer.goal_x != ref_x || peer.goal_y != ref_y) continue;
        }

        c.group_move_register_member(player, static_cast<int32_t>(i), &member_count);
    }

    // ---- 0x0048283d-0x00482876: leg (wave) routing --------------------------------------------------
    const int32_t move_group_id = u.move_group_id;
    int32_t       leg_count;
    if (member_count < 2) {
        leg_count = member_count;
    } else {
        leg_count = c.pathfind_route_leg_group_and_sort(member_count, static_cast<int32_t>(ref_x),
                                                        static_cast<int32_t>(ref_y));
    }
    int32_t remaining_count = member_count;
    int32_t wave_cursor     = leg_count;

    // ---- 0x00482876-0x00482f24: commit each wave, settle its members, compact the next wave --------
    for (;;) {
        c.group_move_order_commit(static_cast<int32_t>(ref_x), static_cast<int32_t>(ref_y), player,
                                  leg_count, move_group_id, is_move_flag, attack_class);

        for (int32_t w = 0; w < leg_count; ++w) {
            --remaining_count;

            group_scratch_member &scratch  = own.group_move_scratch_at(w);
            const int32_t         unit_idx = scratch.unit_idx;
            unit                 &member   = own.unit_at(player, unit_idx);

            // 0x004828e0-0x0048290f: lift the temporary self-obstacle the group pathfinder set.
            own.passable_at(member.x, member.y) = static_cast<uint8_t>(scratch.saved_passable);
            if (index == unit_idx) {
                own.tick_budget() = 0.0; // 0x00482921-0x0048292b: zeroes BOTH dwords of the double.
            }

            if (member.path_slot_id == 0xffu) {
                // 0x00482954-0x0048297c: no path slot assigned yet -- small clock bump, next.
                member.activity_clock += 2.0; // _DAT_0050145a, read-memory-confirmed 2.0
            } else if (v.path_buffers[static_cast<uint32_t>(player) * PATH_WAYPOINTS_PER_PLAYER +
                                      static_cast<uint32_t>(member.path_slot_id) * PATH_WAYPOINTS_PER_SLOT +
                                      0]
                               .run_length == 0 ||
                       c.unit_path_step_blocked(player, unit_idx) != 0) {
                // ---- 0x00482981-0x00482ce9: path exhausted or blocked ------------------------------
                c.path_free_slot(player, unit_idx);

                bool redirected = false;
                if (member.order == GM_ORDER_ATTACK_BUILDING || member.order == GM_ORDER_ATTACK_UNIT_RETURN ||
                    member.order == GM_ORDER_ATTACK_UNIT) {
                    // 0x00482a48-0x00482b45: still in weapon range of its own target -> just re-issue
                    // the attack order in place instead of retrying the move.
                    const int32_t member_attack_class = c.target_class(
                        static_cast<uint16_t>(member.target_ref), static_cast<uint16_t>(member.target_index));
                    const int32_t tile_x = gm_fine_to_tile_signed(member.target_fine_x);
                    const int32_t tile_y = gm_fine_to_tile_signed(member.target_fine_y);
                    if (c.unit_in_weapon_range(player, unit_idx, tile_x, tile_y, member_attack_class) != 0) {
                        c.unit_set_state_order_of(player, unit_idx, static_cast<int16_t>(member.order), 1);
                        c.unit_notify_status(player, unit_idx, GM_NOTIFY_STATUS_TARGET_LOST);
                        redirected = true;
                    }
                }

                if (!redirected) {
                    // 0x00482b45-0x00482ce2: bump the retry counter; escalate at >=4.
                    ++member.path_blocked_retry_count;
                    if (member.path_blocked_retry_count < 4) {
                        member.activity_clock += 2.0; // _DAT_00501462, read-memory-confirmed 2.0
                    } else {
                        if (member.target_ref != 0) {
                            c.target_release_ref(player, unit_idx, 1u);
                            member.target_ref   = 0;
                            member.target_index = 0;
                        }
                        if (member.order == GM_ORDER_ATTACK_UNIT_RETURN) {
                            c.unit_order_move_auto(player, unit_idx, member.home_x, member.home_y);
                        } else {
                            c.unit_notify_status(player, unit_idx, GM_NOTIFY_STATUS_TARGET_LOST);
                        }
                        c.unit_set_state_order_of(player, unit_idx, static_cast<int16_t>(GM_STATE_STOP_TO_DEFAULT),
                                                  1);
                        own.passable_at(member.x, member.y) = 0;
                    }
                }
            } else {
                // ---- 0x00482cee-0x00482ebb: good path -- settle this member into the new state -----
                const cfg_unit &member_proto = v.cfg_units[member.unit_proto_id];
                if (member.order == member_proto.move_op_code) {
                    c.unit_set_order_param(player, unit_idx, 1);
                }
                if (index == unit_idx) {
                    c.unit_set_state_of(player, unit_idx, static_cast<int16_t>(member_proto.move_op_code));
                } else {
                    c.unit_set_state_of(player, unit_idx, static_cast<int16_t>(GM_STATE_MOVE_PATH_12));
                }

                if (move_group_id != 0 && leg_count > 1 && member.order != GM_ORDER_ATTACK_BUILDING &&
                    member.order != GM_ORDER_ATTACK_UNIT_RETURN && member.order != GM_ORDER_ATTACK_UNIT) {
                    member.goal_x = static_cast<uint8_t>(scratch.tile_col);
                    member.goal_y = static_cast<uint8_t>(scratch.tile_row);
                }
                member.path_cursor              = 0;
                member.path_blocked_retry_count = 0;
                c.unit_notify_status(player, unit_idx, GM_NOTIFY_STATUS_WAVE_READY);
            }
        }

        // ---- 0x00482ec0-0x00482f1a: compact the next wave (same wave_rank run) to the front --------
        if (remaining_count > 0) {
            const int32_t target_rank = own.group_move_scratch_at(wave_cursor).wave_rank;
            leg_count                 = 0;
            // Evaluation order matches the asm/.c exactly (wave_rank read BEFORE the bound check, not
            // after) -- see the uncertainty note below on why that order matters here.
            while (own.group_move_scratch_at(wave_cursor).wave_rank == target_rank &&
                   wave_cursor < member_count) {
                own.group_move_scratch_at(leg_count) = own.group_move_scratch_at(wave_cursor);
                ++wave_cursor;
                ++leg_count;
            }
        }

        if (remaining_count < 1) return;
    }
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

void unit_state_group_marshal() {
    sim_state st = state();
    detail::unit_state_group_marshal(st.read, st.own, live_unit_state_group_marshal_calls());
}


} // namespace mh::sim
