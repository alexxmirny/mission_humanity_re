//
// tact/tact_unit_weapons_tick.cpp -- see tact_unit_weapons_tick.h. Translated from the DISASSEMBLY,
// not from Ghidra's C.
//
#include "tact/tact_unit_weapons_tick.h"

#include "addr/mh_calls.gen.h"  // frontier callees (Law 4) -- includes unit_death_tick/unit_kneel_tick,
                                // called as the ORIGINAL per the batch context even though sibling
                                // agents are translating those two bodies in this same batch
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "tact/tact_unit_cmd_advance.h"
#include "tact/tact_unit_cmd_stance.h"
#include "tact/tact_unit_set_anim_state.h"
#include "tact/tact_unit_stand_tick.h"
#include "state/rebind_targets.gen.h"

namespace mh::tact {

const unit_weapons_tick_calls &live_unit_weapons_tick_calls() {
    static const unit_weapons_tick_calls c = {
        MH_LIBMH_BIND(time_GetCurrentTime),
        MH_LIBMH_BIND(llm_tact_unit_cmd_queue_advance),
        MH_LIBMH_BIND(llm_tact_unit_death_tick),
        MH_LIBMH_BIND(llm_tact_unit_fire_weapon),
        MH_LIBMH_BIND(llm_tact_unit_cmd_advance),
        MH_LIBMH_BIND(llm_tact_unit_rotate_tick),
        MH_LIBMH_BIND(llm_tact_unit_move_tick),
        MH_LIBMH_BIND(llm_tact_unit_kneel_tick),
        MH_LIBMH_BIND(llm_tact_unit_stand_tick),
        MH_LIBMH_BIND(llm_tact_unit_mine_arm_tick),
        MH_LIBMH_BIND(llm_tact_unit_set_anim_state),
        MH_LIBMH_BIND(llm_tact_unit_cmd_teleport_jump_tick),
        MH_LIBMH_BIND(llm_tact_unit_cmd_advance_with_defstat),
        MH_LIBMH_BIND(llm_tact_unit_cmd_stance_off),
        MH_LIBMH_BIND(llm_tact_unit_cmd_stance_on),
        MH_LIBMH_BIND(llm_tact_unit_cmd_queue_resubmit_run),
    };
    return c;
}

namespace detail {

void unit_weapons_tick(const tact_view &tv, tact_store &own, const unit_weapons_tick_calls &c,
                       int32_t unit_idx) {
    tact_unit &u = own.unit_at(unit_idx);

    // @0x0042f34a-0x0042f361: read ONCE, before the loop; reused by every iteration's
    // CHARACTER_TYPES lookups below.
    const uint8_t char_type = u.type;

    for (;;) {
        // @0x0042f364-0x0042f379: the per-call time gate -- a FRESH read every iteration, never
        // reused across iterations.
        const double now = c.time_get_current_time();
        if (now <= u.move_state_timer) {
            // @0x0042f8bd-0x0042f8da
            u.status &= 0xf7;
            return;
        }

        // @0x0042f37f-0x0042f3b6: this function is the documented sole writer of both globals (see
        // tact_state.h's own comment on tact_view::move_cur_col/row).
        own.move_cur_col()      = u.pos_col;
        own.move_cur_row()      = u.pos_row;
        const int32_t cmd_index = u.cmd_index; // re-read every iteration, not hoisted

        // @0x0042f3b9-0x0042f3e4
        c.unit_cmd_queue_advance(unit_idx, static_cast<uint32_t>(cmd_index));
        if (u.anim_state == 0x1f) {
            const int32_t death_result = c.unit_death_tick(unit_idx);
            if (death_result != 0) return; // @0x0042f8da -- direct RET, no status-bit clear
            continue;                      // @0x0042f3e4 -> loop restart
        }

        // @0x0042f3e9-0x0042f42a
        if (u.attack_cmd_op == 2) {
            c.unit_fire_weapon(unit_idx, cmd_index, u.attack_gun_toggle);
            u.status |= 8;
        }

        // @0x0042f430-0x0042f4dd
        if (u.cmd_queue[cmd_index].op == 7) {
            if (u.cmd_queue[cmd_index].arg0 != 0) {
                u.face_interrupt_flag = 0;
                u.face_cmd_op         = 6;
                u.face_cmd_target_dir = u.cmd_queue[cmd_index].arg0;
                u.face_cmd_arg1       = 0;
                u.face_cmd_arg2       = 0;
                u.face_cmd_arg3       = 0;
            }
            c.unit_cmd_advance(unit_idx, cmd_index); // called even when arg0 == 0
        }

        // @0x0042f4dd-0x0042f509
        if (u.face_cmd_op == 6 && u.progress == 0) {
            c.unit_rotate_tick(unit_idx);
            continue; // @0x0042f509 -> loop restart, skipping everything below
        }

        // @0x0042f50e-0x0042f533
        if ((u.status & 0x40) != 0) {
            u.move_state_timer = c.time_get_current_time();
            continue; // @0x0042f533
        }

        // @0x0042f538-0x0042f58d
        if ((u.status & 0x20) != 0 && u.progress == 0) {
            u.status           = (uint8_t)((u.status & 0xdf) | 0x40);
            u.move_state_timer = c.time_get_current_time();
            continue; // @0x0042f58d
        }

        // @0x0042f592-0x0042f5d6: the deferred-WAIT gate.
        if (u.cmd_wait_until_time > 0.0) {
            if (c.time_get_current_time() < u.cmd_wait_until_time) {
                // @0x0042f5bf: a SECOND, independent time read -- not a reuse of the compare above.
                u.move_state_timer = c.time_get_current_time();
                return; // @0x0042f5d1 -- direct RET, no status-bit clear
            }
            // else: the wait has elapsed (or was never armed) -- fall through to the dispatch.
        }

        // @0x0042f5d6-0x0042f8b8: the command-queue op dispatch. An explicit compare chain, not a
        // jump table (dead-ends G61 does not apply). Every arm below reconverges at @0x0042f8b8,
        // which jumps straight back to the loop top -- reproduced here as simply falling off the end
        // of the loop body (no arm returns or breaks out of the for(;;) besides the two above).
        const uint16_t op = u.cmd_queue[cmd_index].op;
        if (op < 9) {
            if (op < 4) {
                if (op == 0) {
                    // @0x0042f6ad
                    if (u.owner == 0) u.status &= 0xfb;
                    u.move_state_timer = c.time_get_current_time();
                } else if (op == 1) {
                    // @0x0042f760
                    u.cmd_wait_until_time = 0.0;
                    const double dt       = ((u.status & 4) == 0)
                                                ? tv.character_types[char_type].speed
                                                : tv.character_types[char_type].speed /
                                                tv.character_types[char_type].run_speed;
                    c.unit_move_tick(unit_idx, cmd_index, dt);
                }
                // op in {2,3}: no-op.
            } else if (op < 5) { // == 4
                // @0x0042f7d6
                c.unit_kneel_tick(unit_idx);
                u.move_state_timer += tv.character_types[char_type].kneel_time;
            } else if (op < 6) { // == 5
                // @0x0042f803: SAME field as the op==4 arm (kneel_time), transcribed literally.
                c.unit_stand_tick(unit_idx);
                u.move_state_timer += tv.character_types[char_type].kneel_time;
            } else if (op == 8) {
                // @0x0042f6f1
                const uint16_t wait_ticks = u.cmd_queue[cmd_index].arg0;
                u.cmd_wait_until_time     = c.time_get_current_time() + (double)wait_ticks;
                c.unit_cmd_advance(unit_idx, cmd_index);
            }
            // op in {6,7}: no-op.
        } else if (op < 10) { // == 9
            // @0x0042f84d
            if (*tv.mines_enabled == 0) {
                u.progress = 0;
                c.unit_set_anim_state(unit_idx, 0);
                // @0x0042f882: a FRESH re-read of the field, not the cached `cmd_index` local.
                c.unit_cmd_advance(unit_idx, u.cmd_index);
            } else {
                c.unit_mine_arm_tick(unit_idx);
            }
        } else if (op < 0x1c) {
            if (op < 0xb) { // == 0xa
                // @0x0042f893
                c.unit_cmd_teleport_jump_tick(unit_idx, cmd_index);
            } else if (op == 0xb) {
                // @0x0042f8a0
                c.unit_cmd_advance_with_defstat(unit_idx, cmd_index);
            }
            // op in [0xc,0x1b]: no-op.
        } else if (op < 0x1d) { // == 0x1c
            // @0x0042f840
            c.unit_cmd_stance_off(unit_idx);
        } else if (op < 0x40) {
            if (op == 0x1e) {
                // @0x0042f830
                c.unit_cmd_stance_on(unit_idx);
            }
            // op in {0x1d} u [0x1f,0x3f]: no-op.
        } else if (op < 0x41) { // == 0x40
            // @0x0042f733
            c.unit_cmd_queue_resubmit_run(unit_idx, cmd_index);
            u.move_state_timer += tv.character_types[char_type].speed;
        } else if (op == 0x7f) {
            // @0x0042f8ad
            c.unit_cmd_advance(unit_idx, cmd_index);
        }
        // else (op in [0x41,0x7e] u [0x80,0xffff]): no-op.
    }
}

} // namespace detail

void unit_weapons_tick(int32_t unit_idx) {
    tact_state st = state();
    detail::unit_weapons_tick(st.read, st.own, live_unit_weapons_tick_calls(), unit_idx);
}

} // namespace mh::tact


namespace mh::tact {


} // namespace mh::tact
