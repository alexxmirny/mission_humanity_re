//
// tact_unit_weapons_tick_selftest.cpp -- offline oracle for
//   llm_tact_unit_weapons_tick @0x0042f338 (libmh/tact/tact_unit_weapons_tick.cpp)
//
// WHY OFFLINE, NOT RIG: see the header banner (tact_unit_weapons_tick.h) -- every one of the 15
// outward callees is indirected through `unit_weapons_tick_calls`, including two (unit_death_tick,
// unit_kneel_tick) whose sibling *bodies* are being translated in this same batch (bit-independence
// means this function still calls their ORIGINALs) and one (unit_cmd_teleport_jump_tick) whose own
// closure is proven-hazardous to run for real under shadow. Mocking every callee sidesteps all of
// it. Correctness rests entirely on this oracle.
//
// WHY A CONTROLLABLE CLOCK, AND WHY EVERY CASE NEEDS A "TERMINATE" TAIL VALUE: the function's body
// is a `for(;;)` loop whose ONLY exit conditions are (a) the top-of-loop time gate
// (`now <= move_state_timer` -> clear-and-return, 0x0042f364-0x0042f37f/0x0042f8bd) and (b) two
// direct-return paths (unit-died, wait-gate-still-waiting, both -> 0x0042f8da). EVERY dispatch arm
// -- and the no-op default -- falls through to 0x0042f8b8, which jumps straight back to the loop
// top. So a case that dispatches an op must ALSO arrange for the loop's SECOND top-of-loop check to
// return, or the mocked callee fires again (or forever). `g_clock` is a FIFO queue of exact,
// pre-programmed return values -- one entry per call whose RETURN VALUE the case actually exercises
// (the initial gate-pass read, an arm's own extra read, a wait-gate comparison read, ...) -- plus,
// for every case that reaches the reconverge point, one final `CLOCK_TERMINATE` sentinel
// (-1.0e9) that guarantees the SECOND top-of-loop check returns regardless of what
// move_state_timer became during the dispatch (every value this file ever stores into
// move_state_timer is a small, ordinary double; -1.0e9 is below all of them). Exhausting the queue
// WITHOUT a case providing enough entries is a bug signal, not a normal path: `clock_mock::next()`
// then returns -INFINITY, which forces the very next gate check to take its "stop" branch
// unconditionally (so the process never hangs) while `g_clock.calls` vs. the case's own queue size
// still fails loudly.
//
// THE SHARED RECORDER: every one of the 16 calls-struct members (15 callees + the clock) pushes a
// tag onto ONE shared `g_rec.order` log, so a case that needs call ORDER (not just count) -- e.g.
// the op==9 mines-disabled arm's set_anim_state-before-cmd_advance -- can assert it directly.
// EVERY case asserts `g_rec.total()` (the sum across all 15 non-clock callees), per the brief: "a
// stray extra dispatch must fail," not just "the expected callee fired."
//
#include "tact/tact_unit_weapons_tick.h"

#include "tact_test_support.h"

#include <cmath>
#include <limits>
#include <string>
#include <tuple>
#include <utility>

namespace mh::tact::test {

namespace {
using namespace mh::tact;

constexpr int32_t UNIT_IDX      = 12; // distinctive, mid-range TACT_UNIT_SLOTS (129) index
constexpr int32_t CMD_IDX       = 10; // has valid neighbors 9/11 for the byte-offset case
constexpr uint8_t CHAR_TYPE_IDX = 3;  // within TACT_CHARACTER_TYPE_SLOTS (16)

// See the file banner: the universal "make the loop's second top-of-loop check return" sentinel.
constexpr double CLOCK_TERMINATE = -1.0e9;

// ---- the controllable clock -------------------------------------------------------------------
struct clock_mock {
    std::vector<double> queue;
    size_t              pos   = 0;
    int32_t             calls = 0;

    double next() {
        ++calls;
        if (pos < queue.size()) return queue[pos++];
        // Safety net only -- see the file banner. A correctly-sized case never reaches this.
        return -std::numeric_limits<double>::infinity();
    }
    void reset() { *this = clock_mock{}; }
};
clock_mock g_clock;

// ---- the 15 callee recorders + the shared call-order log --------------------------------------
struct weapons_tick_recorder {
    std::vector<std::string> order; // one tag per outward call (incl. "clock"), in call order

    std::vector<std::pair<int32_t, uint32_t>>          cmd_queue_advance;
    std::vector<int32_t>                               death_tick;
    std::vector<std::tuple<int32_t, int32_t, int32_t>> fire_weapon;
    std::vector<std::pair<int32_t, int32_t>>           cmd_advance;
    std::vector<int32_t>                               rotate_tick;
    std::vector<std::tuple<int32_t, int32_t, double>>  move_tick;
    std::vector<int32_t>                               kneel_tick;
    std::vector<int32_t>                               stand_tick;
    std::vector<int32_t>                               mine_arm_tick;
    std::vector<std::pair<int32_t, uint8_t>>           set_anim_state;
    std::vector<std::pair<int32_t, int32_t>>           teleport_jump_tick;
    std::vector<std::pair<int32_t, int32_t>>           advance_with_defstat;
    std::vector<int32_t>                               stance_off;
    std::vector<int32_t>                               stance_on;
    std::vector<std::pair<int32_t, int32_t>>           queue_resubmit_run;

