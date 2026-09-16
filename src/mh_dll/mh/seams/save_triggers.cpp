//
// seams/save_triggers.cpp -- the save/load A/B TRIGGERS, moved out of save/save_live.cpp on
// 2026-09-09 (LIFT-TABLE S6.1, docs/libmh-abi.md section 6).
//
// These three are harness, not save logic. Each is a one-line forward through the game's ENTRY
// (`mh::call::<root>`), and that is the entire point: the same trigger runs the ORIGINAL in an
// unpromoted run and OURS in a promoted one, so one harness verb exercises both arms and a trigger
// that fired while its promotion silently failed cannot look like a verified save. Their only
// callers are in seams/harness.cpp.
//
// WHY THEY MOVED. save/save_live.cpp compiles into libmh.vcxproj, so three raw VA calls were
// sitting in the standalone build for work that build never performs -- LIB-VA0's measure counted
// them, correctly, as a VA dependency. seams/ is not part of libmh, and a VA call is legal here by
// construction. The declarations stay in save/save_live.h, where harness.cpp already finds them.
//
// DO NOT "SIMPLIFY" THESE TO CALL THE PROMOTED BODY DIRECTLY. Routing through the entry is what
// makes them two-armed; calling mh::save::promoted_* would make an unpromoted run silently execute
// libmh code, which is the one thing the A/B rig exists to tell apart.
//
#include "save/save_live.h"

#include "addr/mh_calls.gen.h"

#include <cstdint>

namespace mh::save {

unsigned loadgame_now(char *save_name) {
    return static_cast<unsigned>(mh::call::llm_game_load(save_name));
}

unsigned savegame_now(char *save_name) {
    return static_cast<unsigned>(mh::call::game_SaveGame(save_name));
}

unsigned save_planet_now(int planet, unsigned mode) {
    return mh::call::map_SavePlanetToDisk(static_cast<uint32_t>(planet), mode);
}

} // namespace mh::save
