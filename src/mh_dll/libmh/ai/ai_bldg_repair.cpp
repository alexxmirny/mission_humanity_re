//
// ai/ai_bldg_repair.cpp -- see ai_bldg_repair.h. Translated from the DISASSEMBLY
// (tmp/decomp_a2/llm_strat_ai_scan_bldg_repair_upgrade_004e5e04.asm), not from Ghidra's C: the
// decompile casts the building's `energy` (a double, addr/mh_structs.gen.h) to `float` in its first
// OR clause, which the assembly does not do -- see the FP note below.
//
#include "ai/ai_bldg_repair.h"


namespace mh::ai {
namespace detail {

namespace {

// Building.state values the original tests by literal (0x64/0x6a/0x6b/0x82), transcribed from the
// Ghidra plate's CONSTRUCTION/CHARGE_STEP/DISMANTLING/UPGRADING labels (mh_map_object_building.state
// carries no C++ enum type yet, so these are local, file-scope constants rather than an invented
// shared helper). "Busy" gates both the repair and the upgrade queueing paths identically.
inline constexpr uint16_t BLDG_STATE_CONSTRUCTION = 0x64;
inline constexpr uint16_t BLDG_STATE_CHARGE_STEP  = 0x6a;
inline constexpr uint16_t BLDG_STATE_DISMANTLING  = 0x6b;
inline constexpr uint16_t BLDG_STATE_UPGRADING    = 0x82;

inline bool bldg_state_busy(uint16_t state) {
    return state == BLDG_STATE_CONSTRUCTION || state == BLDG_STATE_CHARGE_STEP ||
           state == BLDG_STATE_DISMANTLING || state == BLDG_STATE_UPGRADING;
}

} // namespace

void scan_bldg_repair_upgrade(const ai_view &v, const ai_store &own, const ai_calls &gc,
                              int32_t player) {
    (void)own; // this function writes nothing of its own -- every effect goes through `gc`

    const player_data &pd = v.players[player];

    // COUNT-DRIVEN roster walk, same idiom as turret_threat_rescan: `remaining` seeds from the
    // building-0 slot's `index` field reinterpreted as the ushort bit pattern the original reads,
    // and is decremented only when a scanned slot is occupied (building_id != 0); `building_index`
    // advances every iteration regardless. A fixed BUILDINGS_PER_PLAYER bound would over- or
    // under-run on a sparse roster.
    uint32_t remaining      = (uint16_t)building_of(v, player, 0).index;
    int32_t  building_index = 1;

    while (remaining != 0) {
        const building &b = building_of(v, player, building_index);
        if (b.building_id != 0) {
            const cfg_building &cb = v.cfg_buildings[b.building_id];

            // FP: repair_ratio is a FLOAT (AI.SCR fRepairRatio); cfg_building::energy and
            // building::energy are both DOUBLEs. The original loads the float, FMULs it by the
            // double config-max energy (widening to the x87 stack's extended precision), and FCOMPs
            // that against the double instance energy -- there is no float truncation anywhere in
            // this comparison, unlike the decompile's `(float)` cast on both operands. Do not route
            // this through a narrower type.
            const bool ratio_damaged = (double)*v.repair_ratio * cb.energy > b.energy;
            // Second OR clause: strictly-below-max AND this occupied slot is the player's cached
            // mother-building type. Short-circuited exactly as the original's JNC/JNZ pair only
            // evaluates the building_id compare when the energy compare already passed.
            const bool mother_damaged =
                (b.energy < cb.energy) &&
                ((uint32_t)b.building_id == pd.ai_mother_building_type);

            // `do_upgrade_check` mirrors the original's control flow exactly: LAB_004e5fd5 (the
            // upgrade-eligibility block) is reached whenever repair was NOT queued this call --
            // not damaged, not alive, or busy -- and is NOT reached (the function goes straight to
            // the per-building tail) once repair processing actually ran, regardless of whether that
            // run found-and-kept an existing entry, found-and-removed a stale one, or queued a new
            // one. Preserve this "at most one queue mutation per building" shape; it is not an
            // independent if/else, it is one flag threading three original jump targets together.
            bool do_upgrade_check = true;

            if (ratio_damaged || mother_damaged) {
                if (gc.bldg_is_alive(player, building_index) != 0 && !bldg_state_busy(b.state)) {
                    do_upgrade_check = false;
                    // Scan the AI build queue for an existing repair entry (status low nibble == 3)
                    // targeting this building. `slot == count` after the loop means "not found".
                    uint32_t slot = 0;
                    for (; slot < (uint32_t)pd.ai_bldg_queue_count; ++slot) {
                        const auto &qe = pd.ai_bldg_queue[slot];
                        if ((qe.status & 0xf) == 3 && qe.building_index == building_index) {
                            // A matching entry exists: drop it only if the building has since died;
                            // either way, this call makes no further queue mutation for it.
                            if (gc.bldg_is_alive(player, building_index) == 0)
                                gc.queue_remove_at(player, slot);
                            break;
                        }
                    }
                    // `pd.ai_bldg_queue_count` MUST STAY A LIVE RE-READ here -- do not hoist it into
                    // a local before or around the queue_remove_at call above. The original re-reads
                    // the same dword at 0x004e5f9f, AFTER that callee may have decremented it, and
                    // when the removed entry was the queue's LAST slot the post-removal count equals
                    // the match slot, so this branch fires immediately after a removal. That
                    // data-dependent aliasing between the callee's write and this read is behaviour,
                    // not an accident (confirmed by the reimpl-verify review).
                    if (slot == (uint32_t)pd.ai_bldg_queue_count) {
                        gc.queue_bldg_repair(player, building_index);
                        gc.queue_rotate_newest_to_front(player);
                    }
                }
            }

            if (do_upgrade_check) {
                const int32_t upgrade_index = cb.upgrade_index;
                if (upgrade_index != 0) {
                    // The gate is "is the UPGRADED building type available to this player" --
                    // ai_building_type_available is indexed by a cfg Building[] index, and
                    // Building[].upgrade_index IS one. The translator found this region carried as
                    // `uint8_t _pad_0x25bb0[200]`, i.e. undefined padding, and flagged it rather than
                    // inventing an offset; it was named and typed in Ghidra on 2026-08-01 from its 20
                    // code referrers (llm_strat_bldg_find_by_type_for_player and the AI construction
                    // planners all gate on the same byte == 1). Its length is the INDEX DOMAIN
                    // (Building[] is [100]), not the 200-byte gap -- see the field comment.
                    if (pd.ai_building_type_available[upgrade_index] == 1) {
                        if (gc.bldg_is_alive(player, building_index) != 0 && !bldg_state_busy(b.state)) {
                            // Same queue-scan shape as the repair path, but status low nibble == 4
                            // and terminating in queue_bldg_upgrade (no rotate-newest-to-front call
                            // here -- that asymmetry is in the original, not a translation slip).
                            uint32_t slot = 0;
                            for (; slot < (uint32_t)pd.ai_bldg_queue_count; ++slot) {
                                const auto &qe = pd.ai_bldg_queue[slot];
                                if ((qe.status & 0xf) == 4 && qe.building_index == building_index) {
                                    if (gc.bldg_is_alive(player, building_index) == 0)
                                        gc.queue_remove_at(player, slot);
                                    break;
                                }
                            }
                            if (slot == (uint32_t)pd.ai_bldg_queue_count) {
                                gc.queue_bldg_upgrade(player, (uint32_t)building_index);
                            }
                        }
                    }
                }
            }

            --remaining;
        }
        ++building_index;
    }
    // The original's tail `JMP 0x004e7932` is a shared Watcom epilogue, not a call -- it means
    // `return`.
}

} // namespace detail

void scan_bldg_repair_upgrade(int32_t player) {
    const ai_state st = state();
    detail::scan_bldg_repair_upgrade(st.read, st.own, live_calls(), player);
}


} // namespace mh::ai
