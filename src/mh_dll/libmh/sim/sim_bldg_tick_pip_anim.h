//
// sim/sim_bldg_tick_pip_anim.h -- per-building 'pip' status-icon animation ticker (RI-SIM, SIM1B
// building_tick machinery slice). Translated from the DISASSEMBLY
// (tmp/decomp/llm_strat_bldg_tick_pip_anim_00479244.asm), not from the Ghidra .c draft -- the draft's
// `*(int *)Building[...].undef_block` cast-through-pointer is a decompiler artifact around a field
// Ghidra has not split out (see the declared need below), not a real indirection.
//
//   llm_strat_bldg_tick_pip_anim @0x00479244 (591 B) -- for each of up to
//     Building[building.building_id].pip_slot_count 'pip' slots on the building INSTANCE: if
//     pip_level[i] > 0, snapshot elapsed = GAME_CLOCK - pip_timer[i] and refresh pip_timer[i] =
//     GAME_CLOCK ONCE (before any chain-advance), then drain that single elapsed snapshot against
//     Anim[pip_frame[i]+1].time/.next, advancing pip_frame[i] by .next each cycle or, on chain end
//     (.next == 0), restarting it at A_OGIEN[pip_level[i]-1]. No outward calls other than
//     the inert stack-capacity probe (translator brief rule 6) -- no `calls` struct.
//
// ---- FIXED 2026-08-13: the chain-restart table is A_OGIEN, not a per-pip-level table ---------------
// The original translation (2026-08-12) read `pip_level_base_frame[pip_level[i]]` from a region bound
// at 0xc38730. That was wrong: the instruction's real folded constant is 0xc3873c (confirmed via
// ModRM byte decode and Ghidra's own decompile literal `DAT_00c3873c[level]`), a DIFFERENT address 12
// bytes/3 entries later -- the 0xc38730 derivation was a hex-transcription slip. 0xc3873c + level*4
// lands in A_OGIEN (cfg_t_frame_index[4] @0xc38740, the game's hardcoded 'fire' effect animation frame
// table, populated at boot by cfg_ConstructAnims -- confirmed via find-cross-references and its own
// decompile). pip_level[i] is always >=1 in this branch (the `pip_level[i] <= 0` guard above skips
// it otherwise), so `A_OGIEN[pip_level[i]-1]` is the exact 0-based index into the 4-entry table --
// unlike the old formula, which was always at least 1 entry out of bounds. This was a REAL confirmed
// shadow divergence (156514/181434 calls, ~86%, on one building) that a `reimpl-verify` static pass
// did not catch -- and it was invisible to
// xref-based "sole reader" search (Ghidra does not record a data xref for `[reg+disp32]` where disp32
// is a raw folded constant and reg holds a dynamically-computed index).
//
#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

namespace detail {

// llm_strat_bldg_tick_pip_anim @0x00479244. See the header banner above for the full derivation; the
// .cpp carries the per-branch address citation. Takes the mutable store (not just the view) because
// pip_frame[i]/pip_timer[i] are written in place on the building instance.
void bldg_tick_pip_anim(const sim_view &v, sim_store &store, uint16_t player, int32_t b_index);

} // namespace detail

// ---- the public surface, in original-behaviour terms -----------------------------------------
// Parameter types match the committed prototype in addr/mh_calls.gen.h exactly (the drift gate
// enforces this on export/shadow installation): void(uint16_t player, int32_t b_index).

void bldg_tick_pip_anim(uint16_t player, int32_t b_index);

namespace detail {
} // namespace detail

} // namespace mh::sim
