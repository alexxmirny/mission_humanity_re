//
// tact/tact_unit_spawn.h -- TACT1C slice: the DISPOSITION-section mission spawner, allocating a
// roster slot and stamping a new tactical unit's starting state.
//
//   llm_tact_unit_spawn @0x0042b948 (0x486)
//   int __mh_watcall_ebx_volatile llm_tact_unit_spawn(int char_type, int col, int row,
//                                                     byte facing_dir, byte def_stat, int hp_pct)
//
// PROOF: OFFLINE, mandatory per the batch context -- llm_rand @0x004da98b is called DIRECTLY
// (0x0042bc05) to jitter the spawned unit's frame_interval, and llm_rand/llm_rand_below_fx are
// TACT-CUT2 UNGATED effectful shared callees (a PRNG-advance is shared state; double-firing it
// under a shadow snapshot/restore window desyncs silently, with no entry gate anywhere to close
// the hole). Any function that calls llm_rand is therefore proven OFFLINE and never shadow-armed,
// full stop -- not a per-function judgement call. All five outward calls (llm_fatal_cleanup,
// utils_abort, time_GetCurrentTime x4, llm_rand, llm_tact_unit_vision_add) are indirected via a
// `_calls` struct so the offline oracle can mock every one (tact precedent:
// tact_mission_end_return_to_strategic.h, tact_unit_despawn.h). "review_required": true in
// tools/data/tact_migration.json.
//
// DERIVATION (tmp/decomp_tact/llm_tact_unit_spawn_0042b948.asm):
//
// 1. @0x0042b969-0x0042b982: guard -- if `passable[col][row] == PASSABLE_BLOCKED` (occupied), the
//    original fatal-aborts (llm_fatal_cleanup(); utils_abort(0); never returns). Index formula
//    `(col<<8)+row` matches `passable_at(tv, col, row)` (col is the OUTER/x index, row is y,
//    consistent throughout this function and with tact_unit_despawn.h).
// 2. @0x0042b987-0x0042b9c2: linear scan for a free roster slot -- `for (i = TACT_UNIT_FIRST_SLOT
//    (1); i <= TACT_UNIT_LAST_SLOT (0x80); ++i) if (units[i].type == 0) { slot = i; break; }`.
//    Slot 0 is never tested (matches tact_state.h's own documented off-by-one). If no free slot is
//    found the function returns 0 immediately (@0x0042b9c8) without touching any state.
// 3. @0x0042b9d4-0x0042b9ed: validate the CHARACTER class -- if
//    `character_types[char_type].id == 0` (an unfilled/empty class slot), fatal-abort. No bound
//    check against TACT_CHARACTER_TYPE_SLOTS (16) is performed either here or anywhere the index
//    is used below -- `char_type` is trusted, exactly as the original trusts it.
// 4. @0x0042b9ed-0x0042ba8c: stamp the roster record's identity/placement fields, in this exact
//    order: type (truncated char_type), owner (= character_types[char_type].who, a PLAIN byte
//    copy -- the WHO-XOR-key un-mangling mh_llm_tact_character_type::who's own comment describes
//    happened at mission-PARSE time, not here), status=0, pos_col/pos_row (truncated col/row),
//    vision_angle/vision_dist (copied from the character type), facing_dir, def_stat (both straight
//    from the parameters).
// 5. @0x0042ba92-0x0042bad8: move_state_timer = wander_check_time = time_GetCurrentTime() (two
//    SEPARATE calls, two separate stores -- not one value copied twice); cmd_wait_until_time = 0.0
//    (mh_tact_unit_record::cmd_wait_until_time's own field comment names these exact two store
//    addresses, 0x0042babd/0x0042bac7, as one instance of the compiler's "zero a double via two
//    32-bit MOV-0s" idiom -- a plain `= 0.0` reproduces the identical bit pattern).
// 6. @0x0042bad8-0x0042baff: progress=0, move_path_slot=0, move_path_step=0.
// 7. @0x0042baff-0x0042bb17: `tile_objects[col][row].building = (uint16_t)slot` -- writes the
//    roster slot index into the tile's `.building` field (mh_map_tile_object_data offset 0x2, NOT
//    `.unit` at offset 0x4). This is confirmed two independent ways: Ghidra's own decompile of
//    THIS function spells it `tile_objects[col][row].building` (tmp/decomp_tact/…0042b948.c), and
//    the literal displacement folded into the instruction (`(col<<0xb)+(row<<3)+0xd1ec82`, i.e.
//    tile_objects base 0xd1ec80 **+2**) matches `offsetof(mh_map_tile_object_data, building)==0x2`
//    exactly, not `unit`'s 0x4. See the NOTE below -- the already-shipped tact_unit_despawn.cpp
//    writes this SAME literal address under the WRONG field name.
// 8. @0x0042bb17-0x0042bb1a: `_G_LLM_TACT_UNIT_ACTIVE_COUNT = slot` -- an ASSIGNMENT of the found
//    slot index, NOT an increment. See the NOTE below.
// 9. @0x0042bb1f-0x0042bb2f: `passable[col][row] = PASSABLE_BLOCKED (0)` -- the tile is now
//    occupied.
// 10. @0x0042bb2f-0x0042bb95: `hp_pct` range guard. If `hp_pct < 1 || hp_pct > 100`: reset
//     `units[slot].type = 0` (releases the slot back to the free pool) and return 0 -- WITHOUT
//     undoing steps 7-9: the tile_objects/passable writes and the active-count assignment already
//     made above are permanent even on this failure path. Every other field written in step 4-6
//     (owner, status, pos_col/row, vision_*, facing_dir, def_stat, the three timers/counters) is
//     also left exactly as written -- only `.type` is rolled back. Preserved literally (Law 2):
//     this is a real "half-spawned, tile now blocked, slot nominally free" state the original
//     leaves behind on out-of-range hp_pct.
// 11. @0x0042bb3d-0x0042bb68 (only when hp_pct is in range): `hp = (energy_zx * hp_pct) / 100`
//     where `energy_zx` is `character_types[char_type].energy` READ VIA MOVZX (zero-extended as
//     unsigned 16-bit) despite the field's C type being `int16_t` (0x0042bb41: `MOVZX EDX,word ptr
//     […]`, not MOVSX) -- reproduced here by reading through `uint16_t` before widening to
//     `int32_t`. The division is a genuine IDIV (signed, truncates toward zero, brief rule 8); a
//     plain C++ `/` on `int32_t` reproduces it exactly since both truncate toward zero. Floored at
//     1 if the result is 0 (@0x0042bb5b-0x0042bb61).
// 12. @0x0042bb68-0x0042bbf3: hp written; move_retry_wait/move_retry_attempts/
//     move_stuck_countdown/cmd_index all reset to 0; anim_frame_time = time_GetCurrentTime();
//     frame_index=0; anim_cycle_time = time_GetCurrentTime() (a THIRD and FOURTH
//     time_GetCurrentTime() call, distinct from step 5's two).
// 13. @0x0042bc05-0x0042bc2b: `frame_interval = (double)(llm_rand() / 0x1999) +
//     UNIT_ANIM_FRAME_INTERVAL_BASE` -- another genuine IDIV (signed truncation). The addend is
//     `_G_LLM_TACT_UNIT_ANIM_FRAME_INTERVAL_BASE`, a double @0x005002f8 that Ghidra's own decompile
//     already names (tmp/decomp_tact/…0042b948.c line 92) but which has NO tact_view member and no
//     mh_addrs.gen.h/mh_regions.gen.h entry yet -- declared as a need below (not a DAT_/auto-label
//     situation; the global is already named in the DB, just not threaded into the state view).
// 14. @0x0042bc31-0x0042bcb5: gun setup -- active_gun=0; gun1_bullets/gun2_bullets copied from
//     `fx_type_table[number_gun1]`/`fx_type_table[number_gun2]` (character_types[char_type]'s own
//     gun-id bytes, used unchecked as fx_type_table indices, exactly as the original does);
//     gun1_magazines/gun2_magazines = the same table's `.magazines` MINUS ONE (a plain byte
//     decrement -- wraps to 0xff if magazines was 0, reproduced by unsigned-byte-store truncation,
//     no divergence risk).
// 15. @0x0042bcbb-0x0042bd48: all 128 cmd_queue entries' op/arg0/arg1/arg2/arg3 zeroed (5 of the 6
//     fields per entry -- `interrupt_flag` is DELIBERATELY NOT touched, matching
//     mh_llm_tact_unit_cmd_entry::interrupt_flag's own field comment: "NOT cleared by
//     llm_tact_unit_cmd_advance … so this byte is STALE whenever op == 0" -- spawn inherits
//     whatever interrupt_flag byte the slot's previous occupant (or process init) left behind).
// 16. @0x0042bd48-0x0042bdb4: the immediate ATTACK/AIM record's op/gun-toggle/arg1/aim_x/aim_y
//     zeroed (attack_cmd_op, attack_gun_toggle, attack_cmd_arg1, aim_x, aim_y) -- but, exactly like
//     step 15, `attack_interrupt_flag` is left UNTOUCHED. The FACE/TURN immediate record
//     (face_interrupt_flag, face_cmd_op, face_cmd_target_dir, face_cmd_arg1/2/3) and
//     move_aborted_op are not touched AT ALL by this function. anim_state=0; squad_group_id=0xff
//     (the "unassigned" sentinel, mh_tact_unit_record::squad_group_id's own comment).
// 17. @0x0042bdb4-0x0042bdbf: `llm_tact_unit_vision_add(slot)`; return `slot`.
//
// FIELDS THIS FUNCTION DELIBERATELY DOES NOT INITIALISE (inherits whatever the slot's previous
// occupant, or process start-up, left behind -- a translation that zeroes any of these diverges):
// weapon_timer, sprite_id, cmd_queue[*].interrupt_flag, attack_interrupt_flag, face_interrupt_flag,
// face_cmd_op, face_cmd_target_dir, face_cmd_arg1, face_cmd_arg2, face_cmd_arg3, move_aborted_op,
// move_redirect_col, move_redirect_row, click_preview_facing.
//
// NOTE -- a discovered divergence in an ALREADY-SHIPPED sibling file, surfaced by this
// translation, not something this file can fix (tact_unit_despawn.cpp is not among the files this
// unit may write): despawn's own disassembly clears the exact same literal tile address this
// function writes (`(pos_col<<0xb)+(pos_row<<3)+0xd1ec82`), and Ghidra's own decompile of
// llm_tact_unit_despawn (tmp/decomp_tact/llm_tact_unit_despawn_00432048.c line 26) spells it
// `tile_objects[bVar1][bVar2].building = 0;` -- the SAME field this function writes. But the
// committed `tact_unit_despawn.cpp` (@src/mh_dll/libmh/tact/tact_unit_despawn.cpp:47-48) instead
// writes `tile.unit[0] = 0; tile.unit[1] = 0;` -- offset 0x4/0x5, a DIFFERENT field
// (mh_map_tile_object_data::unit, the mode-dual-reading occupancy/preview-overlay byte pair
// mode_planes.h documents, written for TACTICAL by a wholly different function,
// llm_tact_tile_rebuild_occupancy_layer). So despawn never actually clears `.building`, and this
// spawn function's own successor call can find the slot's old `.building` stamp still present.
// Flagged for the conductor; not fixed here.
//
#pragma once
#include <cstdint>

