//
// sim/sim_order_dispatch_admin.cpp -- one third of llm_strat_order_queue_dispatch (RI-SIM / SIM1C).
// See sim_order_dispatch.h for the whole-function contract and sim_order_dispatch.cpp for the loop
// this is an arm of. Translated from the DISASSEMBLY (tmp/decomp/llm_strat_order_queue_dispatch_
// 00466892.asm), not from Ghidra's C draft -- see the batch context (tmp/decomp_sim/
// _CONTEXT_dispatch.md) on why the draft is not trustworthy for this function specifically.
//
// TWO PIECES, NEITHER OF WHICH DEFINES A SHADOW SITE OF ITS OWN (the whole function is armed once,
// from sim_order_dispatch.cpp's install_shadow_dispatch):
//
//   detail::dispatch_admin_order   0x00469691-0x0046997e. The ADMIN table -- the fall-through path
//                                  for every order kind the router does not recognise (and the unit/
//                                  storage kinds' own unrecognised-order_code default), keyed on the
//                                  record's order_code via decode_admin_order() (sim_order_dispatch.h).
//                                  23 arms (index 0..22); index 0 is the default -- and where the
//                                  scan's one-byte over-read past the table lands -- and its body is
//                                  empty (falls straight to the loop tail).
//
//   detail::dispatch_lockstep_extend   0x004695d4-0x0046961a. The kind-0xf0 body, which belongs to NO
//                                  table -- the router jumps to it directly on owner_and_kind's high
//                                  nibble == 0xf0 (ORDER_KIND_LOCKSTEP), before either switch runs.
//                                  NOT YET DECLARED in sim_order_dispatch.h (conductor-owned); the
//                                  conductor adds the declaration and the router's call to it.
//
#include "sim/sim_order_dispatch.h"

#include "state/host_events.h" // the kick-answer slot the split wait overlay clears (R9)

