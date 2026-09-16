//
// ai/ai_spiral_table_init.h -- the strategic AI's spiral tile-offset table builder (RI-AI / AI1E).
//
// One function, called once (callers_in_ai == 0, callers_total == 1 per ai_migration.json --
// nothing in this cluster calls it; it is presumably the game's own one-shot AI init path). It is
// the SOLE WRITER of the three globals every spiral scan in this cluster reads: the (dx, dy) offset
// table itself, its per-ring cumulative cell-count table, and the live cell count. Everywhere else
// in the cluster these three are read-only (ai_view::spiral_offsets / spiral_ring_cell_counts, and
// the comment on SITE_SCAN_SPIRAL_RADIUS in ai_state.h) -- this is where they get built.
//
#pragma once
#include <cstdint>

#include "ai/ai_state.h"

namespace mh::ai {

// The logic over an EXPLICIT state, so `net_selftest.exe aitest` can drive it over heap buffers
// with no game and no rig. The wrapper below is this applied to state(); the split costs one
// inlined call.
namespace detail {

// llm_strat_ai_spiral_table_init @0x004dc597.
//
// Fills _G_LLM_STRAT_AI_TILE_SPIRAL_OFFSETS with every (dx, dy) in [-127, 127] x [-127, 127] whose
// squared distance from the origin is < 0x3f02 (~127.0^2), in double-loop (dx outer, dy inner)
// generation order, then sorts that whole run in place by squared distance via the GAME's qsort and
// the GAME's own comparator (the two must stay paired -- see ai_calls::qsort's comment on
// ai_state.h and gc.spiral_offset_sort_cmp's comment below: a host sort would permute equal-radius
// ties differently and desync anything that depends on within-ring order). Finally derives
// _G_LLM_STRAT_AI_TILE_SPIRAL_RING_CELL_COUNTS[0..127]: ring_cell_counts[r] becomes the index of the
// first (sorted) offset whose squared distance exceeds r^2, i.e. a cumulative "how many offsets are
// within radius r" count -- read that way by every consumer (ai_state.h's spiral_offsets comment:
// "offsets[0 .. ring_cell_counts[r]) is every tile within radius r").
//
// No parameters and no ai_view read: nothing here depends on game/player state, only on the fixed
// geometry constants baked into the original.
void spiral_table_init(const ai_store &own, const ai_calls &gc);

// The qsort comparator @0x004dc546, translated (LIB-CRT 2026-09-08). It was NOT a callable Ghidra
// function -- a bare LAB_004dc546 one instruction before spiral_table_init's own entry -- and the
// address was handed to the game's qsort as DATA, which is why no census could see it and why it
// survived every earlier sweep. Ascending by squared radius over `llm_strat_ai_spiral_offset`
// {int8 dx; int8 dy;}, returning strictly -1/0/+1.
//
// Selected by MH_CRT_CMP (crt/crt_select.h): the hosted build still passes the VA to the game's
// qsort, and only the standalone build calls this. The two must stay paired -- our qsort and the
// original's permute equal-radius ties the same way only because crt/crt_qsort.h is a transcription
// of 0x004de8a6, and pairing OUR comparator with THEIR sort (or the reverse) is a desync.
int32_t spiral_offset_sort_cmp(void *a, void *b);

} // namespace detail

void spiral_table_init();

} // namespace mh::ai
