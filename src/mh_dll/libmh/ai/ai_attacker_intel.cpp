//
// ai/ai_attacker_intel.cpp -- see ai_attacker_intel.h. Translated from the DISASSEMBLY
// (tmp/decomp_ai/llm_strat_ai_bldg_register_visible_building_004db22f.asm) plus the two call sites,
// never from Ghidra's C.
//
// EVERY absolute in that listing was resolved against addr/mh_structs.gen.h rather than
// transcribed. player_data's base is 0xe6dec0 and its stride 0x288fc (the shift/add chain at
// 0x004db274-0x004db288 computes 2556 + 2556*64 = 166140 = 0x288fc), which reproduces:
//   0xe96348 -> +0x28488 ai_intel_seen_count      0xe7e380 -> +0x104c0 is_alien_race
//   0xe96368 -> +0x284a8 ai_intel_flags           0xe7df0c -> +0x1004c ai_expand_gate_value
//   0xe96388 -> +0x284c8 ai_player_relation       0xe6dee4/8 -> +0x24/+0x28 ai_home_tile_x/y
// buildings' base is 0xc3d2a0 with a 0x6aa4 row stride (the shift/add chain at
// 0x004db3d7-0x004db3f2 computes 27300) and a 0x111 record stride, so 0xc3d363/0xc3d364 are
// building::x / ::y; the cfg Building table is 0xd9ec80 stride 0x842, so 0xd9ec88 is Building::type.
// No byte offset and no literal VA appears below (Law 1).
//
// THE ONE THING THAT LOOKS LIKE A TRANSLATION BUG AND IS NOT. The original indexes the cfg
// Building[] TYPE table with `victim_index` -- IMUL EAX,EDI,0x842 at 0x004db2bc / 0x004db31a /
// 0x004db378 -- while indexing the building ROSTER with the same value at 0x004db3f5. Those are two
// different index spaces (a roster slot is 0..99 and so is a cfg type id, so the original is
// self-consistent by accident) and every other AI site in this tree reaches the cfg record as
// cfg_buildings[buildings[p][i].building_id]. It is reproduced VERBATIM: the three kind bits below
// are therefore keyed on Building[roster_slot].type, which is very likely not what the author
// meant. Do not unify the two -- an oracle divergence is the only thing that could prove which one
// the game depends on, and unifying them would silence it.
//
// THE OTHER SURPRISE: the dead compare at 0x004db465 (`CMP EDX,ESI / JZ`) can never be taken. The
// early-out at 0x004db26e already established the two nibbles differ, and the only write to ECX in
// between (0x004db25b) preserves its low nibble. Reproduced anyway -- a branch the original cannot
// take is still part of what the oracle compares, and leaving it out is a decision the evidence
// does not support.
//
#include "ai/ai_attacker_intel.h"


