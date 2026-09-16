//
// ai/ai_attacker_intel.h -- the AI's DAMAGE-REPORT hook (RI-AI / AI1B, antichain layer 0).
//
// GHIDRA CALLS THIS `llm_strat_ai_bldg_register_visible_building` @0x004db22f AND THAT NAME IS
// WRONG. Nothing here is about visibility, and the function fires for unit victims as well as
// building ones. It is the notification the SIM raises when one player's unit damages another
// player's object: both call sites are llm_strat_bldg_kill_credit @0x0044cab9 and
// llm_strat_unit_kill_credit @0x0044ce03, i.e. outside the AI cluster, and what it updates is the
// VICTIM's per-aggressor intel row. The name is left alone deliberately -- renaming a function
// ripples through mh_calls.gen.h / mh_export.gen.h / mh_shadow.gen.h, the migration manifest and
// the arming ini, so it is queued in tools/data/ghidra_findings.json for a prep session rather
// than changed under an unattended one. Grep for the Ghidra name; this file is it.
//
// THE PARAMETER NAMES BELOW ARE NOT GHIDRA'S EITHER, for the same reason. The committed prototype
// reads (building_id, observer_owner_byte, unit_id, target_class_byte, victim_destroyed), which
// describes an observer sighting a target. The two call sites say otherwise, and they say it
// identically (disassembled from F:\games\mh_en\mh_en_clean.bak.exe, md5 b3b389e8..., the same
// image as Ghidra's /eng/mh.exe):
//
//   0x0044caa7  movzx ecx, word [ebp+0x10]     ECX <- the AGGRESSOR's packed ref  (a caller arg)
//   0x0044caab  mov   ebx, [ebp+0x14]          EBX <- the AGGRESSOR's unit index  (a caller arg)
//   0x0044caae  mov   eax, [ebp-0x14]          EAX <- the VICTIM's owner
//   0x0044cab1  or    al, 0x40                        ... | 0x40 = "victim is a BUILDING"
//   0x0044cab3  movzx edx, ax                  EDX <- the VICTIM's packed ref
//   0x0044cab6  mov   eax, [ebp-0x20]          EAX <- the VICTIM's roster index
//   0x0044cab9  call  0x4db22f
//
// and the unit-victim site at 0x0044cdf1-0x0044ce03 is byte-for-byte the same shape with
// `or al, 0x80` -- "victim is a UNIT". Two things follow that the committed names actively hide:
// the 0x40 test inside the body is "is the damaged object a building", not "is the observer one";
// and the row of every table written here is the DAMAGED player while the column is the AGGRESSOR.
// The same marshalling is a second, independent derivation of `victim_destroyed` being the fifth
// (stack) argument: [ebp-0x34] / [ebp-0x3c] is the 0/1 the site computes from
// pending_damage >= energy immediately above the push.
//
// SO WHAT THE FUNCTION MEANS: "player A's unit just hurt something of mine -- record it." It bumps
// my per-aggressor hit counter, records what KIND of building of mine was hit and whether the hit
// landed inside my home radius, marks A hostile, and adds the damaged object to my AI target list.
//
#pragma once
#include <cstdint>

#include "ai/ai_state.h"

namespace mh::ai {

// player_data::ai_intel_flags bits, all set through the low byte of an int32 element. The three
// building-KIND bits are selected off the victim's own race (see race_*_type in ai_state.h).
inline constexpr int32_t INTEL_FLAG_BUILDING_HIT = 0x01; // any building of mine was hit
inline constexpr int32_t INTEL_FLAG_MINE_HIT     = 0x02;
inline constexpr int32_t INTEL_FLAG_TURRET_HIT   = 0x04;
inline constexpr int32_t INTEL_FLAG_NEAR_HOME    = 0x08; // within ai_expand_gate_value of my home tile
inline constexpr int32_t INTEL_FLAG_MOTHER_HIT   = 0x10;

// The two ref KIND bits the callers stamp into the victim's ref. 0x40 is the same bit
// ref_is_building_by_40() tests in ai_state.h; 0x80 is its unit counterpart, and the body reads it
// on the AGGRESSOR's ref rather than the victim's.
inline constexpr uint32_t REF_KIND_BUILDING = 0x40;
inline constexpr uint32_t REF_KIND_UNIT     = 0x80;
// What an aggressor ref is rewritten to when the aggressor is an AIRCRAFT: owner nibble | 0x20.
// The rewrite is NOT a flag on the intel table -- it survives only as far as the fourth argument of
// target_list_add, which is the sole consumer of the aggressor ref after this point.
inline constexpr uint32_t REF_KIND_AIRCRAFT = 0x20;

// The sentinel the tail-call gate compares against. -1 means "no object", and the original checks
// it AFTER having already indexed the rosters with it.
inline constexpr int32_t VICTIM_INDEX_NONE = -1;

// WHICH PATH A CALL TOOK. Instrumentation, not behaviour -- the original returns void. It exists
// because a call COUNT is not coverage here: the same-owner early-out and the "victim is a unit"
// gate each discard most of the body, so a site reporting tens of thousands of clean calls can
// have run the interesting half zero times. The shadow arm aggregates these so the run's own log
// says how many calls did work, and `aitest` asserts the path rather than inferring it.
struct register_report {
    enum class path {
        same_owner,      // the two owner nibbles matched -- returned having written nothing
        unit_victim,     // victim was not a building: hit counter + relation + target list only
        building_victim, // the full path, including the intel-flag block
    } taken = path::same_owner;

    bool    aircraft_retag  = false; // the aggressor was a unit AND an aircraft
    bool    counted         = false; // ai_intel_seen_count was incremented
    int32_t flags_or        = 0;     // the OR of every ai_intel_flags bit set by this call
    bool    relation_stamps = false; // ai_player_relation[aggressor] was set to -1
    bool    target_added    = false; // target_list_add was called
};

// The logic over an EXPLICIT state, so `net_selftest.exe aitest` can drive it over heap buffers
// with no game and no rig. The wrapper below is this applied to state(); the split costs one
// inlined call.
namespace detail {

// llm_strat_ai_bldg_register_visible_building @0x004db22f -- see the file header for the parameter
// identities, which are NOT the committed Ghidra names.
//
//   victim_index         the damaged object's index in ITS OWN roster (buildings[] or units[])
//   victim_ref           that object's owner nibble | REF_KIND_BUILDING or REF_KIND_UNIT
//   aggressor_unit_index the attacking unit's index in its owner's unit roster
//   aggressor_ref        the attacking object's packed ref
//   victim_destroyed     1 when the hit brought pending_damage up to energy. NEVER READ by the
//                        body -- it is in the signature only because both callers push it and the
//                        callee purges it (RET 0x4 @0x004db496). Do not tidy it away.
register_report register_attacker_damage(const ai_view &v, const ai_store &own, const ai_calls &gc,
                                         int32_t victim_index, uint32_t victim_ref,
                                         uint32_t aggressor_unit_index, uint32_t aggressor_ref,
                                         int32_t victim_destroyed);

} // namespace detail

void register_attacker_damage(int32_t victim_index, uint32_t victim_ref,
                              uint32_t aggressor_unit_index, uint32_t aggressor_ref,
                              int32_t victim_destroyed);

} // namespace mh::ai
