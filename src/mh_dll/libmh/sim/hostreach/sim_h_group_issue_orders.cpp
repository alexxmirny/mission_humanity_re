//
// sim/hostreach/sim_h_group_issue_orders.cpp -- see sim_h_group_issue_orders.h. Translated from the
// DISASSEMBLY (tmp/decomp_sim/llm_strat_group_issue_attack_order_00444894.asm,
// tmp/decomp_sim/llm_strat_group_issue_enter_building_order_004451d2.asm), not from the .c drafts.
//
#include "sim/hostreach/sim_h_group_issue_orders.h"

#include "state/host_events.h"  // mh::state::evt -- the LIFT-NOTIFY event channel
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder (MH_LIBMH_BIND)
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const group_issue_orders_calls &live_group_issue_orders_calls() {
    static const group_issue_orders_calls c = {
        MH_LIBMH_BIND(llm_strat_unit_select_weapon),
        MH_LIBMH_BIND(llm_strat_unit_order_attack_target),
        MH_LIBMH_BIND(llm_strat_unit_order_attack_target_alt),
        MH_LIBMH_BIND(llm_strat_unit_order_attack_unit),
        MH_LIBMH_BIND(llm_strat_unit_order_attack_building_reposition),
        MH_LIBMH_BIND(llm_strat_unit_order_exit_storage),
        MH_LIBMH_BIND(llm_unit_state_is_boarding),
        // CONDUCTOR RESOLUTION (SIM1-H, 2026-09-10). The writer read these as frontier originals
        // with no rebind row "or ever", and was wrong about both -- gen_libmh_calls named them:
        //
        //   llm_strat_race_alert_sound_emit is class `event-channel:event` in the adjudication
        //   ledger: LIFT-NOTIFY converted it to LIBMH_EVK_SND_RACE_ALERT, and the
        //   hosted sink routes onto the same thunk synchronously at emit (R5-exact), so the emitted
        //   sound is byte-for-byte the same call at the same instant. The exact sibling precedent is
        //   lockstep/lt_reload_snapshot_resync.cpp:22, which binds its own race_alert_sound_emit
        //   member this way -- matched here rather than invented.
        //
        //   llm_strat_group_order_ack_voice IS a rebindable row: it is a `verified` orders_issue
        //   ledger row with an MH_EXPORT_REPLACE in orders/issue/issue_promote.cpp, and
        //   SHIP_PROMOTE_ORDERS_ISSUE is 1, so `mh::call::` was already reaching our body through
        //   the installed E9. Same code at ship; the macro just stops it being a VA site.
        mh::state::evt::snd_race_alert,
        MH_LIBMH_BIND(llm_strat_group_order_ack_voice),
    };
    return c;
}

namespace {

// The cfg type test both functions share: skip a cargo-heli group member (attack_order) / require the
// extra boarding check (enter_building_order). UNIT_TYPE_A_HELI_CARGO/H_HELI_CARGO come from
// sim/sim_order_enqueue.h (included via the header).
inline bool is_cargo_heli(uint32_t type) {
    return type == UNIT_TYPE_A_HELI_CARGO || type == UNIT_TYPE_H_HELI_CARGO;
}

} // namespace

