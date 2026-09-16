#include "sim/sim_unit_update_rotation.h"

#include "ai/ai_state.h"        // ai_say / trace_budget -- the shared trace sink, not AI state
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const unit_update_rotation_calls &live_unit_update_rotation_calls() {
    static const unit_update_rotation_calls c = {
        MH_LIBMH_BIND(llm_strat_unit_update_target_tracking),
        MH_LIBMH_BIND(llm_strat_unit_update_target2_tracking),
        MH_LIBMH_BIND(llm_strat_unit_get_coords),
        MH_LIBMH_BIND(llm_strat_dir_from_to),
        MH_LIBMH_BIND(llm_strat_unit_soldiers_set_heading),
    };
    return c;
}

namespace {

// llm_strat_unit_state members this switch touches (word fields `order`@0x4 and `state`@0x6 in
// map_object_unit -- Ghidra applies the SAME cfg-enum tag to both, per sim_order_enqueue.h's identical
// finding on this domain). Not emitted as a real C++ enum by the struct generator (the tag is
// decompiler documentation only), so named locally here -- re-derived per-TU rather than shared, same
// move as sim_order_enqueue.h's own UNIT_STATE_* set and sim_unit_state_predicates.cpp's fine_to_tile()
// (translator brief rule 4: no new shared helpers). Kept in an anonymous namespace, not the header, so
// this file's copy can never collide with sim_order_enqueue.h's if both TUs are ever linked together.
// Names and values are Ghidra's own decompiler-applied symbolic constants, read off the exported .c.
constexpr uint16_t STATE_STOP_TO_DEFAULT    = 0x01;
constexpr uint16_t STATE_DIE_EXPLODE        = 0x02; // boundary only -- see the .c's `lVar2 < DIE_EXPLODE`
constexpr uint16_t STATE_CORPSE_FOW_DECAY   = 0x04; // boundary only -- state<this captures {2,3}
constexpr uint16_t STATE_GROUP_MARSHAL      = 0x0a;
constexpr uint16_t STATE_GROUP_STEP         = 0x0b; // boundary only -- state<this (given state>=0xa) is GROUP_MARSHAL alone
constexpr uint16_t STATE_MOVE_WALKER        = 0x0f;
constexpr uint16_t STATE_PARKED             = 0x1f;
constexpr uint16_t STATE_ATTACK_UNIT        = 0x1a;
constexpr uint16_t STATE_ATTACK_UNIT_RETURN = 0x1b;
constexpr uint16_t STATE_ATTACK_BUILDING    = 0x1c;
constexpr uint16_t STATE_HOVER_ENGAGE       = 0x2e;
constexpr uint16_t STATE_HOVER_ENGAGE_2F    = 0x2f;
constexpr uint16_t STATE_DESCEND_CRUISE     = 0x30; // boundary only -- state<this (given state>=0x2f) is HOVER_ENGAGE_2F alone
constexpr uint16_t STATE_HOVER_DISENGAGE    = 0x37;

} // namespace

