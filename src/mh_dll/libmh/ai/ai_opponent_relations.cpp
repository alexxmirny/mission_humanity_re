//
// ai/ai_opponent_relations.cpp -- see ai_opponent_relations.h. Translated from the DISASSEMBLY
// (tmp/decomp/llm_strat_ai_update_opponent_relations_004d74de.asm), not from Ghidra's C: the
// decompile's raw pointer arithmetic for the roster occupancy test, the resource/mine-yield weight
// table indexing, and the ai_resource_need_score write are all resolved against
// tools/data/dll_struct_layouts.json + mh_structs.gen.h instead of being transcribed as byte
// offsets -- see the header comment and the translation brief's three called-out hazards.
//
#include "ai/ai_opponent_relations.h"

#include "fp/x87_shapes.h" // CRT-X87: the hoisted x87 blocks (the asm moved, it did not change)

namespace mh::ai {
namespace detail {

namespace {

// utils_math_trunc @0x004d0596 (`MH_UNAVAILABLE__parameter_storage_not_marshallable` in
// mh_calls.gen.h -- an x87-register-only leaf with no stack-passable signature). unit_weapon_power
// below performs a REAL CALL to it (0x004d72ff), not an inlined copy of its body the way
// ai_group_muster_pick.cpp's weapon_power_add_and_trunc shows -- but it is just as unmarshallable
// here (ST0 in, ST0 out), so it is reproduced as the same instruction sequence rather than reached
// through mh::call. Round toward zero via a temporary FPU control-word swap (RC=11 truncate),
// matching utils_math_trunc's own body exactly.
int32_t trunc_float_to_int32(float value) {
    return ::mh::fp::trunc_float_to_int32(value);
}

} // namespace

int32_t unit_weapon_power(const ai_view &v, uint32_t player, uint32_t unit_id) {
    const unit &u = unit_of(v, player, (int32_t)unit_id);

    // 0x004d72bb-0x004d72f9: unconditional 4-slot walk, no early-out on an empty/disabled slot.
    float total = 0.0f; // dword[ESP+8], zeroed (0x004d72b1)
    for (int32_t slot = 0; slot < 4; ++slot) {
        if (u.weapons[slot].enabled_2 != 0) {                    // 0x004d72ce/0x004d72d5
            const uint8_t weapon_id = u.weapons[slot].weapon_id; // 0x004d72d7
            // FADD onto the float accumulator at full (double) precision, then FSTP narrows back to
            // float32 EVERY iteration (0x004d72ea/0x004d72f1) -- not truncated to an int per-slot.
            total = (float)(total + v.cfg_weapons[weapon_id].power[player]); // 0x004d72de-0x004d72f1
        }
    }
    return trunc_float_to_int32(total); // one truncate, after the loop (0x004d72fb-0x004d7307)
}

void update_opponent_relations(const ai_view &v, const ai_store &own, const ai_calls &gc,
                               uint32_t                                       assessed_player,
                               mh::game::mh_llm_strat_ai_opponent_assessment *out) {
    // ---- the assessed player's own lazy score-cache refresh (hazard #3) --------------------------
    // Gated on THEIR OWN ai_enabled being 0 (their AI pipeline disabled), not on anything about the
    // ticking player who called us. Both callees run for real against the original binary and may
    // themselves touch further player_data state beyond what is shown here; the direct store below
    // is the only write this function makes itself for this branch.
    if (v.players[assessed_player].ai_enabled == 0) {
        gc.score_build_categories((int32_t)assessed_player);
        const int32_t tier                                  = gc.player_score_tier((int32_t)assessed_player);
        own.players[assessed_player].ai_resource_need_score = tier;
    }

    out->soldier_count             = 0;
    out->soldier_power             = 0;
    out->ground_count              = 0;
    out->ground_power              = 0;
    out->heli_count                = 0;
    out->heli_power                = 0;
    out->plane_count               = 0;
    out->plane_power               = 0;
    out->building_count            = 0;
    out->mine_count                = 0;
    out->turret_count              = 0;
    out->aa_capability_flags       = 0;
    out->weighted_resource_score   = 0;
    out->weighted_mine_yield_score = 0;
    // out->total_unit_power (+0x20) is DELIBERATELY NOT zeroed here -- the original's zero-init
    // block (0x004d752a-0x004d7561) skips it too (it stores 0x0,0x4,0x8,0xc,0x10,0x14,0x18,0x1c,
    // then jumps straight to 0x24,0x28,0x2c,0x30,0x34,0x38): it is unconditionally overwritten right
    // after the unit loop below, so the original never bothers.

    // ---- unit roster: classify every live unit, tally count + summed weapon power per class, OR in
    // the unit-AA-capability bit. COUNT-DRIVEN roster walk, NOT index-bounded -- the same idiom as
    // ai_turret_threat_rescan's building walk / ai_scan_visible's unit walk: `remaining` seeds from
    // the raw 2 bytes at units[assessed_player][0]'s `unit_above` field (offset 0) reassembled as a
    // little-endian ushort (it is a uint8_t[2] in the generated header, not a scalar -- no
    // reinterpret_cast, just the same byte-assembly ai_scan_visible.cpp already uses for the same
    // field); `unit_id` starts at 1 and increments every iteration; `remaining` decrements only when
    // the slot is occupied. The occupancy test itself (hazard #1) is the NEIGHBOURING field of that
    // same slot -- unit_proto_id at +2, not unit_above at +0.
    {
        const unit &u0        = unit_of(v, assessed_player, 0);
        uint32_t    remaining = uint32_t(u0.unit_above[0]) | (uint32_t(u0.unit_above[1]) << 8);
        int32_t     unit_id   = 1;
        while (remaining != 0) {
            if (unit_of(v, assessed_player, unit_id).unit_proto_id != 0) {
                if (gc.unit_is_ai_soldier(assessed_player, (uint32_t)unit_id) != 0) {
                    out->soldier_count += 1;
                    out->soldier_power += gc.unit_weapon_power(assessed_player, (uint32_t)unit_id);
                } else if (gc.unit_is_ai_ground((uint16_t)assessed_player, (uint32_t)unit_id) != 0) {
                    out->ground_count += 1;
                    out->ground_power += gc.unit_weapon_power(assessed_player, (uint32_t)unit_id);
                } else if (gc.unit_is_ai_heli(assessed_player, (uint32_t)unit_id) != 0) {
                    out->heli_count += 1;
                    out->heli_power += gc.unit_weapon_power(assessed_player, (uint32_t)unit_id);
                } else if (gc.unit_is_ai_plane(assessed_player, (uint32_t)unit_id) != 0) {
                    out->plane_count += 1;
                    out->plane_power += gc.unit_weapon_power(assessed_player, (uint32_t)unit_id);
                }
                // Tested for EVERY occupied unit regardless of which (or none) of the four classes
                // matched above -- the original falls through to this test unconditionally.
                if (gc.unit_has_aa_weapon(assessed_player, unit_id) != 0) {
                    out->aa_capability_flags |= 1;
                }
                --remaining;
            }
            ++unit_id;
        }
    }
    out->total_unit_power =
        out->heli_power + out->soldier_power + out->ground_power + out->plane_power;

    // ---- building roster: classify every live building as mine / turret / neither. Same
    // count-driven idiom, seeded from buildings[assessed_player][0].index -- a plain int16_t here,
    // no byte-array reassembly needed (unlike the unit roster's unit_above).
    {
        uint32_t remaining      = (uint16_t)building_of(v, assessed_player, 0).index;
        int32_t  building_index = 1;
        while (remaining != 0) {
            const building &b = building_of(v, assessed_player, building_index);
            if (b.building_id != 0) {
                out->building_count += 1;
                const cfg_building &cb = v.cfg_buildings[b.building_id];
                if (cb.type == BLDG_TYPE_H_MINE || cb.type == BLDG_TYPE_A_MINE) {
                    out->mine_count += 1;
                    // Two OUT-POINTER scratch records, fresh per mine: `quality_out` is fully
                    // overwritten by the call below (and unread here, matching the original -- it
                    // never reads its own local_24 either); `yield_out` is zeroed then filled at
                    // indices [1..4] by the callee (index 0 is a caller-owned total this function
                    // never touches, per calc_mine_yield_estimate's own contract).
                    mine_yield   yield_out{};
                    mine_quality quality_out{};
                    // committed llm_strat_ai_calc_mine_yield_estimate's 4th/5th params are int32_t *;
                    // the real payloads are mh::ai::mine_yield (by_resource[0..4]) and
                    // mh::ai::mine_quality (near/mid/far ring counts).
                    gc.calc_mine_yield_estimate((int32_t)b.building_id, (uint32_t)b.x, (uint32_t)b.y,
                                                reinterpret_cast<int32_t *>(&yield_out),
                                                reinterpret_cast<int32_t *>(&quality_out));
                    // hazard #2: weighted mine yield via resource_value_weight() (id-1 indexed),
                    // NOT raw pointer indexing off _G_LLM_STRAT_AI_ACTIVE_PLAYER_COUNT.
                    for (int32_t r = RESOURCE_ID_FIRST; r <= RESOURCE_ID_LAST; ++r) {
                        out->weighted_mine_yield_score += yield_out.by_resource[r] * resource_value_weight(v, r);
                    }
                } else if (cb.type == BLDG_TYPE_H_TURRET || cb.type == BLDG_TYPE_A_TURRET) {
                    out->turret_count += 1;
                    if (gc.bldg_has_aa_weapon(assessed_player, building_index) != 0) {
                        out->aa_capability_flags |= 2;
                    }
                }
                --remaining;
            }
            ++building_index;
        }
    }

    // ---- weighted resource stockpile (hazard #2 again): player_resources[assessed_player][1..4] *
    // resource_value_weight(1..4).
    for (int32_t r = RESOURCE_ID_FIRST; r <= RESOURCE_ID_LAST; ++r) {
        out->weighted_resource_score += v.player_resources[assessed_player * RESOURCE_SLOTS_PER_PLAYER + r] *
                                        resource_value_weight(v, r);
    }
}

} // namespace detail

void update_opponent_relations(uint32_t                                       assessed_player,
                               mh::game::mh_llm_strat_ai_opponent_assessment *out) {
    const ai_state st = state();
    detail::update_opponent_relations(st.read, st.own, live_calls(), assessed_player, out);
}

int32_t unit_weapon_power(uint32_t player, uint32_t unit_id) {
    const ai_state st = state();
    return detail::unit_weapon_power(st.read, player, unit_id);
}


} // namespace mh::ai