namespace mh::sim::detail {

// ---- the kind-0xf0 body: the lockstep-horizon extension request ------------------------------
//
// llm_strat_order @0x004695d4-0x0046961a. Reached directly by the router (CMP owner_and_kind-kind,
// 0xf0 / JZ) -- it is not behind either sparse switch and has no order_code test of its own; the
// record's PARAM0 (not order_code) is what is tested here, against the literal event id 0xd (the
// struct comment names this "kind 0xf0: event id").
//
// Two gates, both must hold, tested in this order (asm: CMP param0,0xd / JNZ skip; TEST flags,0x40 /
// JNZ skip):
//   (1) order.param0 == 0xd
//   (2) the 0x40 bit of net_lockstep_status_flags is CLEAR (TEST+JNZ: JNZ takes the "bit already set"
//       skip, so the body runs only when the bit reads 0 -- i.e. only the FIRST such record per
//       "extension not yet requested" window fires the call/send pair below).
//
// When both hold, in this exact order (asm 0x004695eb-0x00469615):
//   1. llm_net_lockstep_extend_ui_enter() -- called BEFORE the bit is set or the horizon computed.
//   2. net_lockstep_status_flags |= 0x40 -- so it fires once until something else clears the bit.
//   3. lockstep_horizon = game_clock + lockstep_step_size (FLD/FADD/FSTP, x87 double; NO comparison
//      anywhere in this body, so there is no NaN-vs-ordering subtlety to preserve here -- unlike the
//      28 `FLDZ;FCOMP;JNC` energy gates elsewhere in the function, this is a straight-line add with
//      nothing branching on the float's value). touches_floats is true purely because of the
//      arithmetic, not because of any comparison.
//   4. llm_net_send_lockstep_extend(horizon) -- pushed as two dwords (high half then low half) in the
//      asm, which is cdecl's plain double-on-stack convention; the `calls` thunk hides that and takes
//      a `double` directly.
//
// Both outward calls here are in the SIM-CUT effect seam's `effectful` set (sim_order_dispatch.h's
// header note) -- see uncertainties[] on the "three outward calls" wording in the batch brief, which
// this body's actual call count (two, both effectful) does not match.
void dispatch_lockstep_extend(const sim_view &v, sim_store &own, const dispatch_calls &c,
                              const dispatch_ctx &x) {
    const order &rec = own.order_at(x.slot);
    if (rec.param0 != 0xd) return;
    if ((own.net_lockstep_status_flags() & 0x40u) != 0) return;

    // LIFT-SCREEN -- llm_net_lockstep_extend_ui_enter SPLIT. The original (0x004c85bd) is three
    // statements: GAME_MODE_SAVED = GAME_MODE @0x004c85c8, the wait overlay @0x004c85e8, and
    // GAME_MODE = 8 @0x004c85ed. Two of the three are state and belong to libmh; only the middle
    // one is a screen.
    //
    // ORDER: the saved-mode store reads the PRE-overlay mode, and the overlay's own arming guard
    // reads GAME_MODE as well, so it lands BEFORE the call and the = 8 after. NO arming latch is
    // needed here, unlike the timekeeper's sibling site: the overlay's internal mode-3 write is
    // unobservable because nothing runs between it and the = 8 that overwrites it.
    c.ovl_mode_saved_set(own.game_mode());
    // The overlay's OVERLAY_RESULT = -1 @0x004c7ddb, in libmh's terms: opening a fresh wait
    // overlay discards any kick answer still pending from the last one (R9's slot).
    mh::state::screen_answer_clear(LIBMH_SCR_ANS_LOCKSTEP_KICK);
    (void)c.llm_net_lockstep_wait_player_overlay_show(static_cast<int32_t>(*v.player_side));
    own.game_mode() = 8;
    own.net_lockstep_status_flags() |= 0x40u;

    const double horizon   = *v.game_clock + *v.lockstep_step_size;
    own.lockstep_horizon() = horizon;
    c.llm_net_send_lockstep_extend(horizon);
}

// ---- the ADMIN table: 23 arms, keyed on decode_admin_order(order.order_code) -------------------
//
// Every arm reads the record fresh from own.order_at(x.slot) (no caching across the switch -- there
// is nothing here that could go stale within one dispatch, but the convention matches every other
// sim TU: `own` is a live handle, not a snapshot). x.player / x.object_index are the loop's own
// [EBP-0x34] / [EBP-0x30] locals, computed once per record by the (sibling) loop TU and handed down
// unchanged -- every arm below reads them, never the record's owner_and_kind/unit_index fields
// directly, exactly as the assembly does (MOV EAX,[EBP-0x34] / [EBP-0x30], never a fresh
// owner_and_kind decode inside the switch).
void dispatch_admin_order(const sim_view &v, sim_store &own, const dispatch_calls &c, const dispatch_ctx &x) {
    const order &rec = own.order_at(x.slot);

    switch (decode_admin_order(rec.order_code)) {
        case admin_arm::DEFAULT: // unrecognised order_code (and the scan's over-read byte, which is
                                 // not an opcode) -- empty arm, straight to the loop tail.
            break;

        case admin_arm::UNIT_STATUS_BIT_SET: // 0x34: llm_unit_status_bit_set(player, object_index, args[0])
            c.llm_unit_status_bit_set((int32_t)x.player, x.object_index, (uint8_t)rec.args[0]);
            break;

        case admin_arm::UNIT_STATUS_BIT_CLEAR: // 0x35: llm_unit_status_bit_clear(player, object_index, args[0])
            c.llm_unit_status_bit_clear((int32_t)x.player, x.object_index, (uint8_t)rec.args[0]);
            break;

        case admin_arm::DEBUG_ROLL_RANDOM: // 0xe7: llm_debug_roll_random() -- no arguments.
            c.llm_debug_roll_random();
            break;

        case admin_arm::GAME_SPEED_INCREASE: // 0xe8: llm_game_speed_increase(player)
            c.llm_game_speed_increase(x.player);
            break;

        case admin_arm::GAME_SPEED_DECREASE: // 0xe9: llm_game_speed_decrease(player)
            c.llm_game_speed_decrease(x.player);
            break;

        case admin_arm::UNIT_CREATE: { // 0xeb: create a unit of type args[1] at (args[4], args[5]) for `player`. Which create
            // path runs is gated on the PROTOTYPE'S cfg type, not on anything in the order record itself:
            // type <= 0xe (soldier-ish types) -> llm_unit_create_soldier; type > 0xe -> llm_strat_unit_create.
            // Both calls take the SAME argument shape (x=args[4], y=args[5], unit=args[1], player,
            // is_ship/param_5 = the literal 1 the asm pushes in both arms).
            const cfg_unit &proto = v.cfg_units[(uint32_t)rec.args[1]];
            if (proto.type > 0xeu) {
                c.llm_strat_unit_create((uint32_t)rec.args[4], (uint32_t)rec.args[5],
                                        (uint16_t)rec.args[1], (uint16_t)x.player, 1);
            } else {
                c.llm_unit_create_soldier((uint32_t)rec.args[4], (uint32_t)rec.args[5],
                                          (uint16_t)rec.args[1], (uint16_t)x.player, 1);
            }
            break;
        }

        case admin_arm::POPULATION_ADD_REMOVE: // 0xec: population delta, three-way on args[1] (0x00469835-0x00469877): ==0 -> nothing;
            // >0 -> llm_strat_population_add(player, args[1]); <=0 (i.e. <0, since ==0 already excluded)
            // -> llm_strat_population_remove(player, args[1]). The guard slot and the store slot are the
            // SAME (args[1] both times) -- no guard/store mismatch on this arm.
            if (rec.args[1] != 0) {
                if (rec.args[1] > 0)
                    c.llm_strat_population_add((uint16_t)x.player, rec.args[1]);
                else
                    c.llm_strat_population_remove(x.player, rec.args[1]);
            }
            break;

        case admin_arm::RESOURCE_ADD_SPEND: // 0xed: resource delta, three-way on args[2] (0x004697dc-0x00469830): ==0 -> nothing;
            // >0 -> llm_resource_add(player, args[3], args[2]); <=0 -> game_SpendResource(player, args[3],
            // args[2]). Same-slot guard/store as case 7, just resource_index=args[3] / amount=args[2].
            if (rec.args[2] != 0) {
                if (rec.args[2] > 0)
                    c.llm_resource_add((int32_t)x.player, rec.args[3], rec.args[2]);
                else
                    c.game_SpendResource((int32_t)x.player, rec.args[3], rec.args[2]);
            }
            break;

        case admin_arm::PROJECTS_COLLECT: // 0xee: llm_progress_collect_available_projects(player)
            c.llm_progress_collect_available_projects(x.player);
            break;

        case admin_arm::RECHECK_BUILDINGS: // 0xef: llm_progress_recheck_buildings(player)
            c.llm_progress_recheck_buildings(x.player);
            break;

        case admin_arm::RECHECK_PROJECTS: // 0xf0: llm_progress_recheck_projects(player)
            c.llm_progress_recheck_projects(x.player);
            break;

        case admin_arm::RECHECK_PLANET_ALL: // 0xf1: llm_progress_recheck_planet_system_all_players() -- no arguments, no player.
            c.llm_progress_recheck_planet_system_all_players();
            break;

        case admin_arm::BLDG_QUEUE_CONSTRUCT: // 0xf2: llm_bldg_queue_construction(player, building_type=args[0], x=args[4], y=args[5])
            c.llm_bldg_queue_construction((int32_t)x.player, rec.args[0], rec.args[4], rec.args[5]);
            break;

        case admin_arm::UNIT_RECRUIT: // 0xf3: llm_unit_recruit(player, unit_type_id=args[1])
            c.llm_unit_recruit(x.player, (uint32_t)rec.args[1]);
            break;

        case admin_arm::DIPLOMACY_SET_RELATION: // 0xf4: llm_diplomacy_set_relation(player, other_player=args[2], relation=args[3])
            c.llm_diplomacy_set_relation((int32_t)x.player, rec.args[2], (uint8_t)rec.args[3]);
            break;

        case admin_arm::PLAYER_SET_AI_HUMAN: { // 0xf5: only when the GLOBAL PlayerSide equals args[2] (an "is this local player's own
            // event" gate -- the record is being applied on every machine, but the human/AI toggle only
            // takes effect on the machine whose own side matches): args[3]==0 -> llm_game_player_set_ai
            // (player); args[3]!=0 -> llm_game_player_set_human(player). No else branch when PlayerSide
            // doesn't match -- the arm is then a no-op.
            const int32_t side = (int32_t)(uint16_t)*v.player_side;
            if (side == rec.args[2]) {
                if (rec.args[3] == 0)
                    c.llm_game_player_set_ai((uint8_t)x.player);
                else
                    c.llm_game_player_set_human((uint8_t)x.player);
            }
            break;
        }

        case admin_arm::ENERGY_REFILL_FULL: // 0xf6: llm_unit_bldg_energy_refill_full(player_and_flags=args[2], target_index=args[3])
            c.llm_unit_bldg_energy_refill_full((uint32_t)rec.args[2], (uint32_t)rec.args[3]);
            break;

        case admin_arm::APPLY_SCALED_DAMAGE: // 0xf7: llm_unit_bldg_apply_scaled_damage(target_selector=args[2], target_index=args[3])
            c.llm_unit_bldg_apply_scaled_damage((uint32_t)rec.args[2], rec.args[3]);
            break;

        case admin_arm::APPLY_LETHAL_DAMAGE: // 0xf8: llm_unit_bldg_apply_lethal_damage(target_ref=args[2], target_index=args[3])
            c.llm_unit_bldg_apply_lethal_damage((uint32_t)rec.args[2], rec.args[3]);
            break;

        case admin_arm::FOW_REVEAL_FULL: // 0xf9: llm_map_fow_reveal_full(args[2]) -- the sole argument comes from the RECORD
            // (args[2]), NOT from x.player like every neighbouring arm; verified against the asm, which
            // loads only args[2] into EAX before the call and never touches [EBP-0x34].
            c.llm_map_fow_reveal_full((uint32_t)rec.args[2]);
            break;

        case admin_arm::CREDIT_CONQUEST_KILLS: // 0xfa: llm_combat_credit_planet_conquest_kills(player)
            c.llm_combat_credit_planet_conquest_kills(x.player);
            break;

        case admin_arm::UNIT_NOTIFY_STATUS: // 0xfb: llm_strat_unit_notify_status(player, object_index, status_code=0). The asm falls
            // straight through into the tail with no intervening jmp -- `break` here is the same control
            // flow (there is no case after this one to fall into).
            c.llm_strat_unit_notify_status(x.player, x.object_index, 0);
            break;
    }
}

} // namespace mh::sim::detail
