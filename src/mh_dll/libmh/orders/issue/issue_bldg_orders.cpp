//
// orders/issue/issue_bldg_orders.cpp -- the building order wrappers (RI-ORDERS / O4A, pilot slice).
//
// Six wrappers, translated as the PILOT for O4-0: they are what makes `net_selftest issuetest`
// non-vacuous, and they were chosen to span the shapes the other 106 come in --
//
//   activate                 constant triple, no guard, no scratch, replicated lane
//   deactivate_enqueue       the IMMEDIATE lane (llm_strat_order_enqueue), 16-bit param storage
//   production_add           guarded on buildings[].online_state, TWO scratch slots
//   production_remove        a DIFFERENT guard (state != 100) and ONE scratch slot -- see below
//   load_resource            two scratch slots written out of index order (3 then 2)
//   repair_cycle_start       two guards, a double compare, and a read of the cfg TYPE table
//
// DERIVED FROM THE .asm, not the .c. Addresses in the comments are EN /eng/mh.exe.
//
// THE ARGUMENT SHAPE IS THE SAME AT ALL SIX SITES and is worth stating once here rather than six
// times below. The original loads, in this order:
//     ECX = order_code   EBX = param0   EAX = player; OR AL,0x40; MOVZX EDX,AX   EAX = MOVZX bldg
// so the call is dispatch(unit_id = (uint16_t)bldg_idx, player = (uint16_t)(player | KIND_BLDG),
// op_code = param0, arg = order_code). The `OR` is an 8-BIT op on AL, but 0x40 fits in a byte, so
// over the low 16 bits it is exactly `player | 0x40`. param0 and order_code are the SAME constant at
// every one of these six sites; that is a property of this family, NOT a rule -- the domain has
// sites where they differ, which is why the oracle compares them separately.
//
// PLAYER IS NARROWED TO 16 BITS BEFORE THE INDEX MULTIPLY (`MOVZX EAX,word ptr [EBP-n]` then
// `IMUL EAX,EAX,0x6aa4`) but the OWNER byte is built from the FULL dword (`MOV EAX,dword ptr
// [EBP-n]` then `OR AL,0x40`). Two different widths of the same parameter in one function. Preserved
// as written; a "tidier" translation that narrowed once at the top would diverge for player > 0xffff.
//
#include "orders/issue/issue_state.h"


