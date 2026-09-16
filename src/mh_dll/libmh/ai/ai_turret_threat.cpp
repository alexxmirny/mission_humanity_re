//
// ai/ai_turret_threat.cpp -- see ai_turret_threat.h. Translated from the DISASSEMBLY
// (tmp/decomp_ai_a_l1/llm_strat_ai_turret_threat_rescan_004d8526.asm), not from Ghidra's C: the
// decompile's `extraout_ECX` / `extraout_ECX_00` noise is an artifact of Ghidra assuming
// llm_strat_bldg_is_alive clobbers ECX -- in the assembly ECX is simply the building-loop index,
// live across the call, throughout. There is no register-provenance ambiguity to resolve here.
//
#include "ai/ai_turret_threat.h"


namespace mh::ai {
namespace detail {

void turret_threat_rescan(const ai_view &v, const ai_store &own, const ai_calls &gc,
                          uint32_t player) {
    own.players[player].ai_turret_rescan_pending = 0;

    // The grid wiped/re-stamped below is ALWAYS `player`'s own. This is pure pointer arithmetic on
    // a parameter that is constant for the whole call -- taking the address once is not the
    // "cached read of mutable state" the brief warns against, since nothing here dereferences
    // player_data through it until the callee does.
    uint8_t *own_threat_grid = &own.players[player].ai_tile_flags_grid[0];
    gc.grid_clear_threat_bit(own_threat_grid, *v.map_width, *v.map_height);

    // Outer loop: every active player (UNSIGNED compare against active_player_count -- the
    // original is `CMP ESI,[active_player_count] / JC`), skipping the ticking player itself.
    for (uint32_t p = 0; p < (uint32_t)*v.active_player_count; ++p) {
        if (p == player) continue;

        // COUNT-DRIVEN roster walk, NOT index-bounded: `remaining` starts at p's live building
        // count (buildings[p][0].index, reinterpreted as the ushort bit pattern the original
        // reads it as) and the loop runs while it is nonzero. `building_index` is incremented
        // EVERY iteration; `remaining` is decremented only when the slot is occupied
        // (building_id != 0) -- an empty slot advances the index without consuming the count. A
        // fixed-bound rewrite over BUILDINGS_PER_PLAYER would behave differently whenever the
        // roster is sparse.
        uint32_t remaining      = (uint16_t)building_of(v, p, 0).index;
        int32_t  building_index = 1;
        while (remaining != 0) {
            const building &b = building_of(v, p, building_index);
            if (b.building_id != 0) {
                // The roster owner passed to bldg_is_alive is `p` (the player being scanned), not
                // `player` -- matches the original's (EAX=player_00, EDX=building_index) call.
                if (gc.bldg_is_alive((int32_t)p, building_index) != 0) {
                    const cfg_building &cb = v.cfg_buildings[b.building_id];
                    if (cb.type == BLDG_TYPE_A_TURRET || cb.type == BLDG_TYPE_H_TURRET) {
                        const int32_t weapon_id = cb.weapon_id;
                        if (weapon_id != 0) {
                            const cfg_weapon &w = v.cfg_weapons[weapon_id];
                            // `range_max` IS INDEXED BY `p`, the scanned/turret-owning player --
                            // NOT `player`, the ticking player whose grid this call stamps. Read
                            // straight off the .asm (0x004d8659..0x004d865e: EBX = ESI*4 = p*4,
                            // added into the Weapon[] row offset before the range_max load). Using
                            // `player` here instead is the wrong-but-plausible mistake the
                            // conductor flagged, and it is invisible in a one-player test since
                            // the scanned player is never `player` on this path.
                            gc.grid_stamp_threat_ring(own_threat_grid, *v.map_width, *v.map_height,
                                                      b.x, b.y, w.range_max[p]);
                        }
                    }
                }
                --remaining;
            }
            ++building_index;
        }
    }
    // The original's tail `JMP 0x004d4e61` is a shared Watcom epilogue, not a call -- it means
    // `return`.
}

} // namespace detail

void turret_threat_rescan(uint32_t player) {
    const ai_state st = state();
    detail::turret_threat_rescan(st.read, st.own, live_calls(), player);
}


} // namespace mh::ai
