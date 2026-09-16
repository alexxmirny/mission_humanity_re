//
// sim/sim_unit_predict_coords.cpp -- see sim_unit_predict_coords.h. Translated from the DISASSEMBLY
// (tmp/decomp/llm_strat_unit_predict_coords_after_delay_00448d55.asm), not from Ghidra's .c: the
// draft's overall shape is right, but every `units[...]`/`Unit[...]` field read below was re-derived
// address-by-address against the raw MOVZX/IMUL chain rather than trusted, and the FCOMP/JNC guard was
// independently re-read for its NaN polarity (see the guard's own comment) rather than transcribed as
// the draft's `0.0 < time_delta`.
//
#include "sim/sim_unit_predict_coords.h"

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"
#include "fp/x87.h" // CRT-X87: the shared x87 truncation helpers

namespace mh::sim {

const unit_predict_coords_after_delay_calls &live_unit_predict_coords_after_delay_calls() {
    static const unit_predict_coords_after_delay_calls c = {
        MH_LIBMH_BIND(llm_strat_unit_get_coords),
        MH_LIBMH_BIND(llm_strat_facing24_to_delta),
    };
    return c;
}

namespace {

// utils_math_trunc @0x004d0596 (`MH_UNAVAILABLE__parameter_storage_not_marshallable` in
// mh_calls.gen.h -- ST0 in/out, x87-register-only, no stack-passable signature). This function's one
// call site (0x00448e73) is an ORDINARY `CALL utils_math_trunc`, not a compiler-inlined copy of its
// body -- same situation sim_unit_refund.cpp's refund_amount() and ai_active_unit_tick.cpp's
// trunc_toward_zero already document for their own call sites, reproduced here as the identical
// instruction sequence rather than reached through mh::call or substituted with std::trunc.
//
// WIDTH VERIFIED AT THIS SITE, not assumed from a sibling: opcode bytes at 0x00448e78 are `db 5d e0`
// -> 0xDB, ModRM 0x5D (mod=01, reg=011, rm=101) -> reg field 3 -> 0xDB /3 == FISTP m32int, the 32-bit
// form -- matches ai_active_unit_tick.cpp's helper (NOT sim_unit_refund.cpp's, which uses the same
// 32-bit form too in that particular case but documents that the two forms diverge elsewhere in this
// cluster; here it is independently confirmed from this function's own bytes).
int32_t trunc_toward_zero(double value) {
    return ::mh::fp::trunc_i32(value);
}

// _DAT_00500cfd -- CONDUCTOR-CONFIRMED via ReVA read-memory (2026-08-11): bytes `52 B8 1E 85 EB 51 E0
// 3F` (LE) == 0.51 exactly, NOT the 0.5 this translator inferred by analogy to sim_unit_refund.cpp's
// DAT_005014be. See the header for why the two constants disagree on a narrow fractional band.
inline constexpr double PREDICT_STEP_ROUND_OFFSET = 0.51; // DAT_00500cfd, get-bytes confirmed

} // namespace

namespace detail {

void unit_predict_coords_after_delay(const sim_view &v, const unit_predict_coords_after_delay_calls &c,
                                     uint16_t player, int32_t unit_idx, double time_delta,
                                     uint32_t *out_x, uint32_t *out_y) {
    // 0x00448d62-0x00448d84: forward straight through to llm_strat_unit_get_coords -- fills
    // *out_x/*out_y with the unit's CURRENT fine coordinates. This function's own out-params ARE the
    // callee's out-params; no local staging. out_x/out_y are uint32_t* (this function's own committed
    // signature); get_coords' committed out-params are int32_t* (TACT1-P C6, 2026-09-04) -- same
    // 32-bit quantity, cast at this call.
    c.get_coords(player, unit_idx, reinterpret_cast<int32_t *>(out_x), reinterpret_cast<int32_t *>(out_y));

    // 0x00448d90-0x00448d98: FLDZ; FCOMP double ptr [time_delta]; FNSTSW AX; SAHF; JNC -> skip(return).
    // JNC fires iff ORDERED and 0.0>=time_delta, i.e. ordered and time_delta<=0.0 (unordered/NaN sets
    // CF=1, so JNC does NOT fire on NaN -- the original falls through into the step computation on a
    // NaN time_delta rather than returning). Plain C++ `<=` is itself false on an unordered operand,
    // so `if (time_delta <= 0.0) return;` reproduces the branch EXACTLY -- the same NaN-safe idiom
    // sim_unit_update_rotation.cpp's Gate 1 documents for the identical FLDZ/FCOMP/JNC shape. A naive
    // `if (!(time_delta > 0.0)) return;` would get the NaN case backwards.
    if (time_delta <= 0.0) return;

    const unit     &u     = unit_of(v, (uint32_t)player, unit_idx);
    const cfg_unit &proto = v.cfg_units[u.unit_proto_id];

    // 0x00448d9a-0x00448ddd: only a unit currently IN its own class's move-step state advances at
    // all -- state must equal Unit[proto].move_op_code. reimpl-verify (2026-08-11): the two sides
    // are NOT the same native width -- move_op_code is an 8-bit field (MOVZX EBX,byte ptr
    // [...+0xe4a183] @0x00448dba, matching sim_unit_recruit.h's independent uint8_t documentation
    // of the same field) while state is 16-bit (MOVZX EAX,word ptr [...+0xdd8c4e] @0x00448dd4);
    // both are zero-extended before the 32-bit CMP @0x00448ddb, so `(uint16_t)proto.move_op_code`
    // below is correct -- it is a widening cast of an 8-bit value, not a narrowing one.
    if (u.state != (uint16_t)proto.move_op_code) return;

    // 0x00448de4-0x00448e1c: per-player cfg step speed (double[9], player-indexed).
    double step_speed = proto.step_speed[player];

    // 0x00448e1f-0x00448e64: ground-ish classes (type < UNIT_TYPE_A_HELI) further scale by the
    // unit's OWN move_step_speed_scale; heli/plane/mother classes use the cfg value unscaled. Signed
    // compare (CMP dword,0xe / JG), matching the field's own uint32_t storage -- same ladder shape as
    // sim_unit_housing_count.cpp's over the identical cfg_unit::type field.
    if ((int32_t)proto.type < (int32_t)UNIT_TYPE_A_HELI) step_speed *= u.move_step_speed_scale;

    // 0x00448e67-0x00448e78: steps = trunc(time_delta/step_speed + PREDICT_STEP_ROUND_OFFSET). Plain
    // C++ division/addition against `double` locals reproduces the FDIV/FADD chain exactly under this
    // TU's /arch:IA32 /fp:precise build (sim_state.h's FP LANDMINE banner); trunc_toward_zero() above
    // reproduces the CALL utils_math_trunc + FISTP tail.
    const double  ratio = time_delta / step_speed + PREDICT_STEP_ROUND_OFFSET;
    const int32_t steps = trunc_toward_zero(ratio);

    // 0x00448e7b-0x00448e9b: direction delta for the unit's TARGET facing (facing_target, NOT
    // facing_current).
    int32_t dx = 0, dy = 0;
    c.facing24_to_delta(u.facing_target, &dx, &dy);

    // 0x00448ea0-0x00448ed4: advance by (dx,dy)*steps -- IMUL (signed) then a plain 32-bit ADD, each
    // axis independently masked (AND, not modulo) by the map's PIXEL-space wrap masks
    // general.bw_mask/bh_mask. NOTE: NOT the fields sim_state.h's map_width_mask()/map_height_mask()
    // helpers read (those are the TILE-space width_mask/height_mask of the SAME mh_llm_strat_map_geom
    // record, at different offsets) -- using those helpers here would be a real bug, not a style
    // choice.
    const int32_t delta_x = dx * steps;
    const int32_t delta_y = dy * steps;
    *out_x                = (*out_x + (uint32_t)delta_x) & v.geom->bw_mask;
    *out_y                = (*out_y + (uint32_t)delta_y) & v.geom->bh_mask;
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

void unit_predict_coords_after_delay(uint32_t player, int32_t unit_idx, uint32_t unused_ebx,
                                     uint32_t unused_ecx, double time_delta, uint32_t *out_x,
                                     uint32_t *out_y) {
    (void)unused_ebx; // dead register param -- see the header banner
    (void)unused_ecx; // dead register param -- see the header banner
    const sim_view v = state().read;
    detail::unit_predict_coords_after_delay(v, live_unit_predict_coords_after_delay_calls(),
                                            (uint16_t)player, unit_idx, time_delta, out_x, out_y);
}

//
// This site writes no sim-state region at all (confirmed against tools/data/sim_migration.json's
// writes_shared=[]/writes_island=[] for this function, and both outward callees are themselves pure
// with respect to sim state -- see the header banner), so there is nothing a differential shadow arm
// could compare beyond the two out-pointers, which net_selftest.exe simtest already exercises
// directly by calling detail::unit_predict_coords_after_delay over swept inputs and diffing
// *out_x/*out_y against the original. Precedent: mh::orders::integrity_check (SIM1C), documented
// "not shadowable" for the identical reason and verified via simtest alone, no shadow arm.

} // namespace mh::sim
