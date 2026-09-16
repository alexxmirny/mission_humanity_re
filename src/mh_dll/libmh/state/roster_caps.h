//
// state/roster_caps.h -- the per-player roster capacities, ONE derived source (SB-BIND T2).
//
// docs/state-boundary.md D6.3 is the defect this closes: the eight per-player caps were
// `inline constexpr` values duplicated across five module headers, mirroring the exe's own CMP
// bounds. That made the reimplementation and the shipped 500-buildings cap-raise build
// non-composable -- exactly what Law 3 exists to prevent -- because our indexing would resolve
// player p's row at p*100 while the patched binary had put it at p*500.
//
// THE DERIVATION NEEDS NO NEW DATA, which is why this is a small file rather than a registry
// change. Every roster region's size is already MAX_PLAYERS * cap * sizeof(record), verified
// exactly for all eight against tools/data/state_regions.json:
//
//     units       186400 = 8 * 100 * 233     turrets      14080 = 8 * 32 * 55
//     buildings   218400 = 8 * 100 * 273     productions  25920 = 8 *  8 * 405
//     soldiers     23200 = 8 * 100 *  29     mines        14336 = 8 * 32 * 56
//     storage      48800 = 8 *  25 * 244     labs          1600 = 8 * 25 *  8
//
// So the cap is `live_size / sizeof(record) / MAX_PLAYERS` -- the same shape
// sim_state.cpp's writable_state_tables() has used since SIM0 for unit_slots and friends, with the
// player dimension divided out. A host that binds a 500-cap roster gets 500 here for free, with
// nothing to declare and nothing to keep in step.
//
// WHY NOT THE BIND TABLE'S `count` FIELD. libmh_region_bind carries one, and T1 stores it
// (live_count). Deriving from the SIZE is strictly better for the property we actually want: it
// needs nothing from the host beyond binding the right number of bytes, whereas a declared count is
// a second thing that can be omitted or disagree. `count` remains available as an explicit override
// for a host whose region is not a clean MAX_PLAYERS-way split.
//
// THREE THINGS THAT DELIBERATELY STAY CONSTANT, so nobody "finishes the job" by deriving them too:
//
//   MAX_PLAYERS             -- 8 is in the RECORD LAYOUT (four [8] sub-arrays inside
//                              game_player_data), so a derived value would index past struct
//                              members, not past an array. Decision + measurement: D6.9.
//   PATH_SLOTS_PER_PLAYER   -- 100, and it tracks the UNIT cap conceptually (one path slot per
//                              unit). MEASURED: the shipped cap-raise manifest does NOT touch
//                              _G_LLM_STRAT_PATH_BUFFERS (0 of 7770 patches), so on a cap-raised
//                              exe units 100..499 genuinely have no path slot. That asymmetry is
//                              the shipped patch's, and reproducing it is fidelity; "fixing" it
//                              here would make our indexing disagree with the binary's.
//   PROD_SHUTTLE_SLOTS_PER_PLAYER -- 10, likewise untouched by the manifest.
//
#pragma once
#include <cstdint>

#include "addr/mh_regions.gen.h"
#include "addr/mh_structs.gen.h"
#include "state/region_runtime.h"

namespace mh::state {

// The player dimension. NOT derived -- see the header comment and D6.9.
inline constexpr int32_t ROSTER_PLAYERS = 8;

// The eight per-player row capacities. One struct rather than eight loose values so a view binds
// them in one assignment and a caller cannot pick up seven of them.
struct roster_caps {
    int32_t units;
    int32_t buildings;
    int32_t soldiers;
    int32_t storage;
    int32_t turrets;
    int32_t productions;
    int32_t labs;
    int32_t mines;
};

// The .bss values -- what the stock binary has, and what every one of the five module headers used
// to spell for itself. Kept as the fixture default and as the documentation of the stock strides;
// NOT what production indexing reads (that is live_roster_caps below).
inline constexpr roster_caps STOCK_ROSTER_CAPS = {
    100, // units       stride 0x5b04 / record 0xe9
    100, // buildings   stride 0x6aa4 / record 0x111
    100, // soldiers    record 0x1d
    25,  // storage     stride 0x17d4 / record 0xf4
    32,  // turrets     row stride 0x6e0 @0x0046780d / record 0x37
    8,   // productions row stride 0xca8 @0x004685d4 / record 0x195
    25,  // labs        row stride 0xc8  @0x0046898e / record 8
    32,  // mines       row stride 0x700 @0x0046d5cb / record 0x38
};

// Derived from the sizes the HOST bound. Re-resolved per call, nothing cached and nothing static --
// the same ST2/Law 3 posture as state() itself, so a rebase between calls is followed rather than
// missed.
inline roster_caps live_roster_caps() {
    using namespace mh::game;
    roster_caps c{};
    c.units = per_player_capacity(RID_UNITS, sizeof(mh_map_object_unit), ROSTER_PLAYERS);
    c.buildings =
        per_player_capacity(RID_BUILDINGS, sizeof(mh_map_object_building), ROSTER_PLAYERS);
    c.soldiers = per_player_capacity(RID_STRAT_SOLDIERS, sizeof(mh_llm_strat_crew_soldier), ROSTER_PLAYERS);
    c.storage =
        per_player_capacity(RID_UNIT_STORAGE, sizeof(mh_map_object_unit_storage), ROSTER_PLAYERS);
    c.turrets = per_player_capacity(RID_TURRETS, sizeof(mh_map_object_turret), ROSTER_PLAYERS);
    c.productions =
        per_player_capacity(RID_PRODUCTIONS, sizeof(mh_map_object_production), ROSTER_PLAYERS);
    c.labs  = per_player_capacity(RID_LABS, sizeof(mh_map_object_lab), ROSTER_PLAYERS);
    c.mines = per_player_capacity(RID_MINES, sizeof(mh_map_object_mine), ROSTER_PLAYERS);
    return c;
}

} // namespace mh::state
