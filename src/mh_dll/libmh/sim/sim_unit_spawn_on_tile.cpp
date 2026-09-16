//
// sim/sim_unit_spawn_on_tile.cpp -- see sim_unit_spawn_on_tile.h. Translated from the DISASSEMBLY
// (tmp/decomp/llm_strat_unit_spawn_on_tile_00464119.asm), not from Ghidra's C: the draft's overall
// shape (occupied-tile early-out -> slot search -> per-slot record init -> tile occupancy ->
// housing bookkeeping -> notify) is structurally faithful, but the exact slot-search bound, the
// header-row increment's placement relative to init_record, and every field's storage width were
// re-walked against the raw CMP/JZ/JNZ/JL targets and MOV/FILD/FSTP opcode widths per the
// translator brief, not trusted from the draft's `[x][y]` indexing sugar or its `local_1c` typing.
//
// ---- DECLARED NEED: see the header for the missing sim_store::unit_housing_at() accessor. This TU
// will not compile until it lands -- by design, matching sim_unit_create_soldier.cpp's precedent.
//
#include "sim/sim_unit_spawn_on_tile.h"

#include "addr/mh_calls.gen.h"     // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"           // ai_say / trace_budget -- the shared trace sink, not AI state
#include "sim/sim_order_enqueue.h" // UNIT_STATE_STOP_TO_DEFAULT -- see the anon-namespace note below
#include "addr/mh_rebind.gen.h"    // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const unit_spawn_on_tile_calls &live_unit_spawn_on_tile_calls() {
    static const unit_spawn_on_tile_calls c = {
        MH_LIBMH_BIND(llm_strat_unit_init_record),
        MH_LIBMH_BIND(map_fow_UpdateFoWPlus),
        MH_LIBMH_BIND(llm_strat_ai_notify_unit_lifecycle),
    };
    return c;
}

namespace {

// units[player][0].order per-player HEADER-ROW increment (0x0046419f, `INC word ptr`, width WORD).
// Third independent occurrence of this exact idiom in the closure (sim_unit_create_soldier.cpp's
// kUnitZeroOrderHeaderIncrement, llm_unit_recruit.h's own hazard note, now this one) -- named
// LOCALLY rather than shared or folded into UNIT_STATE_STOP_TO_DEFAULT, matching both siblings'
// caution that the numeric coincidence (both are 1) is not proof of shared enum semantics.
inline constexpr uint16_t kUnitZeroOrderHeaderIncrement = 1;

// The ASM's own IMMEDIATE slot-search bound (0x00464164: `CMP dword ptr [...], 0x5b`) -- 0x5b = 91,
// deliberately NOT sim_state.h's UNITS_PER_PLAYER (100). See the header hazard note: this is a real
// disagreement in the original, reproduced verbatim, not a named-constant substitution.
inline constexpr int32_t SPAWN_ON_TILE_SLOT_SEARCH_BOUND = 0x5b;

} // namespace

