//
// save/save_format.h -- the save file's VERSION GATE, reimplemented (RI-SAVE / SV1, batch A).
//
// Closure analysis, block table and open questions: docs/save-format.md. This is the first batch, and
// it is deliberately the version gate rather than the block loop, for three reasons:
//
//   1. IT IS THE LOAD-BEARING CLAUSE. SV1 and SV1-P both hinge on "a mismatched save is REFUSED
//      rather than silently misparsed" -- the failure mode that costs a user their game. Everything
//      else in the serializer is recoverable; this is not.
//   2. IT IS PURE. No file handle, no LZW, no game state -- it reads a 40-byte header buffer and the
//      build's own version index and returns a number. docs/save-format.md's shadowability section
//      says the rest of this subsystem is structurally un-shadowable because a file write cannot be
//      rolled back between two arms; this part sidesteps that entirely by having no I/O at all.
//   3. IT IS THE WHOLE OF THE FORMAT'S BACKWARD COMPATIBILITY. The detected slot is not just
//      accept/reject -- it gates three legacy read paths further down llm_game_load, so getting it
//      wrong mis-reads an old save rather than refusing it.
//
// Spec read directly out of /eng/mh.exe at 0x004476b9-0x00447778 (the header compare) and the three
// `CMP [EBP-0x1c],n` gates that follow.
//
#pragma once
#include <cstdint>

namespace mh::save {

inline constexpr int32_t VERSION_SLOTS      = 6;    // the table at 0x005d08f4 holds six entries
inline constexpr int32_t VERSION_STRING_LEN = 0x28; // 40 bytes, the `MOV ECX,0x28` fed to REPE CMPSB
inline constexpr int32_t VERSION_REJECT     = -1;   // `MOV [EBP-0x1c],0xffffffff` -- the initial value,
                                                    // and the sentinel the refuse test compares against

// The demo build's answer. THE ORIGINAL HAS NO VALUE HERE: on the `build_index == 0` path it verifies
// the header and then falls through WITHOUT ever assigning the detected-version local, so the three
// compat gates below would read uninitialised stack. That path is STATICALLY DEAD in the shipped
// image (build_index is a baked constant, 5), so this is not a bug anyone can hit -- but a
// reimplementation must still return something, and returning a defined "newest" is the only choice
// that cannot mis-read a real file. Documented rather than silently invented; see docs/save-format.md.
inline constexpr int32_t VERSION_DEMO_OK = VERSION_SLOTS - 1;

// The table and the build's own index, as pointers, so lockstest-style unit tests can drive this over
// plain arrays with no game -- the same shape mh::lockstep::engine_state uses and for the same reason.
struct version_table {
    const char    *slots;       // VERSION_SLOTS * VERSION_STRING_LEN bytes, NOT nul-terminated
    const int32_t *build_index; // _G_LLM_GAME_MISSION_VERSION_INDEX (0x005d09e4), a baked constant
};

// Returns the matched slot index, or VERSION_REJECT if the header matches nothing -- in which case
// llm_game_load closes the file and returns 0, i.e. refuses the save.
//
// TWO THINGS A PLAUSIBLE TRANSLATION GETS WRONG:
//  * THE SCAN STARTS AT 1, NOT 0 (`MOV [i],1` then `CMP [i],6 / JL`). Slot 0 is the demo string, and a
//    non-demo build will NOT accept a demo save -- it falls off the end of the loop and refuses.
//  * THERE IS NO EARLY EXIT. The loop runs 1..5 unconditionally and simply overwrites the result on
//    each match, so the LAST match wins. The six strings are distinct, so this is unobservable today;
//    it is reproduced because "unobservable given the current data" is not the same as "equivalent",
//    and a future table with a duplicate would diverge.
int32_t detect_version(const version_table &vt, const char *header);

// The three backward-compatibility gates, each named for what it actually decides rather than for its
// comparison. All three are false for a save this build wrote (it stamps slot 5), so they exist purely
// to read an older *Extermination* file in the *Mission: Humanity* build.
inline bool has_legacy_extra_block(int32_t v) { return v < 2; }       // CMP [EBP-0x1c],0x2 -- one extra 0x78b4 block
inline bool has_narrow_message_queue(int32_t v) { return v < 4; }     // narrow-string queue needing conversion
inline bool has_invasion_advisor_blocks(int32_t v) { return v >= 5; } // pre-1.03 files simply lack these two

// Bound to the live image.
const version_table &live_version_table();
int32_t              detect_version_live(const char *header);

} // namespace mh::save
