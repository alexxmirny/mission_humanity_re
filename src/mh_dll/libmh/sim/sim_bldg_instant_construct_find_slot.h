//
// sim/sim_bldg_instant_construct_find_slot.h -- llm_strat_bldg_instant_construct_find_slot
// @0x0046d229 (RI-SIM, X-TL-DRAIN step 4).
//
// `int __mh_watcall_ecx_ebx_volatile llm_strat_bldg_instant_construct_find_slot(
//      tile_coord tile_col, tile_coord tile_row, cfg_t_building_index building_type_id,
//      int initial_workers, game_t_Player player)`
// -- tile_col=EAX, tile_row=EDX, building_type_id=EBX, initial_workers=ECX, player=Stack[0x4],
// `RET 0x4` (callee pops the stack argument). The signature was analyzed and committed FIRST, at EN
// v404, before a line of this file existed (X-TL-DRAIN step 1); mh_calls.gen.h /
// sig_llm_strat_bldg_instant_construct_find_slot are the committed form this module matches.
//
// Translated from the DISASSEMBLY (tmp/decomp/llm_strat_bldg_instant_construct_find_slot_0046d229.asm)
// plus a byte read of the jump table at 0x0046d24f. The Ghidra .c draft was used as a map only.
//
// ---- WHAT IT IS, AND THE TRAP IN READING IT ------------------------------------------------------
//
// The build-placement click-confirm path's slot finder: pick a free TYPE-SPECIFIC sub-object slot
// (production / mine / turret / storage-dock / lab, chosen by the cfg building type), then a free
// overall buildings-roster slot, stage five values into the order scratch, and dispatch order 0xea
// (the instant-construct case) at that roster slot. Sole original caller: llm_strat_input_update.
//
// **ITS REAL OUTPUT IS THE ORDER-SCRATCH STAGING, NOT THE RETURN VALUE.** The committed plate at
// 0x0046d229 says so explicitly and it is the one thing a plausible-looking reimplementation gets
// wrong: a body that computes the same roster index and returns it, without reproducing the
// `scratch_reset` + four `scratch_set_field` calls IN THE ORIGINAL'S FIELD ORDER, is wrong -- the
// order 0xea handler reads those four scratch fields, not the return value, and the return value is
// consumed only as a "did anything happen" flag. The four indices and their sources, straight off the
// call sites, are:
//
//   0x0046d4ab  set_field(4, [EBP-0x28])  = tile_col
//   0x0046d4b8  set_field(5, [EBP-0x24])  = tile_row
//   0x0046d4c5  set_field(1, [EBP-0x1c])  = initial_workers
//   0x0046d4d2  set_field(0, [EBP-0x20])  = building_type_id     (XOR EAX,EAX -- index 0)
//
// Note the field indices are NOT in ascending order and do NOT match the parameter order. That is the
// original's own sequence and it is reproduced literally; `scratch_set_field` is an indexed store, so
// the ORDER of these four is not observable in the final scratch contents -- but the INDEX->VALUE
// pairing absolutely is, and a transposition (tile_col into 5, tile_row into 4) would place the
// building at the mirrored tile. The offline oracle asserts the pairing with four distinct,
// non-symmetric seeds for exactly that reason.
//
// **`initial_workers` CARRIES TWO SENTINELS**, resolved three hops away in map_CreateBuilding and
// recorded in the committed plate: -1 means "use the cfg `builder_count` default", -2 means "staff
// from the free human population, capped at that default". This function does NOT interpret them --
// it stages the value through scratch field 1 untouched, and that pass-through is the correct
// behaviour. It is documented here so a later reader does not "helpfully" clamp or validate it.
//
// ---- THE TYPE SWITCH: DERIVED FROM THE JUMP TABLE, NOT FROM THE DECOMPILE ------------------------
//
// 0x0046d2ee-0x0046d311:  `sel = Building[building_type_id].type - 1` (DEC AL, so a BYTE subtract);
// `CMP sel,0x25 / JA default` bounds it to 0..0x25; `JMP dword ptr CS:[sel*4 + 0x46d24f]`. The table
// is 38 dwords, read out of the image directly (ReVa read-memory @0x0046d24f, 152 bytes) and mapped
// through cfg_enum_E_BUILDING's own member values. Reading it settled the one ambiguity the .c draft
// could not: two arms have IDENTICAL bounds (32) and only their base addresses tell them apart --
//     0x46d39a  base 0x00d03480, stride 0x38, DWORD compare  ->  `mines`   (map_object_mine[8][32])
//     0x46d35a  base 0x00cc0fe0, stride 0x37, WORD  compare  ->  `turrets` (map_object_turret[8][32])
// -- and the mapping was then cross-checked a second, independent way: ai_state.h's already-committed
// BLDG_TYPE_A_MINE(2)/A_TURRET(5) put table index 1 on the mines arm and index 4 on the turrets arm,
// which is what the bases say. `map_object_turret::b_index` being genuinely int16_t while every
// sibling's is int32_t is recorded in mh_structs.gen.h's own field comment; the WORD compare here is
// the second witness for it.
//
// Three groups have no scan at all:
//   * SINGLE (sub_slot = 1 unconditionally, 0x0046d456): PLANT, COLONY, MOTHER, MAIN_BASE, RELAY,
//     SILOS, CIVIL -- both races, 14 members.
//   * DEFAULT (no slot, function returns 0): type 0 (UNDEFINED), A_BIURO(17), H_BYURO(37), the two
//     unused ids 19/20, and everything above 38. The two "biuro"/office types falling through to the
//     default is REAL and deliberate in the original, not a gap in this transcription.
//
// ---- THE TWO SCAN BOUNDS THAT ARE NOT THE ARRAY'S EXTENT ------------------------------------------
//
// Four of the six scans run to the array's full capacity and are written against the roster caps, the
// same convention sim_bldg_construct_finalize.cpp uses for the identical scans:
//     productions  < 8    == caps.productions
//     turrets      < 0x20 == caps.turrets
//     mines        < 0x20 == caps.mines
//     labs         < 0x19 == caps.labs
// TWO DO NOT, and they are written as literals with the divergence stated, because writing them as
// caps would silently change behaviour under a cap hike:
//     unit_storage < 0x10 (16)  while map_object_unit_storage[8][25] has 25 -- slots 16..24 are never
//                               offered by this path. sim_bldg_construct_finalize.cpp's own comment
//                               already names this ("unlike the `_enqueue` sibling's hardcoded 16"),
//                               and the sibling it names is this function's dead twin.
//     buildings    < 0x5b (91)  while map_object_building[8][100] has 100 -- slots 91..99 are never
//                               offered by this path either.
// This is the hardcoded-second-bound class the retro-binary-patching notes warn about: a cap hike
// that enlarges the arrays without touching these two literals leaves this path scanning the old
// range. Reproducing the literal is what Law 2 asks for; changing it is a game-behaviour decision,
// not a translation one.
//
// ---- THE SHADOW ARM ------------------------------------------------------------------------------
//
// ARMABLE, with `llm_strat_order_dispatch` INERT and `_G_LLM_STRAT_ORDER_SCRATCH_ARGS` declared.
// The reasoning, both halves:
//   * dispatch must be inert. It routes into llm_strat_order_stage_scheduled ->
//     llm_strat_order_integrity_check, whose SESSION_MODE 3 failure path forces a return to the main
//     menu (the closure walk reaches llm_menu_force_return_to_main at depth 4). Running it a second
//     time would end the match exactly when our body is wrong -- libmh/orders/order_queue.cpp's own
//     the order container's dispatch records the same refusal for the same callee.
//   * scratch_reset / scratch_set_field RUN FOR REAL. Their entire write set is
//     _G_LLM_STRAT_ORDER_SCRATCH_ARGS (shadow_region_closure, depth 1), an ordinary data region the
//     manifest declares, so the restore between arms undoes them. Stubbing them would throw away the
//     only meaningful comparison this site has.
// So the site compares the RETURN VALUE and the ORDER SCRATCH -- which is to say, it compares exactly
// the two things this function produces, including the staging the plate warns about. Verified it is
// not vacuous: nonzero declared regions AND a non-void return.
//
#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