namespace detail {

int32_t unit_spawn_on_tile(const sim_view &v, sim_store &own, const unit_spawn_on_tile_calls &c,
                           uint32_t x, uint32_t y, uint16_t unit_proto_id, uint16_t player) {
    // 0x0046413a-0x0046414f: early-out -- tile already occupied (class_owner != 0). x is the OUTER
    // index (tile_x), y the inner (tile_y) -- see the header's indexing hazard note.
    if (tile_at(v, (int32_t)x, (int32_t)y).class_owner != 0) return 0;

    // 0x0046415d-0x0046416a / LAB_0046433b (0x0046433b): slot search over i in [1, 0x5b). Exhausting
    // the search falls to the SAME `return 0;` as the occupied-tile early-out above -- both paths
    // are indistinguishable to the caller, matching the original.
    for (int32_t slot = 1; slot < SPAWN_ON_TILE_SLOT_SEARCH_BOUND; ++slot) {
        if (unit_of(v, (uint32_t)player, slot).unit_proto_id != 0) continue; // occupied, keep looking

        // 0x0046419f: the units[player][0].order header-row increment, BEFORE init_record -- see the
        // anon-namespace comment. Note the target is units[player][0], NOT units[player][slot].
        unit &roster_header = own.unit_at((uint32_t)player, 0);
        roster_header.order = (uint16_t)(roster_header.order + kUnitZeroOrderHeaderIncrement);

        // 0x004641b1: llm_strat_unit_init_record (ORIGINAL, mh::call) is what actually stamps
        // units[player][slot].unit_proto_id -- everything below that reads `u.unit_proto_id` off the
        // record (rather than the `unit_proto_id` PARAMETER) does so because the original re-reads
        // memory too (0x004642e0 and 0x0046431e are both fresh loads, not register reuse).
        c.unit_init_record((int32_t)slot, (uint32_t)unit_proto_id, (uint32_t)player);

        unit &u = own.unit_at((uint32_t)player, (uint32_t)slot);

        // 0x004641c9-0x004641e5: order/state=STOP_TO_DEFAULT (word stores), the new unit's OWN
        // order/state -- distinct from the header-row increment above (a different field meaning,
        // per the header note).
        u.order = UNIT_STATE_STOP_TO_DEFAULT;
        u.state = UNIT_STATE_STOP_TO_DEFAULT;

        // 0x00464201-0x00464220: x/y truncated to byte, matching unit::x/y's uint8_t storage.
        u.x = (uint8_t)x;
        u.y = (uint8_t)y;

        // 0x00464242-0x00464278: passable[x][y] is read TWICE in the asm (origin_tile_was_passable,
        // then again for the FILD/FSTP double conversion), both before the 0x004642b9 clear and with
        // no intervening write -- so reusing the just-stored field for the second is behaviorally
        // identical to a second independent read, matching sim_unit_create_soldier.cpp's own
        // precedent for the SAME idiom. The FILD is a 16-bit (word) load of a zero-extended byte
        // (opcode `df45dc` = FILD m16int); a plain `(double)` cast reproduces its numeric result
        // exactly (int-to-double of a 0-255 value is exact regardless of source width -- see the
        // header hazard note).
        u.origin_tile_was_passable = v.passable[(x << 8) | y];
        u.move_step_speed_scale    = (double)u.origin_tile_was_passable;

        // 0x0046428f-0x004642aa: claim the tile -- building=slot (word), class_owner=player|0x80
        // (byte; player truncated to its low byte BEFORE the OR, matching the asm's DL-register
        // order). BOTH writes land here, before passable is cleared below.
        own.tile_object_at((int32_t)x, (int32_t)y).building    = (uint16_t)slot;
        own.tile_object_at((int32_t)x, (int32_t)y).class_owner = (uint8_t)((uint8_t)player | 0x80u);

        // 0x004642b9: passable cleared AFTER the tile_objects claim above -- exact original order.
        own.passable_at((int32_t)x, (int32_t)y) = 0;

        // 0x004642c0-0x004642c7: housing_stats.used_soldiers += 1 (dword INC) -- DECLARED NEED, see
        // the header.
        own.unit_housing_at((uint32_t)player).used_soldiers += 1;

        // 0x004642cd-0x00464329: BOTH outward calls re-read units[player][slot].unit_proto_id off
        // the record (u.unit_proto_id, the value init_record just wrote) rather than the
        // `unit_proto_id` parameter -- matching sim_unit_create_soldier.cpp's identical note. Order:
        // FoW update runs BEFORE the AI lifecycle notify, both reading state this same call already
        // wrote (the tile claim above, and the record init_record wrote).
        c.fow_update_plus(player, x, y, (uint8_t)v.cfg_units[u.unit_proto_id].sight);
        c.ai_notify_unit_lifecycle(player, u.unit_proto_id, (uint32_t)slot, 4u);

        return (int32_t)slot;
    }

    return 0;
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

int32_t unit_spawn_on_tile(uint32_t x, uint32_t y, uint16_t unit_proto_id, uint16_t player) {
    sim_state st = state();
    return detail::unit_spawn_on_tile(st.read, st.own, live_unit_spawn_on_tile_calls(), x, y,
                                      unit_proto_id, player);
}


} // namespace mh::sim
