//
// ai/ai_notify_removed.cpp -- see ai_notify_removed.h. Translated from the DISASSEMBLY
// (tmp/decomp_ai/llm_strat_ai_notify_object_removed_004db551.asm), not from Ghidra's C: the draft
// renders every `JZ 0x004dadfb` as a fall-out of an enclosing `if`, which hides that those are
// RETURNS into another function's tail-merged epilogue, and it drops the whole 456-byte
// influence-reseed phase into a bare `llm_strat_ai_grid_stamp_seeds();` with no arguments.
//
#include "ai/ai_notify_removed.h"


namespace mh::ai {
namespace detail {

namespace {

// The six cfg Building types the original tests, in the original's order (0x004db658-0x004db68c).
// They are exactly the three race-paired families ai_state.h already names -- turret, mine, relay --
// but the original tests ALL SIX unconditionally, with no is_alien_race selector, so this must NOT
// be written with the race_*_type() helpers.
bool is_structural_building_type(uint8_t type) {
    return type == BLDG_TYPE_H_TURRET || type == BLDG_TYPE_A_TURRET || type == BLDG_TYPE_H_MINE ||
           type == BLDG_TYPE_A_MINE || type == BLDG_TYPE_H_RELAY || type == BLDG_TYPE_A_RELAY;
}

// The influence seed the removal stamps into a player's grid. 0 everywhere except on the OWNER's
// own grid for a NON-structural building, which gets 2 (0x004db68e vs the XOR at 0x004db695).
// The sibling llm_strat_ai_notify_map_changed makes the same call with 7 / 5 / 4, i.e. this is the
// "clear it again" end of the same three-way choice.
inline constexpr int32_t GRID_SEED_CLEAR       = 0;
inline constexpr int32_t GRID_SEED_OWNER_PLAIN = 2;

} // namespace

notify_report notify_object_removed(const ai_view &v, const ai_store &own, const ai_calls &gc,
                                    uint32_t flags, uint32_t object_index, int32_t hard_remove) {
    notify_report  rep{};
    const uint32_t owner = ref_owner(flags);

    // ---- PHASE 1: influence re-seed, BUILDINGS ONLY (0x004db56a-0x004db729) -------------------
    //
    // Note the roster row is the OWNER's throughout, while the grid written is player p's. The
    // original re-derives buildings[owner][object_index] three separate times inside this loop
    // (0x004db5a5, 0x004db64a, 0x004db6d8); it is the same record each time, so it is hoisted here.
    if ((flags & REF_BLDG_BIT) != 0) {
        const building &b       = building_of(v, owner, (int32_t)object_index);
        const uint16_t  bldg_id = b.building_id;
        const uint8_t   btype   = v.cfg_buildings[bldg_id].type;

        for (uint32_t p = 0; p < (uint32_t)*v.active_player_count; ++p) {
            // A removed TURRET makes every OTHER AI player want a threat rescan. Both halves of
            // the gate matter: p must be an AI (0x004db5db) and must not be the owner (0x004db5ea).
            if (btype == BLDG_TYPE_A_TURRET || btype == BLDG_TYPE_H_TURRET) {
                if (v.players[p].ai_enabled != 0 && owner != p) {
                    own.players[p].ai_turret_rescan_pending = 1;
                    ++rep.rescan_flags;
                }
            }

            // Everything below is for AI players only (0x004db60c).
            if (v.players[p].ai_enabled == 0) continue;

            // The seed value: 0 for everyone but the owner, and 0 for the owner too when the
            // building is one of the six structural types. Only the owner's grid, for a plain
            // building, is re-seeded to 2.
            const int32_t seed = (p == owner && !is_structural_building_type(btype))
                                     ? GRID_SEED_OWNER_PLAIN
                                     : GRID_SEED_CLEAR;

            // The stencil is the cfg row's 10x10 footprint mask, handed over as a bare pointer
            // exactly as ai_construction_sites.cpp does for the same family of __cdecl helpers --
            // the callee's parameter carries its committed pointee (uint8_t *, TACT1-P C6,
            // 2026-09-04) and casting the const away at the call site is the one place ai_view
            // cannot express what the original does.
            uint8_t *stencil = const_cast<uint8_t *>(&v.cfg_buildings[bldg_id].area[0][0]);
            gc.grid_stamp_seeds(own.players[p].ai_tile_flags_grid, *v.map_width, *v.map_height,
                                stencil, FOOTPRINT_SPAN, FOOTPRINT_SPAN, (int32_t)b.x, (int32_t)b.y,
                                seed);
            ++rep.reseeds;
            if (seed == GRID_SEED_OWNER_PLAIN) ++rep.seed2;
        }
    }

    // ---- PHASE 2: heli-mother abort, then the target purge (0x004db72f-0x004db78c) ------------
    //
    // A removed UNIT-class object whose cfg type is a heli mother abandons the notification
    // entirely -- these are RETURNS (JZ 0x004dadfb), not a skip of the loop below.
    if ((flags & REF_UNIT_BITS) != 0) {
        const unit   &u     = unit_of(v, owner, (int32_t)object_index);
        const int32_t utype = v.cfg_units[u.unit_proto_id].type;
        if (utype == UNIT_TYPE_A_HELI_MOTHER || utype == UNIT_TYPE_H_HELI_MOTHER) {
            rep.heli_abort = true;
            return rep;
        }
    }

    // Drop the object from EVERY active player's target list -- including the owner's, and
    // regardless of whether anybody is an AI. `flags` is passed through whole (the packed ref),
    // not the owner nibble.
    for (uint32_t p = 0; p < (uint32_t)*v.active_player_count; ++p) {
        gc.target_list_remove((int32_t)p, flags, (int32_t)object_index);
        ++rep.purges;
    }

    // ---- PHASE 3: owner bookkeeping, AI owners only (0x004db78e-0x004db900) --------------------
    if (v.players[owner].ai_enabled == 0) return rep;
    rep.owner_is_ai = true;

    if ((flags & REF_BLDG_BIT) != 0) {
        const building &b       = building_of(v, owner, (int32_t)object_index);
        const uint16_t  bldg_id = b.building_id;

        // Reconcile the AI build queue -- but only for a SOFT remove, and only when the removed
        // building was not the type the owner currently has cached as its turret candidate
        // (0x004db7ed compares the roster's building_id against player_data::ai_turret_candidate,
        // which is a cfg Building id, not a roster index).
        if ((int32_t)bldg_id != v.players[owner].ai_turret_candidate && hard_remove == 0) {
            gc.queue_reconcile_bldg_change(owner, object_index);
            rep.reconciled = true;
        }

        // The original RE-READS buildings[owner][object_index].building_id after that call
        // (0x004db828) rather than reusing what it read at 0x004db7e2; `bldg_id` is cached
        // across it here instead. That is equivalent, and the equivalence was checked rather
        // than assumed: llm_strat_ai_queue_reconcile_bldg_change's write set is player_data and
        // nothing else (its own module header records the per-callee audit), so no path inside
        // it can store into the buildings roster. Raised as a suspicion by the aliasing lens of
        // the 2026-08-05 reimpl-verify pass and refuted by the adjudicator on that evidence.

        // Everything below is MINES only (0x004db836/0x004db83f).
        const uint8_t btype = v.cfg_buildings[bldg_id].type;
        if (btype != BLDG_TYPE_A_MINE && btype != BLDG_TYPE_H_MINE) return rep;
        rep.was_mine = true;

        // Release the resource site this mine was occupying. `status` holds the occupying
        // building's ROSTER INDEX while a mine stands on the site (written by
        // llm_strat_ai_notify_bldg_constructed @0x004db084) -- see the field comment. A hard
        // remove INVALIDATES the site; a soft one reopens it as a build candidate. The loop stops
        // at the first match.
        // The count compare is UNSIGNED in the original (CMP ESI,[count] / JC at 0x004db899), and
        // the field is read MOVZX-then-compared against the full dword object_index (0x004db855 /
        // 0x004db85c) -- not truncated to 16 bits on both sides.
        const uint32_t site_count = (uint32_t)v.players[owner].ai_resource_site_count;
        for (uint32_t i = 0; i < site_count; ++i) {
            resource_site &site = own.players[owner].ai_resource_sites[i];
            if ((uint32_t)(uint16_t)site.status != object_index) continue;
            site.status      = hard_remove != 0 ? static_cast<int16_t>(RESOURCE_SITE_STATUS_INVALID)
                                                : static_cast<int16_t>(RESOURCE_SITE_STATUS_OPEN);
            rep.site_release = true;
            return rep;
        }
        return rep;
    }

    // UNIT branch. -1 and 0xffff are both "not in a group" and both are tested, in that order.
    const uint32_t group = gc.unit_get_ai_group_index((int32_t)owner, (int32_t)object_index);
    if (group == 0xffffffffu || group == 0xffffu) return rep;

    // The unlink is what DECREMENTS member_count; this function then increments a DIFFERENT field.
    gc.group_member_unlink((int32_t)owner, (int32_t)group, (int32_t)object_index);
    rep.unlinked = true;

    unit_group &g = own.players[owner].ai_groups[group];
    ++g.reinforce_pending;

    // Disband only when the group is now empty AND its index is >= 5 -- the first five groups are
    // the seeds llm_strat_spawn_ai_base creates and are never removed (0x004db8ee, a SIGNED JL).
    if (g.member_count != 0) return rep;
    if ((int32_t)group < AI_SEED_GROUP_COUNT) return rep;
    gc.group_remove((int32_t)owner, group);
    rep.disbanded = true;
    return rep;
}

} // namespace detail

void notify_object_removed(uint32_t flags, uint32_t object_index, int32_t hard_remove) {
    const ai_state st = state();
    (void)detail::notify_object_removed(st.read, st.own, live_calls(), flags, object_index,
                                        hard_remove);
}

// ---- the differential-oracle arm ---------------------------------------------------------------

} // namespace mh::ai
