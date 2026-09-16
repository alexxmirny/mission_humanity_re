//
// ai/ai_construction_sites.cpp -- see ai_construction_sites.h. Translated from the DISASSEMBLY
// (tmp/decomp_a3/llm_strat_ai_scan_construction_sites_004e426b.asm), NOT from Ghidra's C
// (tmp/decomp_a3/llm_strat_ai_scan_construction_sites_004e426b.c): that decompile is actively
// misleading in the way _CONTEXT.md warns about -- it assumes the __cdecl callee
// llm_scan_masked_table_for_empty_cell clobbers EAX/ECX/EDX (it does not; it pushes/pops EBX, ECX,
// EDX, ESI and EDI), and every `extraout_ECX`/`extraout_EDX`/`extraout_EAX` in the .c is that wrong
// assumption manufacturing a fresh SSA value where the original simply kept a register live. Three
// concrete lies that would have been reproduced blind: (1) the spiral loop's anchor Y is fed back
// from the PREVIOUS iteration's result in the .c; the .asm shows a FIXED anchor with each iteration
// computing anchor+offset independently. (2) the .c invents two different local variables for "the
// draw used to queue construction" and "the draw used to notify"; the .asm shows EDI and EDX are
// both set from the SAME rand_below_ai(count) draw each iteration, and EDI is simply the copy that
// survives the queue_construction call. (3) the spiral-ring bound reads as a magic index
// (`_G_LLM_STRAT_AI_TILE_SPIRAL_RING_CELL_COUNTS[5]`) -- correct, but radius 5 is a literal in the
// original, not a derived value; see spiral_ring_cell_counts[5] below.
//
// EVERY offset used against `pd`/`own.players[player]` below was independently re-derived from the
// player_data base address (0x00e6dec0, addr/mh_regions.gen.h) against the raw hex literals in the
// .asm, not copied from the field comments -- e.g. the 0x80000000 mode flag tested at 0x004e4298/
// 0x004e429f and again at 0x004e4664 is byte [player_data+0x104cb] (the MSB of
// ai_build_plan_len_and_flag at +0x104c8), which matches that field's own Ghidra comment about a
// "ring/spiral-scan-around-threat mode for llm_strat_ai_scan_construction_sites" recorded elsewhere.
//
#include "ai/ai_construction_sites.h"