namespace mh::ai {
namespace detail {

register_report register_attacker_damage(const ai_view &v, const ai_store &own, const ai_calls &gc,
                                         int32_t victim_index, uint32_t victim_ref,
                                         uint32_t aggressor_unit_index, uint32_t aggressor_ref,
                                         int32_t victim_destroyed) {
    // Purged by the callee and never read by it -- see the header. Named and consumed here so the
    // signature keeps the caller's push balanced and no compiler warning tempts anyone to drop it.
    (void)victim_destroyed;

    register_report rep;

    // 0x004db249. THE AGGRESSOR'S AIRCRAFT RE-TAG. `TEST CL,0x80` reads the low byte of the
    // AGGRESSOR's ref -- so this asks "was I hit by a UNIT", not anything about the victim. On an
    // aircraft the ref is rewritten to (owner | 0x20) and the original kind bit is DISCARDED, which
    // matters only downstream: the rewritten value reaches nothing but target_list_add's fourth
    // argument, because every other consumer masks it to the owner nibble anyway.
    if ((aggressor_ref & REF_KIND_UNIT) != 0) {
        if (gc.unit_is_aircraft(aggressor_ref, (int32_t)aggressor_unit_index) != 0) {
            aggressor_ref      = (aggressor_ref & 0xfu) | REF_KIND_AIRCRAFT;
            rep.aircraft_retag = true;
        }
    }

    const uint32_t victim_owner    = victim_ref & 0xfu;    // ESI
    const uint32_t aggressor_owner = aggressor_ref & 0xfu; // EBX

    // 0x004db26e. Self-damage and friendly fire between two objects of one player record nothing at
    // all -- not even the hit counter -- so the diagonal of all three per-opponent tables stays 0.
    if (victim_owner == aggressor_owner) {
        rep.taken = register_report::path::same_owner;
        return rep;
    }

    const player_data &pd = v.players[victim_owner];
    player_data       &wp = own.players[victim_owner];

    // 0x004db290. The hit counter, and note it is OUTSIDE the "victim is a building" gate below --
    // it counts damage events of BOTH kinds. That asymmetry against ai_intel_flags is deliberate in
    // the original (the INC precedes the TEST at 0x004db296) and is easy to lose in translation.
    wp.ai_intel_seen_count[aggressor_owner] += 1;
    rep.counted = true;

    // 0x004db296. `TEST byte ptr [victim_ref],0x40` -- the whole intel-flag block runs only when the
    // damaged object is a BUILDING. For a unit victim the callers stamp 0x80 instead and everything
    // between here and 0x004db43a is skipped.
    if ((victim_ref & REF_KIND_BUILDING) != 0) {
        rep.taken = register_report::path::building_victim;

        // The ORs are byte-wide in the original (`OR byte ptr [.. + col*4 + ..],imm`) against an
        // int32 element. Every bit set fits in the low byte and OR never carries, so an int32-wide
        // OR is bit-for-bit the same store as far as this array's contents are concerned.
        auto set_flag = [&](int32_t bit) {
            wp.ai_intel_flags[aggressor_owner] |= bit;
            rep.flags_or |= bit;
        };

        set_flag(INTEL_FLAG_BUILDING_HIT); // 0x004db2a0, unconditional inside the gate

        // The three race-paired kind tests, 0x004db2a7-0x004db3a4. Each re-reads is_alien_race and
        // re-multiplies the cfg index; the repetition is the original's, and it is kept because the
        // three reads are three separate loads of a field another arm could in principle change.
        const uint8_t victim_type = v.cfg_buildings[victim_index].type;
        if (victim_type == race_mine_type(pd.is_alien_race))
            set_flag(INTEL_FLAG_MINE_HIT); // 0x004db2e8
        if (v.cfg_buildings[victim_index].type == race_turret_type(pd.is_alien_race))
            set_flag(INTEL_FLAG_TURRET_HIT); // 0x004db346
        if (v.cfg_buildings[victim_index].type == race_mother_type(pd.is_alien_race))
            set_flag(INTEL_FLAG_MOTHER_HIT); // 0x004db3a4

        // 0x004db3ac-0x004db432. "Did they hit me close to home?" The gate value is SQUARED on the
        // spot with a 32-bit IMUL, so it wraps rather than saturating; the multiply is written
        // unsigned here purely to keep that wrap defined in C++ -- the low 32 bits are identical.
        // The comparison is `CMP EAX,[EBP-0x18] / JNC`, i.e. UNSIGNED strictly-less-than.
        const uint32_t  radius    = (uint32_t)pd.ai_expand_gate_value;
        const uint32_t  radius_sq = radius * radius;
        const building &b         = building_of(v, victim_owner, victim_index);
        const uint32_t  dist_sq =
            gc.toroidal_dist_sq(pd.ai_home_tile_x, pd.ai_home_tile_y, b.x, b.y);
        if (dist_sq < radius_sq)
            set_flag(INTEL_FLAG_NEAR_HOME); // 0x004db432
    } else {
        rep.taken = register_report::path::unit_victim;
    }

    // 0x004db43a-0x004db472. THE HOSTILITY STAMP. Non-zero foreign-change flag re-stamps
    // unconditionally; with the flag clear the stamp is made only when the relation is still
    // UNSET (0), so an existing +1 (self/friendly) or an existing -1 is left alone. The
    // `aggressor_owner == victim_owner` half of the fall-through is unreachable -- see the file
    // header -- and is reproduced rather than elided.
    if (*v.foreign_bldg_change_flag != 0 ||
        (aggressor_owner != victim_owner && pd.ai_player_relation[aggressor_owner] == 0)) {
        wp.ai_player_relation[aggressor_owner] = -1;
        rep.relation_stamps                    = true;
    }

    // 0x004db47c. The sentinel test happens LAST, after the rosters have already been indexed with
    // the very value being tested -- so a -1 victim_index has by then read cfg_buildings[-1] and
    // building_of(owner, -1) on the building path. Reproduced in order; the read is what the
    // original does and the oracle compares writes, not reads.
    if (victim_index != VICTIM_INDEX_NONE) {
        // Argument order read off 0x004db481-0x004db48b, which now also matches the callee's
        // committed parameter names (finding 2026-08-03-1549-1, applied 2026-08-21): EAX = the
        // victim's owner nibble (-> player), EDX = the victim's full ref (-> victim_ref), EBX = the
        // victim's roster index (-> victim_index), ECX = the aggressor's ref as rewritten above
        // (-> aggressor_ref), and the pushed dword = the aggressor's roster index (-> aggressor_index).
        gc.target_list_add(victim_owner, (int32_t)victim_ref, victim_index, aggressor_ref,
                           (int32_t)aggressor_unit_index);
        rep.target_added = true;
    }
    return rep;
}

} // namespace detail

void register_attacker_damage(int32_t victim_index, uint32_t victim_ref,
                              uint32_t aggressor_unit_index, uint32_t aggressor_ref,
                              int32_t victim_destroyed) {
    const ai_state st = state();
    (void)detail::register_attacker_damage(st.read, st.own, live_calls(), victim_index, victim_ref,
                                           aggressor_unit_index, aggressor_ref, victim_destroyed);
}


} // namespace mh::ai
