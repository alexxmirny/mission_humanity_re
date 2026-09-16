//
// ai/ai_active_unit_tick.cpp -- see ai_active_unit_tick.h. Translated from the DISASSEMBLY
// (tmp/decomp/llm_strat_ai_active_unit_tick_004ee30b.asm), cross-checked field-for-field against
// Ghidra's .c (tmp/decomp/llm_strat_ai_active_unit_tick_004ee30b.c), which is a faithful rendering
// of this one except for the compiler's folded pointer arithmetic (the swap-with-last memcpy, see
// below) and the `(ushort)`/`(uint)` casts that ARE the signedness evidence, not decoration -- every
// one of those was re-derived from the JCC in the .asm rather than trusted from the .c.
//
#include "ai/ai_active_unit_tick.h"

#include "fp/x87.h" // CRT-X87: the shared x87 truncation helpers

namespace mh::ai {
namespace {

// The x87 chain at 0x004ed070-0x004ed07b: FLD a double, CALL utils_math_trunc @0x004d0596 (FSTCW /
// RC=11,PC=11 / FLDCW / FRNDINT / FLDCW restore -- the same body ai_group_muster_pick.cpp's
// weapon_power_add_and_trunc and ai_mine_yield.cpp's x87_scale_and_trunc already inline elsewhere in
// this cluster), then FISTP dword (a 32-bit store here, NOT the 64-bit FISTP those two use -- the
// original's own stack slot is `dword ptr [EBP + -0x20]`). Private to this translation unit: unlike
// its two siblings, nothing else in the AI cluster needs this exact shape.
int32_t trunc_toward_zero(double value) {
    return ::mh::fp::trunc_i32(value);
}

} // namespace

namespace detail {

void active_unit_tick(const ai_view &v, const ai_store &own, const ai_calls &gc, int32_t player) {
    // ---- ENTRY: same four instructions as llm_strat_ai_player_tick's tail -----------------------
    // player_data[player].ai_player_relation[player] = FOREIGN_BLDG_CHANGE_FLAG ? -1 : 1. Translated
    // independently per the batch context -- NOT factored into a shared helper (brief rule 4).
    own.players[player].ai_player_relation[player] = (*v.foreign_bldg_change_flag == 0) ? 1 : -1;

    // The group index is `uint` in the original (local_20) and the loop bound compare is unsigned
    // (CMP EDX,[ai_group_count] / JC), so both are read as unsigned here -- harmless in practice
    // since ai_group_count never exceeds 0x20, but matching the assembly rather than assuming.
    for (uint32_t g = 0; g < (uint32_t)v.players[player].ai_group_count; ++g) {
        // Reset UNCONDITIONALLY for every group, before the per-group dispatch below -- matches
        // LAB_004ecba4's own instruction order (0x004ecba4, before the g==0 test).
        *own.scan_target_count = 0;

        bool ran_shared_scan = false;
        if (g == 0) {
            gc.holding_pen_scan_targets(player, 0);
            gc.target_list_invalidate_by_id(player, 0);
            gc.target_list_refresh_mothers(player);
            gc.target_list_scan_visible_enemies(player);
            ran_shared_scan = true;
        } else {
            const int16_t goal = v.players[player].ai_groups[g].goal;
            if (goal == 3 || goal == 8 || goal == 10 || goal == 0xb) {
                gc.holding_pen_scan_targets(player, g);
                gc.target_list_invalidate_by_id(player, (int32_t)g);
                gc.group_seed_resolved_target(player, (int32_t)g);
                ran_shared_scan = true;
            } else if (goal == 7) {
                gc.holding_pen_scan_targets(player, g);
                gc.target_list_invalidate_by_id(player, (int32_t)g);
                gc.target_list_invalidate_by_id(
                    player, (int32_t)v.players[player].ai_groups[g].link_target_group);
                ran_shared_scan = true;
            }
            // ANY OTHER GOAL VALUE: fall through WITHOUT running the shared body at all -- confirmed
            // at 0x004ecc2d-0x004ecc35 (CMP ...,0x7 / JNZ 0x004ed0f6, straight to the per-group loop
            // increment). This is hazard 9 of the translator brief -- the easiest thing here to get
            // subtly wrong is skipping this `if` and always running the shared body.
        }
        if (!ran_shared_scan) continue;

        // ---- shared body (LAB_004ecc5d) --------------------------------------------------------
        *own.attack_candidate_count = 0;

        // THE UNIT WALK: an intrusive linked list. head_unit/ai_group_next are both uint16_t in the
        // original and zero-extended on every read (matches the field types directly -- no mask
        // needed for these two).
        uint32_t unit_id = v.players[player].ai_groups[g].head_unit;
        while (unit_id != 0) {
            // THE ENERGY GATE. `player & 0xf` here is CONFIRMED REAL in the assembly (0x004ecc94
            // `AND EAX,0xf`), not a decompiler artifact -- reproduced exactly even though it is a
            // no-op for every valid player index 0..7 (MAX_PLAYERS = 8, so the low nibble never
            // differs from the full value). This is the ONE masked read in the whole function; the
            // ai_group_next read two lines below uses the FULL, unmasked player index
            // (0x004ecce3-0x004eccea has no AND at all).
            if (0.0 < unit_of(v, (uint32_t)player & 0xfu, (int32_t)unit_id).energy) {
                if (gc.unit_is_order_pending((uint32_t)player, unit_id) == 0) {
                    gc.attack_candidate_add(player, (int32_t)unit_id);
                    // ROUTED THROUGH gc, NOT called as C++ directly. The first draft called
                    // mh::ai::detail::unit_should_abandon_target (ai_abandon_target.cpp, translated
                    // in batch A) because the struct had no slot for it -- a genuine gap, now
                    // closed. The reason to prefer the member is ATTRIBUTION at this armed site: a
                    // direct call would have put OUR abandon_target inside OUR arm while the
                    // original arm ran the original's, so a divergence could have come from either
                    // body and this site's own log line would still have named only this one.
                } else if (gc.unit_should_abandon_target((uint32_t)player, (int32_t)unit_id) != 0) {
                    // llm_strat_unit_issue_default_order @0x00469e72 -- an ORIGINAL engine function
                    // (note the bare `unit_` prefix, not `ai_`), unrelated to any AI-cluster helper
                    // of a similar name. The original explicitly narrows player to (ushort) here
                    // (matches this member's own committed prototype).
                    gc.unit_issue_default_order((uint16_t)player, (int32_t)unit_id);
                }
            }
            unit_id = unit_of(v, (uint32_t)player, (int32_t)unit_id).ai_group_next;
        }

        if (*own.scan_target_count == 0) continue;

        gc.scan_target_list_sort(own.scan_targets, (uint32_t)*own.scan_target_count);
        scan_target_entry *cur = own.scan_targets;

        while (*own.scan_target_count != 0 && *own.attack_candidate_count != 0) {
            // ---- the turret-threat window scan (holding-pen group only) -----------------------
            if ((cur->class_flags & 0x20u) != 0 && g == 0) {
                const int32_t radius = gc.building_defense_weapon_range(
                    (int32_t)ref_owner(cur->target_ref), cur->target_index);
                if (radius != 0) {
                    bool           blocked    = false;
                    const uint32_t ring_cells = v.spiral_ring_cell_counts[radius];
                    for (uint32_t i = 0; i < ring_cells; ++i) {
                        const uint32_t gx = ((uint32_t)(int32_t)v.spiral_offsets[i].dx +
                                             (uint32_t)cur->tile_x) &
                                            *v.map_width_mask;
                        const uint32_t gy = ((uint32_t)(int32_t)v.spiral_offsets[i].dy +
                                             (uint32_t)cur->tile_y) &
                                            *v.map_height_mask;
                        // player_data[player].ai_tile_flags_grid is [256][256] flat, (x<<8)|y --
                        // read-only here, so through the const view rather than the store.
                        const uint8_t cell =
                            v.players[player].ai_tile_flags_grid[(gx << 8) | gy] & 0xfu;
                        if (cell != 0 && cell < 6) {
                            blocked = true;
                            break;
                        }
                    }
                    if (!blocked) cur->class_flags = (uint16_t)(cur->class_flags | 0x100u);
                }
            }

            // ---- per-attack-candidate scoring ----------------------------------------------------
            // Every comparison's signedness below was read off its JCC, not assumed:
            //   range_sq < weapon_range_sq         -- UNSIGNED (JNC @0x004ece20)
            //   dist_sq <= weapon_range_sq          -- UNSIGNED (JA  @0x004ece56)
            //   range_sq < dist_sq                  -- UNSIGNED (JNC @0x004ece71)
            // range_sq is zero-extended (MOVZX) at every one of the three sites, so it is compared
            // as a plain nonnegative uint32_t despite its uint16_t storage.
            const uint32_t candidate_count = (uint32_t)*own.attack_candidate_count;
            for (uint32_t i = 0; i < candidate_count; ++i) {
                attack_candidate &cand = own.attack_candidates[i];
                cand.score             = 0;
                cand.dist_sq           = (int32_t)gc.toroidal_dist_sq((int32_t)cand.x, (int32_t)cand.y,
                                                                      (int32_t)cur->tile_x, (int32_t)cur->tile_y);

                if ((uint32_t)cur->range_sq < cand.weapon_range_sq) cand.score += 1;
                if ((cur->class_flags & 0x100u) != 0 && g == 0) cand.score -= 100; // NOT `= -100`
                if ((uint32_t)cand.dist_sq <= cand.weapon_range_sq) cand.score += 1;
                if ((uint32_t)cur->range_sq < (uint32_t)cand.dist_sq) cand.score += 1;

                // AA/ground weapon-class match: candidate's OWN cfg Unit.type selects which of the
                // target's two weapon-class predicates must fail for the point to be awarded. `type`
                // is read as `(int)` (Ghidra's own rendering, and the JLE at 0x004ecebc/SETLE at
                // 0x004ecf08 are both signed) even though the field is declared uint32_t.
                const uint16_t proto = unit_of(v, (uint32_t)player, (int32_t)cand.unit_index)
                                           .unit_proto_id;
                const int32_t type = (int32_t)v.cfg_units[proto].type;
                if (cur->range_sq == 0 ||
                    (type > 0xe &&
                     gc.target_ref_has_aa_weapon(cur->target_ref, cur->target_index) == 0) ||
                    (type <= 0xe &&
                     gc.target_ref_has_ground_weapon(cur->target_ref, cur->target_index) == 0)) {
                    cand.score += 1;
                }
                if (cur->counter_target_index == cand.unit_index &&
                    (cur->counter_target_ref & 0xa0u) != 0) {
                    cand.score += 1;
                }
            }

            // ---- best-candidate pick ---------------------------------------------------------
            // Seeds match the assembly exactly: best_score = -0x7fffffff (NOT INT32_MIN, i.e. NOT
            // -0x80000000 -- `MOV EBX,0x80000001` at 0x004ecf6b), best_index = 0xffffffff, and a
            // candidate must ALSO score >= 0 to be eligible (the separate `CMP word ...,0 / JL` at
            // 0x004ecf84, redundant with the seed but present in the assembly and reproduced).
            uint32_t best_index = 0xffffffffu;
            int32_t  best_score = -0x7fffffff;
            for (uint32_t j = 0; j < (uint32_t)*own.attack_candidate_count; ++j) {
                const attack_candidate &cand  = own.attack_candidates[j];
                const int32_t           score = cand.score;
                if (score > best_score && score >= 0) {
                    // Ground- and AA-weapon eligibility are evaluated with NO short-circuit on the
                    // ground check's OWN result: if target_ref&0xc0 is set AND the ground call
                    // returns 0 (fails), the assembly still falls through to try the AA branch
                    // (0x004ecfa5-0x004ecfa9) rather than rejecting outright. The `!accept` guard
                    // below reproduces exactly that fallthrough, not an independent OR-short-circuit.
                    bool accept = false;
                    if ((cur->target_ref & 0xc0u) != 0) {
                        accept = gc.unit_has_ground_weapon((uint32_t)player, cand.unit_index) != 0;
                    }
                    if (!accept && (cur->target_ref & 0x20u) != 0) {
                        accept = gc.unit_has_aa_weapon((uint32_t)player, cand.unit_index) != 0;
                    }
                    if (accept) {
                        best_score = score;
                        best_index = j;
                    }
                }
            }

            if (best_index == 0xffffffffu) {
                // No eligible candidate for this target: drop it and move on.
                *own.scan_target_count -= 1;
                ++cur;
                continue;
            }

            const attack_candidate committed = own.attack_candidates[best_index];
            gc.commit_attack_order((uint32_t)player, committed.unit_index, (uint32_t)cur->target_ref,
                                   cur->target_index);
            gc.group_enter_hold(player, (int32_t)g);

            // THE SWAP-WITH-LAST removal. The original is
            // `memcpy(&candidates[best_index], candidates_base - 0x10 + count*0x10, 0x10)` then
            // `--count` -- Ghidra folds the `-1` into the array's byte base
            // (`count*0x10 + 0x1022304`, and 0x1022304 == candidates_base(0x1022314) - 0x10), which
            // is exactly `&candidates[count - 1]`. Ordinary struct assignment over the named,
            // trivially-copyable 0x10-byte record reproduces the same bytes.
            const uint32_t count              = (uint32_t)*own.attack_candidate_count;
            own.attack_candidates[best_index] = own.attack_candidates[count - 1];
            *own.attack_candidate_count       = (int32_t)(count - 1);

            if ((cur->class_flags & 0x80u) != 0) continue; // already scored once -- skip the tail

            // ---- incoming-damage tail ---------------------------------------------------------
            // BOTH branches resolve the target via ref_owner(cur->target_ref); the branch itself is
            // ref_is_building_by_a0 (== the same test the window scan / class_flags producers use
            // throughout this function), NOT ref_is_building_by_40 -- see the field comment on
            // target_ref for why the two tests disagree on nibble 0 and both are legitimately live
            // elsewhere in the AI cluster.
            //
            // BOTH tally reads are MOVZX (zero-extend), even though the underlying fields are typed
            // int16_t in this codebase -- reproduced via an explicit uint16_t reinterpretation so a
            // logically-negative tally reads as a large positive one here, matching the original.
            uint16_t incoming_tally;
            double   target_energy;
            if (ref_is_building_by_a0(cur->target_ref)) {
                const building &b = building_of(v, ref_owner(cur->target_ref), cur->target_index);
                incoming_tally    = (uint16_t)b.incoming_damage_tally;
                target_energy     = b.energy;
            } else {
                const unit &u  = unit_of(v, ref_owner(cur->target_ref), cur->target_index);
                incoming_tally = (uint16_t)u.incoming_threat_damage;
                target_energy  = u.energy;
            }
            const int32_t truncated = trunc_toward_zero(target_energy);
            // SIGNED compare (`CMP EDX,[mem] / JL` at 0x004ed081) -- tally is always nonnegative
            // (zero-extended above) so the sign only matters for `truncated`, which the original
            // treats as signed too.
            if ((int32_t)incoming_tally >= truncated) {
                cur->priority_score -= 1;
                cur->class_flags = (uint16_t)(cur->class_flags | 0x80u);
            }
        }
    }

    // The ONLY exit path in the function (ai_group_count <= group index).
    own.players[player].ai_target_list_count = 0;
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

void active_unit_tick(int32_t player) {
    const ai_state st = state();
    detail::active_unit_tick(st.read, st.own, live_calls(), player);
}


} // namespace mh::ai
