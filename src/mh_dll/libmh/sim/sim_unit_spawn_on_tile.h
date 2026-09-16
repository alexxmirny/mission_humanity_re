//
// sim/sim_unit_spawn_on_tile.h -- the direct-on-tile unit materializer (RI-SIM / SIM1A fourth
// slice).
//
// One function: llm_strat_unit_spawn_on_tile @0x00464119 (0x233 bytes), `int __mh_watcall_ecx_ebx_
// volatile` -- EAX=x, EDX=y, BX(low 16)=unit_proto_id, CX(low 16)=player, matching the committed
// addr/mh_calls.gen.h prototype `llm_strat_unit_spawn_on_tile(uint32_t x, uint32_t y, uint16_t
// unit_proto_id, uint16_t player) -> int32_t` exactly.
//
// PLATE (existing, still accurate): materialize a unit directly on a map tile (deploy, not docked):
// claims map::g::tile_objects[x][y] (building=slot, class_owner=player|0x80), sets unit x/y/
// state=STOP_TO_DEFAULT, marks passable=false, saves origin_tile_was_passable, bumps
// used_soldiers, updates FoW, and notifies the AI lifecycle. No-ops if the tile already has a
// class_owner. Returns the new unit id or 0.
//
// ---- HAZARD: TWO DIFFERENT return-0 PATHS, both real ------------------------------------------
// (1) tile_objects[x][y].class_owner != 0 (tile already occupied) -- the EARLY-OUT, before the
//     roster search even starts (0x00464148/0x0046414f).
// (2) the roster slot search exhausts all 90 candidate slots without finding unit_proto_id==0
//     (0x00464164-0x0046416a -> LAB_0046433b). Both land on the identical `return 0;` -- there is
//     no way for a caller to distinguish "occupied" from "roster full" from the return value alone,
//     and that is the original's own behavior, not a simplification here.
//
// ---- HAZARD: the slot-search bound is a raw ASM IMMEDIATE, 0x5b (91), NOT UNITS_PER_PLAYER (100)
// (0x00464164: `CMP dword ptr [...], 0x5b`) -- read directly off the CMP, reproduced as its own
// local constant rather than substituted with sim_state.h's UNITS_PER_PLAYER=100. The two DISAGREE
// (91 vs 100): this function can never place a unit past slot 90, even though the roster array
// itself is 100 wide (and the grand-build roster is larger still) -- a real, load-bearing fact
// about the original, not a bug to "fix" here. Reported as a finding, not corrected.
//
// ---- HAZARD: the units[player][0].order HEADER-ROW increment, BEFORE llm_strat_unit_init_record
// (0x0046419f: a bare `INC word ptr`, width WORD/uint16_t) -- the SAME idiom
// sim_unit_create_soldier.cpp's `kUnitZeroOrderHeaderIncrement` and llm_unit_recruit.h's own hazard
// note already document at their own call sites (0x00463c2d / 0x00464076) on the exact same field:
// slot 0 of a player's roster is (ab)used as a per-player counter, not a live unit. This is now the
// THIRD independent occurrence of the identical idiom. Numerically it adds 1 (matching
// UNIT_STATE_STOP_TO_DEFAULT's value), but per those two siblings' own caution, whether that is the
// SAME enum-semantic "order" or merely a coincidentally-equal bit pattern is UNVERIFIED -- kept as
// its own local constant here too, not folded into UNIT_STATE_STOP_TO_DEFAULT.
//
// ---- HAZARD: ORDER OF EFFECTS, preserved verbatim (0x004641b1-0x00464329) ----------------------
// llm_strat_unit_init_record (ORIGINAL, mh::call) is what actually stamps
// units[player][slot].unit_proto_id -- called BEFORE this function sets order/state/x/y/origin_
// tile_was_passable/move_step_speed_scale on the SAME record. tile_objects[x][y] is claimed
// (building THEN class_owner) BEFORE passable[x][y] is cleared to 0, and BOTH tile writes happen
// before the housing-stat bump. map_fow_UpdateFoWPlus and llm_strat_ai_notify_unit_lifecycle run
// LAST, and both re-read units[player][slot].unit_proto_id off the record (the value init_record
// just wrote) rather than reusing the `unit_proto_id` PARAMETER -- same re-read-after-init_record
// shape sim_unit_create_soldier.cpp's own header documents for the identical reason. Every one of
// these orderings is preserved exactly, not just the individual writes.
//
// ---- HAZARD: passable[x][y] is read TWICE in the asm (0x00464242 for origin_tile_was_passable,
// 0x00464257 for the move_step_speed_scale conversion), both BEFORE the 0x004642b9 clear and with
// no intervening write to that tile -- so the two reads are provably equal, and the .cpp reuses the
// just-stored `origin_tile_was_passable` field for the second rather than re-indexing `v.passable`,
// matching sim_unit_create_soldier.cpp's own precedent for the identical shape
// (`u.move_step_speed_scale = (double)u.origin_tile_was_passable;`).
//
// ---- HAZARD: the int-to-double conversion width (0x00464262 `FILD word ptr`, opcode `df45dc` =
// FILD m16int, i.e. a 16-BIT load, not 8 or 32) -- resolved, not left ambiguous: the source value
// is a zero-extended byte (0-255), which always has bit 15 clear, so a 16-bit signed FILD and an
// 8-bit unsigned value agree bit-for-bit, and int-to-double is exact for any value in that range
// regardless of the source integer's width. A plain `(double)` cast reproduces the FILD's numeric
// result exactly; no x87 rounding-mode dance is needed here (unlike sim_unit_update_soldiers.cpp's
// trunc_axis_delta, which truncates a FRACTION and genuinely needs the control-word swap).
//
// ---- HAZARD: tile_objects[x][y].class_owner is a BYTE (both the read-side CMP and the write are
// `byte ptr`), stored as `(uint8_t)player | 0x80` -- player is truncated to its low byte BEFORE the
// OR (the asm loads it into DL, an 8-bit register, before the OR), matching
// sim_unit_create_soldier.cpp's identical `(uint8_t)((uint8_t)player | 0x80u)` cast order.
//
// ---- HAZARD: tile_objects/passable indexing is (x<<8)|y with x OUTER -- confirmed directly off
// the asm's own address arithmetic ((x<<11)+(y<<3) for the 8-byte tile_object record, (x<<8)+y for
// the 1-byte passable cell; ADD and OR/`<<8|` agree here because y<256 never carries into x's bits)
// -- i.e. `x` in this function's signature IS `tile_x` and `y` IS `tile_y`, matching sim_state.h's
// tile_at()/passable_at() convention exactly; no swap.
//
// ---- DECLARED NEED: sim_store has no MUTABLE accessor for _G_LLM_STRAT_UNIT_HOUSING_STATS -------
// sim_view::unit_housing (RID_STRAT_UNIT_HOUSING_STATS) already exists as a READ-ONLY pointer (added
// for llm_unit_recruit's caps checks), but this function WRITES `used_soldiers` on it
// (0x004642c0-0x004642c7, `INC dword ptr` -- 32-bit, matching housing_stats::used_soldiers's
// existing int32_t field type; no width surprise). No sim_store accessor exists yet. Needed,
// mirroring every other per-record accessor in sim_store (population_at() is the closest shape):
//   sim_state.h, on sim_store:
//     housing_stats &unit_housing_at(uint32_t player) { return unit_housing_[player]; }
//   plus a private `housing_stats *unit_housing_;` ctor parameter bound via
//   `ptr<housing_stats>(RID_STRAT_UNIT_HOUSING_STATS)` in sim_state.cpp's state() -- same RID the
//   read-only `unit_housing` pointer already binds (a second legitimate binding of the same region,
//   same precedent as order-queue's `horizon` / this batch's `soldiers`/`soldier_at()`).
// Indexed by PLAYER directly (the asm's `SHL EAX,0x6` = player*64 = sizeof(housing_stats)), not a
// separate "housing category" index -- the array is [MAX_PLAYERS], one row per player.
//
#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// ---- the outward calls -----------------------------------------------------------------------
//
// Indirected for the module's usual reason, and this file learned it the mechanical way: the first
// draft called all three DIRECTLY as `mh::call::X(...)` inside the detail:: body, which reaches into
// the live game image and makes the body untestable by net_selftest.exe simtest. That is not a style
// preference -- it is what the translation lint reported as three MISSING callees on 2026-08-10,
// the first sim-domain run of that lint. Same shape as sim_unit_soldier_anim.h's struct.
struct unit_spawn_on_tile_calls {
    // llm_strat_unit_init_record @0x004619b0 -- stamps units[player][slot].unit_proto_id (and the
    // unit_above self-index). Called BEFORE every field write below reads the record back.
    void (*unit_init_record)(int32_t unit_idx, uint32_t unit_proto_id, uint32_t player);

