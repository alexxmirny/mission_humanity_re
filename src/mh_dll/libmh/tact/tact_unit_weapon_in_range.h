//
// tact/tact_unit_weapon_in_range.h -- TACT1C: the min/max weapon-range gate.
//
//   llm_tact_unit_weapon_in_range @0x00433c0d (0x14a)
//   int __mh_watcall_ebx_volatile llm_tact_unit_weapon_in_range(int unit_idx, int target_x, int target_y)
//
// A PURE QUERY over live table state: writes nothing (0 direct / 0 transitive write cells --
// measured write closure: 1 function reachable, 0 regions written anywhere), so the
// write-set preflight correctly flags it NOT SHADOWABLE by region diffing. It IS shadowable by
// RETURN VALUE alone (gen_dll_shadow.py's `compare_return`, default on for a non-void function):
// the return is a real range-gate verdict computed from live unit/character-type/fx-type state,
// not a value predetermined by an ini flag -- same posture as tact_dir24_approach.h's
// calc_approach_dir24_to_tile_stamp.
//
// DERIVATION (tmp/decomp_tact/llm_tact_unit_weapon_in_range_00433c0d.asm):
//
// 1. @0x00433c2c-0x00433c57: the unit's tile anchor converted to a pixel-space center point:
//    `center_col_px = unit.pos_col*32 + 16`, `center_row_px = unit.pos_row*32 + 16` (SHL 5 / ADD
//    0x10 on each zero-extended byte -- the same tile-center convention used elsewhere in this
//    subsystem).
// 2. @0x00433c5a-0x00433c86: `weapon_type_id = (&character_types[unit.type].number_gun1)[unit.active_gun]`
//    -- the SAME "+0/+1 bias onto an adjacent pair of gun1/gun2 fields" idiom
//    `mh_tact_unit_record::active_gun`'s own field comment documents for `height_gun1`
//    (number_gun1/number_gun2 are adjacent bytes at character_type offsets 0x1c/0x1d). `unit.type`
//    is the unit-record field at offset 0 (Ghidra: unnamed in the export comment, but the struct's
//    own field is `type` -- "building type id").
// 3. @0x00433c89-0x00433d3a: `dist = trunc(sqrt((center_col_px-target_x)^2 + (center_row_px-target_y)^2))`,
//    computed via the marshalled `llm_sqrt` + the non-marshallable `utils_math_trunc` (ST0 in/out,
//    reproduced as a private inline-`__asm` helper, same shape as sim_path_slot_dist.cpp's
//    `trunc_dword` / sim_projectile_tick.cpp's `trunc_only`). The ORIGINAL recomputes this exact
//    distance TWICE (once for the range_min gate at 0x00433c96-0x00433ccc, once for the range_max
//    gate at 0x00433cf4-0x00433d2a) from the SAME unchanged inputs -- provably idempotent, not a
//    second, differently-derived value -- so this translation computes it ONCE.
// 4. @0x00433c8d-0x00433ce7: if `fx_type_table[weapon_type_id].range_min > 0` (else the near-range
//    gate is skipped entirely) and `dist < range_min`: return 0 (too close).
// 5. @0x00433ceb-0x00433d45: if `fx_type_table[weapon_type_id].range_max > 0` (else the far-range
//    gate is skipped entirely, unconditional in-range) and `dist > range_max`: return 0 (too far).
// 6. Otherwise: return 1 (in range).
//
#pragma once
#include <cstdint>

#include "tact/tact_state.h"

namespace mh::tact {

// The one outward call, indirected for offline testability -- same shape as
// tact_calc_dir24.h's calc_dir24_calls.
struct unit_weapon_in_range_calls {
    double (*sqrt_fn)(double x); // llm_sqrt @0x004da9c0
};

const unit_weapon_in_range_calls &live_unit_weapon_in_range_calls();

namespace detail {

// llm_tact_unit_weapon_in_range @0x00433c0d. See the header banner for the full derivation.
int32_t unit_weapon_in_range(const tact_view &v, const unit_weapon_in_range_calls &c, int32_t unit_idx,
                             int32_t target_x, int32_t target_y);

} // namespace detail

int32_t unit_weapon_in_range(int32_t unit_idx, int32_t target_x, int32_t target_y);


} // namespace mh::tact