    int32_t total() const {
        return (int32_t)(cmd_queue_advance.size() + death_tick.size() + fire_weapon.size() +
                         cmd_advance.size() + rotate_tick.size() + move_tick.size() +
                         kneel_tick.size() + stand_tick.size() + mine_arm_tick.size() +
                         set_anim_state.size() + teleport_jump_tick.size() +
                         advance_with_defstat.size() + stance_off.size() + stance_on.size() +
                         queue_resubmit_run.size());
    }
    void reset() { *this = weapons_tick_recorder{}; }
};
weapons_tick_recorder g_rec;

int32_t order_pos(const char *tag) {
    for (size_t i = 0; i < g_rec.order.size(); ++i)
        if (g_rec.order[i] == tag) return (int32_t)i;
    return -1;
}

// unit_death_tick's controllable return value (death_result).
int32_t g_death_tick_return = 0;

// The op==9 mines-disabled arm's mutation hook: when non-null, the unit_set_anim_state mock writes
// g_mutate_value through g_mutate_target -- used to mutate u.cmd_index mid-call so the case can
// prove llm_tact_unit_cmd_advance receives a FRESH re-read, not the cached local (0x0042f882).
uint8_t *g_mutate_target = nullptr;
uint8_t  g_mutate_value  = 0;

void reset_all() {
    g_rec.reset();
    g_clock.reset();
    g_death_tick_return = 0;
    g_mutate_target     = nullptr;
    g_mutate_value      = 0;
}

const unit_weapons_tick_calls &rec_calls() {
    static const unit_weapons_tick_calls c = {
        // time_get_current_time @0x00427616 -- call 1..7/7 depending on path
        []() -> double {
            g_rec.order.push_back("clock");
            return g_clock.next();
        },
        // llm_tact_unit_cmd_queue_advance @0x004313cf -- step 4, 0x0042f3b9-0x0042f3e4
        [](int32_t unit_idx, uint32_t cmd_index) {
            g_rec.order.push_back("cmd_queue_advance");
            g_rec.cmd_queue_advance.emplace_back(unit_idx, cmd_index);
        },
        // llm_tact_unit_death_tick @0x0042fb0b -- step 4b, 0x0042f3d1-0x0042f3de
        [](int32_t unit_idx) -> int32_t {
            g_rec.order.push_back("death_tick");
            g_rec.death_tick.push_back(unit_idx);
            return g_death_tick_return;
        },
        // llm_tact_unit_fire_weapon @0x00430855 -- step 5, 0x0042f3fa-0x0042f40e
        [](int32_t building_id, int32_t weapon_subindex, int32_t fire_arg) {
            g_rec.order.push_back("fire_weapon");
            g_rec.fire_weapon.emplace_back(building_id, weapon_subindex, fire_arg);
        },
        // llm_tact_unit_cmd_advance @0x00431229 -- step 6 (0x0042f4d8), op 8 (0x0042f729), op 9
        // mines-disabled (0x0042f88c), op 0x7f (0x0042f8b3)
        [](int32_t unit_index, int32_t cmd_slot_index) {
            g_rec.order.push_back("cmd_advance");
            g_rec.cmd_advance.emplace_back(unit_index, cmd_slot_index);
        },
        // llm_tact_unit_rotate_tick @0x004307c5 -- step 7, 0x0042f4fe-0x0042f504
        [](int32_t unit_idx) {
            g_rec.order.push_back("rotate_tick");
            g_rec.rotate_tick.push_back(unit_idx);
        },
        // llm_tact_unit_move_tick @0x0042fb9b -- op 1, 0x0042f7c6-0x0042f7cc
        [](int32_t unit_idx, int32_t cmd_slot, double dt) {
            g_rec.order.push_back("move_tick");
            g_rec.move_tick.emplace_back(unit_idx, cmd_slot, dt);
        },
        // llm_tact_unit_kneel_tick @0x00430363 -- op 4, 0x0042f7d6-0x0042f7dc
        [](int32_t unit_id) {
            g_rec.order.push_back("kneel_tick");
            g_rec.kneel_tick.push_back(unit_id);
        },
        // llm_tact_unit_stand_tick @0x0043047d -- op 5, 0x0042f803-0x0042f809
        [](int32_t unit_idx) {
            g_rec.order.push_back("stand_tick");
            g_rec.stand_tick.push_back(unit_idx);
        },
        // llm_tact_unit_mine_arm_tick @0x0042f935 -- op 9 mines-enabled, 0x0042f856-0x0042f85c
        [](int32_t unit_id) {
            g_rec.order.push_back("mine_arm_tick");
            g_rec.mine_arm_tick.push_back(unit_id);
        },
        // llm_tact_unit_set_anim_state @0x00430f03 -- op 9 mines-disabled, 0x0042f873-0x0042f876
        [](int32_t building_id, uint8_t state) {
            g_rec.order.push_back("set_anim_state");
            g_rec.set_anim_state.emplace_back(building_id, state);
            if (g_mutate_target != nullptr) *g_mutate_target = g_mutate_value;
        },
        // llm_tact_unit_cmd_teleport_jump_tick @0x00433464 -- op 0xa, 0x0042f893-0x0042f899
        [](int32_t unit_idx, int32_t cmd_queue_slot) {
            g_rec.order.push_back("teleport_jump_tick");
            g_rec.teleport_jump_tick.emplace_back(unit_idx, cmd_queue_slot);
        },
        // llm_tact_unit_cmd_advance_with_defstat @0x0042f8e4 -- op 0xb, 0x0042f8a0-0x0042f8a6
        [](int32_t unit_idx, int32_t cmd_or_slot_index) {
            g_rec.order.push_back("advance_with_defstat");
            g_rec.advance_with_defstat.emplace_back(unit_idx, cmd_or_slot_index);
        },
        // llm_tact_unit_cmd_stance_off @0x0043060f -- op 0x1c, 0x0042f840-0x0042f846
        [](int32_t unit_index) {
            g_rec.order.push_back("stance_off");
            g_rec.stance_off.push_back(unit_index);
        },
        // llm_tact_unit_cmd_stance_on @0x00430580 -- op 0x1e, 0x0042f830-0x0042f836
        [](int32_t unit_index) {
            g_rec.order.push_back("stance_on");
            g_rec.stance_on.push_back(unit_index);
        },
        // llm_tact_unit_cmd_queue_resubmit_run @0x0043069e -- op 0x40, 0x0042f733-0x0042f739
        [](int32_t unit_idx, int32_t queue_slot) {
            g_rec.order.push_back("queue_resubmit_run");
            g_rec.queue_resubmit_run.emplace_back(unit_idx, queue_slot);
        },
    };
    return c;
}

// Seeds every gate the op-switch cases must pass THROUGH to reach the switch cleanly (steps
// 4b/5/6/7/8/9/10 all skip) so a case only has to override the field it is actually testing.
// type/move_state_timer/cmd_index/cmd_queue are always case-specific -- not set here.
void seed_neutral_precondition(tact_unit &u) {
    u.anim_state          = 0;    // != 0x1f -> skip the death check (step 4b, 0x0042f3c8)
    u.attack_cmd_op       = 0;    // != 2 -> skip the fire arm (step 5, 0x0042f3f0)
    u.face_cmd_op         = 0;    // != 6 -> skip the rotate arm (step 7, 0x0042f4e4)
    u.progress            = 0x05; // != 0 -> keeps steps 7/9's progress==0 half off by default
    u.status              = 0x02; // bits 0x40, 0x20 both off -> reach the wait-gate (steps 8/9)
    u.cmd_wait_until_time = 0.0;  // <=0.0 -> skip the wait-gate block (step 10, 0x0042f599)
}

// speed=2.5 (SPEED), run_speed=4.0 (RUN, so speed/run_speed=0.625 exact), kneel_time=1.25 (KNEEL,
// shared by op==4 AND op==5), death_time=9.75 (DEATH, distinct from kneel_time -- op==5 must NOT
// use this field, per the .cpp's own "transcribed literally, not fixed" note).
void seed_character_type(tact_fixture &fx) {
    mh::tact::character_type &ct = fx.character_types[CHAR_TYPE_IDX];
    ct.speed                     = 2.5;
    ct.run_speed                 = 4.0;
    ct.kneel_time                = 1.25;
    ct.death_time                = 9.75;
}

} // namespace

void run_unit_weapons_tick_tests() {
    // ============================================================================================
    // Loop-top time gate (step 2, 0x0042f364-0x0042f37f / clear-return 0x0042f8bd-0x0042f8da).
    // FCOMP+JBE -- pinned INCLUSIVE (now <= timer -> return) via bit-adjacent doubles.
    // ============================================================================================

    // T-GATE-EQ: now == move_state_timer EXACTLY -> JBE fires -> clear-and-return, ZERO dispatch.
    // Also proves step 3 (move_cur_col/row write) never ran: seeded with sentinels that must
    // survive.
    {
        tact_fixture fx;
        reset_all();
        fx.move_cur_col    = 777;
        fx.move_cur_row    = 888;
        tact_unit &u       = fx.units[UNIT_IDX];
        u.move_state_timer = 50.0;
        u.status           = 0xff; // any nonzero, so we can prove bit 0x08 specifically clears
        u.pos_col          = 17;
        u.pos_row          = 42;

        g_clock.queue = {50.0}; // now == timer exactly

        tact_view  tv  = fx.view();
        tact_store own = fx.store();
        detail::unit_weapons_tick(tv, own, rec_calls(), UNIT_IDX);

        ck_eq((uint32_t)g_clock.calls, 1u, "T-GATE-EQ: exactly one time read, 0x0042f36b");
        ck_eq((uint32_t)g_rec.total(), 0u, "T-GATE-EQ: ZERO of the 15 callees fire");
        ck_eq((uint32_t)own.unit_at(UNIT_IDX).status, 0xf7u,
              "T-GATE-EQ: status &= 0xf7 (bit 0x08 cleared), 0x0042f8ca-0x0042f8d4");
        ck_eq((uint32_t)fx.move_cur_col, 777u, "T-GATE-EQ: MOVE_CUR_COL untouched -- step 3 never ran");
        ck_eq((uint32_t)fx.move_cur_row, 888u, "T-GATE-EQ: MOVE_CUR_ROW untouched -- step 3 never ran");
    }

    // T-GATE-JUST-ABOVE: now is the SMALLEST double strictly greater than move_state_timer ->
    // JBE does NOT fire -> proceeds into the body. Uses a no-op cmd_queue.op (3) as the vehicle;
    // also confirms step 3's writes DO happen on this side of the boundary.
    {
        tact_fixture fx;
        reset_all();
        tact_unit &u = fx.units[UNIT_IDX];
        seed_neutral_precondition(u);
        u.move_state_timer        = 50.0;
        u.pos_col                 = 17;
        u.pos_row                 = 42;
        u.cmd_index               = CMD_IDX;
        u.cmd_queue[CMD_IDX].op   = 3;    // a no-op gap value
        u.cmd_queue[CMD_IDX].arg0 = 0x0e; // distinct from op

        const double just_above = std::nextafter(50.0, std::numeric_limits<double>::infinity());
        g_clock.queue           = {just_above, CLOCK_TERMINATE};

        tact_view  tv  = fx.view();
        tact_store own = fx.store();
        detail::unit_weapons_tick(tv, own, rec_calls(), UNIT_IDX);

        ck_eq((uint32_t)g_clock.calls, 2u, "T-GATE-JUST-ABOVE: gate-pass read + iteration-2 terminate read");
        ck_eq((uint32_t)g_rec.total(), 1u, "T-GATE-JUST-ABOVE: only cmd_queue_advance fires (op 3 is a no-op)");
        ck_eq((uint32_t)g_rec.cmd_queue_advance.size(), 1u, "T-GATE-JUST-ABOVE: cmd_queue_advance called once, 0x0042f3bc");
        if (g_rec.cmd_queue_advance.size() == 1) {
            ck_eq((uint32_t)g_rec.cmd_queue_advance[0].first, (uint32_t)UNIT_IDX, "T-GATE-JUST-ABOVE: cmd_queue_advance unit arg");
            ck_eq((uint32_t)g_rec.cmd_queue_advance[0].second, (uint32_t)CMD_IDX, "T-GATE-JUST-ABOVE: cmd_queue_advance cmd_index arg");
        }
        ck_eq((uint32_t)fx.move_cur_col, 17u, "T-GATE-JUST-ABOVE: MOVE_CUR_COL := pos_col, 0x0042f38d");
        ck_eq((uint32_t)fx.move_cur_row, 42u, "T-GATE-JUST-ABOVE: MOVE_CUR_ROW := pos_row, 0x0042f3a0");
    }

    // ============================================================================================
    // Step 4b: the death check (0x0042f3c1-0x0042f3e4).
    // ============================================================================================

    // T-DEATH-RETURNS: anim_state==0x1f, death_result != 0 -> DIRECT return @0x0042f8da, status
    // NOT cleared (unlike the top-gate return).
    {
        tact_fixture fx;
        reset_all();
        tact_unit &u = fx.units[UNIT_IDX];
        seed_neutral_precondition(u);
        u.anim_state        = 0x1f;
        u.move_state_timer  = 10.0;
        u.status            = 0x33; // distinctive, to prove it survives untouched
        g_death_tick_return = 7;    // any nonzero

        g_clock.queue = {100.0}; // single gate-pass read; no second iteration

        tact_view  tv  = fx.view();
        tact_store own = fx.store();
        detail::unit_weapons_tick(tv, own, rec_calls(), UNIT_IDX);

        ck_eq((uint32_t)g_clock.calls, 1u, "T-DEATH-RETURNS: exactly one time read");
        ck_eq((uint32_t)g_rec.total(), 2u, "T-DEATH-RETURNS: cmd_queue_advance + death_tick only");
        ck_eq((uint32_t)g_rec.death_tick.size(), 1u, "T-DEATH-RETURNS: unit_death_tick called once, 0x0042f3d7");
        if (g_rec.death_tick.size() == 1)
            ck_eq((uint32_t)g_rec.death_tick[0], (uint32_t)UNIT_IDX, "T-DEATH-RETURNS: death_tick unit arg");
        ck_eq((uint32_t)own.unit_at(UNIT_IDX).status, 0x33u,
              "T-DEATH-RETURNS: status UNCHANGED -- direct return @0x0042f8da does not clear it");
    }

    // T-DEATH-CONTINUES: death_result == 0 -> `continue` (0x0042f3e4 -> loop top). Second
    // iteration's gate returns immediately (no second death_tick call, no re-dispatch).
    {
        tact_fixture fx;
        reset_all();
        tact_unit &u = fx.units[UNIT_IDX];
        seed_neutral_precondition(u);
        u.anim_state        = 0x1f;
        u.move_state_timer  = 10.0;
        u.status            = 0xff;
        g_death_tick_return = 0;

        g_clock.queue = {100.0, CLOCK_TERMINATE};

        tact_view  tv  = fx.view();
        tact_store own = fx.store();
        detail::unit_weapons_tick(tv, own, rec_calls(), UNIT_IDX);

        ck_eq((uint32_t)g_clock.calls, 2u, "T-DEATH-CONTINUES: gate-pass read + iteration-2 terminate read");
        ck_eq((uint32_t)g_rec.total(), 2u, "T-DEATH-CONTINUES: cmd_queue_advance + death_tick, exactly once each");
        ck_eq((uint32_t)g_rec.death_tick.size(), 1u, "T-DEATH-CONTINUES: death_tick NOT called again on iteration 2");
        ck_eq((uint32_t)own.unit_at(UNIT_IDX).status, 0xf7u,
              "T-DEATH-CONTINUES: iteration 2 terminates via the top gate, which DOES clear bit 0x08");
    }

    // ============================================================================================
    // Step 5: attack_cmd_op == 2 (0x0042f3e9-0x0042f42a).
    // ============================================================================================

    // T-FIRE: fire_weapon(unit, cmd_index, attack_gun_toggle); status |= 8. The OR'd bit is
    // observably cleared again by THIS SAME call's own iteration-2 top-gate return (0x0042f8bd) --
    // real behavior, not a test artifact, so the final assert reflects it explicitly.
    {
        tact_fixture fx;
        reset_all();
        tact_unit &u = fx.units[UNIT_IDX];
        seed_neutral_precondition(u);
        u.attack_cmd_op         = 2;
        u.attack_gun_toggle     = 0x2244; // distinctive sentinel
        u.status                = 0x10;   // bit 0x08 initially OFF, some other bit ON
        u.move_state_timer      = 10.0;
        u.cmd_index             = CMD_IDX;
        u.cmd_queue[CMD_IDX].op = 3; // no-op, so only step 5 is under test

        g_clock.queue = {100.0, CLOCK_TERMINATE};

        tact_view  tv  = fx.view();
        tact_store own = fx.store();
        detail::unit_weapons_tick(tv, own, rec_calls(), UNIT_IDX);

        ck_eq((uint32_t)g_rec.total(), 2u, "T-FIRE: cmd_queue_advance + fire_weapon only");
        ck_eq((uint32_t)g_rec.fire_weapon.size(), 1u, "T-FIRE: unit_fire_weapon called once, 0x0042f40e");
        if (g_rec.fire_weapon.size() == 1) {
            const auto &a = g_rec.fire_weapon[0];
            ck_eq((uint32_t)std::get<0>(a), (uint32_t)UNIT_IDX, "T-FIRE: fire_weapon unit arg");
            ck_eq((uint32_t)std::get<1>(a), (uint32_t)CMD_IDX, "T-FIRE: fire_weapon weapon_subindex == cmd_index");
            ck_eq((uint32_t)std::get<2>(a), 0x2244u, "T-FIRE: fire_weapon fire_arg == attack_gun_toggle, 0x0042f401");
        }
        ck_eq((uint32_t)own.unit_at(UNIT_IDX).status, 0x10u,
              "T-FIRE: final status == 0x10 -- 0x08 was OR'd in @0x0042f420 then cleared again by "
              "iteration 2's own top-gate return @0x0042f8bd, all within this one call");
    }

    // ============================================================================================
    // Step 6: cmd_queue[cmd_index].op == 7 -- the immediate FACE/TURN arm (0x0042f430-0x0042f4dd).
    // ============================================================================================

    // T-FACE-ARG0-NONZERO: arg0 != 0 -> arms the immediate record, THEN unconditionally calls
    // cmd_advance.
    {
        tact_fixture fx;
        reset_all();
        tact_unit &u = fx.units[UNIT_IDX];
        seed_neutral_precondition(u);
        u.move_state_timer        = 10.0;
        u.cmd_index               = CMD_IDX;
        u.cmd_queue[CMD_IDX].op   = 7;
        u.cmd_queue[CMD_IDX].arg0 = 10;   // dir24 heading, nonzero
        u.face_interrupt_flag     = 0x77; // sentinel, must become 0
        u.face_cmd_target_dir     = 0x4444;
        u.face_cmd_arg1           = 0x1111;
        u.face_cmd_arg2           = 0x2222;
        u.face_cmd_arg3           = 0x3333;

        g_clock.queue = {100.0, CLOCK_TERMINATE};

        tact_view  tv  = fx.view();
        tact_store own = fx.store();
        detail::unit_weapons_tick(tv, own, rec_calls(), UNIT_IDX);

        ck_eq((uint32_t)g_rec.total(), 2u, "T-FACE-ARG0-NONZERO: cmd_queue_advance + cmd_advance");
        ck_eq((uint32_t)g_rec.cmd_advance.size(), 1u, "T-FACE-ARG0-NONZERO: cmd_advance called once, 0x0042f4d8");
        if (g_rec.cmd_advance.size() == 1) {
            ck_eq((uint32_t)g_rec.cmd_advance[0].first, (uint32_t)UNIT_IDX, "T-FACE-ARG0-NONZERO: cmd_advance unit arg");
            ck_eq((uint32_t)g_rec.cmd_advance[0].second, (uint32_t)CMD_IDX, "T-FACE-ARG0-NONZERO: cmd_advance cmd_index arg");
        }
        ck_eq((uint32_t)own.unit_at(UNIT_IDX).face_interrupt_flag, 0u, "T-FACE-ARG0-NONZERO: face_interrupt_flag := 0, 0x0042f469");
        ck_eq((uint32_t)own.unit_at(UNIT_IDX).face_cmd_op, 6u, "T-FACE-ARG0-NONZERO: face_cmd_op := 6, 0x0042f477");
        ck_eq((uint32_t)own.unit_at(UNIT_IDX).face_cmd_target_dir, 10u,
              "T-FACE-ARG0-NONZERO: face_cmd_target_dir := cmd_queue[cmd_index].arg0, 0x0042f494-0x0042f49b");
        ck_eq((uint32_t)own.unit_at(UNIT_IDX).face_cmd_arg1, 0u, "T-FACE-ARG0-NONZERO: face_cmd_arg1 := 0, 0x0042f4a9");
        ck_eq((uint32_t)own.unit_at(UNIT_IDX).face_cmd_arg2, 0u, "T-FACE-ARG0-NONZERO: face_cmd_arg2 := 0, 0x0042f4b9");
        ck_eq((uint32_t)own.unit_at(UNIT_IDX).face_cmd_arg3, 0u, "T-FACE-ARG0-NONZERO: face_cmd_arg3 := 0, 0x0042f4c9");
    }

    // T-FACE-ARG0-ZERO: arg0 == 0 -> the record is NOT armed (all five face_* fields survive
    // untouched), but cmd_advance is STILL called unconditionally.
    {
        tact_fixture fx;
        reset_all();
        tact_unit &u = fx.units[UNIT_IDX];
        seed_neutral_precondition(u);
        u.move_state_timer        = 10.0;
        u.cmd_index               = CMD_IDX;
        u.cmd_queue[CMD_IDX].op   = 7;
        u.cmd_queue[CMD_IDX].arg0 = 0;
        u.face_interrupt_flag     = 0x77;
        u.face_cmd_target_dir     = 0x4444;
        u.face_cmd_arg1           = 0x1111;
        u.face_cmd_arg2           = 0x2222;
        u.face_cmd_arg3           = 0x3333;

        g_clock.queue = {100.0, CLOCK_TERMINATE};

        tact_view  tv  = fx.view();
        tact_store own = fx.store();
        detail::unit_weapons_tick(tv, own, rec_calls(), UNIT_IDX);

        ck_eq((uint32_t)g_rec.total(), 2u, "T-FACE-ARG0-ZERO: cmd_queue_advance + cmd_advance (still called), 0x0042f460/0x0042f4d8");
        ck_eq((uint32_t)g_rec.cmd_advance.size(), 1u, "T-FACE-ARG0-ZERO: cmd_advance called even though arg0==0");
        ck_eq((uint32_t)own.unit_at(UNIT_IDX).face_interrupt_flag, 0x77u, "T-FACE-ARG0-ZERO: face_interrupt_flag UNTOUCHED");
        ck_eq((uint32_t)own.unit_at(UNIT_IDX).face_cmd_target_dir, 0x4444u, "T-FACE-ARG0-ZERO: face_cmd_target_dir UNTOUCHED");
        ck_eq((uint32_t)own.unit_at(UNIT_IDX).face_cmd_arg1, 0x1111u, "T-FACE-ARG0-ZERO: face_cmd_arg1 UNTOUCHED");
        ck_eq((uint32_t)own.unit_at(UNIT_IDX).face_cmd_arg2, 0x2222u, "T-FACE-ARG0-ZERO: face_cmd_arg2 UNTOUCHED");
        ck_eq((uint32_t)own.unit_at(UNIT_IDX).face_cmd_arg3, 0x3333u, "T-FACE-ARG0-ZERO: face_cmd_arg3 UNTOUCHED");
    }

    // ============================================================================================
    // Step 7: face_cmd_op == 6 && progress == 0 -> rotate_tick + continue (0x0042f4dd-0x0042f509).
    // ============================================================================================
    {
        tact_fixture fx;
        reset_all();
        tact_unit &u = fx.units[UNIT_IDX];
        seed_neutral_precondition(u);
        u.face_cmd_op           = 6;
        u.progress              = 0;
        u.move_state_timer      = 10.0;
        u.cmd_index             = CMD_IDX;
        u.cmd_queue[CMD_IDX].op = 3; // not 7 -- keep step 6 out of this case

        g_clock.queue = {100.0, CLOCK_TERMINATE};

        tact_view  tv  = fx.view();
        tact_store own = fx.store();
        detail::unit_weapons_tick(tv, own, rec_calls(), UNIT_IDX);

        ck_eq((uint32_t)g_rec.total(), 2u, "T-ROTATE: cmd_queue_advance + rotate_tick only");
        ck_eq((uint32_t)g_rec.rotate_tick.size(), 1u, "T-ROTATE: unit_rotate_tick called once, 0x0042f504");
        if (g_rec.rotate_tick.size() == 1)
            ck_eq((uint32_t)g_rec.rotate_tick[0], (uint32_t)UNIT_IDX, "T-ROTATE: rotate_tick unit arg");
    }

    // ============================================================================================
    // Steps 8/9: status bits 0x40 / 0x20 -- both `continue`, no callee dispatch (0x0042f50e-0x0042f58d).
    // ============================================================================================

    // T-STATUS-0x40: overwrites move_state_timer unconditionally with a FRESH time read (call 2/7).
    {
        tact_fixture fx;
        reset_all();
        tact_unit &u = fx.units[UNIT_IDX];
        seed_neutral_precondition(u);
        u.status                = 0x42; // bit 0x40 set
        u.move_state_timer      = 10.0;
        u.cmd_index             = CMD_IDX;
        u.cmd_queue[CMD_IDX].op = 3;

        constexpr double NEW_TIMER = 321.5;
        g_clock.queue              = {100.0, NEW_TIMER, CLOCK_TERMINATE};

        tact_view  tv  = fx.view();
        tact_store own = fx.store();
        detail::unit_weapons_tick(tv, own, rec_calls(), UNIT_IDX);

        ck_eq((uint32_t)g_rec.total(), 1u, "T-STATUS-0x40: only cmd_queue_advance fires -- no callee dispatch");
        ck_eq_d(own.unit_at(UNIT_IDX).move_state_timer, NEW_TIMER,
                "T-STATUS-0x40: move_state_timer := time_GetCurrentTime() unconditionally, 0x0042f521-0x0042f52d");
    }

    // T-STATUS-0x20: requires progress==0 too; status := (status & 0xdf) | 0x40, THEN
    // move_state_timer := a fresh read.
    {
        tact_fixture fx;
        reset_all();
        tact_unit &u = fx.units[UNIT_IDX];
        seed_neutral_precondition(u);
        u.status                = 0x22; // bits 0x02|0x20 set (0x40 off)
        u.progress              = 0;    // required for this arm
        u.move_state_timer      = 10.0;
        u.cmd_index             = CMD_IDX;
        u.cmd_queue[CMD_IDX].op = 3;

        constexpr double NEW_TIMER = 654.25;
        g_clock.queue              = {100.0, NEW_TIMER, CLOCK_TERMINATE};

        tact_view  tv  = fx.view();
        tact_store own = fx.store();
        detail::unit_weapons_tick(tv, own, rec_calls(), UNIT_IDX);

        ck_eq((uint32_t)g_rec.total(), 1u, "T-STATUS-0x20: only cmd_queue_advance fires -- no callee dispatch");
        ck_eq((uint32_t)own.unit_at(UNIT_IDX).status, 0x42u,
              "T-STATUS-0x20: status := (status & 0xdf) | 0x40 == (0x22 & 0xdf) | 0x40 == 0x42, 0x0042f562-0x0042f575");
        ck_eq_d(own.unit_at(UNIT_IDX).move_state_timer, NEW_TIMER,
                "T-STATUS-0x20: move_state_timer := time_GetCurrentTime(), 0x0042f57b-0x0042f587");
    }

    // ============================================================================================
    // Step 10: the deferred-WAIT gate (0x0042f592-0x0042f5d6). FCOMP+JC -- pinned STRICT
    // (now < cmd_wait_until_time -> still waiting) via bit-adjacent doubles, the opposite sense
    // from the top-of-loop gate above.
    // ============================================================================================

    // T-WAIT-ELAPSED-EQ: the wait-check read == cmd_wait_until_time EXACTLY -> NOT "still
    // waiting" (JC requires strictly below) -> falls through to the dispatch in the SAME pass.
    {
        tact_fixture fx;
        reset_all();
        tact_unit &u = fx.units[UNIT_IDX];
        seed_neutral_precondition(u);
        constexpr double W      = 50.0;
        u.cmd_wait_until_time   = W;
        u.move_state_timer      = 10.0;
        u.cmd_index             = CMD_IDX;
        u.cmd_queue[CMD_IDX].op = 3; // no-op, so only the gate itself is under test

        g_clock.queue = {100.0, W, CLOCK_TERMINATE}; // gate-pass, wait-check(==W), iter-2 terminate

        tact_view  tv  = fx.view();
        tact_store own = fx.store();
        detail::unit_weapons_tick(tv, own, rec_calls(), UNIT_IDX);

        ck_eq((uint32_t)g_clock.calls, 3u, "T-WAIT-ELAPSED-EQ: gate-pass + wait-check + iter-2 terminate");
        ck_eq((uint32_t)g_rec.total(), 1u, "T-WAIT-ELAPSED-EQ: only cmd_queue_advance -- falls through to a no-op dispatch");
        ck_eq_d(own.unit_at(UNIT_IDX).move_state_timer, 10.0,
                "T-WAIT-ELAPSED-EQ: move_state_timer UNCHANGED by the wait-gate itself (op 3 doesn't touch it either) "
                "until iteration 2's own top-gate return");
    }

    // T-WAIT-STILL-WAITING: the wait-check read is the LARGEST double strictly below
    // cmd_wait_until_time -> JC fires -> DIRECT return @0x0042f8da, status NOT cleared, and
    // move_state_timer takes a SECOND, INDEPENDENT time read -- not a reuse of the compare value.
    {
        tact_fixture fx;
        reset_all();
        tact_unit &u = fx.units[UNIT_IDX];
        seed_neutral_precondition(u);
        constexpr double W      = 50.0;
        u.cmd_wait_until_time   = W;
        u.move_state_timer      = 10.0;
        u.status                = 0x08; // FIRE bit only -- must survive untouched
        u.cmd_index             = CMD_IDX;
        u.cmd_queue[CMD_IDX].op = 3;

        const double     just_below = std::nextafter(W, -std::numeric_limits<double>::infinity());
        constexpr double MOVE_TIMER = 42.0; // deliberately distinct from just_below
        g_clock.queue               = {100.0, just_below, MOVE_TIMER};

        tact_view  tv  = fx.view();
        tact_store own = fx.store();
        detail::unit_weapons_tick(tv, own, rec_calls(), UNIT_IDX);

        ck_eq((uint32_t)g_clock.calls, 3u, "T-WAIT-STILL-WAITING: gate-pass + wait-check + the second, independent read");
        ck_eq((uint32_t)g_rec.total(), 1u, "T-WAIT-STILL-WAITING: only cmd_queue_advance -- direct return before the switch");
        ck_eq((uint32_t)own.unit_at(UNIT_IDX).status, 0x08u,
              "T-WAIT-STILL-WAITING: status untouched -- 0x0042f5d1 returns without the &=0xf7 clear");
        ck_eq_d(own.unit_at(UNIT_IDX).cmd_wait_until_time, W, "T-WAIT-STILL-WAITING: cmd_wait_until_time UNCHANGED");
        ck_eq_d(own.unit_at(UNIT_IDX).move_state_timer, MOVE_TIMER,
                "T-WAIT-STILL-WAITING: move_state_timer == the THIRD clock value (a fresh call, 0x0042f5bf), "
                "NOT the second (just_below) -- not a reuse of the compare, 0x0042f5b2 vs 0x0042f5bf");
    }

    // ============================================================================================
    // Steps 11/12: the op dispatch (0x0042f5d6-0x0042f8b8). One case per terminal arm.
    // ============================================================================================

    // T-OP0-OWNER-ZERO: op==0, owner==0 -> status &= 0xfb (clears bit 0x04); move_state_timer :=
    // a fresh read regardless of owner. 0x0042f6ad-0x0042f6ec.
    {
        tact_fixture fx;
        reset_all();
        seed_character_type(fx);
        tact_unit &u = fx.units[UNIT_IDX];
        seed_neutral_precondition(u);
        u.type                  = CHAR_TYPE_IDX;
        u.owner                 = 0;
        u.status                = 0x04; // only bit 0x04 set
        u.move_state_timer      = 10.0;
        u.cmd_index             = CMD_IDX;
        u.cmd_queue[CMD_IDX].op = 0;

        constexpr double NEW_TIMER = 999.0;
        g_clock.queue              = {100.0, NEW_TIMER, CLOCK_TERMINATE};

        tact_view  tv  = fx.view();
        tact_store own = fx.store();
        detail::unit_weapons_tick(tv, own, rec_calls(), UNIT_IDX);

        ck_eq((uint32_t)g_rec.total(), 1u, "T-OP0-OWNER-ZERO: only cmd_queue_advance -- op 0 touches no callee");
        ck_eq((uint32_t)own.unit_at(UNIT_IDX).status, 0u, "T-OP0-OWNER-ZERO: status &= 0xfb -> 0x00, 0x0042f6ca-0x0042f6d4");
        ck_eq_d(own.unit_at(UNIT_IDX).move_state_timer, NEW_TIMER, "T-OP0-OWNER-ZERO: move_state_timer := fresh read, 0x0042f6da-0x0042f6e6");
    }

    // T-OP0-OWNER-NONZERO: owner!=0 -> the clear is SKIPPED; move_state_timer update still happens.
    {
        tact_fixture fx;
        reset_all();
        seed_character_type(fx);
        tact_unit &u = fx.units[UNIT_IDX];
        seed_neutral_precondition(u);
        u.type                  = CHAR_TYPE_IDX;
        u.owner                 = 7; // != 0
        u.status                = 0x04;
        u.move_state_timer      = 10.0;
        u.cmd_index             = CMD_IDX;
        u.cmd_queue[CMD_IDX].op = 0;

        constexpr double NEW_TIMER = 888.0;
        g_clock.queue              = {100.0, NEW_TIMER, CLOCK_TERMINATE};

        tact_view  tv  = fx.view();
        tact_store own = fx.store();
        detail::unit_weapons_tick(tv, own, rec_calls(), UNIT_IDX);

        ck_eq((uint32_t)g_rec.total(), 1u, "T-OP0-OWNER-NONZERO: only cmd_queue_advance");
        ck_eq((uint32_t)own.unit_at(UNIT_IDX).status, 0x04u, "T-OP0-OWNER-NONZERO: status UNCHANGED -- clear skipped, 0x0042f6bb (JNZ)");
        ck_eq_d(own.unit_at(UNIT_IDX).move_state_timer, NEW_TIMER, "T-OP0-OWNER-NONZERO: move_state_timer STILL updated regardless of owner");
    }

    // T-OP1-RUN-OFF: op==1, status&4==0 -> dt = character.speed; cmd_wait_until_time cleared to
    // 0.0. 0x0042f760-0x0042f7d1.
    {
        tact_fixture fx;
        reset_all();
        seed_character_type(fx);
        tact_unit &u = fx.units[UNIT_IDX];
        seed_neutral_precondition(u);
        u.type                  = CHAR_TYPE_IDX;
        u.status                = 0x02; // bit 0x04 off
        u.cmd_wait_until_time   = -5.0; // distinctive nonzero, so the := 0.0 clear is observable
        u.move_state_timer      = 10.0;
        u.cmd_index             = CMD_IDX;
        u.cmd_queue[CMD_IDX].op = 1;

        g_clock.queue = {100.0, CLOCK_TERMINATE};

        tact_view  tv  = fx.view();
        tact_store own = fx.store();
        detail::unit_weapons_tick(tv, own, rec_calls(), UNIT_IDX);

        ck_eq((uint32_t)g_rec.total(), 2u, "T-OP1-RUN-OFF: cmd_queue_advance + move_tick");
        ck_eq((uint32_t)g_rec.move_tick.size(), 1u, "T-OP1-RUN-OFF: unit_move_tick called once, 0x0042f7cc");
        if (g_rec.move_tick.size() == 1) {
            const auto &a = g_rec.move_tick[0];
            ck_eq((uint32_t)std::get<0>(a), (uint32_t)UNIT_IDX, "T-OP1-RUN-OFF: move_tick unit arg");
            ck_eq((uint32_t)std::get<1>(a), (uint32_t)CMD_IDX, "T-OP1-RUN-OFF: move_tick cmd_slot arg");
            ck_eq_d(std::get<2>(a), fx.character_types[CHAR_TYPE_IDX].speed, "T-OP1-RUN-OFF: dt == character.speed (2.5), 0x0042f7a7-0x0042f7ba");
        }
        ck_eq_d(own.unit_at(UNIT_IDX).cmd_wait_until_time, 0.0, "T-OP1-RUN-OFF: cmd_wait_until_time := 0.0, 0x0042f767-0x0042f771");
    }

    // T-OP1-RUN-ON: status&4!=0 -> dt = character.speed / character.run_speed.
    {
        tact_fixture fx;
        reset_all();
        seed_character_type(fx);
        tact_unit &u = fx.units[UNIT_IDX];
        seed_neutral_precondition(u);
        u.type                  = CHAR_TYPE_IDX;
        u.status                = 0x06; // bits 0x02|0x04 -- bit 0x04 ON
        u.cmd_wait_until_time   = -5.0;
        u.move_state_timer      = 10.0;
        u.cmd_index             = CMD_IDX;
        u.cmd_queue[CMD_IDX].op = 1;

        g_clock.queue = {100.0, CLOCK_TERMINATE};

        tact_view  tv  = fx.view();
        tact_store own = fx.store();
        detail::unit_weapons_tick(tv, own, rec_calls(), UNIT_IDX);

        ck_eq((uint32_t)g_rec.total(), 2u, "T-OP1-RUN-ON: cmd_queue_advance + move_tick");
        ck_eq((uint32_t)g_rec.move_tick.size(), 1u, "T-OP1-RUN-ON: unit_move_tick called once");
        if (g_rec.move_tick.size() == 1) {
            const double expected = fx.character_types[CHAR_TYPE_IDX].speed / fx.character_types[CHAR_TYPE_IDX].run_speed;
            ck_eq_d(std::get<2>(g_rec.move_tick[0]), expected,
                    "T-OP1-RUN-ON: dt == speed/run_speed == 0.625 exact, 0x0042f792-0x0042f7a2");
        }
    }

    // T-OP4-KNEEL: unit_kneel_tick(unit); move_state_timer += character.kneel_time. 0x0042f7d6-0x0042f7fe.
    {
        tact_fixture fx;
        reset_all();
        seed_character_type(fx);
        tact_unit &u = fx.units[UNIT_IDX];
        seed_neutral_precondition(u);
        u.type                  = CHAR_TYPE_IDX;
        u.move_state_timer      = 100.0;
        u.cmd_index             = CMD_IDX;
        u.cmd_queue[CMD_IDX].op = 4;

        g_clock.queue = {200.0, CLOCK_TERMINATE};

        tact_view  tv  = fx.view();
        tact_store own = fx.store();
        detail::unit_weapons_tick(tv, own, rec_calls(), UNIT_IDX);

        ck_eq((uint32_t)g_rec.total(), 2u, "T-OP4-KNEEL: cmd_queue_advance + kneel_tick");
        ck_eq((uint32_t)g_rec.kneel_tick.size(), 1u, "T-OP4-KNEEL: unit_kneel_tick called once, 0x0042f7dc");
        if (g_rec.kneel_tick.size() == 1)
            ck_eq((uint32_t)g_rec.kneel_tick[0], (uint32_t)UNIT_IDX, "T-OP4-KNEEL: kneel_tick unit arg");
        ck_eq_d(own.unit_at(UNIT_IDX).move_state_timer, 100.0 + fx.character_types[CHAR_TYPE_IDX].kneel_time,
                "T-OP4-KNEEL: move_state_timer += character.kneel_time (1.25), 0x0042f7ec-0x0042f7f8");
    }

    // T-OP5-STAND: unit_stand_tick(unit); move_state_timer += character.kneel_time -- the SAME
    // field as op==4, NOT death_time. death_time is seeded distinct (9.75) so a field mix-up fails.
    // 0x0042f803-0x0042f82b.
    {
        tact_fixture fx;
        reset_all();
        seed_character_type(fx);
        tact_unit &u = fx.units[UNIT_IDX];
        seed_neutral_precondition(u);
        u.type                  = CHAR_TYPE_IDX;
        u.move_state_timer      = 100.0;
        u.cmd_index             = CMD_IDX;
        u.cmd_queue[CMD_IDX].op = 5;

        g_clock.queue = {200.0, CLOCK_TERMINATE};

        tact_view  tv  = fx.view();
        tact_store own = fx.store();
        detail::unit_weapons_tick(tv, own, rec_calls(), UNIT_IDX);

        ck_eq((uint32_t)g_rec.total(), 2u, "T-OP5-STAND: cmd_queue_advance + stand_tick");
        ck_eq((uint32_t)g_rec.stand_tick.size(), 1u, "T-OP5-STAND: unit_stand_tick called once, 0x0042f809");
        if (g_rec.stand_tick.size() == 1)
            ck_eq((uint32_t)g_rec.stand_tick[0], (uint32_t)UNIT_IDX, "T-OP5-STAND: stand_tick unit arg");
        ck_eq_d(own.unit_at(UNIT_IDX).move_state_timer, 100.0 + fx.character_types[CHAR_TYPE_IDX].kneel_time,
                "T-OP5-STAND: move_state_timer += character.kneel_time (1.25) -- NOT death_time (9.75), 0x0042f819-0x0042f825");
    }

    // T-OP8-WAIT: cmd_wait_until_time := now + (double)arg0; THEN cmd_advance. 0x0042f6f1-0x0042f72e.
    {
        tact_fixture fx;
        reset_all();
        seed_character_type(fx);
        tact_unit &u = fx.units[UNIT_IDX];
        seed_neutral_precondition(u);
        u.type                    = CHAR_TYPE_IDX;
        u.move_state_timer        = 50.0;
        u.cmd_index               = CMD_IDX;
        u.cmd_queue[CMD_IDX].op   = 8;
        u.cmd_queue[CMD_IDX].arg0 = 7; // distinct from op (8)

        g_clock.queue = {100.0, 200.0, CLOCK_TERMINATE}; // gate-pass, op8's own read, iter-2 terminate

        tact_view  tv  = fx.view();
        tact_store own = fx.store();
        detail::unit_weapons_tick(tv, own, rec_calls(), UNIT_IDX);

        ck_eq((uint32_t)g_rec.total(), 2u, "T-OP8-WAIT: cmd_queue_advance + cmd_advance");
        ck_eq_d(own.unit_at(UNIT_IDX).cmd_wait_until_time, 207.0,
                "T-OP8-WAIT: cmd_wait_until_time == 200.0 + 7 == 207.0, 0x0042f6fe-0x0042f71d");
        ck_eq((uint32_t)g_rec.cmd_advance.size(), 1u, "T-OP8-WAIT: cmd_advance called once, 0x0042f729");
        if (g_rec.cmd_advance.size() == 1) {
            ck_eq((uint32_t)g_rec.cmd_advance[0].first, (uint32_t)UNIT_IDX, "T-OP8-WAIT: cmd_advance unit arg");
            ck_eq((uint32_t)g_rec.cmd_advance[0].second, (uint32_t)CMD_IDX, "T-OP8-WAIT: cmd_advance cmd_index arg");
        }
    }

    // T-OP9-MINES-DISABLED: MINES_ENABLED==0 -> progress:=0; set_anim_state(unit,0); THEN a FRESH
    // re-read of u.cmd_index (NOT the cached local) feeds cmd_advance, 0x0042f863-0x0042f88c. The
    // set_anim_state mock MUTATES u.cmd_index mid-call so a translation that reused the cached
    // local instead of re-reading the field receives the STALE value and fails here.
    {
        tact_fixture fx;
        reset_all();
        seed_character_type(fx);
        tact_unit &u = fx.units[UNIT_IDX];
        seed_neutral_precondition(u);
        u.type                    = CHAR_TYPE_IDX;
        u.move_state_timer        = 50.0;
        u.cmd_index               = CMD_IDX;
        u.cmd_queue[CMD_IDX].op   = 9;
        u.cmd_queue[CMD_IDX].arg0 = 0x55; // unused by op==9
        u.progress                = 0x07; // nonzero, so we can observe it become 0
        fx.mines_enabled          = 0;

        constexpr uint8_t FRESH_CMD_INDEX = 77;
        g_mutate_target                   = &u.cmd_index;
        g_mutate_value                    = FRESH_CMD_INDEX;

        g_clock.queue = {100.0, CLOCK_TERMINATE};

        tact_view  tv  = fx.view();
        tact_store own = fx.store();
        detail::unit_weapons_tick(tv, own, rec_calls(), UNIT_IDX);

        ck_eq((uint32_t)g_rec.total(), 3u, "T-OP9-MINES-DISABLED: cmd_queue_advance + set_anim_state + cmd_advance, nothing else");
        ck_eq((uint32_t)own.unit_at(UNIT_IDX).progress, 0u, "T-OP9-MINES-DISABLED: progress := 0, 0x0042f86a");
        ck_eq((uint32_t)g_rec.set_anim_state.size(), 1u, "T-OP9-MINES-DISABLED: set_anim_state called once, 0x0042f876");
        if (g_rec.set_anim_state.size() == 1) {
            ck_eq((uint32_t)g_rec.set_anim_state[0].first, (uint32_t)UNIT_IDX, "T-OP9-MINES-DISABLED: set_anim_state unit arg");
            ck_eq((uint32_t)g_rec.set_anim_state[0].second, 0u, "T-OP9-MINES-DISABLED: set_anim_state state arg is the literal 0");
        }
        ck_eq((uint32_t)g_rec.cmd_advance.size(), 1u, "T-OP9-MINES-DISABLED: cmd_advance called once, 0x0042f88c");
        if (g_rec.cmd_advance.size() == 1) {
            ck_eq((uint32_t)g_rec.cmd_advance[0].first, (uint32_t)UNIT_IDX, "T-OP9-MINES-DISABLED: cmd_advance unit arg");
            ck_eq((uint32_t)g_rec.cmd_advance[0].second, (uint32_t)FRESH_CMD_INDEX,
                  "T-OP9-MINES-DISABLED: cmd_advance received the MUTATED u.cmd_index (77), NOT the cached local (10), 0x0042f882");
        }
        const int32_t anim_pos = order_pos("set_anim_state");
        const int32_t adv_pos  = order_pos("cmd_advance");
        ck(anim_pos >= 0 && adv_pos >= 0 && anim_pos < adv_pos,
           "T-OP9-MINES-DISABLED: set_anim_state fires BEFORE cmd_advance, 0x0042f876 < 0x0042f88c");
    }

    // T-OP9-MINES-ENABLED: MINES_ENABLED!=0 -> unit_mine_arm_tick(unit) ONLY; progress untouched.
    // 0x0042f84d-0x0042f861.
    {
        tact_fixture fx;
        reset_all();
        seed_character_type(fx);
        tact_unit &u = fx.units[UNIT_IDX];
        seed_neutral_precondition(u);
        u.type                  = CHAR_TYPE_IDX;
        u.move_state_timer      = 50.0;
        u.cmd_index             = CMD_IDX;
        u.cmd_queue[CMD_IDX].op = 9;
        u.progress              = 0x07;
        fx.mines_enabled        = 1;

        g_clock.queue = {100.0, CLOCK_TERMINATE};

        tact_view  tv  = fx.view();
        tact_store own = fx.store();
        detail::unit_weapons_tick(tv, own, rec_calls(), UNIT_IDX);

        ck_eq((uint32_t)g_rec.total(), 2u, "T-OP9-MINES-ENABLED: cmd_queue_advance + mine_arm_tick only");
        ck_eq((uint32_t)g_rec.mine_arm_tick.size(), 1u, "T-OP9-MINES-ENABLED: unit_mine_arm_tick called once, 0x0042f85c");
        if (g_rec.mine_arm_tick.size() == 1)
            ck_eq((uint32_t)g_rec.mine_arm_tick[0], (uint32_t)UNIT_IDX, "T-OP9-MINES-ENABLED: mine_arm_tick unit arg");
        ck_eq((uint32_t)g_rec.set_anim_state.size(), 0u, "T-OP9-MINES-ENABLED: set_anim_state NOT called");
        ck_eq((uint32_t)g_rec.cmd_advance.size(), 0u, "T-OP9-MINES-ENABLED: cmd_advance NOT called");
        ck_eq((uint32_t)own.unit_at(UNIT_IDX).progress, 0x07u, "T-OP9-MINES-ENABLED: progress UNTOUCHED on this arm");
    }

    // T-OP0XA-TELEPORT: unit_cmd_teleport_jump_tick(unit, cmd_index). 0x0042f893-0x0042f899.
    {
        tact_fixture fx;
        reset_all();
        seed_character_type(fx);
        tact_unit &u = fx.units[UNIT_IDX];
        seed_neutral_precondition(u);
        u.type                  = CHAR_TYPE_IDX;
        u.move_state_timer      = 50.0;
        u.cmd_index             = CMD_IDX;
        u.cmd_queue[CMD_IDX].op = 0xa;

        g_clock.queue = {100.0, CLOCK_TERMINATE};

        tact_view  tv  = fx.view();
        tact_store own = fx.store();
        detail::unit_weapons_tick(tv, own, rec_calls(), UNIT_IDX);

        ck_eq((uint32_t)g_rec.total(), 2u, "T-OP0XA-TELEPORT: cmd_queue_advance + teleport_jump_tick");
        ck_eq((uint32_t)g_rec.teleport_jump_tick.size(), 1u, "T-OP0XA-TELEPORT: called once, 0x0042f899");
        if (g_rec.teleport_jump_tick.size() == 1) {
            ck_eq((uint32_t)g_rec.teleport_jump_tick[0].first, (uint32_t)UNIT_IDX, "T-OP0XA-TELEPORT: unit arg");
            ck_eq((uint32_t)g_rec.teleport_jump_tick[0].second, (uint32_t)CMD_IDX, "T-OP0XA-TELEPORT: cmd_index arg");
        }
    }

    // T-OP0XB-DEFSTAT: unit_cmd_advance_with_defstat(unit, cmd_index). 0x0042f8a0-0x0042f8a6.
    {
        tact_fixture fx;
        reset_all();
        seed_character_type(fx);
        tact_unit &u = fx.units[UNIT_IDX];
        seed_neutral_precondition(u);
        u.type                  = CHAR_TYPE_IDX;
        u.move_state_timer      = 50.0;
        u.cmd_index             = CMD_IDX;
        u.cmd_queue[CMD_IDX].op = 0xb;

        g_clock.queue = {100.0, CLOCK_TERMINATE};

        tact_view  tv  = fx.view();
        tact_store own = fx.store();
        detail::unit_weapons_tick(tv, own, rec_calls(), UNIT_IDX);

        ck_eq((uint32_t)g_rec.total(), 2u, "T-OP0XB-DEFSTAT: cmd_queue_advance + advance_with_defstat");
        ck_eq((uint32_t)g_rec.advance_with_defstat.size(), 1u, "T-OP0XB-DEFSTAT: called once, 0x0042f8a6");
        if (g_rec.advance_with_defstat.size() == 1) {
            ck_eq((uint32_t)g_rec.advance_with_defstat[0].first, (uint32_t)UNIT_IDX, "T-OP0XB-DEFSTAT: unit arg");
            ck_eq((uint32_t)g_rec.advance_with_defstat[0].second, (uint32_t)CMD_IDX, "T-OP0XB-DEFSTAT: cmd_index arg");
        }
    }

    // T-OP0X1C-STANCE-OFF: unit_cmd_stance_off(unit). 0x0042f840-0x0042f846.
    {
        tact_fixture fx;
        reset_all();
        seed_character_type(fx);
        tact_unit &u = fx.units[UNIT_IDX];
        seed_neutral_precondition(u);
        u.type                  = CHAR_TYPE_IDX;
        u.move_state_timer      = 50.0;
        u.cmd_index             = CMD_IDX;
        u.cmd_queue[CMD_IDX].op = 0x1c;

        g_clock.queue = {100.0, CLOCK_TERMINATE};

        tact_view  tv  = fx.view();
        tact_store own = fx.store();
        detail::unit_weapons_tick(tv, own, rec_calls(), UNIT_IDX);

        ck_eq((uint32_t)g_rec.total(), 2u, "T-OP0X1C-STANCE-OFF: cmd_queue_advance + stance_off");
        ck_eq((uint32_t)g_rec.stance_off.size(), 1u, "T-OP0X1C-STANCE-OFF: called once, 0x0042f846");
        if (g_rec.stance_off.size() == 1)
            ck_eq((uint32_t)g_rec.stance_off[0], (uint32_t)UNIT_IDX, "T-OP0X1C-STANCE-OFF: unit arg");
    }

    // T-OP0X1E-STANCE-ON: unit_cmd_stance_on(unit). 0x0042f830-0x0042f836.
    {
        tact_fixture fx;
        reset_all();
        seed_character_type(fx);
        tact_unit &u = fx.units[UNIT_IDX];
        seed_neutral_precondition(u);
        u.type                  = CHAR_TYPE_IDX;
        u.move_state_timer      = 50.0;
        u.cmd_index             = CMD_IDX;
        u.cmd_queue[CMD_IDX].op = 0x1e;

        g_clock.queue = {100.0, CLOCK_TERMINATE};

        tact_view  tv  = fx.view();
        tact_store own = fx.store();
        detail::unit_weapons_tick(tv, own, rec_calls(), UNIT_IDX);

        ck_eq((uint32_t)g_rec.total(), 2u, "T-OP0X1E-STANCE-ON: cmd_queue_advance + stance_on");
        ck_eq((uint32_t)g_rec.stance_on.size(), 1u, "T-OP0X1E-STANCE-ON: called once, 0x0042f836");
        if (g_rec.stance_on.size() == 1)
            ck_eq((uint32_t)g_rec.stance_on[0], (uint32_t)UNIT_IDX, "T-OP0X1E-STANCE-ON: unit arg");
    }

    // T-OP0X40-RESUBMIT: unit_cmd_queue_resubmit_run(unit, cmd_index); move_state_timer +=
    // character.speed. 0x0042f733-0x0042f75b.
    {
        tact_fixture fx;
        reset_all();
        seed_character_type(fx);
        tact_unit &u = fx.units[UNIT_IDX];
        seed_neutral_precondition(u);
        u.type                  = CHAR_TYPE_IDX;
        u.move_state_timer      = 100.0;
        u.cmd_index             = CMD_IDX;
        u.cmd_queue[CMD_IDX].op = 0x40;

        g_clock.queue = {200.0, CLOCK_TERMINATE};

        tact_view  tv  = fx.view();
        tact_store own = fx.store();
        detail::unit_weapons_tick(tv, own, rec_calls(), UNIT_IDX);

        ck_eq((uint32_t)g_rec.total(), 2u, "T-OP0X40-RESUBMIT: cmd_queue_advance + queue_resubmit_run");
        ck_eq((uint32_t)g_rec.queue_resubmit_run.size(), 1u, "T-OP0X40-RESUBMIT: called once, 0x0042f739");
        if (g_rec.queue_resubmit_run.size() == 1) {
            ck_eq((uint32_t)g_rec.queue_resubmit_run[0].first, (uint32_t)UNIT_IDX, "T-OP0X40-RESUBMIT: unit arg");
            ck_eq((uint32_t)g_rec.queue_resubmit_run[0].second, (uint32_t)CMD_IDX, "T-OP0X40-RESUBMIT: cmd_index arg");
        }
        ck_eq_d(own.unit_at(UNIT_IDX).move_state_timer, 100.0 + fx.character_types[CHAR_TYPE_IDX].speed,
                "T-OP0X40-RESUBMIT: move_state_timer += character.speed (2.5), 0x0042f749-0x0042f755");
    }

    // T-OP0X7F-ADVANCE: unit_cmd_advance(unit, cmd_index). 0x0042f8ad-0x0042f8b3.
    {
        tact_fixture fx;
        reset_all();
        seed_character_type(fx);
        tact_unit &u = fx.units[UNIT_IDX];
        seed_neutral_precondition(u);
        u.type                  = CHAR_TYPE_IDX;
        u.move_state_timer      = 50.0;
        u.cmd_index             = CMD_IDX;
        u.cmd_queue[CMD_IDX].op = 0x7f;

        g_clock.queue = {100.0, CLOCK_TERMINATE};

        tact_view  tv  = fx.view();
        tact_store own = fx.store();
        detail::unit_weapons_tick(tv, own, rec_calls(), UNIT_IDX);

        ck_eq((uint32_t)g_rec.total(), 2u, "T-OP0X7F-ADVANCE: cmd_queue_advance + cmd_advance");
        ck_eq((uint32_t)g_rec.cmd_advance.size(), 1u, "T-OP0X7F-ADVANCE: called once, 0x0042f8b3");
        if (g_rec.cmd_advance.size() == 1) {
            ck_eq((uint32_t)g_rec.cmd_advance[0].first, (uint32_t)UNIT_IDX, "T-OP0X7F-ADVANCE: unit arg");
            ck_eq((uint32_t)g_rec.cmd_advance[0].second, (uint32_t)CMD_IDX, "T-OP0X7F-ADVANCE: cmd_index arg");
        }
    }

    // T-NOOP-GAPS: representative op values from every gap the .cpp's if-chain leaves as "no-op" --
    // {2,3} within op<4, {6,7 alone -- 7 has its own case above} within op<9, [0xc,0x1b], {0x1d} u
    // [0x1f,0x3f], [0x41,0x7e], [0x80,0xffff]. Only cmd_queue_advance should ever fire.
    {
        const std::vector<uint16_t> gap_ops = {2, 6, 0xc, 0x1d, 0x2a, 0x41, 0xffff};
        for (uint16_t gap_op : gap_ops) {
            tact_fixture fx;
            reset_all();
            seed_character_type(fx);
            tact_unit &u = fx.units[UNIT_IDX];
            seed_neutral_precondition(u);
            u.type                  = CHAR_TYPE_IDX;
            u.move_state_timer      = 50.0;
            u.cmd_index             = CMD_IDX;
            u.cmd_queue[CMD_IDX].op = gap_op;

            g_clock.queue = {100.0, CLOCK_TERMINATE};

            tact_view  tv  = fx.view();
            tact_store own = fx.store();
            detail::unit_weapons_tick(tv, own, rec_calls(), UNIT_IDX);

            ck_eq((uint32_t)g_rec.total(), 1u, "T-NOOP-GAPS: op no-op value dispatches nothing beyond cmd_queue_advance");
        }
    }

    // ============================================================================================
    // Item 5: the op-value byte-offset subtlety. cmd_queue is based at tact_unit_record+0x54, each
    // entry is 0xb bytes (interrupt_flag +0, op +1, arg0 +3, arg1 +5, arg2 +7, arg3 +9), so a wrong
    // base or a wrong per-entry field offset reads a NEIGHBOR slot or a NEIGHBOR field. Neighboring
    // slots get DISTINCT, dispatchable ops with DISTINCT arg0s; op==8 (WAIT) is the vehicle because
    // its result folds BOTH the slot index (which op fires) AND the field offset (arg0's value)
    // into one observable double.
    // ============================================================================================
    {
        tact_fixture fx;
        reset_all();
        seed_character_type(fx);
        tact_unit &u = fx.units[UNIT_IDX];
        seed_neutral_precondition(u);
        u.type                        = CHAR_TYPE_IDX;
        u.move_state_timer            = 50.0;
        u.cmd_index                   = CMD_IDX;
        u.cmd_queue[CMD_IDX - 1].op   = 0x40; // resubmit_run -- must NOT fire
        u.cmd_queue[CMD_IDX - 1].arg0 = 0x11;
        u.cmd_queue[CMD_IDX].op       = 8;    // WAIT -- the slot under test
        u.cmd_queue[CMD_IDX].arg0     = 0x22; // 34 decimal, distinct from op(8) and both neighbors
        u.cmd_queue[CMD_IDX + 1].op   = 0x1e; // stance_on -- must NOT fire
        u.cmd_queue[CMD_IDX + 1].arg0 = 0x33;

        g_clock.queue = {100.0, 300.0, CLOCK_TERMINATE};

        tact_view  tv  = fx.view();
        tact_store own = fx.store();
        detail::unit_weapons_tick(tv, own, rec_calls(), UNIT_IDX);

        ck_eq((uint32_t)g_rec.total(), 2u, "T-BYTE-OFFSET: cmd_queue_advance + cmd_advance ONLY -- no resubmit_run, no stance_on");
        ck_eq((uint32_t)g_rec.queue_resubmit_run.size(), 0u, "T-BYTE-OFFSET: neighbor[cmd_index-1]'s op (0x40) did NOT fire");
        ck_eq((uint32_t)g_rec.stance_on.size(), 0u, "T-BYTE-OFFSET: neighbor[cmd_index+1]'s op (0x1e) did NOT fire");
        ck_eq_d(own.unit_at(UNIT_IDX).cmd_wait_until_time, 334.0,
                "T-BYTE-OFFSET: cmd_wait_until_time == 300.0 + 0x22(34) == 334.0 -- proves arg0 was read from "
                "cmd_queue[cmd_index], not a neighbor slot (317/351) nor a shifted field, base 0x826124+cmd_index*0xb");
        ck_eq((uint32_t)g_rec.cmd_advance.size(), 1u, "T-BYTE-OFFSET: cmd_advance called once, 0x0042f729");
        if (g_rec.cmd_advance.size() == 1)
            ck_eq((uint32_t)g_rec.cmd_advance[0].second, (uint32_t)CMD_IDX, "T-BYTE-OFFSET: cmd_advance cmd_index == CMD_IDX, not a neighbor");
    }
}

} // namespace mh::tact::test