    // map_fow_UpdateFoWPlus @0x0049681a -- reveals the new unit's sight radius. Runs AFTER the tile
    // claim, and reads the state this call already wrote.
    void (*fow_update_plus)(uint32_t player, uint32_t x, uint32_t y, uint8_t sight);

    // llm_strat_ai_notify_unit_lifecycle @0x004dbb38 -- kind 4 = "unit appeared". Last of the three.
    void (*ai_notify_unit_lifecycle)(uint16_t player, uint16_t unit_type, uint32_t unit_id,
                                     uint32_t kind);
};

const unit_spawn_on_tile_calls &live_unit_spawn_on_tile_calls();

namespace detail {

// llm_strat_unit_spawn_on_tile @0x00464119. See the header hazards above for the full derivation.
// Returns the newly-placed unit's roster slot (1..90), or 0 if the tile is already occupied OR the
// slot search (bounded at 0x5b=91, NOT UNITS_PER_PLAYER) is exhausted.
int32_t unit_spawn_on_tile(const sim_view &v, sim_store &own, const unit_spawn_on_tile_calls &c,
                           uint32_t x, uint32_t y, uint16_t unit_proto_id, uint16_t player);

} // namespace detail

// Live wrapper: the logic applied to state(). Matches the original's committed
// `__mh_watcall_ecx_ebx_volatile` shape (addr/mh_calls.gen.h's llm_strat_unit_spawn_on_tile).
int32_t unit_spawn_on_tile(uint32_t x, uint32_t y, uint16_t unit_proto_id, uint16_t player);

namespace detail {
} // namespace detail

} // namespace mh::sim
