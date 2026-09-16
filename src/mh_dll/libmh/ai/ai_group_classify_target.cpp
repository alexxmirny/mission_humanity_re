//
// ai/ai_group_classify_target.cpp -- see ai_group_classify_target.h. Translated from the DISASSEMBLY
// (tmp/decomp/llm_strat_ai_group_classify_target_object_004eb48e.asm), not from Ghidra's .c
// (tmp/decomp/llm_strat_ai_group_classify_target_object_004eb48e.c), which the .c ITSELF already
// uses `target_record->field` accesses for (the fresh export, against the
// llm_strat_ai_scan_target_entry retype) but still disagrees with the assembly in one place:
//
//   THE UNIT ARM'S WEAPON-RANGE SQUARING. The .c truncates `llm_strat_unit_max_weapon_range`'s
//   return to ushort BEFORE squaring (`uVar3 = (ushort)uVar4; ...range_sq = uVar3*uVar3;`). The
//   .asm does not: `CALL ...; MOV word[EBX+0xe],AX; IMUL EAX,EAX` -- the IMUL is the 32-BIT form and
//   operates on the callee's raw EAX, not on a 16-bit-truncated copy of it, and only the low 16 bits
//   of THAT product are ever stored. The two differ whenever the callee's return exceeds 0xffff.
//   The .asm wins (see the translator brief, rule 1); see the unit arm below.
//
// EVERY ABSOLUTE RE-DERIVED against addr/mh_structs.gen.h's offsetof asserts, not trusted from either
// source's literals:
//   buildings base 0xc3d2a0 (= 0xc3d2a2 - offsetof(building_id)=2); x/y/sub_id/energy/
//     incoming_damage_tally offsets 0xc3/0xc4/0xc6/0x19/0x10f match 0xc3d363/0xc3d364/0xc3d366/
//     0xc3d2b9/0xc3d3af exactly.
//   units base 0xdd8c48; x/y/target_index/target_ref/energy/incoming_threat_damage/ai_group_index
//     offsets 0x84/0x85/0x8a/0x8c/0x18/0xdc/0xd8 match 0xdd8ccc/0xdd8ccd/0xdd8cd2/0xdd8cd4/0xdd8c60/
//     0xdd8d24/0xdd8d20 exactly.
//   Building cfg table base 0xd9ec80, .type offset 8 -> 0xd9ec88. Weapon cfg table base 0xc3a520,
//     .range_max offset 0x36 -> 0xc3a556 (stride 0x16c, matching WEAPON_TYPE_COUNT's IMUL ...,0x16c).
//   player_data base 0xe6dec0; ai_tile_flags_grid offset 0x3c, ai_mother_building_type offset
//     0x288b8 -- both match the (player * 0x288fc + ...) address arithmetic exactly.
//   turrets base 0xcc0fe0 (region "turrets", 14080 = 8 * 32 * 0x37) -- see the DECLARED NEED below,
//     this region has no struct type and no ai_view member yet.
//
#include "ai/ai_group_classify_target.h"

#include "fp/x87.h" // CRT-X87: the shared x87 truncation helpers

