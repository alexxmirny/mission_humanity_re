//
// sim/sim_storage_find_home_for_unit.cpp -- see sim_storage_find_home_for_unit.h. Translated from the
// DISASSEMBLY (tmp/decomp_sim/llm_strat_storage_find_home_for_unit_00463328.asm), not from the Ghidra
// .c draft.
//
#include "sim/sim_storage_find_home_for_unit.h"

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"        // ai_say / trace_budget -- the shared trace sink, not AI state
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const storage_find_home_for_unit_calls &live_storage_find_home_for_unit_calls() {
    static const storage_find_home_for_unit_calls c = {
        MH_LIBMH_BIND(llm_bldg_storage_accepts_unit_type),
        MH_LIBMH_BIND(llm_strat_storage_type_accepts_unit),
    };
    return c;
}

namespace detail {

int32_t storage_find_home_for_unit(const sim_view &v, sim_store &own,
                                   const storage_find_home_for_unit_calls &c, uint32_t player,
                                   uint32_t unit_type, int32_t probe_slot) {
    // Every read of player/unit_type in the assembly goes through MOVZX WORD -- masked once here, see
    // the header banner (same fix sim_prod_shuttle_complete.cpp already applied for the identical
    // raw-export-thunk hazard).
    const uint32_t p  = player & 0xFFFFu;
    const uint32_t ut = unit_type & 0xFFFFu;

    // 0x0046335a/0x0046335e: CMP probe_slot,0 / JNZ -- the probe branch takes priority.
    if (probe_slot != 0) {
        // ---- probe a single named slot (0x004635cf-0x004636aa) -------------------------------------
        const unit_storage &slot = storage_of(v, p, probe_slot);
        const building     &b    = building_of(v, p, slot.b_index);

        // 0x00463602-0x00463609: llm_strat_storage_type_accepts_unit(building_id, unit_type) -- takes
        // the raw ids, does its own cfg lookup internally (unlike the round-robin branch's callee).
        const int32_t accepts = c.storage_type_accepts_unit(b.building_id, static_cast<uint16_t>(ut));

        // 0x0046360e-0x00463699: all four conditions must hold (accepts, built+staffed, online,
        // occupancy<50) -- pure AND, any failure funnels to the same "return 0" tail.
        if (accepts != 0 && b.built_flags == BUILT_FLAGS_OPERATIONAL && b.online_state != 0 &&
            slot.occupancy < 0x32) {
            return probe_slot;
        }
        return 0;
    }

    // ---- round-robin search (0x00463364-0x004636b1) --------------------------------------------
    //
    // 0x0046336e: Unit[unit_type].type, a full DWORD read (matches the field's uint32_t storage --
    // see sim_unit_housing_count.cpp's identical read at the same cfg offset).
    const uint32_t type = v.cfg_units[ut].type;

    // 0x00463377-0x00463406: classify `type` into one of the four persistent per-class cursors, OR
    // return 1 immediately (heli-mother/-shuttle/-cargo), OR leave the cursor unassigned -- see the
    // header banner's derivation and its uncertainty on the two "unassigned" arms.
    int32_t cursor = 0; // DECLARED SUBSTITUTION for the original's uninitialized stack read in the two
                        // arms below that never assign -- see header banner uncertainty. 0 is a value
                        // outside the persisted cursor's own range [1,24], so it cannot be mistaken for
                        // a real prior cursor; the following `cursor += 1` wrap-clamp then starts the
                        // scan at slot 1, same as any freshly-reset persistent cursor would.
    if (type < UNIT_TYPE_A_HELI) {
        if (type == UNIT_TYPE_UNDEFINED) {
            // 0x004633a6-0x004633b0 (JC to LAB_004633b0, straight to LAB_00463406): cursor left
            // untouched in the original. See header uncertainty.
        } else if (type < UNIT_TYPE_A_WALKER) {
            cursor = own.unit_housing_at(p).rr_cursor_soldiers; // 0x004633a8-0x004633e8
        } else {
            cursor = own.unit_housing_at(p).rr_cursor_vehicles; // 0x004633b2-0x004633bf
        }
    } else if (type < UNIT_TYPE_A_PLANE) {
        cursor = own.unit_housing_at(p).rr_cursor_helis; // 0x0046337b-0x004633c4-0x004633d1
    } else if (type < UNIT_TYPE_A_HELI_MOTHER) {
        cursor = own.unit_housing_at(p).rr_cursor_planes; // 0x00463383-0x004633d6-0x004633e3
    } else if (type <= UNIT_TYPE_H_HELI_CARGO) {
        return 1; // 0x00463389-0x004633fa: unconditional early return, bypasses the scan entirely.
    } else {
        // 0x00463393 (JMP straight to LAB_00463406): cursor left untouched in the original. See
        // header uncertainty.
    }

    // 0x00463351-0x00463357: `remaining` seeds from storage_of(v,p,0).b_index -- see header
    // uncertainty on what this repurposed read actually counts.
    int32_t remaining = storage_of(v, p, 0).b_index;

    // 0x00463406-0x004636b1: the scan. scan_budget caps at one full cycle (0x19=25, 0x00463410-
    // adjacent); remaining is the independent early-out. PRESERVED AS TWO SEPARATE LITERALS from the
    // persistent-cursor wrap bound below -- see header banner "FUNCTION-SPECIFIC HAZARD", do not
    // unify.
    for (int32_t scan_budget = 0; scan_budget < 0x19 && remaining != 0; ++scan_budget) {
        // 0x00463423-0x0046342c: advance THEN wrap -- every iteration, including the first.
        cursor += 1;
        if (cursor > 0x18) cursor = 1;

        const unit_storage &slot = storage_of(v, p, cursor);
        // 0x00463446-0x0046344d: JZ past the DEC and the whole accept chain -- an empty slot costs an
        // iteration but not a `remaining` decrement.
        if (slot.b_index == 0) continue;

        --remaining; // 0x00463453

        const building &b             = building_of(v, p, slot.b_index);
        const uint8_t   building_type = v.cfg_buildings[b.building_id].type; // 0x00463490-0x0046349d

        // 0x004634a8: llm_bldg_storage_accepts_unit_type(Building[...].type, Unit[unit_type].type) --
        // looks up BOTH cfg TYPE fields itself, unlike the probe branch's callee. `type` here is the
        // SAME value already classified above (word-narrowed at the call site, 0x00463460, matching
        // the callee's uint16_t parameter -- not a second independent read).
        const int32_t accepts = c.bldg_storage_accepts_unit_type(static_cast<int16_t>(building_type),
                                                                 static_cast<uint16_t>(type));

        // 0x004634ad-0x00463534: all four conditions must hold, same AND-chain shape as the probe
        // branch.
        if (accepts != 0 && b.built_flags == BUILT_FLAGS_OPERATIONAL && b.online_state != 0 &&
            slot.occupancy < 0x32) {
            // 0x0046353b-0x004635ba: a SECOND, fresh classification of Unit[unit_type].type (not a
            // cached reuse of `type` above) decides which persistent cursor field to update. Same
            // four-way split as above, minus the "return 1" arm (unreachable here -- any type in
            // [0x13,0x18] already returned before the loop) and minus a distinct UNDEFINED case
            // (folded into "no write" below, matching 0x0046356c-0x00463572's own shape).
            const uint32_t type2 = v.cfg_units[ut].type;
            if (type2 < UNIT_TYPE_A_WALKER) {
                if (type2 != UNIT_TYPE_UNDEFINED) {
                    own.unit_housing_at(p).rr_cursor_soldiers = cursor; // 0x004635aa-0x004635b4
                }
                // type2 == UNDEFINED: no write (0x00463572).
            } else if (type2 < UNIT_TYPE_A_HELI) {
                own.unit_housing_at(p).rr_cursor_vehicles = cursor; // 0x00463574-0x0046357e
            } else if (type2 < UNIT_TYPE_A_PLANE) {
                own.unit_housing_at(p).rr_cursor_helis = cursor; // 0x00463586-0x00463590
            } else if (type2 < UNIT_TYPE_A_HELI_MOTHER) {
                own.unit_housing_at(p).rr_cursor_planes = cursor; // 0x00463598-0x004635a2
            }
            // type2 >= UNIT_TYPE_A_HELI_MOTHER: no write (0x00463566's unconditional JMP past every
            // assignment -- only reachable via the same type>0x18 uncertainty noted above, since
            // [0x13,0x18] cannot reach the loop body at all).

            return cursor; // 0x004635ba-0x004635c0
        }
        // reject (any condition failed): falls through to the loop increment, same as an empty slot.
    }

    return 0; // 0x004636aa: budget exhausted or remaining==0 with no accepted slot.
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

int32_t storage_find_home_for_unit(uint32_t player, uint32_t unit_type, int32_t probe_slot) {
    sim_state st = state();
    return detail::storage_find_home_for_unit(st.read, st.own, live_storage_find_home_for_unit_calls(),
                                              player, unit_type, probe_slot);
}


} // namespace mh::sim
