//
// ai/ai_notify_removed.h -- the AI's object-removed notification hook (RI-AI / AI1C layer 0).
//
// llm_strat_ai_notify_object_removed @0x004db551, 948 bytes, seven callers -- every path by which
// the sim tells the strategic AI that a map object has just stopped existing
// (llm_strat_bldg_apply_damage, llm_strat_unit_apply_damage, llm_strat_bldg_state_dismantle_finish,
// llm_strat_unit_state_remove_silent, llm_strat_bldg_state_to_unit and two more).
//
// `flags` is a PACKED TARGET REF, not a player id: the low nibble is the owner, bit 0x40 says the
// object is a BUILDING and bits 0xa0 say it is a UNIT (ai_state.h REF_BLDG_BIT / REF_UNIT_BITS --
// the two tests disagree on nibble 0 and the original uses both, which is why they are two named
// predicates rather than one). `hard_remove` separates a permanent destruction from a reversible
// removal, and it changes exactly two things: whether a mine's resource site is INVALIDATED or
// merely reopened, and whether the build queue is reconciled at all.
//
// THREE PHASES, and only the third is what the function's name suggests:
//   (1) an influence-grid RE-SEED across every AI player, buildings only;
//   (2) a target-list PURGE across every active player, with a heli-mother early return in front
//       of it that abandons the whole notification;
//   (3) OWNER bookkeeping -- build-queue reconcile + resource-site release for a building, group
//       member unlink (+ possible group disband) for a unit.
//
// THE FUNCTION HAS NO EPILOGUE OF ITS OWN. Every exit is a jump to llm_strat_target_release_ref's
// tail-merged epilogue at 0x004dadfb, so Ghidra renders each as `caseD_2` and the .c reads as if
// control fell into a switch case. They are plain returns; read the .asm.
//
#pragma once
#include <cstdint>

#include "ai/ai_state.h"

namespace mh::ai {

// The logic over an EXPLICIT state, so `net_selftest.exe aitest` can drive it over heap buffers
// with recording call stubs -- no game and no rig. The wrapper below is this applied to state().
namespace detail {

// WHICH ARMS A CALL ACTUALLY TOOK. Returned rather than counted internally because this hook fires
// on every removed object and most calls take one short arm -- so a shadow site's call count says
// almost nothing about coverage, and the aggregate the arm prints from these fields is what
// distinguishes "clean over 4000 calls" from "clean over 4000 calls that all returned at the first
// gate". Nothing in the game reads it; it costs one returned struct.
struct notify_report {
    int32_t reseeds      = 0;     // grid_stamp_seeds calls made (phase 1)
    int32_t seed2        = 0;     // of those, ones that used the owner-plain seed value 2
    int32_t rescan_flags = 0;     // ai_turret_rescan_pending stores
    bool    heli_abort   = false; // returned at the heli-mother gate
    int32_t purges       = 0;     // target_list_remove calls
    bool    owner_is_ai  = false; // phase 3 ran at all
    bool    reconciled   = false; // queue_reconcile_bldg_change called
    bool    was_mine     = false; // reached the resource-site scan
    bool    site_release = false; // a site actually held this building's index
    bool    unlinked     = false; // group_member_unlink called
    bool    disbanded    = false; // group_remove called (the only path to the order queue)
};

// llm_strat_ai_notify_object_removed @0x004db551.
notify_report notify_object_removed(const ai_view &v, const ai_store &own, const ai_calls &gc,
                                    uint32_t flags, uint32_t object_index, int32_t hard_remove);

} // namespace detail

void notify_object_removed(uint32_t flags, uint32_t object_index, int32_t hard_remove);

} // namespace mh::ai