namespace mh::orders::issue {

namespace {

// buildings[player][index], with the original's own arithmetic: the player half narrowed to 16 bits,
// the index half not. `0x6aa4` == BUILDINGS_PER_PLAYER * sizeof(building).
mh::game::mh_map_object_building &bldg_at(const issue_view &v, uint32_t player, int32_t index) {
    return v.buildings[(int32_t)(uint16_t)player * v.caps.buildings + index];
}

// The owner byte the container receives: the kind nibble OR'd into the low byte of `player`.
uint16_t owner_of(uint32_t player, uint32_t kind) { return (uint16_t)(player | kind); }

} // namespace

// ---- llm_strat_bldg_order_activate @0x0046e0f2 -------------------------------------------------
// Unconditional: no guard, no scratch. Order 0x80/0x80.
void detail::bldg_order_activate(const issue_view &, const order_sink &s, uint32_t player,
                                 uint16_t bldg_idx) {
    s.dispatch(bldg_idx, owner_of(player, KIND_BLDG), 0x80, 0x80);
}

// ---- llm_strat_bldg_order_deactivate_enqueue @0x0046e1b5 ---------------------------------------
// The `_enqueue` twin: it calls llm_strat_order_enqueue @0x00466094 DIRECTLY, bypassing the
// replicated-lane router, so the order never reaches the wire. Order 0x81/0x81.
// Its first parameter's committed storage is AX:2, so it is already 16-bit on entry -- but the body
// still does `OR AL,0x40` on it, which is the same expression as everywhere else in this file.
void detail::bldg_order_deactivate_enqueue(const issue_view &, const order_sink &s,
                                           uint32_t player, uint16_t bldg_idx) {
    s.enqueue(bldg_idx, owner_of(player, KIND_BLDG), 0x81, 0x81);
}

// ---- llm_strat_bldg_order_production_add @0x0046dae0 --------------------------------------------
// GUARD: `CMP word ptr [.. + 0xc3d2b7],0x0` / `JZ` -- buildings[..].online_state (+0x17) must be
// NON-ZERO or the function emits nothing at all. The scratch pair is args[0] = unit_type and
// args[1] = 1, the 1 being a literal `MOV EDX,0x1` -- the `_count` sibling @0x0046dbd0 is this
// function with that constant lifted to a parameter.
// NOTE the guard reads `online_state` while the sibling below reads `state`. Different fields,
// different constants, adjacent functions.
void detail::bldg_order_production_add(const issue_view &v, const order_sink &s, uint32_t player,
                                       int32_t bldg_idx, int32_t unit_type) {
    if (bldg_at(v, player, bldg_idx).online_state == 0) return;
    s.scratch_set_field(0, unit_type);
    s.scratch_set_field(1, 1);
    s.dispatch((uint16_t)bldg_idx, owner_of(player, KIND_BLDG), 0x6d, 0x6d);
}

// ---- llm_strat_bldg_order_production_remove @0x0046dcbe -----------------------------------------
// GUARD: `CMP word ptr [.. + 0xc3d2ad],0x64` / `JZ` -- buildings[..].state (+0xd) must NOT be 100.
// 100 is the under-construction state (see the field comment in mh_structs.gen.h), so this reads as
// "you cannot cancel production in a building that is still being built".
// ONE scratch slot, args[0] = unit_type. There is no args[1] here.
void detail::bldg_order_production_remove(const issue_view &v, const order_sink &s, uint32_t player,
                                          int32_t bldg_idx, int32_t unit_type) {
    if (bldg_at(v, player, bldg_idx).state == 100) return;
    s.scratch_set_field(0, unit_type);
    s.dispatch((uint16_t)bldg_idx, owner_of(player, KIND_BLDG), 0x6f, 0x6f);
}

// ---- llm_strat_bldg_order_load_resource @0x0046ebd8 ---------------------------------------------
// Unguarded. Order 0xdd/0xdd. The two scratch writes go to slots 3 and 2, IN THAT ORDER -- the
// original loads `MOV EAX,0x3` first and `MOV EAX,0x2` second. Slot order is observable (the
// container copies the scratch array wholesale, so the final contents are the same either way, but
// a differing CALL ORDER is a differing emission and the oracle compares the sequence), so it is
// preserved rather than sorted.
// The two payload parameters are unnamed in the committed prototype (`a2` in EBX, `param_4` in ECX)
// because the order's handler has not been read; naming them here would be inventing semantics.
void detail::bldg_order_load_resource(const issue_view &, const order_sink &s, uint32_t player,
                                      int32_t bldg_idx, int32_t arg3, int32_t arg2) {
    s.scratch_set_field(3, arg3);
    s.scratch_set_field(2, arg2);
    s.dispatch((uint16_t)bldg_idx, owner_of(player, KIND_BLDG), 0xdd, 0xdd);
}

// ---- llm_strat_bldg_order_repair_cycle_start @0x0046e330 ----------------------------------------
// TWO guards, both of which must let it through. Order 0x6a/0x6a.
//
//  (1) `FLD double [bldg.energy]` / `FCOMP double [cfg.energy]` / `FNSTSW`/`SAHF` / `JNC` -- the
//      classic Watcom x87 compare. FCOMP sets C0=1 when ST0 < src AND when the compare is
//      UNORDERED (either operand NaN gives C0=C2=C3=1); FNSTSW puts C0 in AH bit 0 and SAHF moves
//      it to CF, so CF == C0. `JNC` leaves the function exactly when C0 == 0, i.e. when the two are
//      ORDERED and bldg.energy >= cfg.energy. It emits only while the building is BELOW its type's
//      full charge -- which is what makes this "start a repair cycle". (ENERGY here is the HP-like
//      charge stat, not the POWER resource.)
//
//      WRITTEN AS `>=`, AND THE APPARENTLY EQUIVALENT `!(a < b)` IS WRONG. The two differ on
//      exactly one input class: NaN. `!(NaN < x)` is TRUE, so that form returns -- but the original
//      does not, because an unordered compare sets CF and the JNC is not taken. `NaN >= x` is
//      FALSE, so this form falls through, which is what the hardware does. Caught by the
//      adversarial review of this slice (2026-08-27), not by the translation.
//  (2) `CMP byte ptr [cfg.type],0x22` / JZ-out, then `CMP byte ptr [cfg.type],0x0e` / JNZ-in --
//      building TYPES 0x22 and 0x0e are excluded. The second test's polarity is inverted in the
//      listing (JNZ skips the JMP that leaves), which is the compiler folding `type == 0x22 ||
//      type == 0x0e` into a shared exit; read as a disjunction it is straightforward.
//
// The cfg row is Building[bldg.building_id] -- `MOVZX EDX,word [bldg.building_id]` then
// `IMUL EDX,EDX,0x842`. The building record is re-loaded from scratch for each of the three reads
// in the original (three identical index multiplies); that is a codegen artifact with no observable
// effect, so it is written once here.
void detail::bldg_order_repair_cycle_start(const issue_view &v, const order_sink &s, uint32_t player,
                                           int32_t bldg_idx) {
    const mh::game::mh_map_object_building       &b   = bldg_at(v, player, bldg_idx);
    const mh::game::mh_cfg_final_struct_Building &cfg = v.cfg_buildings[b.building_id];

    if (b.energy >= cfg.energy) return; // JNC 0x0046e400 -- NOT !(a < b); see the note above
    if (cfg.type == 0x22) return;       // JZ  0x0046e3e3 -> JMP out
    if (cfg.type == 0x0e) return;       // JNZ 0x0046e3e5 falls through to the same JMP out

    s.dispatch((uint16_t)bldg_idx, owner_of(player, KIND_BLDG), 0x6a, 0x6a);
}

// ---- the public forms ---------------------------------------------------------------------------

void bldg_order_activate(uint32_t player, uint16_t bldg_idx) {
    detail::bldg_order_activate(live_view(), live_sink(), player, bldg_idx);
}
void bldg_order_deactivate_enqueue(uint32_t player, uint16_t bldg_idx) {
    detail::bldg_order_deactivate_enqueue(live_view(), live_sink(), player, bldg_idx);
}
void bldg_order_production_add(uint32_t player, int32_t bldg_idx, int32_t unit_type) {
    detail::bldg_order_production_add(live_view(), live_sink(), player, bldg_idx, unit_type);
}
void bldg_order_production_remove(uint32_t player, int32_t bldg_idx, int32_t unit_type) {
    detail::bldg_order_production_remove(live_view(), live_sink(), player, bldg_idx, unit_type);
}
void bldg_order_load_resource(uint32_t player, int32_t bldg_idx, int32_t arg3, int32_t arg2) {
    detail::bldg_order_load_resource(live_view(), live_sink(), player, bldg_idx, arg3, arg2);
}
void bldg_order_repair_cycle_start(uint32_t player, int32_t bldg_idx) {
    detail::bldg_order_repair_cycle_start(live_view(), live_sink(), player, bldg_idx);
}


} // namespace mh::orders::issue


namespace mh::orders::issue {
} // namespace mh::orders::issue
