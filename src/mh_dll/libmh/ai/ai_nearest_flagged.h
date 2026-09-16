//
// ai/ai_nearest_flagged.h -- nearest CONNECTED building to a point (RI-AI / AI1B, layer 4).
//
// llm_strat_ai_find_nearest_flagged_building @0x004e4e54, 203 bytes. Its only reference in the whole
// image is llm_strat_ai_plan_turret_upgrade @0x004e500c, which uses the returned index to read that
// building's x/y and feed llm_strat_tile_midpoint_wrapped -- i.e. the result is the CONNECTION
// PARTNER to bridge a new turret toward, not a proximity veto. (The Ghidra plate said the opposite
// until 2026-08-03; see ghidra_findings 2026-08-03-1505-1.)
//
// THE FLAG IS THE ROSTER RECORD'S OWN BIT, NOT AN AI PLANE. TEST byte ptr [.. + 0xc3d2a4],0x1
// @0x004e4eca reads map::object::building.built_flags bit 0x1 -- "connected/reached", set by
// llm_bldg_set_connected_flag. This body references no data address outside the roster,
// llm_strat_toroidal_dist_sq and the Watcom prologue/epilogue helpers; in particular it does NOT
// read _G_LLM_STRAT_AI_BLDG_CONNECTIVITY_BASE, which ai_state.h claimed until.
//
// THE RETURN GATE READS THE LAST CANDIDATE'S DISTANCE, NOT THE BEST, and this is the one thing in
// the function that a "sensible" reimplementation gets wrong. Two distinct frame slots are in play:
//
//   [EBP-0x10]  the winning INDEX      -- init -1 @0x004e4e6f, updated @0x004e4efc
//   ESI         the winning DISTANCE   -- init 0xffffffff @0x004e4e6a, updated @0x004e4eff
//   [EBP-0x14]  the LAST distance      -- written on EVERY evaluated candidate @0x004e4ef1,
//                                         BEFORE the two tests that decide whether it wins
//
// and the exit is `CMP dword ptr [EBP-0x14],0xf / JA` @0x004e4f07, reading the third of those. So
// the function answers -1 whenever the LAST candidate examined happened to be within 0xf, however
// good the actual nearest match was. The encodings settle that the slots are distinct rather than a
// decompiler artifact: 8945ec / 837dec0f address EBP-0x14, 895df0 / 8b45f0 address EBP-0x10.
// REPRODUCED VERBATIM. Fixing it would change the AI's placement decisions.
//
// WHEN NOTHING IS EVALUATED THE ORIGINAL READS UNINITIALISED STACK -- [EBP-0x14] lives in the
// SUB ESP,0x8 region @0x004e4e64 and nothing writes it on that path. It cannot matter: the index
// slot still holds the -1 from @0x004e4e6f, so BOTH branches of the gate return -1. `last_dist` is
// therefore initialised to 0 here, which takes the `<= 0xf` branch, and the answer is the same one
// the original gives for any garbage the slot could hold. This is the whole reason the
// indeterminate read is a footnote rather than a blocker.
//
// THE WALK IS THE STANDARD COUNT-DRIVEN ROSTER SHAPE (see ai_turret_threat.cpp): index from 1,
// budget = buildings[player][0].index, and an EMPTY slot does NOT consume the budget --
// CMP word ptr [.. + 0xc3d2a2],0x0 / JZ 0x004e4f02 @0x004e4ec0 jumps PAST the DEC ECX at
// 0x004e4f01. A live-but-unflagged building DOES consume it (JZ 0x004e4f01 @0x004e4ed1).
//
// A ZERO DISTANCE IS REJECTED (TEST EAX,EAX / JBE @0x004e4ef4) -- which excludes the query point's
// own building when (x, y) is a roster building's tile, and is how the caller avoids pairing a
// building with itself. Ties go to the FIRST candidate (CMP EAX,ESI / JNC @0x004e4ef8 is a
// >=-skip, so only a strictly smaller distance replaces the winner).
//
#pragma once
#include <cstdint>

#include "ai/ai_state.h"

namespace mh::ai {

namespace detail {

// Takes the call set only for llm_strat_toroidal_dist_sq, which is pure.
int32_t find_nearest_flagged_building(const ai_view &v, const ai_calls &gc, int32_t player,
                                      int32_t x, int32_t y);

} // namespace detail

int32_t find_nearest_flagged_building(int32_t player, int32_t x, int32_t y);

} // namespace mh::ai
