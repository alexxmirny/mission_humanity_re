//
// sim/sim_unit_fire_at_target.cpp -- see sim_unit_fire_at_target.h. Translated from the DISASSEMBLY
// (tmp/decomp_sim/llm_strat_unit_fire_at_target_if_aimed_004862c6.asm,
// tmp/decomp_sim/llm_strat_unit_fire_at_target_00486506.asm), not from the Ghidra .c drafts.
//
#include "sim/sim_unit_fire_at_target.h"

#include "addr/mh_calls.gen.h"      // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"            // ai_say / trace_budget -- the shared trace sink, not AI state
#include "sim/sim_unit_set_state.h" // mh::sim::unit_set_state -- the reimplemented sibling this pair calls
#include "addr/mh_rebind.gen.h"     // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const unit_fire_at_target_calls &live_unit_fire_at_target_calls() {
    static const unit_fire_at_target_calls c = {
        MH_LIBMH_BIND(llm_strat_unit_get_coords),
        MH_LIBMH_BIND(llm_strat_dir_from_to),
        mh::sim::unit_set_state,
        MH_LIBMH_BIND(llm_strat_unit_fire_weapon),
    };
    return c;
}

namespace {

// Shared by both functions below: the two mirrored circular-tolerance checks (see the header banner
// and sim_unit_fire_at_target2_if_aimed.h's own derivation comment for the hand-traced wraparound
// proof). Transcribed as the literal two-boolean AND per function, not collapsed to a single
// min()/abs() expression -- this helper only factors the DUPLICATION across this file's two call
// sites, it does not change the shape.
bool circular_aim_ok(int32_t cur_facing, int32_t target_dir, int32_t tol) {
    const int32_t d       = cur_facing - target_dir;
    const bool    check_a = (cur_facing <= target_dir) || (d <= tol) || (0x18 - tol <= d);
    const int32_t e       = target_dir - cur_facing;
    const bool    check_b = (target_dir <= cur_facing) || (e <= tol) || (0x18 - tol <= e);
    return check_a && check_b;
}

} // namespace

namespace detail {

// llm_strat_unit_fire_at_target_if_aimed @0x004862c6.
void unit_fire_at_target_if_aimed(const sim_view &v, const unit_fire_at_target_calls &c) {
    const unit &u = *v.cur_unit;

    // 0x004862de-0x004862f6: own_y=[EBP-0x1c], own_x=[EBP-0x20] -- see the header banner for the
    // out_x/out_y binding derivation.
    int32_t own_y = 0, own_x = 0;
    c.unit_get_coords(*v.cur_player, *v.cur_index, &own_x, &own_y);

    // 0x00486303-0x0048631f: target_dir = dir_from_to(own_x, own_y, target_fine_x, target_fine_y).
    const int32_t target_dir = c.dir_from_to(own_x, own_y, u.target_fine_x, u.target_fine_y);

    // 0x004862f7-0x00486300: cur_facing = (int)cur_unit->facing_current (byte, zero-extended).
    const int32_t cur_facing = u.facing_current;

    // 0x00486327-0x0048633c: tol = (int)cfg_units[cur_unit->unit_proto_id].weapon_facing_tolerance.
    const int32_t tol = static_cast<int32_t>(v.cfg_units[u.unit_proto_id].weapon_facing_tolerance);

    if (!circular_aim_ok(cur_facing, target_dir, tol)) return;

    // 0x00486391-0x004863d7: fire the selected weapon at the PRIMARY target. target_ref (int16_t) is
    // zero-extended, matching the target2 sibling's own explicit uint16_t round-trip.
    c.unit_fire_weapon(*v.cur_player, *v.cur_index, u.selected_weapon,
                       static_cast<uint32_t>(static_cast<uint16_t>(u.target_ref)),
                       static_cast<uint16_t>(u.target_index), u.target_fine_x, u.target_fine_y);
}

// llm_strat_unit_fire_at_target @0x00486506. The _if_aimed body above, plus two guards in front (see
// the header banner): an unconditional cfg-driven state force, then a selected_weapon early-out that
// gates the whole aim-check+fire block (the shorter sibling has neither).
void unit_fire_at_target(const sim_view &v, const unit_fire_at_target_calls &c) {
    const unit &u = *v.cur_unit;

    // 0x0048651e-0x0048653f: unconditional side effect, independent of everything below.
    if (v.cfg_units[u.unit_proto_id].move_op_code == 0x12) {
        c.unit_set_state(0x2e);
    }

    // 0x00486540-0x0048654d: no weapon selected -> nothing to fire, skip the aim check entirely.
    if (u.selected_weapon == UNIT_SELECT_WEAPON_NOT_FOUND) return;

    // 0x0048654f-0x00486568: own_y=[EBP-0x18], own_x=[EBP-0x1c].
    int32_t own_y = 0, own_x = 0;
    c.unit_get_coords(*v.cur_player, *v.cur_index, &own_x, &own_y);

    // 0x00486568-0x00486584: target_dir = dir_from_to(own_x, own_y, target_fine_x, target_fine_y).
    const int32_t target_dir = c.dir_from_to(own_x, own_y, u.target_fine_x, u.target_fine_y);

    // 0x0048658c-0x00486595: cur_facing = (int)cur_unit->facing_current.
    const int32_t cur_facing = u.facing_current;

    // 0x0048659c-0x004865ad: tol = (int)cfg_units[cur_unit->unit_proto_id].weapon_facing_tolerance.
    const int32_t tol = static_cast<int32_t>(v.cfg_units[u.unit_proto_id].weapon_facing_tolerance);

    if (!circular_aim_ok(cur_facing, target_dir, tol)) return;

    // 0x00486602-0x00486648: fire the selected weapon at the PRIMARY target -- identical argument set
    // to _if_aimed above.
    c.unit_fire_weapon(*v.cur_player, *v.cur_index, u.selected_weapon,
                       static_cast<uint32_t>(static_cast<uint16_t>(u.target_ref)),
                       static_cast<uint16_t>(u.target_index), u.target_fine_x, u.target_fine_y);
}

} // namespace detail

} // namespace mh::sim

namespace mh::sim {

// ---- the public wrappers -------------------------------------------------------------------------

void unit_fire_at_target_if_aimed() {
    sim_state st = state();
    detail::unit_fire_at_target_if_aimed(st.read, live_unit_fire_at_target_calls());
}

void unit_fire_at_target() {
    sim_state st = state();
    detail::unit_fire_at_target(st.read, live_unit_fire_at_target_calls());
}


} // namespace mh::sim