namespace detail {

// THE INVARIANT BOTH BODIES BELOW DEPEND ON, named here because it lives in OTHER files (SIM1-H
// reimpl-verify, lens B, 2026-09-10). The original re-reads `_G_LLM_STRAT_CTRL_GROUPS[0].unit_ids[i]`
// from memory at EVERY use site inside one loop iteration -- four or five separate loads per
// iteration, including across the outward call. These translations read it ONCE per iteration into a
// local and reuse it. That is equivalent only because the two functions called in between --
// `llm_strat_unit_select_weapon` and `llm_unit_state_is_boarding` -- write NOTHING: both are
// documented pure in their own translations (sim/sim_unit_select_weapon.cpp: "Pure read, no callees,
// no writes"; sim/sim_unit_is_boarding.h: "no write of any kind", writes_shared empty). If either of
// those two claims is ever weakened, this caching becomes a real divergence and both loops here must
// go back to re-reading the array at each use. Reviewed and cleared, but the dependency is on a fact
// in a different file, which is why it is written down rather than left to be re-derived.
//
// ---- llm_strat_group_issue_attack_order @0x00444894 -----------------------------------------------
void group_issue_attack_order(const sim_view &v, const group_issue_orders_calls &c, uint32_t param_1,
                              uint32_t player, uint16_t selector) {
    bool any_issued = false; // [EBP-0x1c] -- the ONLY "did we issue" flag; see the header's bVar1 note.

    if (selector < 0x40) {
        // ---- 0x004448df/0x004448e4: selector == 0x20 -------------------------------------------
        if (selector == 0x20) {
            for (int32_t i = 0; i < v.ctrl_groups[0].count; ++i) {
                const uint32_t unit_id = v.ctrl_groups[0].unit_ids[i];
                const uint32_t type    = v.cfg_units[unit_of(v, static_cast<uint32_t>(*v.player_side),
                                                             static_cast<int32_t>(unit_id))
                                                      .unit_proto_id]
                                          .type;
                if (is_cargo_heli(type)) continue; // 0x00444ae2-0x00444b22

                // 0x00444b24-0x00444b3c: select_weapon(PlayerSide, unit_id, mode=2).
                const uint8_t weapon = c.select_weapon(
                    static_cast<uint16_t>(*v.player_side), static_cast<int32_t>(unit_id), 2u);
                if (weapon == 0x64) continue; // 0x00444b47/0x00444b4b
                // 0x00444b4d-0x00444b68: skip only when player==PlayerSide AND unit_id==param_1.
                if (static_cast<uint16_t>(player) == static_cast<uint16_t>(*v.player_side) &&
                    unit_id == param_1)
                    continue;

                // 0x00444b6e-0x00444b8c: order_attack_target(PlayerSide, unit_id, player, param_1, weapon).
                c.order_attack_target(static_cast<uint32_t>(*v.player_side), static_cast<int32_t>(unit_id),
                                      static_cast<uint32_t>(static_cast<uint16_t>(player)),
                                      static_cast<int32_t>(param_1), static_cast<uint32_t>(weapon));
                any_issued = true; // 0x00444b91
            }
        }
    } else if (selector < 0x41) {
        // ---- 0x00444ba2: selector == 0x40 (only value reachable here) --------------------------
        for (int32_t i = 0; i < v.ctrl_groups[0].count; ++i) {
            const uint32_t unit_id = v.ctrl_groups[0].unit_ids[i];
            const uint32_t type    = v.cfg_units[unit_of(v, static_cast<uint32_t>(*v.player_side),
                                                         static_cast<int32_t>(unit_id))
                                                  .unit_proto_id]
                                      .type;
            if (is_cargo_heli(type)) continue; // 0x00444bef-0x00444c2f

            // 0x00444c31-0x00444c49: select_weapon(PlayerSide, unit_id, mode=1).
            const uint8_t weapon = c.select_weapon(static_cast<uint16_t>(*v.player_side),
                                                   static_cast<int32_t>(unit_id), 1u);
            if (weapon == 0x64) continue; // 0x00444c54/0x00444c58 -- NO self-exclude check here.

            // 0x00444c5a-0x00444c78: order_attack_building_reposition(PlayerSide, unit_id, player,
            // param_1, weapon).
            c.order_attack_building_reposition(
                static_cast<uint32_t>(*v.player_side), static_cast<int32_t>(unit_id),
                static_cast<uint32_t>(static_cast<uint16_t>(player)), static_cast<int32_t>(param_1),
                static_cast<uint32_t>(weapon));
            any_issued = true; // 0x00444c7d
        }
    } else if (selector == 0x80) {
        // ---- 0x004448ef: selector == 0x80 -------------------------------------------------------
        for (int32_t i = 0; i < v.ctrl_groups[0].count; ++i) {
            const uint32_t unit_id = v.ctrl_groups[0].unit_ids[i];
            const uint32_t type    = v.cfg_units[unit_of(v, static_cast<uint32_t>(*v.player_side),
                                                         static_cast<int32_t>(unit_id))
                                                  .unit_proto_id]
                                      .type;
            if (is_cargo_heli(type)) continue; // 0x0044493c/0x00444973 (type==0x17/0x18), skip via LAB_0044497c

            // 0x0044497e-0x0044499e: select_weapon(PlayerSide, unit_id, mode=1).
            const uint8_t weapon = c.select_weapon(static_cast<uint16_t>(*v.player_side),
                                                   static_cast<int32_t>(unit_id), 1u);
            if (weapon == 0x64) continue; // 0x004449a1/0x004449a5
            // 0x004449a7-0x004449c2: skip only when player==PlayerSide AND unit_id==param_1.
            if (static_cast<uint16_t>(player) == static_cast<uint16_t>(*v.player_side) &&
                unit_id == param_1)
                continue;

            // 0x004449cb-0x00444a05: attack_unit fires only when (RSHIFT bit0 OR LSHIFT bit0) is set
            // AND LCTRL is NOT held (neither bit set). Note RSHIFT/LSHIFT test ONLY bit 0 -- see the
            // header banner; LCTRL tests both bits.
            const bool shift_pressed =
                (*v.key_rshift_held & 0x1) != 0 || (*v.key_lshift_held & 0x1) != 0;
            bool use_attack_unit = false;
            if (shift_pressed) {
                const bool lctrl_held = (*v.key_lctrl_held & 0x1) != 0 || (*v.key_lctrl_held & 0x2) != 0;
                use_attack_unit       = !lctrl_held; // local_2c != 0 @0x00444a05 -- see the bVar1-artifact note
            }

            if (use_attack_unit) {
                // 0x00444a09-0x00444a27: order_attack_unit(PlayerSide, unit_id, player, param_1, weapon).
                c.order_attack_unit(static_cast<uint32_t>(*v.player_side), static_cast<int32_t>(unit_id),
                                    static_cast<uint32_t>(static_cast<uint16_t>(player)),
                                    static_cast<int32_t>(param_1), static_cast<uint32_t>(weapon));
            } else {
                // 0x00444a2e-0x00444a63: LALT held -> attack_target_alt, else -> attack_target. Both
                // pass the CONSTANT weapon 4, not the select_weapon result.
                const bool lalt_held = (*v.key_lalt_held & 0x1) != 0 || (*v.key_lalt_held & 0x2) != 0;
                if (lalt_held) {
                    c.order_attack_target_alt(static_cast<uint32_t>(*v.player_side),
                                              static_cast<int32_t>(unit_id),
                                              static_cast<uint32_t>(static_cast<uint16_t>(player)),
                                              static_cast<int32_t>(param_1), 4u);
                } else {
                    c.order_attack_target(static_cast<uint32_t>(*v.player_side),
                                          static_cast<int32_t>(unit_id),
                                          static_cast<uint32_t>(static_cast<uint16_t>(player)),
                                          static_cast<int32_t>(param_1), 4u);
                }
            }
            any_issued = true; // 0x00444a84
        }
    }
    // else: selector is none of {0x20,0x40,0x80} -- no-op, any_issued stays false (0x00444c89).

    if (any_issued) {
        c.race_alert_sound_emit(); // 0x00444c8f
    }
}

// ---- llm_strat_group_issue_enter_building_order @0x004451d2 ---------------------------------------
void group_issue_enter_building_order(const sim_view &v, const group_issue_orders_calls &c,
                                      uint32_t param_1, uint32_t param_2, uint32_t param_3) {
    bool any_issued = false; // [EBP-0x10]

    for (int32_t i = 0; i < v.ctrl_groups[0].count; ++i) { // [EBP-0x14]
        const uint32_t unit_id = v.ctrl_groups[0].unit_ids[i];
        const unit    &u       = unit_of(v, static_cast<uint32_t>(*v.player_side), static_cast<int32_t>(unit_id));
        const uint32_t type    = v.cfg_units[u.unit_proto_id].type;

        // 0x00445245-0x00445283: non-cargo-heli members always issue. A cargo-heli member (0x17 or
        // 0x18) issues only when NOT currently boarding (0x00445285-0x004452b4).
        bool issue = true;
        if (is_cargo_heli(type)) {
            issue = c.unit_state_is_boarding(static_cast<int32_t>(u.state)) == 0;
        }

        if (issue) {
            // 0x004452bd-0x004452da: order_exit_storage(PlayerSide, unit_id, param_1, param_2, param_3)
            // -- register trace EAX/EDX/EBX/ECX/stack.
            c.order_exit_storage(static_cast<uint32_t>(*v.player_side), unit_id,
                                 static_cast<int32_t>(param_1), param_2, param_3);
            any_issued = true; // 0x004452df
        }
    }

    if (any_issued) {
        c.group_order_ack_voice(); // 0x004452f1
    }
}

} // namespace detail

// ---- the public wrappers ---------------------------------------------------------------------------

void group_issue_attack_order(uint32_t param_1, uint32_t player, uint16_t selector) {
    sim_state st = state();
    detail::group_issue_attack_order(st.read, live_group_issue_orders_calls(), param_1, player, selector);
}

void group_issue_enter_building_order(uint32_t param_1, uint32_t param_2, uint32_t param_3) {
    sim_state st = state();
    detail::group_issue_enter_building_order(st.read, live_group_issue_orders_calls(), param_1, param_2,
                                             param_3);
}

} // namespace mh::sim