#include "tact/tact_state.h"

namespace mh::tact {

// The five outward calls this function makes, indirected for offline testability -- same shape as
// tact_mission_end_return_to_strategic_calls.
struct unit_spawn_calls {
    void (*llm_fatal_cleanup)();               // llm_fatal_cleanup @0x0042613b
    void (*utils_abort)(int32_t status);       // utils_abort @0x004da944
    double (*time_get_current_time)();         // time_GetCurrentTime @0x00427616
    int32_t (*llm_rand)();                     // llm_rand @0x004da98b
    void (*unit_vision_add)(int32_t unit_idx); // llm_tact_unit_vision_add @0x0042e385
};

const unit_spawn_calls &live_unit_spawn_calls();

namespace detail {

// llm_tact_unit_spawn @0x0042b948. See the header banner for the full step-by-step derivation.
int32_t unit_spawn(const tact_view &tv, tact_store &own, const unit_spawn_calls &c,
                   int32_t char_type, int32_t col, int32_t row, uint8_t facing_dir,
                   uint8_t def_stat, int32_t hp_pct);

} // namespace detail

int32_t unit_spawn(int32_t char_type, int32_t col, int32_t row, uint8_t facing_dir,
                   uint8_t def_stat, int32_t hp_pct);


} // namespace mh::tact