namespace mh::ai {
namespace detail {

namespace {

// player_data::ai_build_plan_len_and_flag's high bit (see addr/mh_structs.gen.h's field comment).
// Not a shared helper -- this file is its only user among the layer-3 batch.
inline constexpr uint32_t SPIRAL_MODE_FLAG = 0x80000000u;

// The SAME test appears twice in the assembly: once (unrolled x4) at 0x004e4330-0x004e43da counting
// how many of the four alt-mine candidates are usable at all, and once inside the retry loop at
// 0x004e4629/0x004e4649-0x004e4654 picking one. `candidate` is a raw cfg Building[] index (or -1 for
// "no candidate researched"), and player_data::ai_building_type_available is indexed by that same
// domain (see its field comment: "the only value ever tested is == 1").
inline bool mine_alt_candidate_ready(const player_data &pd, int32_t candidate) {
    return candidate != -1 && pd.ai_building_type_available[candidate] == 1;
}

} // namespace

int32_t scan_construction_sites(const ai_view &v, const ai_store &own, const ai_calls &gc,
                                int32_t player, int32_t category) {
    const player_data &pd = v.players[player];

    // 0x004e4298/0x004e429f: TEST byte[player_data+0x104cb],0x80 / JNZ 0x004e4318. When this bit is
    // set the jump lands PAST the pending-turret check AND both expand-gate threshold checks below,
    // straight at the mine-alt-candidate tally (LAB_004e4318) -- i.e. spiral mode skips all three of
    // the ordinary pacing gates, not merely the turret/other ratio gate further down (0x004e457a).
    const bool spiral_mode = (pd.ai_build_plan_len_and_flag & SPIRAL_MODE_FLAG) != 0;

    // Used for the pending-check below (normal mode only) AND again in the roster tally and the
    // turret-spacing rejection loop further down -- computed once regardless of mode, since the
    // tally/spacing uses always run.
    const uint8_t turret_type = race_turret_type(pd.is_alien_race);

    if (!spiral_mode) {
        // 0x004e42ba-0x004e42c3: bail if a turret of this race is already sitting in the AI build
        // queue.
        if (gc.bldg_type_queue_has_pending(player, (uint32_t)turret_type) != 0) return 1;

        // 0x004e42e3/0x004e42ea: UNSIGNED compare (JC) of ai_expand_gate_value against
        // expand_min_ticks.
        if ((uint32_t)pd.ai_expand_gate_value < (uint32_t)*v.expand_min_ticks) return 1;

        // 0x004e4308/0x004e4310: buildings[player][0].index, ZERO-EXTENDED from its 16-bit storage
        // (MOVZX) before a SIGNED compare (JL) against expand_min_building_count -- reproduced with
        // the same (uint16_t) truncation the sibling roster-walks in this cluster use, so a
        // "negative" stored index (there is none in practice) would not sign-extend the way a plain
        // int16_t->int32_t conversion would.
        if ((int32_t)(uint16_t)building_of(v, player, 0).index < *v.expand_min_building_count)
            return 1;
    }

    // ---- LAB_004e4318: common merge point for both modes -------------------------------------------
    //
    // Count how many of the four alt-mine candidates are actually available to build right now. This
    // is NOT the retry draw (that happens later, after the roster tally) -- it is a pure gate: if none
    // of the four are buildable there is nothing to expand with, regardless of mode.
    int32_t available_candidates = 0;
    for (int32_t i = 0; i < 4; ++i) {
        if (mine_alt_candidate_ready(pd, pd.ai_mine_alt_candidates[i])) ++available_candidates;
    }
    if (available_candidates == 0) return 1;

    // ---- roster tally: turret_count vs "other" (0x004e43e2-0x004e455c(loop)) -----------------------
    //
    // COUNT-DRIVEN walk, NOT index-bounded -- same idiom as turret_threat_rescan/scan_bldg_repair_
    // upgrade: `remaining` seeds from buildings[player][0].index and is decremented only when a
    // scanned slot is occupied; `idx` advances every iteration regardless. Bound test is `TEST
    // EDI,EDI / JA` (unsigned != 0).
    //
    // "other" counts a building that is NOT this race's turret, NOT its relay, and NOT its mine --
    // each is a race-selected type constant re-derived from is_alien_race per building, matching the
    // per-race constant pairs already in ai_state.h.
    int32_t other_count  = 0; // [EBP-0x24]
    int32_t turret_count = 0; // [EBP-0x28]
    {
        uint32_t remaining = (uint16_t)building_of(v, player, 0).index;
        int32_t  idx       = 1;
        while (remaining != 0) {
            const building &b = building_of(v, player, idx);
            if (b.building_id != 0) {
                const uint8_t cb_type = v.cfg_buildings[b.building_id].type;
                if (cb_type == turret_type) {
                    ++turret_count;
                } else if (cb_type != race_relay_type(pd.is_alien_race) &&
                           cb_type != race_mine_type(pd.is_alien_race)) {
                    ++other_count;
                }
                --remaining;
            }
            ++idx;
        }
    }

    if (!spiral_mode) {
        // 0x004e4583-0x004e4593: UNSIGNED compare -- bail when turret_count*100 > other_count*ratio.
        if ((uint32_t)(turret_count * 100) > (uint32_t)(other_count * *v.relay_to_mine_ratio_pct))
            return 1;
    }

    // ---- draw a build type: unbounded retry over the 4 alt-mine candidates (0x004e4599-0x004e4654) -
    //
    // `switch` has no default in the assembly (0x004e45a3: CMP EAX,3 / JA -> falls through to the
    // -1 check with whatever was in [EBP-0x20] from a PRIOR iteration). This is unreachable given
    // rand_below_ai's contract (returns a value in [0, range)), so `default` below can never fire in
    // practice; it exists only so the switch is well-formed.
    int32_t candidate_type = -1;
    for (;;) {
        const int32_t r = gc.rand_below_ai(4);
        switch (r) {
            case 0: candidate_type = pd.ai_mine_alt_candidates[0]; break;
            case 1: candidate_type = pd.ai_mine_alt_candidates[1]; break;
            case 2: candidate_type = pd.ai_mine_alt_candidates[2]; break;
            case 3: candidate_type = pd.ai_mine_alt_candidates[3]; break;
            default: break; // unreachable: rand_below_ai(4) is contracted to [0, 4)
        }
        if (mine_alt_candidate_ready(pd, candidate_type)) break;
    }

    const cfg_building &cb        = v.cfg_buildings[candidate_type];
    uint8_t            *footprint = const_cast<uint8_t *>(&cb.area[0][0]);
    uint8_t            *passable  = const_cast<uint8_t *>(v.passable);

    // 0x004e465a: zeroes expand_site_count -- happens ONCE here, after the build type is drawn and
    // before either candidate sweep. Not a per-sweep reset.
    *own.expand_site_count = 0;

    if (spiral_mode) {
        // ---- SPIRAL sweep (0x004e4671-0x004e4732) --------------------------------------------------
        //
        // Anchor = home_tile + quadrant[category] + 1, wrapped, on BOTH axes (0x004e467a/0x004e4693,
        // 0x004e4681/0x004e469a). The per-ring offsets are SIGNED bytes (MOVSX at 0x004e46aa/
        // 0x004e46c0) added to the FIXED anchor each iteration -- not fed back from the previous
        // result (see the file header note on the .c's lie here).
        const int32_t anchor_x =
            (int32_t)(((uint32_t)(pd.ai_home_tile_x + v.quadrant_dx2[category] + 1)) &
                      *v.map_width_mask);
        const int32_t anchor_y =
            (int32_t)(((uint32_t)(pd.ai_home_tile_y + v.quadrant_dy2[category] + 1)) &
                      *v.map_height_mask);

        // Bound is spiral_ring_cell_counts[5] -- radius 5 is a literal baked into this call site in
        // the original (0x004e4726: CMP EDX,[_G_LLM_STRAT_AI_BUILD_SPIRAL_SCAN_COUNT], the alias
        // symbol over the same four bytes as spiral_ring_cell_counts[5]), not a derived value.
        const uint32_t ring_count = v.spiral_ring_cell_counts[5];
        for (uint32_t i = 0; i < ring_count; ++i) {
            const int32_t x =
                (int32_t)(((uint32_t)(anchor_x + v.spiral_offsets[i].dx)) & *v.map_width_mask);
            const int32_t y =
                (int32_t)(((uint32_t)(anchor_y + v.spiral_offsets[i].dy)) & *v.map_height_mask);

            if (gc.footprint_scan_for_blocked_cell(passable, *v.map_width, *v.map_height, footprint,
                                                   FOOTPRINT_SPAN, FOOTPRINT_SPAN, x, y) != 0) {
                const int32_t n      = *own.expand_site_count;
                own.expand_site_x[n] = (uint8_t)x;
                own.expand_site_y[n] = (uint8_t)y;
                ++*own.expand_site_count;
            }
        }
    } else {
        // ---- FULL-MAP sweep (0x004e4737-0x004e49c2) -------------------------------------------------
        //
        // Every (x, y) on the torus. Unlike the spiral branch, this sweep does NOT call
        // footprint_scan_for_blocked_cell per cell -- it filters candidates purely off the per-player
        // ai_tile_flags_grid plane, and only the final site-pick retry loop below validates the
        // footprint. Two mutually exclusive per-cell tests, both keyed off the SAME cell's flag byte
        // (0x004e4770/0x004e47e6):
        //   - cell == 2: usable if ANY of the 4 torus neighbours reads == 3 as a FULL byte
        //     (0x004e477a-0x004e47e0).
        //   - cell == 0: usable if ANY of the 4 torus neighbours, masked with 0x3f, reads == 5
        //     (0x004e4815-0x004e487f).
        //   - anything else: not usable.
        // The X-shifted neighbours ((x-1,y) and (x+1,y)) wrap through map_width_mask; the Y-shifted
        // neighbours ((x,y-1) and (x,y+1)) wrap through map_height_mask -- each axis wraps only its
        // own coordinate, never both.
        const uint32_t wmask = *v.map_width_mask;
        const uint32_t hmask = *v.map_height_mask;

        for (int32_t x = 0; x < *v.map_width; ++x) {
            const int32_t xm1 = (int32_t)(((uint32_t)(x - 1)) & wmask);
            const int32_t xp1 = (int32_t)(((uint32_t)(x + 1)) & wmask);

            for (int32_t y = 0; y < *v.map_height; ++y) {
                const int32_t ym1 = (int32_t)(((uint32_t)(y - 1)) & hmask);
                const int32_t yp1 = (int32_t)(((uint32_t)(y + 1)) & hmask);

                const uint8_t cell = pd.ai_tile_flags_grid[(x << 8) | y];
                bool          usable;
                if (cell == 2) {
                    usable = pd.ai_tile_flags_grid[(xm1 << 8) | y] == 3 ||
                             pd.ai_tile_flags_grid[(xp1 << 8) | y] == 3 ||
                             pd.ai_tile_flags_grid[(x << 8) | ym1] == 3 ||
                             pd.ai_tile_flags_grid[(x << 8) | yp1] == 3;
                } else if (cell == 0) {
                    usable = (pd.ai_tile_flags_grid[(xm1 << 8) | y] & 0x3f) == 5 ||
                             (pd.ai_tile_flags_grid[(xp1 << 8) | y] & 0x3f) == 5 ||
                             (pd.ai_tile_flags_grid[(x << 8) | ym1] & 0x3f) == 5 ||
                             (pd.ai_tile_flags_grid[(x << 8) | yp1] & 0x3f) == 5;
                } else {
                    usable = false;
                }
                if (!usable) continue;

                // ---- turret-spacing rejection (0x004e4885-0x004e4986) -----------------------------
                //
                // Reject this cell if it lies within squared-distance 0x24 of one of the SAME
                // player's own race-turret buildings. dist_sq <= 0x24 rejects (the assembly compares
                // with JA, i.e. only NOT-rejected when dist_sq > 0x24 -- reject when < 0x25).
                //
                // Second COUNT-DRIVEN roster walk, bound test `TEST ECX,ECX / JG` (SIGNED > 0) this
                // time -- transcribed as its own signed comparison rather than unified with the
                // unsigned walk above, per the .asm.
                bool found = true;
                {
                    int32_t remaining2 = (int32_t)(uint16_t)building_of(v, player, 0).index;
                    int32_t idx2       = 1;
                    while (remaining2 > 0) {
                        const building &b = building_of(v, player, idx2);
                        if (b.building_id != 0) {
                            if (v.cfg_buildings[b.building_id].type == turret_type) {
                                const uint32_t dist = gc.toroidal_dist_sq(x, y, b.x, b.y);
                                if (dist <= 0x24) {
                                    found = false;
                                    break; // 0x004e4986: skips the remaining-- / idx++ entirely
                                }
                            }
                            --remaining2;
                        }
                        ++idx2;
                    }
                }

                if (found) {
                    const int32_t n      = *own.expand_site_count;
                    own.expand_site_x[n] = (uint8_t)x;
                    own.expand_site_y[n] = (uint8_t)y;
                    ++*own.expand_site_count;
                }
            }
        }
    }

    if (*own.expand_site_count == 0) return 1;

    // ---- pick a usable site: second unbounded retry loop (0x004e49de-0x004e4a2b) -------------------
    //
    // Every entry in expand_site_x/y already passed footprint_scan_for_blocked_cell against this same
    // candidate_type during the sweep above (the spiral branch calls it directly; the full-map branch
    // only ever records cells whose footprint has not since changed), so in practice this loop always
    // succeeds on its first draw -- but it is transcribed as the original's unbounded retry, not
    // assumed to terminate in one pass.
    int32_t site_idx;
    for (;;) {
        site_idx        = gc.rand_below_ai((uint32_t)*own.expand_site_count);
        const int32_t x = own.expand_site_x[site_idx];
        const int32_t y = own.expand_site_y[site_idx];
        if (gc.footprint_scan_for_blocked_cell(passable, *v.map_width, *v.map_height, footprint,
                                               FOOTPRINT_SPAN, FOOTPRINT_SPAN, x, y) != 0)
            break;
    }

    // 0x004e49ea/0x004e4a2d/0x004e4a7d: the winning index (EDI in the .asm, the copy that survives
    // the queue_construction call) is used for BOTH the queue call and the notify call below -- there
    // is only one draw, read twice here since nothing between the two calls can mutate
    // expand_site_x/y.
    const int32_t site_x = own.expand_site_x[site_idx];
    const int32_t site_y = own.expand_site_y[site_idx];

    // player_data ONLY, and it calls nothing -- appends to the AI's own build queue, does not place a
    // building.
    gc.bldg_queue_construction(player, candidate_type, (int16_t)site_x, (uint16_t)site_y);

    // 0x004e4a5b-0x004e4a76: re-tests the SAME spiral-mode bit and, only when set, ORs 0x20 into the
    // status byte of the JUST-QUEUED entry (ai_bldg_queue[ai_bldg_queue_count - 1]) -- AFTER the queue
    // call, using the count as it stands post-append. `status`'s 0x20 bit is documented (addr/
    // mh_structs.gen.h) as "affordability check waived -> take the normal construction-order path".
    if (spiral_mode) {
        player_data  &me   = own.players[player];
        const int32_t last = me.ai_bldg_queue_count - 1;
        me.ai_bldg_queue[last].status |= 0x20;
    }

    gc.notify_map_changed(player, candidate_type, site_x, site_y);
    return 0;
}

} // namespace detail

int32_t scan_construction_sites(int32_t player, int32_t category) {
    const ai_state st = state();
    return detail::scan_construction_sites(st.read, st.own, live_calls(), player, category);
}


} // namespace mh::ai
