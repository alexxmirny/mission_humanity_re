//
// sim/sim_unit_fire_at_target2_if_aimed.cpp -- see sim_unit_fire_at_target2_if_aimed.h. Translated
// from the DISASSEMBLY (tmp/decomp/llm_strat_unit_fire_at_target2_if_aimed_004863e6.asm), not from
// the Ghidra .c draft -- the draft's `local_24`/`local_20` naming for the get_coords out-params is
// this codebase's already-documented Ghidra local-numbering-vs-real-offset artifact (the two names do
// not correspond to which coordinate is x vs y the way their declaration order suggests); this file
// re-derives the x/y binding from the raw EBP-relative addresses and the committed
// llm_strat_unit_get_coords parameter order instead. The tolerance-check BOOLEAN expression the draft
// prints, however, is value-correct and is reproduced as-is (see the header banner).
//
#include "sim/sim_unit_fire_at_target2_if_aimed.h"

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"        // ai_say / trace_budget -- the shared trace sink, not AI state
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const unit_fire_at_target2_if_aimed_calls &live_unit_fire_at_target2_if_aimed_calls() {
    static const unit_fire_at_target2_if_aimed_calls c = {
        MH_LIBMH_BIND(llm_strat_unit_get_coords),
        MH_LIBMH_BIND(llm_strat_dir_from_to),
        MH_LIBMH_BIND(llm_strat_unit_fire_weapon),
    };
    return c;
}

namespace detail {

void unit_fire_at_target2_if_aimed(const sim_view &v, const unit_fire_at_target2_if_aimed_calls &c) {
    const unit &u = *v.cur_unit;

    // 0x004863fe-0x00486412: llm_strat_unit_get_coords(cur_player, cur_index, &cur_x, &cur_y) -- the
    // committed prototype's 3rd/4th params are (out_x, out_y) in that order, and the asm LEAs EBX with
    // &[EBP-0x20] (-> out_x) and ECX with &[EBP-0x1c] (-> out_y); so [EBP-0x20]=cur_x, [EBP-0x1c]=cur_y
    // (the Ghidra .c draft's local_24/local_20 naming does not track this -- see the file banner).
    int32_t cur_x = 0, cur_y = 0;
    c.unit_get_coords(*v.cur_player, *v.cur_index, &cur_x, &cur_y);

    // 0x0048641c-0x00486433: target_dir = llm_strat_dir_from_to(cur_x, cur_y, target2_fine_x,
    // target2_fine_y). Register trace at the call site: EAX=[EBP-0x20]=cur_x, EDX=[EBP-0x1c]=cur_y,
    // EBX=[cur_unit+0x9a]=target2_fine_x, ECX=[cur_unit+0x9e]=target2_fine_y, matching the committed
    // (x1,y1,x2,y2)->(EAX,EDX,EBX,ECX) convention this same callee/shape uses elsewhere in mh/sim
    // (e.g. llm_gfx_draw_line_clipped, llm_map_tile_distance_wrapped in addr/mh_calls.gen.h).
    const int32_t target_dir = c.dir_from_to(cur_x, cur_y, u.target2_fine_x, u.target2_fine_y);

    // 0x0048643b-0x00486444: cur_facing = (int)cur_unit->facing_current (byte, zero-extended).
    const int32_t cur_facing = u.facing_current;

    // 0x00486447-0x0048645c: tol = (int)Unit[cur_unit->unit_proto_id].weapon_facing_tolerance -- the turret-arc
    // tolerance value. Read RAW off the generated (unfriendly) field name; see the header's DECLARED
    // NEED for the proposed friendly name / offset/type citation (uint32_t @0x22f, Unit base
    // 0x00e4a098 stride 0x23f -- addr/mh_addrs.gen.h's `Unit`).
    const int32_t tol = static_cast<int32_t>(v.cfg_units[u.unit_proto_id].weapon_facing_tolerance);

    // 0x0048645f-0x004864af (check_a) and 0x00486486-0x004864ad (check_b): the two mirrored circular-
    // tolerance tests -- see the header banner for the derivation and the hand-traced wraparound
    // example. Transcribed as the literal two-boolean AND, not collapsed to a single min()/abs().
    const int32_t d       = cur_facing - target_dir;
    const bool    check_a = (cur_facing <= target_dir) || (d <= tol) || (0x18 - tol <= d);
    const int32_t e       = target_dir - cur_facing;
    const bool    check_b = (target_dir <= cur_facing) || (e <= tol) || (0x18 - tol <= e);

    if (check_a && check_b) {
        // 0x004864b1-0x004864f7: fire the selected weapon at target2. target2_ref (int16_t) is
        // zero-extended (MOVZX word), not sign-extended, matching the .c draft's own `(uint)(ushort)`
        // cast -- hence the explicit uint16_t round-trip below rather than a direct int16_t->uint32_t
        // conversion (which would sign-extend a negative target2_ref instead).
        c.unit_fire_weapon(*v.cur_player, *v.cur_index, u.selected_weapon,
                           static_cast<uint32_t>(static_cast<uint16_t>(u.target2_ref)),
                           static_cast<uint16_t>(u.target2_index), u.target2_fine_x, u.target2_fine_y);
    }
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

void unit_fire_at_target2_if_aimed() {
    sim_state st = state();
    detail::unit_fire_at_target2_if_aimed(st.read, live_unit_fire_at_target2_if_aimed_calls());
}


} // namespace mh::sim