namespace mh::ai {
namespace {

// Bit-for-bit the same x87 sequence ai_group_muster_pick.cpp's `weapon_power_add_and_trunc` inlines
// for utils_math_trunc @0x004d0596 (FSTCW / RC=11,PC=11 / FLDCW / FRNDINT / FLDCW restore), just
// specialised to a single `double` IN and a 32-bit `FISTP` OUT -- this function's two trunc call
// sites (0x004eb6a3/0x004eb6a8 and 0x004eb96d/0x004eb972) both store a DWORD, not the QWORD
// ai_group_muster_pick.cpp's callers use. Internal linkage: this is private to this translation, not
// a shared helper (see the translator brief, rule 4 -- no new SHARED helpers).
int32_t trunc_toward_zero_i32(double v) {
    return ::mh::fp::trunc_i32(v);
}

} // namespace

namespace detail {

void group_classify_target_object(const ai_view &v, const ai_calls &gc, uint32_t player_id,
                                  scan_target_entry &tr) {
    // ECX in the original (`AND AL,0xf` off the low byte of target_ref, 0x004eb4ab): the TARGET's
    // OWN owning player -- NOT `player_id`, the AI whose targeting rules are being evaluated. Every
    // read below that indexes buildings[]/units[]/turrets[] by the TARGET's location uses
    // `target_owner`; every read that indexes player_data[]/buildings[] by the AI's OWN player
    // (the threat grid, the mother-building lookup, the counter-target's ai_group_index) uses
    // `player_id` verbatim, exactly as the .asm's ESI (loaded once at entry and never reassigned in
    // either arm until deep inside the UNIT arm's own tail computation) does.
    const uint32_t target_owner = ref_owner((uint32_t)tr.target_ref);

    if (ref_is_building_by_a0(tr.target_ref)) {
        // ---- BUILDING target (.asm 0x004eb6c7-0x004eb99e) ---------------------------------------
        const building &b       = building_of(v, target_owner, tr.target_index);
        tr.tile_x               = b.x;
        tr.tile_y               = b.y;
        tr.counter_target_ref   = 0;
        tr.counter_target_index = 0;
        tr.range_sq             = 0;

        const cfg_building &cb = v.cfg_buildings[b.building_id];
        if (cb.type == BLDG_TYPE_A_TURRET || cb.type == BLDG_TYPE_H_TURRET) {
            ++tr.priority_score;
            // Only the LOW BYTE of class_flags is OR'd in the original (`OR byte ptr [EBX+6],0x20`)
            // -- every flag constant here is < 0x100, so a plain uint16_t `|=` is bit-for-bit
            // identical to the byte-only OR (it can only ever touch bits already inside the low
            // byte), matching the style of ai_engage.cpp's "only the low byte is OR'd... same value"
            // note rather than reaching for a reinterpret_cast.
            tr.class_flags |= 0x20u;

            const uint32_t weapon_id = (uint32_t)cb.weapon_id;
            if (weapon_id != 0) {
                // `MOV AX, word ptr [...]` -- a 16-bit load that leaves EAX's UPPER 16 bits exactly
                // as `weapon_id * sizeof(Weapon)` (the preceding IMUL's full 32-bit result) left
                // them, since a 16-bit MOV to AX never touches the rest of EAX. That upper half is
                // provably 0 for every WEAPON_TYPE_COUNT-bounded weapon_id (max 31 * 0x16c = 0x2af4,
                // well under 0x10000), so the subsequent 32-bit `IMUL EAX,EAX` squaring the whole
                // register is behaviourally identical, for every id this 32-entry table can legally
                // hold, to squaring just the 16-bit range value the way this line does.
                const uint16_t range = (uint16_t)v.cfg_weapons[weapon_id].range_max[target_owner];
                tr.range_sq          = (uint16_t)((uint32_t)range * (uint32_t)range);
            }

            // DECLARED NEED: turrets[target_owner][b.sub_id] -- see the report. Region "turrets"
            // (0xcc0fe0, 14080 bytes = [MAX_PLAYERS][32] of 0x37) exists in mh_regions.gen.h but has
            // no struct type and is not in ai_view. Its field at +0x16 is a packed owner|kind ref
            // (same encoding as target_ref) of whoever this turret slot is tracking as ITS
            // counter-target; the field at +0x18 gates on nonzero ("is this turret currently
            // engaging anything") and its LOW 16 BITS are the tracked object's roster index
            // (`CMP dword ptr [...+0x18],0` then a separate `MOV word ptr [...+0x18]` read).
            const auto &t = turret_of(v, target_owner, b.sub_id);
            if (t.counter_target_slot != 0 && player_id == ref_owner((uint32_t)t.counter_ref)) {
                tr.counter_target_ref   = t.counter_ref;
                tr.counter_target_index = (uint16_t)(uint32_t)t.counter_target_slot;

                // THE ASYMMETRY WITH THE UNIT ARM BELOW: here `(ref & 0xa0) == 0` ALONE decides
                // "counter-target is a building" (.asm 0x004eb85e-0x004eb88a) -- there is NO
                // confirming `(ref & 0x40) != 0` test the way the unit arm's LAB_004eb5ff has. Read
                // twice against the raw bytes because it looks like it should be symmetric with the
                // unit arm and it is not.
                if (ref_is_building_by_a0(tr.counter_target_ref)) {
                    ++tr.priority_score;
                    tr.class_flags |= 0x2u;
                    const building &counter_b = building_of(v, player_id, tr.counter_target_index);
                    if (counter_b.building_id == v.players[player_id].ai_mother_building_type) {
                        ++tr.priority_score;
                        tr.class_flags |= 0x4u;
                    }
                } else {
                    ++tr.priority_score;
                    tr.class_flags |= 0x1u;
                    // `player_id`, not `ref_owner(tr.counter_target_ref)` -- the .asm's ESI here
                    // (0x004eb876, `IMUL ESI,ESI,0x5b04`) is player_id, UNCHANGED since the
                    // prologue, verbatim -- not re-derived from the counter ref.
                    tr.counter_target_ai_group =
                        unit_of(v, player_id, tr.counter_target_index).ai_group_index;
                }
            }
        }

        if (cb.type == BLDG_TYPE_A_MOTHER || cb.type == BLDG_TYPE_H_MOTHER) {
            ++tr.priority_score;
            tr.class_flags |= 0x40u;
        }

        // Shared tail (see the header banner): if the building's already-committed incoming damage
        // has NOT yet caught up to its remaining (truncated-toward-zero) energy, return without the
        // 0x80 penalty below.
        const int32_t  truncated_energy = trunc_toward_zero_i32(b.energy);
        const uint32_t incoming         = (uint16_t)b.incoming_damage_tally; // MOVZX -- unsigned read
        if ((int32_t)incoming < truncated_energy) return;
    } else {
        // ---- UNIT target (.asm 0x004eb4b9-0x004eb6c2) --------------------------------------------
        const unit &u = unit_of(v, target_owner, tr.target_index);
        tr.tile_x     = u.x;
        tr.tile_y     = u.y;

        // Squares the FULL 32-bit callee return, NOT a 16-bit-truncated copy -- see the file banner,
        // this is where the exported .c disagrees with the .asm and the .asm wins. Only the low 16
        // bits of the 32-bit product are ever stored (`MOV word ptr`), which the final (uint16_t)
        // cast reproduces.
        const uint32_t raw_range = gc.unit_max_weapon_range(target_owner, tr.target_index);
        tr.range_sq              = (uint16_t)(raw_range * raw_range);

        // player_data[player_id].ai_tile_flags_grid[(x<<8)|y] -- read TWICE in the original (two
        // adjacent CMPs against the same address, no register cache), reproduced as one local read
        // since nothing between them can observe or mutate player_data. `>= 1 && <= 4` is the
        // unsigned `JC`/`JA` pair's exact range (not `<= 3`/`TILE_INFLUENCE_DIST_NEAR` -- that
        // constant belongs to a different function's different test).
        const uint8_t threat =
            v.players[player_id].ai_tile_flags_grid[((int32_t)u.x << 8) | u.y];
        if (threat >= 1 && threat <= 4) {
            ++tr.priority_score;
            tr.class_flags |= 0x8u;
        }

        if (u.target_index == 0 || player_id != ref_owner((uint32_t)u.target_ref)) {
            tr.counter_target_ref   = 0;
            tr.counter_target_index = 0;
        } else {
            tr.counter_target_ref   = (uint16_t)u.target_ref;
            tr.counter_target_index = (uint16_t)u.target_index;

            if (ref_is_building_by_a0(tr.counter_target_ref)) {
                // Here the a0 test is NOT sufficient by itself -- a confirming `(ref & 0x40) != 0`
                // check gates the bump (.asm 0x004eb5ff/0x004eb606). A counter-target ref whose two
                // tests DISAGREE (a0 says building, 0x40 says not -- the nibble-0 case
                // REF_OWNER_MASK's comment in ai_state.h describes) falls all the way through with
                // NO classification at all: no priority bump, no flag, nothing.
                if (ref_is_building_by_40(tr.counter_target_ref)) {
                    ++tr.priority_score;
                    tr.class_flags |= 0x2u;
                    const building &counter_b = building_of(v, player_id, tr.counter_target_index);
                    if (counter_b.building_id == v.players[player_id].ai_mother_building_type) {
                        ++tr.priority_score;
                        tr.class_flags |= 0x4u;
                    }
                }
            } else {
                ++tr.priority_score;
                tr.class_flags |= 0x1u;
                // `player_id` here too, PROVABLY equal to `ref_owner(tr.counter_target_ref)` by the
                // guard immediately above (reaching this branch already required
                // `player_id == ref_owner(u.target_ref)`) -- matches the .asm's EDI (0x004eb5e8,
                // `IMUL ESI,EDI,0x5b04`), which that same guard proves equals ESI (player_id).
                tr.counter_target_ai_group =
                    unit_of(v, player_id, tr.counter_target_index).ai_group_index;
            }
        }

        const int32_t  truncated_energy = trunc_toward_zero_i32(u.energy);
        const uint32_t incoming         = (uint16_t)u.incoming_threat_damage; // MOVZX -- unsigned read
        if ((int32_t)incoming < truncated_energy) return;
    }

    // Shared penalty tail (.asm LAB_004eb99e, reached by both arms): the target is already carrying
    // enough committed incoming damage to die on its own -- back the priority off by one and flag it
    // "already doomed" so a caller avoids piling another attacker onto it.
    --tr.priority_score;
    tr.class_flags |= 0x80u;
}

} // namespace detail

// ---- the public wrapper -------------------------------------------------------------------------
//
// Signature fixed by sig_llm_strat_ai_group_classify_target_object (mh_export.gen.h): the committed
// callee type is `(uint32_t player_id, void *target_record)`, generated off the ORIGINAL's raw
// __watcall shape. `target_record` is always really a `scan_target_entry *` -- see the header.
void group_classify_target_object(uint32_t player_id, scan_target_entry *target_record) {
    const ai_state st = state();
    detail::group_classify_target_object(st.read, live_calls(), player_id, *target_record);
}


} // namespace mh::ai
