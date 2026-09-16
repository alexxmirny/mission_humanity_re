//
// sim/sim_target_release_ref.cpp -- see sim_target_release_ref.h. Translated from the DISASSEMBLY
// (tmp/decomp/llm_strat_target_release_ref_004dac44.asm); the Ghidra decompile's raw address
// arithmetic in both the mode-0 building-target arm and all of mode-1 is NOT reproduced -- every one
// of those addresses is `unit_at(...)`/`building_at(...)` plus a typed field, verified field-by-field
// against addr/mh_structs.gen.h's own static_assert'd offsets (target_index @0x8a, target_ref @0x8c,
// incoming_threat_damage @0xdc, committed_weapon_damage_est @0xde, engagement_flags @0xe2,
// order_status_flags @0xe3; building incoming_damage_tally's byte offset 0x10f combined with
// buildings_base 0xc3d2a0 and sizeof(building)==0x111 reproduces the .c draft's literal
// `0xc3d3af`/`0x6aa4` constants exactly).
//
#include "sim/sim_target_release_ref.h"

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const target_release_ref_calls &live_target_release_ref_calls() {
    static const target_release_ref_calls c = {
        MH_LIBMH_BIND(llm_strat_unit_estimate_weapon_damage),
    };
    return c;
}

namespace detail {

void target_release_ref(const sim_view &v, sim_store &own, const target_release_ref_calls &c,
                        uint32_t player_idx, int32_t unit_idx, uint32_t mode) {
    (void)v; // every read this function makes is through the ACTING unit, which is mutable state --
             // there is no read-only member it needs from sim_view.

    // 0x004dac5f: AND EDI,0xf -- the acting unit's owning player, masked once and reused by both
    // arms below (both the roster index for `unit_at(owner, unit_idx)` and, in mode 0, the `player`
    // argument to llm_strat_unit_estimate_weapon_damage).
    uint32_t owner = player_idx & 0xf;

    // 0x004dac5c / 0x004dac62-0x004dac6b: AND EBX,0xf; CMP EBX,3; JA -> epilogue; JMP [table]. Modes
    // 2 and 3 (and, since the guard is `> 3` not `!= 2/3`, every value 4..15 too) land on the SAME
    // caseD_2 epilogue block as the fall-through after case 0/1 finish -- i.e. a plain no-op. Modeled
    // as `default:` rather than transcribing the jump table, since every non-0/1 arm is identical.
    switch (mode & 0xf) {
        case 0: {
            // caseD_0, 0x004dac73-0x004dad02.
            unit &u = own.unit_at(owner, unit_idx);

            // 0x004dac81-0x004dac88: TEST engagement_flags,1; JNZ -> epilogue. Already committed --
            // nothing to do (the original does not even touch order_status_flags on this path).
            if ((u.engagement_flags & 0x1u) != 0) {
                break;
            }

            // 0x004dac8e: OR order_status_flags,0x80.
            u.order_status_flags |= 0x80u;

            // 0x004dac95-0x004daca5: load target_ref/target_index BEFORE the call (their values are held
            // live across it in the original, in ECX/EBX), then call
            // llm_strat_unit_estimate_weapon_damage(owner, unit_idx, target_ref, target_index) -- the
            // committed prototype's (player, unit_index, target_ref, target_index) order, confirmed
            // against sim_weapon_damage_calc.h's own derivation of that same callee's parameter naming.
            int32_t  target_ref   = u.target_ref;
            int32_t  target_index = u.target_index;
            uint32_t est          = c.estimate_weapon_damage(static_cast<int32_t>(owner), unit_idx,
                                                             static_cast<uint32_t>(static_cast<uint16_t>(target_ref)),
                                                             static_cast<int32_t>(static_cast<uint16_t>(target_index)));
            // 0x004dacaa: MOV word ptr [...+0xde],AX -- truncating 16-bit store.
            u.committed_weapon_damage_est = static_cast<int16_t>(est);

            // 0x004dacb1-0x004dacb8: TEST target_ref-byte,0x40; JZ -> unit-target arm (LAB_004dad07).
            // Fall-through (bit set) is the building-target arm.
            if ((target_ref & 0x40) == 0) {
                // LAB_004dad07, 0x004dad07-0x004dad38: target owner = target_ref & 0xf (the .c draft's
                // "& 0xff & 0xffffff0f" is a decompiler artifact of a byte-in-register op -- verified
                // against the assembly's `AND AL,0xf` on a zero-extended byte, i.e. plain `& 0xf`).
                uint32_t target_owner = static_cast<uint32_t>(static_cast<uint16_t>(target_ref)) & 0xfu;
                own.unit_at(target_owner, static_cast<int32_t>(static_cast<uint16_t>(target_index)))
                    .incoming_threat_damage += u.committed_weapon_damage_est;
            } else {
                // 0x004dacba-0x004daccf, 0x004dacfa: the building-target arm's raw
                // `owner*0x6aa4 + 0xc3d3af + target_index*0x111` is
                // `&building_at(owner, target_index).incoming_damage_tally` -- 0x111==sizeof(building),
                // 0x6aa4==BUILDINGS_PER_PLAYER*sizeof(building), 0xc3d3af==buildings_base+
                // offsetof(building,incoming_damage_tally).
                uint32_t target_owner = static_cast<uint32_t>(static_cast<uint16_t>(target_ref)) & 0xfu;
                own.building_at(target_owner, static_cast<int32_t>(static_cast<uint16_t>(target_index)))
                    .incoming_damage_tally += u.committed_weapon_damage_est;
            }
            break;
        }
        case 1: {
            // caseD_1, 0x004dad3d-0x004dade4.
            unit &u = own.unit_at(owner, unit_idx);

            // 0x004dad4e: AND word ptr [...+0xe2],0x7ffe -- a compiler-merged 16-bit store of TWO
            // adjacent byte fields (engagement_flags @0xe2 low byte, order_status_flags @0xe3 high
            // byte). Split into the two typed field writes it actually performs; order between them is
            // not observable (independent byte fields).
            u.engagement_flags &= 0xfeu;   // clears bit 0x1
            u.order_status_flags &= 0x7fu; // clears bit 0x80

            int32_t target_ref   = u.target_ref;
            int32_t target_index = u.target_index;
            int16_t committed    = u.committed_weapon_damage_est;

            // 0x004dad60-0x004dad67: TEST target_ref-byte,0x40; JZ -> unit-target arm (LAB_004dadb3).
            // Fall-through (bit set) is the building-target arm. Same two formulas as mode 0's case,
            // SUBTRACTING instead of adding (this mode releases the earlier commit).
            if ((target_ref & 0x40) == 0) {
                uint32_t target_owner = static_cast<uint32_t>(static_cast<uint16_t>(target_ref)) & 0xfu;
                own.unit_at(target_owner, static_cast<int32_t>(static_cast<uint16_t>(target_index)))
                    .incoming_threat_damage -= committed;
            } else {
                uint32_t target_owner = static_cast<uint32_t>(static_cast<uint16_t>(target_ref)) & 0xfu;
                own.building_at(target_owner, static_cast<int32_t>(static_cast<uint16_t>(target_index)))
                    .incoming_damage_tally -= committed;
            }

            // 0x004dadf1: MOV word ptr [...+0xde],0 -- clears the acting unit's OWN cached estimate
            // last, after it has been read and applied above.
            u.committed_weapon_damage_est = 0;
            break;
        }
        default:
            break; // modes 2/3/other: no-op, see the header comment.
    }
}

} // namespace detail

void target_release_ref(uint32_t player_idx, int32_t unit_idx, uint32_t mode) {
    sim_state st = state();
    detail::target_release_ref(st.read, st.own, live_target_release_ref_calls(), player_idx, unit_idx,
                               mode);
}

} // namespace mh::sim
