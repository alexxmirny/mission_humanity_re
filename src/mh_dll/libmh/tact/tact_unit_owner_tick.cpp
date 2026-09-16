//
// tact/tact_unit_owner_tick.cpp -- see tact_unit_owner_tick.h. Translated from the DISASSEMBLY, not
// from Ghidra's C.
//
#include "tact/tact_unit_owner_tick.h"

#include "addr/mh_calls.gen.h"  // frontier callees (Law 4)
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "tact/tact_calc_dir24.h"
#include "tact/tact_dir24_approach.h"
#include "tact/tact_tile_occupancy.h"
#include "tact/tact_unit_enqueue_command.h"
#include "tact/tact_unit_vision.h"
#include "tact/tact_unit_weapon_in_range.h"
#include "state/mode_planes.h"
#include "crt/crt_select.h"        // LIB-CRT: MH_CRT() picks the vendored CRT in the standalone build
#include "state/promoted_select.h" // LIB-REF-SPLIT: MH_PROMOTED

namespace mh::tact {
namespace detail {
namespace {

// Shared by all three firing cases: anim_state 2/3 always fires; otherwise the KNEEL gate is
// fov_probe's 6th out-param [EBP-0x20] == 1 -- "occupant of the far cell has anim_state 2/3"
// (probe @0x0042e353-0x0042e376), i.e. the TARGET is kneeling/prone -- checked at 0x004337b8 /
// 0x004338a1 / 0x00433a08 in the three case bodies; ==1 kneels (op 4), anything else fires.
// (Until 2026-09-02 this gate was mistranslated as the unit's OWN `progress != 1`; caught in the
// TACT1-P red audit and fixed to thread the probe's flag through.) The attack's aim is the far
// cell's tile-center X and a weapon-mount-height-adjusted tile-center Y (@0x004337dc-0x0043382e /
// 0x004338c5-0x00433917 / 0x00433a2c-0x00433a7e -- identical in all three case bodies).
void fire_or_kneel(const tact_view &v, int32_t unit_idx, int32_t far_col, int32_t far_row,
                   int32_t door_flag) {
    const tact_unit &u = v.units[unit_idx];

    const bool go_attack = (u.anim_state == 2) || (u.anim_state == 3) || (door_flag != 1);
    if (!go_attack) {
        MH_LIBMH_BIND(llm_tact_unit_enqueue_command)(unit_idx, /*op=*/4, /*interrupt_flag=*/1, 0, 0, 0, 0);
        return;
    }

    // @0x004337ea-0x004337fd: character_types[type].height_gun1, biased by active_gun (0 or 1) --
    // the SAME byte-pair trick the field's own doc describes, not a name-table lookup.
    const uint8_t *height_gun_base = &v.character_types[u.type].height_gun1;
    const uint8_t  height          = height_gun_base[u.active_gun];

    const int32_t aim_x = far_col * 32 + 16;
    const int32_t aim_y = far_row * 24 + 12 - height;
    MH_LIBMH_BIND(llm_tact_unit_enqueue_command)(unit_idx, /*op=*/2, /*interrupt_flag=*/1, 0, 0,
                                                 (uint16_t)aim_x, (uint16_t)aim_y);
}

// caseD_1 (def_stat 1 GUARD1 / 4 SNIPER) @0x004336c3-0x0043382e. UNIQUE among the three: a
// discarded tile_objects.building read (@0x0043370f-0x00433724, computed and never used again --
// omitted, a pure dead load with no side effect) and a facing-ALIGNMENT gate: fires only when
// |facing_dir - dir24(unit -> far cell)| < 2. No cell2 fallback -- the case simply ends if the
// far-cell probe, the weapon-range check, or the facing gate fails.
void case_guard1_sniper(const tact_view &v, int32_t unit_idx) {
    const tact_unit &u = v.units[unit_idx];

    int32_t far_col = 0, far_row = 0, cell2_col = 0, cell2_row = 0, door_flag = 0;
    // The committed row ALTERNATES the pointee -- `int *far_col, uint *far_row, int *cell2_col,
    // uint *cell2_row, int *door_flag` (TACT1-P C6, 2026-09-04). The locals stay signed because
    // every use below compares and multiplies them as signed; the two `uint *` positions are
    // reinterpreted at the call, which is what the original's dword stores do anyway.
    MH_LIBMH_BIND(llm_tact_fov_probe_far_cell_and_door_state)(
        unit_idx, &far_col, reinterpret_cast<uint32_t *>(&far_row), &cell2_col,
        reinterpret_cast<uint32_t *>(&cell2_row), &door_flag);
    if (!(far_col > 0 && far_row > 0)) return; // @0x004336dc-0x004336e8

    if (MH_LIBMH_BIND(llm_tact_unit_weapon_in_range)(unit_idx, far_col * 32 + 16, far_row * 32 + 12) ==
        0) {
        return; // @0x00433707-0x00433709
    }

    // @0x004336ed-0x00433765: unit-tile-center vs far-cell-tile-center, x scale 32, y scale 24 --
    // deliberately DIFFERENT scales from weapon_in_range's own x=y=32 above; both are literal.
    const int32_t x1    = u.pos_col * 32 + 16;
    const int32_t y1    = u.pos_row * 24 + 12;
    const int32_t x2    = far_col * 32 + 16;
    const int32_t y2    = far_row * 24 + 12;
    const int32_t dir24 = MH_LIBMH_BIND(llm_tact_calc_dir24)(x1, y1, x2, y2);

    int32_t diff = (int32_t)u.facing_dir - dir24;
    if (diff < 0) diff = -diff;
    if (diff >= 2) return; // @0x0043378c-0x00433790: not facing the target -- no action.

    fire_or_kneel(v, unit_idx, far_col, far_row, door_flag);
}

// The fire path shared by caseD_2 and caseD_3 (NO facing gate, NO dead read):
// @0x0043382e's counterparts @0x00433847-0x00433917 / 0x004339ae-0x00433a7e. Returns true if the
// far-cell probe found a target (whether or not weapon_in_range then accepted it -- either way the
// case ends here); false means the probe found nothing and the caller should try the cell2
// fallback.
bool try_fire(const tact_view &v, int32_t unit_idx, int32_t far_col, int32_t far_row,
              int32_t door_flag) {
    if (!(far_col > 0 && far_row > 0)) return false;
    if (MH_LIBMH_BIND(llm_tact_unit_weapon_in_range)(unit_idx, far_col * 32 + 16, far_row * 32 + 12) !=
        0) {
        fire_or_kneel(v, unit_idx, far_col, far_row, door_flag);
    }
    return true;
}

// The cell2 fallback shared by caseD_2 (@0x0043391c-0x00433995) and caseD_3
// (@0x00433a83-0x00433afc): gated by the SAME "queue busy" test as the loop's own prelude gate.
// Returns true iff the gate was satisfied (a face order was attempted, whether or not it actually
// fired because facing already matched the approach direction) -- false is what sends caseD_3 on
// into the wander block.
bool try_face_cell2(tact_store &own, int32_t unit_idx, int32_t cell2_col, int32_t cell2_row) {
    if (!(cell2_col > 0 && cell2_row > 0)) return false;

    tact_unit &u = own.unit_at(unit_idx);
    if (!(u.cmd_queue[u.cmd_index].op == 0 || u.move_retry_wait > 0)) return false;

    const int32_t dir24 =
        MH_LIBMH_BIND(llm_tact_calc_approach_dir24_to_tile_stamp)(unit_idx, cell2_col, cell2_row);
    if (u.facing_dir != dir24) {
        MH_LIBMH_BIND(llm_tact_unit_enqueue_command)(unit_idx, /*op=*/6, /*interrupt_flag=*/1, dir24, 0, 0,
                                                     0);
    }
    return true;
}

// caseD_3's idle-wander re-roll @0x00433b01-0x00433bfe. Matches wander_check_time's own field doc
// in mh_structs.gen.h exactly.
void try_wander(const tact_view &v, tact_store &own, int32_t unit_idx) {
    tact_unit &u = own.unit_at(unit_idx);

    // @0x00433b01-0x00433b36: skip when a live command is queued, or an ATTACK/AIM immediate
    // record is armed.
    if (u.cmd_queue[u.cmd_index].op != 0 || u.attack_cmd_op == 2) return;

    // @0x00433b3b-0x00433b5c: re-roll only once the cooldown has elapsed.
    const double deadline = u.wander_check_time + *v.unit_wander_retry_interval;
    if (MH_PROMOTED_ROW(time_GetCurrentTime)() <= deadline) return;
    u.wander_check_time = MH_PROMOTED_ROW(time_GetCurrentTime)(); // @0x00433b62-0x00433b6e

    // @0x00433b74-0x00433b88: llm_rand() is a 15-bit PRNG (0..0x7FFF, the seed store's own
    // range); dividing by 4096 gives an octant in [0, 7].
    const int32_t octant = MH_CRT(llm_rand)() / 4096;

    const int32_t next_col = u.pos_col + v.dir8_delta_table[octant].dx;
    const int32_t next_row = u.pos_row + v.dir8_delta_table[octant].dy;
    if (mh::tact::passable_at(v, next_col, next_row) == 0) return; // @0x00433bd6-0x00433bdd

    // @0x00433be5-0x00433bec: an octant (0..7) maps onto dir24 (0..23) as octant*3 + 1.
    MH_LIBMH_BIND(llm_tact_unit_enqueue_command)(unit_idx, /*op=*/6, /*interrupt_flag=*/1, octant * 3 + 1,
                                                 0, 0, 0);
}

} // namespace

void unit_owner_tick(const tact_view &v, tact_store &own, uint32_t owner) {
    // step 0 @0x004335bb-0x004335d4.
    MH_LIBMH_BIND(llm_tact_tile_rebuild_occupancy_layer_for_map)(owner == 0 ? 1 : 0);

    // step 1-2 @0x004335db-0x00433619.
    for (int32_t i = TACT_UNIT_FIRST_SLOT; i <= TACT_UNIT_LAST_SLOT; ++i) {
        tact_unit &u = own.unit_at(i);
        if (u.owner != owner || u.type == 0) continue;

        // step 3 @0x00433619-0x0043367c: FIRE-bit / uninterruptible-live-command gate -- and it
        // ABORTS THE WHOLE FUNCTION, not this unit. 0x0043367c is `JMP 0x00433c03`, the EPILOGUE
        // (LEA ESP,[EBP-0x14]...RET), NOT the loop increment at 0x00433bfe every per-unit skip
        // uses. So the first same-owner unit that is firing (status&8) or holds a protected live
        // command ends owner_tick for the frame: no higher slot gets def_stat AI until it clears.
        // Translated as `continue` until 2026-09-02, which the TACT1-P POZ3 A/B caught as the
        // frame-2925 unit-10 red (ours dispatched def_stat AI for slots past the first busy one
        // and issued an immediate ATTACK/FACE stock never did). Byte-faithful form: return.
        if ((u.status & 8) != 0) return;
        const bool has_uninterruptible_live_cmd =
            (u.cmd_queue[u.cmd_index].op != 0) && (u.cmd_queue[u.cmd_index].interrupt_flag == 0);
        if (has_uninterruptible_live_cmd && u.move_retry_wait == 0) return;

        // step 4 @0x00433696-0x004336aa.
        if (u.def_stat > 4) continue;

        // step 5: dispatch on def_stat (0=no-op, 1&4=caseD_1, 2=caseD_2, 3=caseD_3).
        switch (u.def_stat) {
            case 1:
            case 4:
                case_guard1_sniper(v, i);
                break;
            case 2: {
                int32_t far_col = 0, far_row = 0, cell2_col = 0, cell2_row = 0, door_flag = 0;
                MH_LIBMH_BIND(llm_tact_fov_probe_far_cell_and_door_state)(
                    i, &far_col, reinterpret_cast<uint32_t *>(&far_row), &cell2_col,
                    reinterpret_cast<uint32_t *>(&cell2_row), &door_flag);
                if (!try_fire(v, i, far_col, far_row, door_flag)) {
                    try_face_cell2(own, i, cell2_col, cell2_row);
                }
                break;
            }
            case 3: {
                int32_t far_col = 0, far_row = 0, cell2_col = 0, cell2_row = 0, door_flag = 0;
                MH_LIBMH_BIND(llm_tact_fov_probe_far_cell_and_door_state)(
                    i, &far_col, reinterpret_cast<uint32_t *>(&far_row), &cell2_col,
                    reinterpret_cast<uint32_t *>(&cell2_row), &door_flag);
                if (!try_fire(v, i, far_col, far_row, door_flag)) {
                    if (!try_face_cell2(own, i, cell2_col, cell2_row)) {
                        try_wander(v, own, i);
                    }
                }
                break;
            }
            default:
                break; // def_stat 0: no-op.
        }
    }
}

} // namespace detail

void unit_owner_tick(uint32_t owner) {
    tact_state st = state();
    detail::unit_owner_tick(st.read, st.own, owner);
}

} // namespace mh::tact


namespace mh::tact {


} // namespace mh::tact