struct bldg_instant_construct_find_slot_calls {
    void (*order_scratch_reset)();                     // llm_strat_order_scratch_reset @0x00466812
    void (*order_scratch_set_field)(int32_t, int32_t); // llm_strat_order_scratch_set_field @0x0046685d
    // llm_strat_order_dispatch @0x00465fdf. Return DISCARDED at this call site.
    int32_t (*order_dispatch)(uint16_t unit_id, uint32_t player, uint16_t op_code, uint16_t arg);
};

const bldg_instant_construct_find_slot_calls &live_bldg_instant_construct_find_slot_calls();
// The shadow arm's table: the two scratch calls REAL, dispatch a no-op. See the banner.
const bldg_instant_construct_find_slot_calls &shadow_bldg_instant_construct_find_slot_calls();

namespace detail {

// llm_strat_bldg_instant_construct_find_slot @0x0046d229. Returns the buildings-roster slot it
// dispatched at (>=1), or 0 if either scan found nothing.
int32_t bldg_instant_construct_find_slot(const sim_view                               &v,
                                         const bldg_instant_construct_find_slot_calls &c,
                                         int32_t tile_col, int32_t tile_row,
                                         int32_t building_type_id, int32_t initial_workers,
                                         uint32_t player);


} // namespace detail

// Live wrapper: matches sig_llm_strat_bldg_instant_construct_find_slot exactly.
int32_t bldg_instant_construct_find_slot(int32_t tile_col, int32_t tile_row, int32_t building_type_id,
                                         int32_t initial_workers, uint32_t player);

} // namespace mh::sim
