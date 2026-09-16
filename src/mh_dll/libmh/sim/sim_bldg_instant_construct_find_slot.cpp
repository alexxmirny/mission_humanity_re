//
// sim/sim_bldg_instant_construct_find_slot.cpp -- see sim_bldg_instant_construct_find_slot.h.
// Translated from the DISASSEMBLY
// (tmp/decomp/llm_strat_bldg_instant_construct_find_slot_0046d229.asm) plus a byte read of the jump
// table at 0x0046d24f.
//
#include "sim/sim_bldg_instant_construct_find_slot.h"

#include "addr/mh_calls.gen.h"
#include "addr/mh_rebind.gen.h"
#include "ai/ai_state.h"           // ai_say / trace_budget -- the shared trace sink, not AI state
#include "sim/sim_order_enqueue.h" // the committed BUILDING_TYPE_* constants
#include "state/promoted_select.h"
#include "state/rebind_targets.gen.h"

namespace mh::sim {

namespace {

// 0x0046d4dc / 0x0046d4e1: MOV ECX,0xea / MOV EBX,0xea -- the instant-construct order code (Table A
// case 0x19). Named once so the two call arguments cannot drift apart.
inline constexpr uint16_t ORDER_INSTANT_CONSTRUCT = 0x00eau;

// 0x0046d4e9: OR AL,0x40 -- the owner-and-kind tag bit the BUILDING-side wrappers OR onto the player
// index (the unit-side wrapper uses 0x80; see sim_unit_order_disembark_soldiers.cpp). The bit's
// meaning is dispatch's business.
inline constexpr uint32_t DISPATCH_OWNER_TAG_BUILDING = 0x40u;

// The two scan bounds that are NOT the array's extent -- see the header banner. Written as this
// module's own constants, with the instruction that sets each, so a cap hike cannot silently move
// them and a reader cannot mistake them for roster capacities.
inline constexpr int32_t STORAGE_SCAN_LIMIT   = 0x10; // 0x0046d3e0  CMP dword ptr [EBP-0x10],0x10
inline constexpr int32_t BUILDINGS_SCAN_LIMIT = 0x5b; // 0x0046d476  CMP dword ptr [EBP-0x14],0x5b

// The four order-scratch field indices this function stages into, in the original's own call order
// (which is NOT ascending and NOT the parameter order -- see the header banner).
inline constexpr int32_t SCRATCH_FIELD_TILE_COL         = 4; // 0x0046d4ae  MOV EAX,0x4
inline constexpr int32_t SCRATCH_FIELD_TILE_ROW         = 5; // 0x0046d4bb  MOV EAX,0x5
inline constexpr int32_t SCRATCH_FIELD_INITIAL_WORKERS  = 1; // 0x0046d4c8  MOV EAX,0x1
inline constexpr int32_t SCRATCH_FIELD_BUILDING_TYPE_ID = 0; // 0x0046d4d5  XOR EAX,EAX

int32_t dispatch_inert(uint16_t, uint32_t, uint16_t, uint16_t) { return 0; }

} // namespace

const bldg_instant_construct_find_slot_calls &live_bldg_instant_construct_find_slot_calls() {
    static const bldg_instant_construct_find_slot_calls c = {
        MH_PROMOTED_ROW(llm_strat_order_scratch_reset),
        MH_PROMOTED_ROW(llm_strat_order_scratch_set_field),
        MH_PROMOTED_ROW(llm_strat_order_dispatch),
    };
    return c;
}

const bldg_instant_construct_find_slot_calls &shadow_bldg_instant_construct_find_slot_calls() {
    static const bldg_instant_construct_find_slot_calls c = {
        MH_PROMOTED_ROW(llm_strat_order_scratch_reset),
        MH_PROMOTED_ROW(llm_strat_order_scratch_set_field),
        dispatch_inert,
    };
    return c;
}

namespace detail {

int32_t bldg_instant_construct_find_slot(const sim_view                               &v,
                                         const bldg_instant_construct_find_slot_calls &c,
                                         int32_t tile_col, int32_t tile_row,
                                         int32_t building_type_id, int32_t initial_workers,
                                         uint32_t player) {
    // 0x0046d32f / 0x0046d489 and every sibling: MOVZX EAX,word ptr [EBP+0x8] -- EVERY roster index
    // expression in this body reads the player argument TRUNCATED TO 16 BITS. Resolved once here so
    // the truncation appears exactly where the original puts it, and so the `| 0x40` at the dispatch
    // site below can be seen NOT to use it (that site re-reads the raw dword).
    const uint32_t p = (uint32_t)(uint16_t)player;

    // 0x0046d2e7: MOV dword ptr [EBP-0xc],0x0 -- sub_slot starts at 0 and stays 0 for the default
    // arm and for any scan that finds nothing.
    int32_t sub_slot = 0;

    // 0x0046d2ee-0x0046d311: switch on `Building[building_type_id].type` via `sel = type - 1`,
    // `CMP sel,0x25 / JA default`, `JMP [sel*4 + 0x46d24f]`. The case groups below are the jump
    // table read out of the image; see the header banner for the derivation and the two independent
    // cross-checks of the mines/turrets split.
    switch (v.cfg_buildings[building_type_id].type) {
        case BUILDING_TYPE_A_PRODUCTION:
        case BUILDING_TYPE_H_PRODUCTION:
            // caseD_1 @0x0046d318: productions[p][1..8), stride 0x195, row stride 0xca8, DWORD
            // b_index compare @0x0046d342.
            for (int32_t i = 1; i < v.caps.productions; ++i) {
                if (v.productions[p * v.caps.productions + i].b_index == 0) {
                    sub_slot = i;
                    break;
                }
            }
            break;

        case BUILDING_TYPE_A_MINE:
        case BUILDING_TYPE_H_MINE:
            // caseD_2 @0x0046d39a: mines[p][1..32), stride 0x38, row stride 0x700, DWORD b_index
            // compare @0x0046d3c1 (base 0x00d03480).
            for (int32_t i = 1; i < v.caps.mines; ++i) {
                if (v.mines[p * v.caps.mines + i].b_index == 0) {
                    sub_slot = i;
                    break;
                }
            }
            break;

        case BUILDING_TYPE_A_TURRET:
        case BUILDING_TYPE_H_TURRET:
            // caseD_5 @0x0046d35a: turrets[p][1..32), stride 0x37, row stride 0x6e0, and a WORD
            // compare @0x0046d381 (base 0x00cc0fe0) -- map_object_turret::b_index is genuinely
            // int16_t where every sibling's is int32_t.
            for (int32_t i = 1; i < v.caps.turrets; ++i) {
                if (v.turrets[p * v.caps.turrets + i].b_index == 0) {
                    sub_slot = i;
                    break;
                }
            }
            break;

        case BUILDING_TYPE_A_BARRAKS:
        case BUILDING_TYPE_A_GARAGE:
        case BUILDING_TYPE_A_AIRFIELD:
        case BUILDING_TYPE_A_HELIPAD:
        case BUILDING_TYPE_A_PORT:
        case BUILDING_TYPE_A_SHUTTLE:
        case BUILDING_TYPE_H_BARRACKS:
        case BUILDING_TYPE_H_GARAGE:
        case BUILDING_TYPE_H_AIRFIELD:
        case BUILDING_TYPE_H_HELIPAD:
        case BUILDING_TYPE_H_PORT:
        case BUILDING_TYPE_H_SHUTTLE:
            // caseD_7 @0x0046d3d9: unit_storage[p][1..16) -- the bound is a HARDCODED 16 while the
            // array holds 25 (header banner). The INDEX arithmetic still uses the live row stride.
            for (int32_t i = 1; i < STORAGE_SCAN_LIMIT; ++i) {
                if (v.storage[p * v.caps.storage + i].b_index == 0) {
                    sub_slot = i;
                    break;
                }
            }
            break;

        case BUILDING_TYPE_A_LAB:
        case BUILDING_TYPE_H_LAB:
            // caseD_b @0x0046d418: labs[p][1..25), stride 8 (SHL EDX,3), row stride 0xc8.
            for (int32_t i = 1; i < v.caps.labs; ++i) {
                if (v.labs[p * v.caps.labs + i].b_index == 0) {
                    sub_slot = i;
                    break;
                }
            }
            break;

        case BUILDING_TYPE_A_PLANT:
        case BUILDING_TYPE_A_COLONY:
        case BUILDING_TYPE_A_MOTHER:
        case BUILDING_TYPE_A_MAIN_BASE:
        case BUILDING_TYPE_A_RELAY:
        case BUILDING_TYPE_A_SILOS:
        case BUILDING_TYPE_A_CIVIL:
        case BUILDING_TYPE_H_PLANT:
        case BUILDING_TYPE_H_COLONY:
        case BUILDING_TYPE_H_MOTHER:
        case BUILDING_TYPE_H_MAIN_BASE:
        case BUILDING_TYPE_H_RELAY:
        case BUILDING_TYPE_H_SILOS:
        case BUILDING_TYPE_H_CIVIL:
            // caseD_3 @0x0046d456: no scan -- these types have exactly one sub-object slot.
            sub_slot = 1;
            break;

        default:
            // caseD_11 @0x0046d45d, reached by the `JA` bound check (type 0 wraps to 0xff on the
            // DEC AL) and by the table's own three default entries: A_BIURO(17), H_BYURO(37) and the
            // unused ids 19/20. sub_slot stays 0.
            break;
    }

    // 0x0046d45d-0x0046d46a: no sub-object slot -> return 0 without touching the order scratch at
    // all. The scratch is NOT reset on this path, which matters: a caller that staged something
    // earlier still sees it.
    if (sub_slot == 0) return 0;

    // 0x0046d46f-0x0046d47a: the buildings-roster scan, bound a HARDCODED 91 against an array of 100
    // (header banner).
    for (int32_t roster_slot = 1; roster_slot < BUILDINGS_SCAN_LIMIT; ++roster_slot) {
        // 0x0046d489-0x0046d4a4: buildings[p][roster_slot].building_id == 0 -- a WORD compare at
        // base 0x00c3d2a2, i.e. `buildings` (0x00c3d2a0) + 2, which mh_structs.gen.h independently
        // puts at map_object_building::building_id.
        if (building_of(v, p, roster_slot).building_id != 0) continue;

        // 0x0046d4a6-0x0046d4d7: THE REAL OUTPUT. Reset, then four indexed stores in the original's
        // own order. The index->value pairing is the load-bearing part; see the header banner.
        c.order_scratch_reset();
        c.order_scratch_set_field(SCRATCH_FIELD_TILE_COL, tile_col);
        c.order_scratch_set_field(SCRATCH_FIELD_TILE_ROW, tile_row);
        c.order_scratch_set_field(SCRATCH_FIELD_INITIAL_WORKERS, initial_workers);
        c.order_scratch_set_field(SCRATCH_FIELD_BUILDING_TYPE_ID, building_type_id);

        // 0x0046d4dc-0x0046d4f2: EAX = (uint16_t)roster_slot, EDX = (uint16_t)(player | 0x40),
        // EBX = ECX = 0xea. Note the `| 0x40` is applied to the RAW dword argument (MOV EAX,[EBP+8])
        // and then truncated, not to the already-truncated `p` -- same number either way (the OR
        // only touches bit 6), but the expression follows the instructions. Return discarded.
        (void)c.order_dispatch((uint16_t)roster_slot,
                               (uint32_t)(uint16_t)(player | DISPATCH_OWNER_TAG_BUILDING),
                               ORDER_INSTANT_CONSTRUCT, ORDER_INSTANT_CONSTRUCT);

        // 0x0046d4f7-0x0046d4fd: return the roster slot that was dispatched at.
        return roster_slot;
    }

    // 0x0046d501: the roster scan exhausted -- return 0, with the scratch untouched.
    return 0;
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

int32_t bldg_instant_construct_find_slot(int32_t tile_col, int32_t tile_row, int32_t building_type_id,
                                         int32_t initial_workers, uint32_t player) {
    const sim_view v = state().read;
    return detail::bldg_instant_construct_find_slot(v, live_bldg_instant_construct_find_slot_calls(),
                                                    tile_col, tile_row, building_type_id,
                                                    initial_workers, player);
}


} // namespace mh::sim
