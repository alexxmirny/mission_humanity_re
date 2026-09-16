//
// sim/sim_bldg_mother_reelect_primary.h -- llm_strat_mother_reelect_primary @0x00498aad (0x313 B),
// translated from the DISASSEMBLY (tmp/decomp/llm_strat_mother_reelect_primary_00498aad.asm), not
// from the Ghidra .c draft (whose reading agrees with the asm on every branch and store -- verified
// independently rather than trusted, per the translator brief).
//
// Called from llm_strat_bldg_power_network_recompute (a SAME-BATCH SIBLING, SIM1B) to
// (re-)pick a player's "primary mother" -- the one MOTHER unit or building a planet's power/UI logic
// treats as canonical -- whenever the previously-tracked one is gone. Per the batch context file,
// that caller reaches this function through `mh::call::llm_strat_mother_reelect_primary`, i.e. THIS
// address, not a direct C++ call to the body below, even once both live in this source tree.
//
// TWO INDEPENDENT NEAREST-CANDIDATE SEARCHES, NOT ONE SHARED PASS -- the asm reuses the same EBP
// stack slots (-0x24 for the headcount countdown, -0x14/-0x20/-0x18 for chosen/index/best-distance)
// across both blocks, but nothing computed in the first search is read by the second; they are
// modelled here as two separate local-variable groups, matching the task brief's explicit warning not
// to treat the reuse as shared state.
//
//   (1) UNIT search, gated on `profile.primary_mother_unit[planet] == 0` (skipped entirely if a
//       mobile heli-mother is already tracked). Scans units[player][1..99], counting down from
//       trunc(units[player][0].energy) live slots (headcount countdown; a slot whose `.index`-style
//       accounting is wrong drives this negative rather than clamping -- see the sibling function
//       sim_bldg_refresh_all_buildings.h for the same idiom on a different field). A candidate must
//       have energy > 0 (see the NaN note below) AND cfg type A_HELI_MOTHER(0x13) or
//       H_HELI_MOTHER(0x14); the nearest by llm_strat_tile_dist_wrapped(x, y, cand.x, cand.y) wins.
//       If found, writes profile.primary_mother_unit[planet] = chosen slot. NO building-state write
//       happens here -- units have no `.state` gate to satisfy this search.
//
//   (2) BUILDING search, gated on `profile.primary_mother_bldg[planet] == 0`, RE-READ independently
//       from the first check (0x00498c1f is a fresh CMP against the same field, not a cached value --
//       translator brief rule 16). If already nonzero, the whole building search is skipped and the
//       function returns 1 immediately -- this is the function's ONE short-circuit return path.
//       Otherwise scans buildings[player][1..99] the same way, additionally requiring
//       `state == IDLE_NOOP_8C(0x8c)` before the cfg-type gate (H_MOTHER(0x1a)/A_MOTHER(0x06), reused
//       from sim_order_enqueue.h -- see that header's BUILDING_TYPE_* block). If found, writes
//       profile.primary_mother_bldg[planet] = chosen slot AND buildings[player][chosen].state =
//       POWER_PRIMARY_CHECK(0x8a); the function then returns that chosen slot (0 if none found).
//
// RETURN VALUE: 1 if primary_mother_bldg was ALREADY nonzero on entry (short-circuiting search (2)
// entirely, regardless of what search (1) did); otherwise the building slot search (2) chose, or 0 if
// it found none. Search (1)'s own outcome never affects the return value.
//
// THE ENERGY GATE IS `!(energy <= 0.0)`, NOT `energy > 0.0` -- SAME PRECEDENT AS sim_bldg_alive.cpp.
// Both energy tests here (0x00498b46-0x00498b51 for units, 0x00498c99-0x00498ca4 for buildings) are
// the byte-identical FLDZ/FCOMP/FNSTSW/SAHF/JNC sequence that function's header derives: JNC falls
// into the "alive" branch when CF is SET, which x87 sets for BOTH "0.0 < energy" and "unordered"
// (NaN). `!(energy <= 0.0)` matches on a NaN where `energy > 0.0` would not; no reachable state is
// known to put a NaN in these fields, so this is a zero-cost faithfulness fix, not a live divergence.
//
// utils_math_trunc (0x004d0596, `MH_UNAVAILABLE__parameter_storage_not_marshallable` in
// mh_calls.gen.h) is called TWICE, once per search, each immediately FISTP'd to a 32-bit int (opcode
// bytes `db 5d dc` at both 0x00498afc and 0x00498c4f -- 0xDB ModRM 0x5D, reg field 3, i.e. 0xDB /3 ==
// FISTP m32int, confirmed at THIS site rather than assumed from the precedent group). Reproduced as
// this TU's own copy of the trunc_to_int32() idiom (sim_unit_population_remove.cpp's precedent),
// per-TU per the established convention -- not a new shared helper.
//
// UNIT_TYPE_A_HELI_MOTHER(0x13) / H_HELI_MOTHER(0x14) (cfg_enum_E_UNIT_TYPE) have no existing header
// binding anywhere in this codebase (grepped); declared locally in the .cpp's anonymous namespace per
// the batch context file's instruction. BLDG_STATE_IDLE_NOOP_8C(0x8c) / _POWER_PRIMARY_CHECK(0x8a)
// are likewise declared locally, reusing the names from the batch's Ghidra enum dump (sim_state.h
// already carries BLDG_STATE_RUBBLE_SIGHT_DECAY from a different function; these two are new to this
// TU only).
//
// `player` PARTICIPATES AS A FULL 32-BIT VALUE THROUGHOUT -- unlike several sibling functions in this
// batch (e.g. sim_bldg_refresh_all_buildings.h), there is no MOVZX/16-bit narrowing anywhere in this
// asm; every `IMUL .., dword ptr [EBP + player_slot], stride` reads the full stashed dword. Kept as
// int32_t end to end (matching the committed `sig_llm_strat_mother_reelect_primary` prototype
// exactly) rather than introducing a uint16_t narrowing the original does not perform.
//
#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// The one external callee this closure reaches (llm_strat_tile_dist_wrapped, already committed in
// mh_calls.gen.h -- NOT part of this migration slice, stays original), indirected for offline
// testability -- same reason sim_bldg_refresh_all_buildings.h's `refresh_all_buildings_calls` gives.
struct mother_reelect_primary_calls {
    // llm_strat_tile_dist_wrapped @0x0049404e. Argument order (x, y, cand_x, cand_y) matches the
    // committed mh::call:: binding's own (x1, y1, x2, y2) register wiring (EAX/EDX/EBX/ECX) -- see
    // the .cpp for the byte-offset cross-check that confirms cand_x maps to the candidate's `.x`
    // field and cand_y to `.y`, not the other way around.
    int32_t (*tile_dist_wrapped)(int32_t x, int32_t y, int32_t cand_x, int32_t cand_y);
};

const mother_reelect_primary_calls &live_mother_reelect_primary_calls();

namespace detail {

// llm_strat_mother_reelect_primary @0x00498aad. See the header banner above for the full derivation.
int32_t mother_reelect_primary(const sim_view &v, sim_store &own, const mother_reelect_primary_calls &c,
                               int32_t player, int32_t x, int32_t y);

} // namespace detail

// ---- the public surface, in original-behaviour terms -----------------------------------------
// Parameter types match the committed prototype (addr/mh_export.gen.h's sig_llm_strat_mother_
// reelect_primary, `int32_t(__cdecl *)(int32_t player, int32_t x, int32_t y)`) and
// addr/mh_calls.gen.h's own out-call wrapper.

int32_t mother_reelect_primary(int32_t player, int32_t x, int32_t y);

namespace detail {
} // namespace detail

} // namespace mh::sim