namespace detail {

void unit_update_rotation(const sim_view &v, sim_store &own, const unit_update_rotation_calls &c) {
    const uint16_t player = *v.cur_player;         // ambient cursor, cached: see the header's DECLARED NEED
    const int32_t  index  = (int32_t)*v.cur_index; // note above on why caching this is safe here
    const unit    &u      = *v.cur_unit;           // same caching reasoning; see DECLARED NEED

    // ---- THE FP GATES (0x0047d759-0x0047d7d4) ----------------------------------------------------
    //
    // Both compare a double against 0.0 or against turn_speed via FLDZ/FLD;FCOMP;FNSTSW;SAHF, whose
    // CF (after SAHF) is 1 iff (ST0 < src) OR the compare is UNORDERED (either operand NaN) -- the
    // x87 idiom sim_bldg_alive.cpp's header already documents for a single comparison; this function
    // has FOUR (two here, two more in the step loop below), and they do NOT all reduce to the same
    // literal C++ operator:
    //
    //   Gate 1 (0x0047d772, JNC->return): return-branch taken iff ORDERED AND (0.0>=budget), i.e.
    //   ordered budget<=0.0. Plain C++ `if (budget <= 0.0) return;` reproduces this EXACTLY, because
    //   IEEE `<=` is itself false on NaN -- the "ordered" requirement is already built into the
    //   operator. No idiom needed here.
    //
    //   Gate 2 (0x0047d7bf, JNC->continue / JC->spend-and-return): continue-to-state-processing is
    //   taken iff ORDERED AND (budget>=turn_speed). A naive `if (budget < turn_speed)` for the
    //   spend-and-return arm would read a NaN budget as "false" (continues into state processing),
    //   the OPPOSITE of the original (JC fires on unordered -> spend-and-return). The faithful form is
    //   the negated idiom: spend-and-return iff `!(budget >= turn_speed)`.
    //
    // The step-loop pair (0x0047dc3f, 0x0047dc4e) repeats exactly this: the while-condition's
    // "continue" arm and the loop-body's "budget short" arm are BOTH JC-taken (unordered-inclusive),
    // so both need the negated form too. See the loop below.
    double budget = *v.game_clock - u.rotation_clock;
    if (budget <= 0.0) return; // Gate 1 -- plain <=, see above.

    own.cur_unit().rotation_clock = *v.game_clock;

    const double turn_speed = v.cfg_units[u.unit_proto_id].turn_speed[player];
    if (!(budget >= turn_speed)) {               // Gate 2 -- negated idiom, see above.
        own.cur_unit().rotation_clock -= budget; // nets to the ORIGINAL rotation_clock (just-set GAME_CLOCK minus budget)
        return;
    }

    // ---- STATE/ORDER GATE (0x0047d7d4-0x0047d86b) -------------------------------------------------
    // A 9-way OR-chain via cascading integer CMP/JNZ; independently re-walked against every jump
    // target and found to enumerate exactly these 8 states (plus the separate PARKED check below) --
    // equivalent-exhaustive form per the task's "or an equivalent exhaustive switch" allowance, safe
    // to flatten here because this gate is pure (no calls, no side effects either arm).
    const uint16_t state = u.state; // cached once, matching the original's own lVar2 local
    const bool     state_gate_open =
        (v.cfg_units[u.unit_proto_id].independent != 0) || (state == STATE_ATTACK_UNIT) ||
        (state == STATE_ATTACK_UNIT_RETURN) || (state == STATE_ATTACK_BUILDING) ||
        (state == STATE_HOVER_ENGAGE) || (state == STATE_GROUP_MARSHAL) ||
        (state == STATE_STOP_TO_DEFAULT) || (state == STATE_HOVER_DISENGAGE) ||
        (state == STATE_HOVER_ENGAGE_2F);
    if (!state_gate_open) return;
    if (state == STATE_PARKED) return;

    // ---- TARGET HEADING (0x0047d86b-0x0047dc37) ---------------------------------------------------
    //
    // Two small shared tails, matching the two LAB_ targets the assembly jumps/falls into from
    // multiple states. Written as local lambdas (capturing by reference) instead of `goto` -- same
    // observable call sequence and argument order as the assembly, without needing every local
    // declared ahead of a jump the way sim_unit_passive_engage.cpp's literal `goto tail;` did; that
    // file's shared tail had no early-return variant, this one's group_marshal_tail does, which a
    // plain fallthrough-goto would make harder to read here, not easier.
    //
    // get_coords's out-params (0x0047dbe7-0x0047dc00, 0x0047d95c-0x0047d975, 0x0047da97-0x0047dab0,
    // 0x0047dc00-0x0047dc16) are the unit's OWN current fine position; dir_from_to takes
    // (own_x, own_y, target_x, target_y) -- verified x-with-x/y-with-y against the raw register moves
    // at every call site (target_fine_x@0x8e pairs with the x1/out_x slot, target_fine_y@0x92 with
    // y1/out_y, and correspondingly target2_fine_x@0x9a/target2_fine_y@0x9e for the target2 tail).
    uint32_t target_heading = u.facing_target; // the unconditional default (LAB_0047dc2b); every
                                               // branch below either overwrites it or leaves it.

    int32_t own_x = 0, own_y = 0;
    auto    target1_tail = [&]() -> uint32_t {
        c.get_coords(player, index, &own_x, &own_y);
        return (uint32_t)c.dir_from_to(own_x, own_y, u.target_fine_x, u.target_fine_y);
    };
    auto target2_tail = [&]() -> uint32_t {
        c.get_coords(player, index, &own_x, &own_y);
        return (uint32_t)c.dir_from_to(own_x, own_y, u.target2_fine_x, u.target2_fine_y);
    };
    // LAB_0047da67, shared by STOP_TO_DEFAULT (falls straight in) and GROUP_MARSHAL (its own arm jumps
    // here -- see the .c's `goto LAB_0047da67`). Unlike target1_tail/target2_tail above, this one can
    // end the WHOLE FUNCTION (0x0047dad6, target2_ref==0 -> return with nothing done at all, not even
    // the step loop) -- modelled as a bool: false means "the caller must return immediately". Writes
    // the outer `target_heading` directly (captured by reference) rather than returning a value, since
    // the false path has nothing to return.
    auto group_marshal_tail = [&]() -> bool {
        if (u.target2_ref == 0) return false; // 0x0047dad6
        if ((u.target2_ref & 0xa0) != 0) c.update_target2_tracking(player, index);
        target_heading = target2_tail();
        return true;
    };

    if (state < STATE_ATTACK_UNIT) { // 0x0047d883 JC -> state < 0x1a
        if (state < STATE_DIE_EXPLODE) {
            if (state == STATE_STOP_TO_DEFAULT) {
                if (!group_marshal_tail()) return;
            }
            // else (state==0): target_heading keeps its default.
        } else if (state < STATE_CORPSE_FOW_DECAY) {
            return;                                // state == 2 or 3 (0x0047d8f3 JBE)
        } else if (state >= STATE_GROUP_MARSHAL) { // equivalent to the .c's "9 < lVar2"
            if (state < STATE_GROUP_STEP) {
                if (!group_marshal_tail()) return; // state == GROUP_MARSHAL exactly
            } else if (state == STATE_MOVE_WALKER) {
                if (u.target2_ref == 0) {
                    if (u.order == STATE_ATTACK_BUILDING) {
                        target_heading = target1_tail();
                    } else if (u.order == STATE_ATTACK_UNIT || u.order == STATE_ATTACK_UNIT_RETURN) {
                        c.update_target_tracking(player, index);
                        target_heading = target1_tail();
                    }
                    // else: target_heading keeps its default (facing_target).
                } else {
                    if ((u.target2_ref & 0xa0) != 0) c.update_target2_tracking(player, index);
                    target_heading = target2_tail();
                }
            }
            // else (state in (GROUP_STEP..MOVE_WALKER) excl. MOVE_WALKER, or MOVE_WALKER<state<0x1a):
            // target_heading keeps its default.
        }
        // else (state in [4,9]): target_heading keeps its default.
    } else {
        if (state < STATE_ATTACK_BUILDING) { // ATTACK_UNIT or ATTACK_UNIT_RETURN
            c.update_target_tracking(player, index);
            target_heading = target1_tail();
        } else if (state < STATE_HOVER_ENGAGE) {
            if (state == STATE_ATTACK_BUILDING) {
                target_heading = target1_tail();
            } else if (state == STATE_PARKED) {
                return; // dead in practice (PARKED already returned above); transcribed as-is
            }
            // else: target_heading keeps its default.
        } else if (state < STATE_HOVER_ENGAGE_2F) { // state == HOVER_ENGAGE exactly, ORDER-driven
            if (u.order == STATE_ATTACK_BUILDING) {
                target_heading = target1_tail();
            } else if (u.order == STATE_ATTACK_UNIT || u.order == STATE_ATTACK_UNIT_RETURN) {
                c.update_target_tracking(player, index);
                target_heading = target1_tail();
            }
            // else: target_heading keeps its default.
        } else if (state < STATE_DESCEND_CRUISE || state == STATE_HOVER_DISENGAGE) {
            // Cruise-path facing table: _G_LLM_STRAT_MOVE_MICROSTEPS[move_heading][move_microstep].facing.
            target_heading =
                v.move_microsteps[(uint32_t)u.move_heading * MICROSTEPS_PER_HEADING +
                                  (uint32_t)u.move_microstep]
                    .facing;
        }
        // else: target_heading keeps its default.
    }

    // ---- STEP LOOP (0x0047dc37-0x0047dd47) ---------------------------------------------------------
    //
    // `current_facing` is local_24: cached ONCE from facing_current at 0x0047d86b (before the target-
    // heading switch above), and thereafter updated ONLY by this loop's own assignment -- the ORIGINAL
    // never re-reads facing_current from memory for its own loop control, so this local mirrors that
    // exactly rather than reading back through `u`/`own` each iteration (which would happen to alias
    // the same memory, but would not be what the assembly actually does).
    uint32_t current_facing = u.facing_current;

    while (current_facing != target_heading &&
           !(budget <= 0.0)) {         // negated idiom -- see THE FP GATES above; JC-taken loop-continue arm
        if (!(budget >= turn_speed)) { // negated idiom -- JC-taken "budget exhausted" arm
            own.cur_unit().rotation_clock -= budget;
            budget = 0.0;
        } else {
            budget -= turn_speed;

            // Shorter-direction wraparound over the 24-position compass (values 1..24) -- the EXACT
            // original threshold/wrap arithmetic (0xd/0xc, -0xb/-0xc, wrap at 1 and 0x18), not a
            // mod-24 simplification (task hazard note: a simplified form can round differently on
            // negative intermediates).
            int32_t  diff = (int32_t)current_facing - (int32_t)target_heading;
            uint32_t next;
            if ((diff < 0xd || (int32_t)current_facing <= (int32_t)target_heading) &&
                (diff < -0xb || (int32_t)target_heading <= (int32_t)current_facing)) {
                next = current_facing - 1;
                if ((int32_t)(current_facing - 1) < 1) next = current_facing + 0x17;
            } else {
                next = current_facing + 1;
                if (0x18 < (int32_t)(current_facing + 1)) next = current_facing - 0x17;
            }
            current_facing = next;

            unit &wu          = own.cur_unit();
            wu.facing_current = (uint8_t)current_facing;
            // Independent units do NOT get facing_target synced -- asymmetry preserved (re-read fresh,
            // matching the original's own fresh re-derivation of Unit[cur_unit.unit_proto_id] here
            // rather than reusing a value cached above the loop).
            if (v.cfg_units[u.unit_proto_id].independent == 0) wu.facing_target = (uint8_t)current_facing;

            // Soldier-heading propagation: NOTE this re-derives unit_proto_id via ROSTER indexing
            // (units[player][index].unit_proto_id), NOT via cur_unit.unit_proto_id -- a distinct
            // access path in the assembly (0x0047dce1-0x0047dd04) from every other proto_id read in
            // this function, transcribed as a distinct path here too rather than collapsed to `u`.
            if (v.cfg_units[unit_of(v, player, index).unit_proto_id].soldier_count > 0)
                c.soldiers_set_heading(player, index, (uint8_t)current_facing);
        }
    }
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

void unit_update_rotation() {
    sim_state st = state();
    detail::unit_update_rotation(st.read, st.own, live_unit_update_rotation_calls());
}


} // namespace mh::sim
