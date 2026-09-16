//
// tact/tact_unit_spawn.cpp -- see tact_unit_spawn.h. Translated from the DISASSEMBLY, not from
// Ghidra's C.
//
#include "tact/tact_unit_spawn.h"

#include "addr/mh_calls.gen.h" // frontier callees (Law 4)
#include "state/host_api.h"
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "tact/tact_unit_vision.h"
#include "state/rebind_targets.gen.h"
#include "crt/crt_select.h" // LIB-CRT: MH_CRT() picks the vendored CRT in the standalone build

namespace mh::tact {

const unit_spawn_calls &live_unit_spawn_calls() {
    static const unit_spawn_calls c = {
        mh::tact_host().llm_fatal_cleanup,
        mh::tact_host().utils_abort,
        MH_LIBMH_BIND(time_GetCurrentTime),
        MH_CRT(llm_rand),
        MH_LIBMH_BIND(llm_tact_unit_vision_add),
    };
    return c;
}

namespace detail {

int32_t unit_spawn(const tact_view &tv, tact_store &own, const unit_spawn_calls &c,
                   int32_t char_type, int32_t col, int32_t row, uint8_t facing_dir,
                   uint8_t def_stat, int32_t hp_pct) {
    // @0x0042b969-0x0042b982: the tile must be passable, or the original terminates the process.
    if (mh::tact::passable_at(tv, col, row) == mh::state::PASSABLE_BLOCKED) {
        c.llm_fatal_cleanup();
        c.utils_abort(0);
        return 0; // unreachable: utils_abort never returns
    }

    // @0x0042b987-0x0042b9c2: linear scan for a free roster slot. Slot 0 is never tested (matches
    // TACT_UNIT_FIRST_SLOT/TACT_UNIT_LAST_SLOT's own documented off-by-one).
    int32_t slot = -1;
    for (int32_t i = TACT_UNIT_FIRST_SLOT; i <= TACT_UNIT_LAST_SLOT; ++i) {
        if (mh::tact::unit_of(tv, i).type == 0) {
            slot = i;
            break;
        }
    }
    if (slot == -1) {
        // @0x0042b9c8: no free slot -- nothing above has touched any state yet.
        return 0;
    }

    // @0x0042b9d4-0x0042b9ed: the CHARACTER class must be a filled slot, or fatal-abort. Unchecked
    // against TACT_CHARACTER_TYPE_SLOTS, exactly like the original.
    const character_type &ct = tv.character_types[char_type];
    if (ct.id == 0) {
        c.llm_fatal_cleanup();
        c.utils_abort(0);
        return 0; // unreachable: utils_abort never returns
    }

    tact_unit &u = own.unit_at(slot);

    // @0x0042b9ed-0x0042ba8c: identity/placement fields, in the original's own order.
    u.type         = (uint8_t)char_type;
    u.owner        = ct.who; // plain byte copy -- the WHO-XOR-key was already undone at parse time
    u.status       = 0;
    u.pos_col      = (uint8_t)col;
    u.pos_row      = (uint8_t)row;
    u.vision_angle = (uint16_t)ct.angle_see;
    u.vision_dist  = ct.distance_see;
    u.facing_dir   = facing_dir;
    u.def_stat     = def_stat;

    // @0x0042ba92-0x0042bad8: two SEPARATE time_GetCurrentTime() calls/stores (not one value copied
    // twice). cmd_wait_until_time = 0.0 reproduces the original's two-dword zero-store idiom.
    u.move_state_timer    = c.time_get_current_time();
    u.wander_check_time   = c.time_get_current_time();
    u.cmd_wait_until_time = 0.0;

    // @0x0042bad8-0x0042baff
    u.progress       = 0;
    u.move_path_slot = 0;
    u.move_path_step = 0;

    // @0x0042baff-0x0042bb17: stamp the tile's `.building` field with this unit's roster slot --
    // NOT `.unit` (see the header banner NOTE on tact_unit_despawn.cpp's mismatching write).
    own.planes().tile_object_at(col, row).building = (uint16_t)slot;

    // @0x0042bb17-0x0042bb1a: an ASSIGNMENT of the found slot index, not an increment -- see the
    // header banner. Preserved literally.
    own.unit_active_count() = slot;

    // @0x0042bb1f-0x0042bb2f: the tile is now occupied.
    own.planes().passable_at(col, row) = mh::state::PASSABLE_BLOCKED;

    // @0x0042bb2f-0x0042bb95: hp_pct range guard. Out of [1, 100] releases the slot's `.type` back
    // to 0 but does NOT undo the tile_objects/passable writes or the active-count assignment above,
    // and does NOT reset any other field already written -- preserved literally (Law 2).
    if (hp_pct < 1 || hp_pct > 100) {
        u.type = 0;
        return 0;
    }

    // @0x0042bb3d-0x0042bb68: hp = (energy, read ZERO-EXTENDED despite its int16_t C type -- the
    // original uses MOVZX, not MOVSX -- times hp_pct) / 100, a genuine signed IDIV (truncates
    // toward zero, matching C++ `/` on int32_t). Floored at 1.
    const int32_t energy_zx = (int32_t)(uint16_t)ct.energy;
    int32_t       hp_calc   = (energy_zx * hp_pct) / 100;
    if (hp_calc == 0) hp_calc = 1;

    // @0x0042bb68-0x0042bbf3
    u.hp                   = (uint16_t)hp_calc;
    u.move_retry_wait      = 0;
    u.move_retry_attempts  = 0;
    u.move_stuck_countdown = 0;
    u.cmd_index            = 0;
    u.anim_frame_time      = c.time_get_current_time();
    u.frame_index          = 0;
    u.anim_cycle_time      = c.time_get_current_time();

    // @0x0042bc05-0x0042bc2b: another genuine signed IDIV (llm_rand() / 0x1999, truncates toward
    // zero). UNIT_ANIM_FRAME_INTERVAL_BASE is a declared need -- see the header banner point 13.
    const int32_t rand_div = c.llm_rand() / 0x1999;
    u.frame_interval       = (double)rand_div + *tv.unit_anim_frame_interval_base;

    // @0x0042bc31-0x0042bcb5: gun setup. Gun ids used unchecked as fx_type_table indices, exactly
    // as the original. magazines-1 is a plain byte decrement (wraps to 0xff from 0), matching the
    // original's DEC on a byte.
    const fx_type &gun1 = tv.fx_type_table[ct.number_gun1];
    const fx_type &gun2 = tv.fx_type_table[ct.number_gun2];
    u.active_gun        = 0;
    u.gun1_bullets      = gun1.bullets;
    u.gun1_magazines    = (uint8_t)(gun1.magazines - 1);
    u.gun2_bullets      = gun2.bullets;
    u.gun2_magazines    = (uint8_t)(gun2.magazines - 1);

    // @0x0042bcbb-0x0042bd48: all 128 cmd_queue entries' op/arg0..arg3 zeroed -- `interrupt_flag` is
    // DELIBERATELY left untouched (mh_llm_tact_unit_cmd_entry::interrupt_flag's own field comment).
    for (auto &entry : u.cmd_queue) {
        entry.op   = 0;
        entry.arg0 = 0;
        entry.arg1 = 0;
        entry.arg2 = 0;
        entry.arg3 = 0;
    }

    // @0x0042bd48-0x0042bdb4: the immediate ATTACK/AIM record's payload zeroed -- `attack_interrupt_
    // flag` is likewise left untouched. The FACE/TURN record and move_aborted_op are not touched at
    // all by this function (see the header banner's "fields not initialised" list).
    u.attack_cmd_op     = 0;
    u.attack_gun_toggle = 0;
    u.attack_cmd_arg1   = 0;
    u.aim_x             = 0;
    u.aim_y             = 0;
    u.anim_state        = 0;
    u.squad_group_id    = 0xff;

    // @0x0042bdb4-0x0042bdbf
    c.unit_vision_add(slot);
    return slot;
}

} // namespace detail

int32_t unit_spawn(int32_t char_type, int32_t col, int32_t row, uint8_t facing_dir,
                   uint8_t def_stat, int32_t hp_pct) {
    tact_state st = state();
    return detail::unit_spawn(st.read, st.own, live_unit_spawn_calls(), char_type, col, row,
                              facing_dir, def_stat, hp_pct);
}

} // namespace mh::tact


namespace mh::tact {


} // namespace mh::tact
