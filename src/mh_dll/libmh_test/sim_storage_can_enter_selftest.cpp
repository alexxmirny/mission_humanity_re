//
// sim_storage_can_enter_selftest.cpp -- `simtest` oracle for llm_strat_storage_can_enter
// @0x0048a808 (sim/sim_storage_can.h/.cpp, RI-SIM / SIM1-G3).
//
// ARMABLE (arm_ready:true) but NOT COVERED: FOUR consecutive all-AI soaks (15000 steps @1000%,
// default scenario x3 plus a 90-building save-seeded scenario) reached 0 calls for this site. This
// offline oracle is this slice's evidence in the meantime; a future rig run with a targeted save can
// still add T1 coverage on top.
//
// EXPECTED BEHAVIOUR HAND-DERIVED FROM tmp/decomp_sim/llm_strat_storage_can_enter_0048a808.asm (not
// the .cpp) -- see sim_storage_can.h's own header banner for the full step-by-step derivation; this
// file's case comments cite the specific steps each covers.
//
#include "sim/sim_storage_can.h"

#include <cstdint>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

constexpr uint16_t PLAYER       = 1;
constexpr int32_t  STORAGE_SLOT = 4;
constexpr int32_t  B_INDEX      = 6;
constexpr uint32_t UNIT_INDEX   = 0;

struct call_log {
    enum kind_t { W_SPRINTF,
                  PRINT_MSG,
                  SHUTTLE_BIND };
    struct ev {
        kind_t         kind;
        const wchar_t *fmt = nullptr, *a0 = nullptr, *a1 = nullptr; // W_SPRINTF
        uint32_t       bind_player   = 0;
        int32_t        bind_building = 0; // SHUTTLE_BIND
    };
    std::vector<ev> events;
    void            reset() { events.clear(); }
    int32_t         count(kind_t k) const {
        int32_t n = 0;
        for (auto &e : events)
            if (e.kind == k) ++n;
        return n;
    }
};
call_log g_log;
int32_t  g_bind_result = 0;

const storage_can_enter_calls &recording_calls() {
    static const storage_can_enter_calls c = {
        [](void *dst, const wchar_t *format, const wchar_t *a0, const wchar_t *a1) -> int32_t {
            (void)dst;
            call_log::ev e;
            e.kind = call_log::W_SPRINTF;
            e.fmt  = format;
            e.a0   = a0;
            e.a1   = a1;
            g_log.events.push_back(e);
            return 0;
        },
        [](void *text) -> uint32_t {
            (void)text;
            g_log.events.push_back(call_log::ev{call_log::PRINT_MSG});
            return 0;
        },
        [](uint32_t player, int32_t building_index) -> int32_t {
            call_log::ev e;
            e.kind          = call_log::SHUTTLE_BIND;
            e.bind_player   = player;
            e.bind_building = building_index;
            g_log.events.push_back(e);
            return g_bind_result;
        },
    };
    return c;
}

// ---- Common-final-gate cases (bldg_type != A_SHUTTLE/_H_SHUTTLE skips straight here). ----
struct GateSeed {
    uint8_t bldg_type       = 5; // anything != A_SHUTTLE(0xd)/H_SHUTTLE(0x21)
    int32_t soldier_count   = 1; // > 0 arm by default
    int32_t human           = 0;
    int32_t occupancy       = 0;
    int32_t door_mutex_unit = 0;
    int16_t online_state    = 2;
    uint8_t built_flags     = 3; // BUILT_FLAGS_OPERATIONAL
};

int32_t run_gate(sim_fixture &fx, const GateSeed &s) {
    fx.reset();
    unit_storage &st   = fx.storage[PLAYER * STORAGE_PER_PLAYER + STORAGE_SLOT];
    st.b_index         = B_INDEX;
    st.door_mutex_unit = s.door_mutex_unit;
    st.occupancy       = s.occupancy;

    building &b    = fx.b(PLAYER, B_INDEX);
    b.building_id  = B_INDEX;
    b.online_state = s.online_state;
    b.built_flags  = s.built_flags;

    fx.cfg_buildings[B_INDEX].type = s.bldg_type;

    fx.u(PLAYER, UNIT_INDEX).unit_proto_id = 7;
    fx.cfg_units[7].soldier_count          = s.soldier_count;
    fx.cfg_units[7].human                  = s.human;

    sim_view  v   = fx.view();
    sim_store own = fx.store();
    g_log.reset();
    return detail::storage_can_enter(v, own, recording_calls(), PLAYER, UNIT_INDEX,
                                     (uint32_t)STORAGE_SLOT);
}

} // namespace

// ---- G1: ordinary building, soldier_count>0, everything open -> allowed.
void test_gate_soldier_arm_open_allowed() {
    sim_fixture fx;
    GateSeed    s;
    ck_eq((uint32_t)run_gate(fx, s), 1u, "G1: soldier_count>0, ordinary building, open -> allowed");
}

// ---- G2: door_mutex_unit!=0 -> rejected first, in both the soldier_count>0 and <=0 arms.
void test_gate_door_mutex_rejects_both_arms() {
    sim_fixture fx;
    GateSeed    s;
    s.door_mutex_unit = 9;
    ck_eq((uint32_t)run_gate(fx, s), 0u, "G2a: door_mutex_unit!=0 (soldier_count>0 arm) -> rejected");
    GateSeed s2;
    s2.soldier_count   = 0;
    s2.door_mutex_unit = 9;
    ck_eq((uint32_t)run_gate(fx, s2), 0u, "G2b: door_mutex_unit!=0 (soldier_count<=0 arm) -> rejected");
}

// ---- G3: soldier_count>0 arm's capacity boundary is occupancy+human vs 50 (STORAGE_OCCUPANCY_CAP),
// strict `>`, not `>=`.
void test_gate_soldier_arm_occupancy_plus_human_boundary() {
    sim_fixture fx;
    GateSeed    s;
    s.human     = 20;
    s.occupancy = 30; // occupancy+human == 50 == CAP -> still allowed (not > cap)
    ck_eq((uint32_t)run_gate(fx, s), 1u, "G3a: occupancy+human==CAP -> allowed (strict >)");
    s.occupancy = 31; // occupancy+human == 51 > CAP -> rejected
    ck_eq((uint32_t)run_gate(fx, s), 0u, "G3b: occupancy+human>CAP -> rejected");
}

// ---- G4: soldier_count<=0 arm's capacity boundary is occupancy vs 50, strict `>=` (no .human added).
void test_gate_non_soldier_arm_occupancy_boundary() {
    sim_fixture fx;
    GateSeed    s;
    s.soldier_count = 0;
    s.occupancy     = 49;
    ck_eq((uint32_t)run_gate(fx, s), 1u, "G4a: soldier_count<=0, occupancy==CAP-1 -> allowed");
    s.occupancy = 50;
    ck_eq((uint32_t)run_gate(fx, s), 0u, "G4b: soldier_count<=0, occupancy==CAP (strict >=) -> rejected");
}

// ---- G5: online_state!=2 -> rejected at the final gate, even though door/occupancy pass.
void test_gate_online_state_must_be_2() {
    sim_fixture fx;
    GateSeed    s;
    s.online_state = 1;
    ck_eq((uint32_t)run_gate(fx, s), 0u, "G5: online_state!=2 -> rejected");
}

// ---- G6: built_flags!=OPERATIONAL(3) -> rejected at the FINAL gate.
void test_gate_built_flags_must_be_operational() {
    sim_fixture fx;
    GateSeed    s;
    s.built_flags = 1;
    ck_eq((uint32_t)run_gate(fx, s), 0u, "G6: built_flags(1)!=OPERATIONAL(3) -> rejected");
}

namespace {

// ---- Shuttle-path setup: bldg_type == A_SHUTTLE, everything past the capacity gate wide open so a
// case only needs to seed what it is testing.
struct ShuttleSeed {
    int32_t  crew_offset            = 0;  // 0 => capacity_slot == proto
    uint8_t  remaining              = 5;  // cfg_buildings[b].unit_capacity[capacity_slot]
    uint16_t player_side            = 99; // != PLAYER by default (message gate closed)
    uint16_t unit_state             = 0;  // != ENTER_STORAGE_BEGIN(0x24) by default
    int32_t  docked_count           = 0;
    int32_t  entering_soldier_count = 0; // this unit's own cfg soldier_count
    uint32_t entering_soldier_type  = 0;
};

int32_t run_shuttle(sim_fixture &fx, const ShuttleSeed &s) {
    fx.reset();
    fx.player_side = (int16_t)s.player_side;

    unit_storage &st   = fx.storage[PLAYER * STORAGE_PER_PLAYER + STORAGE_SLOT];
    st.b_index         = B_INDEX;
    st.docked_count    = s.docked_count;
    st.door_mutex_unit = 0;
    st.occupancy       = 0;

    building &b    = fx.b(PLAYER, B_INDEX);
    b.building_id  = B_INDEX;
    b.online_state = 2;
    b.built_flags  = 3; // BUILT_FLAGS_OPERATIONAL
    b.shuttle_slot = 1; // != 0 -> bind path not entered unless a case overrides it

    fx.cfg_buildings[B_INDEX].type = 0x0d; // BUILDING_TYPE_A_SHUTTLE
    // capacity_slot = proto - crew_offset; keep it in [0,100). proto=50 leaves headroom either way.
    const int32_t proto                    = 50;
    fx.u(PLAYER, UNIT_INDEX).unit_proto_id = (uint16_t)proto;
    fx.u(PLAYER, UNIT_INDEX).state         = s.unit_state;
    fx.cfg_units[proto].soldier_count      = s.entering_soldier_count;
    fx.cfg_units[proto].soldier_type       = s.entering_soldier_type;

    const int32_t capacity_slot                            = proto - s.crew_offset;
    fx.cfg_buildings[B_INDEX].unit_capacity[capacity_slot] = s.remaining;
    fx.cfg_units[capacity_slot].name                       = 450; // DISTINCT from proto's name (400)
    // Set the RAW proto's name LAST: when crew_offset==0 (the common case), capacity_slot == proto,
    // so this assignment must win over the one above -- cu.name (the type-rejected message's arg)
    // is always the RAW proto's name (400), never the adjusted one, regardless of that collision.
    fx.cfg_units[proto].name = 400; // text_ptrs index for the RAW proto's name

    // cfg_units[7].soldier_count>0 with crew_offset = soldier_count-1: only meaningful when a case
    // sets entering_soldier_count>1; keep the two independent by NOT deriving crew_offset here (the
    // caller passes it explicitly), matching the .cpp's own `(soldier_count==0)?0:(soldier_count-1)`
    // exactly -- so a case exercising a non-1 offset must set entering_soldier_count consistently.

    sim_view  v   = fx.view();
    sim_store own = fx.store();
    g_log.reset();
    return detail::storage_can_enter(v, own, recording_calls(), PLAYER, UNIT_INDEX,
                                     (uint32_t)STORAGE_SLOT);
}

} // namespace

// ---- S1: remaining==0, player is NOT the local player -> rejected, NO print calls at all (the
// message gate needs player==PlayerSide).
void test_shuttle_remaining_zero_foreign_player_no_print() {
    sim_fixture fx;
    ShuttleSeed s;
    s.remaining   = 0;
    s.player_side = 99; // != PLAYER (1)
    ck_eq((uint32_t)run_shuttle(fx, s), 0u, "S1: remaining==0 -> rejected");
    ck_eq((uint32_t)g_log.count(call_log::W_SPRINTF), 0u, "S1: foreign player -> no w_sprintf");
    ck_eq((uint32_t)g_log.count(call_log::PRINT_MSG), 0u, "S1: foreign player -> no print");
}

// ---- S2: remaining==0, local player AND state==ENTER_STORAGE_BEGIN -> rejected AND prints, with the
// reimpl-verify-fixed argument order: reason (TEXT_ID_STORAGE_TYPE_REJECTED, text_ptrs[135]) FIRST,
// name (cu.name's text, text_ptrs[400]) SECOND -- catches the swap the translation originally had.
void test_shuttle_remaining_zero_local_player_prints_reason_then_name() {
    sim_fixture          fx;
    static const wchar_t REASON[] = L"type-reason";
    static const wchar_t NAME[]   = L"type-name";
    ShuttleSeed          s;
    s.remaining   = 0;
    s.player_side = PLAYER;
    s.unit_state  = 0x24; // ENTER_STORAGE_BEGIN

    // run_shuttle() calls fx.reset() internally -- seed text_ptrs AFTER it, or reset() wipes them.
    const int32_t result = run_shuttle(fx, s);
    fx.text_ptrs[135]    = REASON; // TEXT_ID_STORAGE_TYPE_REJECTED
    fx.text_ptrs[400]    = NAME;   // cfg_units[proto].name, set by run_shuttle to 400
    // The print call already happened inside run_shuttle() (before text_ptrs were seeded), so
    // RE-RUN with the same fixture state now that the strings are in place.
    sim_view  v   = fx.view();
    sim_store own = fx.store();
    g_log.reset();
    const int32_t result2 = detail::storage_can_enter(v, own, recording_calls(), PLAYER, UNIT_INDEX,
                                                      (uint32_t)STORAGE_SLOT);
    ck_eq((uint32_t)result, 0u, "S2: rejected (first pass, strings not yet seeded)");
    ck_eq((uint32_t)result2, 0u, "S2: rejected (re-run with strings seeded)");
    ck_eq((uint32_t)g_log.count(call_log::W_SPRINTF), 1u, "S2: local player + ENTER_STORAGE_BEGIN -> prints");
    ck_eq((uint32_t)g_log.count(call_log::PRINT_MSG), 1u, "S2: and the message is sent");
    const call_log::ev &e = g_log.events[0];
    ck(e.a0 == REASON, "S2 FINDING: w_sprintf's FIRST %s arg is the REASON (not swapped with name)");
    ck(e.a1 == NAME, "S2 FINDING: w_sprintf's SECOND %s arg is the raw proto's NAME");
}

// ---- S3: remaining==0, local player but state!=ENTER_STORAGE_BEGIN -> rejected, no print (the
// state half of the message gate).
void test_shuttle_remaining_zero_local_player_wrong_state_no_print() {
    sim_fixture fx;
    ShuttleSeed s;
    s.remaining   = 0;
    s.player_side = PLAYER;
    s.unit_state  = 0x1f; // PARKED, not ENTER_STORAGE_BEGIN
    ck_eq((uint32_t)run_shuttle(fx, s), 0u, "S3: rejected");
    ck_eq((uint32_t)g_log.count(call_log::W_SPRINTF), 0u, "S3: wrong state -> no print despite local player");
}

// ---- S4: docked_units subtraction, entering unit IS crewed (soldier_count!=0) -- only docked units
// sharing the entering unit's soldier_type subtract their OWN soldier_count; a different soldier_type
// is not subtracted at all.
void test_shuttle_docked_subtraction_by_soldier_type_when_crewed() {
    sim_fixture fx;
    ShuttleSeed s;
    s.remaining              = 5;
    s.entering_soldier_count = 1; // crewed -- crew_offset=0 (kept simple; capacity_slot==proto==50)
    s.entering_soldier_type  = 2;
    s.docked_count           = 2;
    (void)run_shuttle(fx, s); // establishes fx state (proto=50 for the entering unit); result unused
    // Now set the docked roster AFTER run_shuttle's reset() -- so re-run with the roster in place.
    fx.storage[PLAYER * STORAGE_PER_PLAYER + STORAGE_SLOT].docked_units[0] = 10; // matching type
    fx.storage[PLAYER * STORAGE_PER_PLAYER + STORAGE_SLOT].docked_units[1] = 11; // different type
    fx.u(PLAYER, 10).unit_proto_id                                         = 60;
    fx.cfg_units[60].soldier_type                                          = 2; // matches entering (2) -> subtract ITS OWN soldier_count
    fx.cfg_units[60].soldier_count                                         = 3;
    fx.u(PLAYER, 11).unit_proto_id                                         = 61;
    fx.cfg_units[61].soldier_type                                          = 9;   // does NOT match -> not subtracted
    fx.cfg_units[61].soldier_count                                         = 100; // huge, to prove it is ignored

    sim_view  v   = fx.view();
    sim_store own = fx.store();
    g_log.reset();
    const int32_t result = detail::storage_can_enter(v, own, recording_calls(), PLAYER, UNIT_INDEX,
                                                     (uint32_t)STORAGE_SLOT);
    // remaining(5) - matching docked's soldier_count(3) = 2 >= entering soldier_count(1) -> allowed.
    // If the mismatched entry (soldier_count 100) were wrongly subtracted too, this would reject.
    ck_eq((uint32_t)result, 1u,
          "S4: only the soldier_type-matching docked unit's soldier_count is subtracted (5-3=2>=1)");
}

// ---- S5: docked_units subtraction, entering unit is NOT crewed (soldier_count==0) -- docked units
// sharing the entering unit's PROTO subtract exactly 1 slot each; a different proto is ignored.
void test_shuttle_docked_subtraction_by_proto_when_not_crewed() {
    sim_fixture fx;
    ShuttleSeed s;
    s.remaining              = 2;
    s.entering_soldier_count = 0; // not crewed
    s.docked_count           = 2;
    (void)run_shuttle(fx, s); // seed the common fixture state (proto=50 for the entering unit)

    unit_storage &st               = fx.storage[PLAYER * STORAGE_PER_PLAYER + STORAGE_SLOT];
    st.docked_units[0]             = 10; // same proto as the entering unit (50) -> subtract 1
    st.docked_units[1]             = 11; // different proto -> not subtracted
    fx.u(PLAYER, 10).unit_proto_id = 50;
    fx.u(PLAYER, 11).unit_proto_id = 51;

    sim_view  v   = fx.view();
    sim_store own = fx.store();
    g_log.reset();
    const int32_t result = detail::storage_can_enter(v, own, recording_calls(), PLAYER, UNIT_INDEX,
                                                     (uint32_t)STORAGE_SLOT);
    // remaining(2) - 1 (only the same-proto docked unit) = 1; storage_full_should_print(1, 0):
    // remaining(1) >= soldier_count(0) -> accept (0 not printed as full).
    ck_eq((uint32_t)result, 1u, "S5: only the proto-matching docked unit subtracts 1 (2-1=1>=0)");
}

// ---- S6: the capacity-full print gate uses the ADJUSTED capacity_slot's name, NOT the raw proto's --
// distinct text_ptrs indices prove which one the call actually used. Also the priority-boundary case:
// remaining<=0 && soldier_count==0 -> reject+print (checked FIRST, before the >= compare).
void test_shuttle_capacity_full_prints_adjusted_name() {
    sim_fixture          fx;
    static const wchar_t REASON[]   = L"cap-reason";
    static const wchar_t ADJ_NAME[] = L"adjusted-name";
    static const wchar_t RAW_NAME[] = L"raw-proto-name";
    ShuttleSeed          s;
    s.remaining   = 1;            // pre-subtraction capacity at capacity_slot (proto - crew_offset)
    s.crew_offset = 1;            // MUST match cu.soldier_count-1 below (2-1=1), since the REAL
                                  // function derives its own crew_offset from cu.soldier_count
                                  // independently of this seed -- this field only controls WHERE
                                  // run_shuttle plants `remaining`/the adjusted name.
    s.entering_soldier_count = 2; // -> the real function's crew_offset = 2-1 = 1 -> capacity_slot=49
    s.entering_soldier_type  = 3;
    s.player_side            = PLAYER;
    s.unit_state             = 0x24; // ENTER_STORAGE_BEGIN
    s.docked_count           = 1;
    (void)run_shuttle(fx, s); // establishes proto=50, capacity_slot=49 (name 450)

    fx.text_ptrs[802] = REASON;   // TEXT_ID_STORAGE_CAPACITY_FULL
    fx.text_ptrs[450] = ADJ_NAME; // cfg_units[capacity_slot=49].name, set by run_shuttle
    fx.text_ptrs[400] = RAW_NAME; // cfg_units[proto=50].name -- must NOT be what gets printed

    // The docked unit uses a DIFFERENT proto (55) from the entering unit's own (50), so seeding its
    // cfg entry cannot accidentally clobber cu.soldier_count (which the real function re-reads for
    // crew_offset) -- only soldier_type needs to match for the soldier_count!=0 subtraction arm.
    unit_storage &st               = fx.storage[PLAYER * STORAGE_PER_PLAYER + STORAGE_SLOT];
    st.docked_units[0]             = 10;
    fx.u(PLAYER, 10).unit_proto_id = 55;
    fx.cfg_units[55].soldier_type  = 3; // matches entering (3) -> subtract ITS OWN soldier_count
    fx.cfg_units[55].soldier_count = 1; // remaining(1) - 1 == 0 -> should_print(0, 2)==true (reject)

    sim_view  v   = fx.view();
    sim_store own = fx.store();
    g_log.reset();
    const int32_t result = detail::storage_can_enter(v, own, recording_calls(), PLAYER, UNIT_INDEX,
                                                     (uint32_t)STORAGE_SLOT);
    ck_eq((uint32_t)result, 0u, "S6: remaining walked down to 0 < soldier_count(2) -> rejected");
    ck_eq((uint32_t)g_log.count(call_log::W_SPRINTF), 1u, "S6: local player + ENTER_STORAGE_BEGIN -> prints");
    const call_log::ev &e = g_log.events[0];
    ck(e.a0 == REASON, "S6: reason arg is the capacity-full reason (802)");
    ck(e.a1 == ADJ_NAME,
       "S6 FINDING: name arg is cfg_units[ADJUSTED capacity_slot].name, not the raw proto's name");
}

// ---- S7: shuttle_slot==0 -> shuttle_slot_bind_default is called; bind_result==-1 rejects the whole
// entry even though capacity/final-gate would otherwise pass.
void test_shuttle_slot_zero_binds_and_minus_one_rejects() {
    sim_fixture fx;
    ShuttleSeed s;
    s.remaining = 10; // plenty, no docked units subtracting
    (void)run_shuttle(fx, s);
    fx.b(PLAYER, B_INDEX).shuttle_slot = 0; // triggers the bind path
    g_bind_result                      = -1;

    sim_view  v   = fx.view();
    sim_store own = fx.store();
    g_log.reset();
    const int32_t result = detail::storage_can_enter(v, own, recording_calls(), PLAYER, UNIT_INDEX,
                                                     (uint32_t)STORAGE_SLOT);
    ck_eq((uint32_t)result, 0u, "S7: bind_result==-1 -> rejected regardless of capacity/final gate");
    ck_eq((uint32_t)g_log.count(call_log::SHUTTLE_BIND), 1u, "S7: shuttle_slot_bind_default was called");
    ck_eq(g_log.events[0].bind_building, B_INDEX, "S7: bind's building_index is st.b_index");
}

// ---- S8: shuttle_slot==0, bind_result!=-1 -> falls through to the common final gate and returns its
// verdict (proving the bind's own return value, once non -1, is NOT itself the function's result).
void test_shuttle_slot_zero_bind_success_falls_through_to_final_gate() {
    sim_fixture fx;
    ShuttleSeed s;
    s.remaining = 10;
    (void)run_shuttle(fx, s);
    fx.b(PLAYER, B_INDEX).shuttle_slot = 0;
    g_bind_result                      = 42; // any non -1 value

    sim_view  v   = fx.view();
    sim_store own = fx.store();
    g_log.reset();
    const int32_t result = detail::storage_can_enter(v, own, recording_calls(), PLAYER, UNIT_INDEX,
                                                     (uint32_t)STORAGE_SLOT);
    ck_eq((uint32_t)result, 1u, "S8: bind succeeds -> common final gate (all open) -> allowed");
    ck_eq((uint32_t)g_log.count(call_log::SHUTTLE_BIND), 1u, "S8: bind was still called");
}

// ---- S9: shuttle_slot!=0 -> the bind call is SKIPPED ENTIRELY (not called at all).
void test_shuttle_slot_nonzero_never_binds() {
    sim_fixture fx;
    ShuttleSeed s;
    s.remaining = 10;
    (void)run_shuttle(fx, s); // run_shuttle's default shuttle_slot is 1 (!=0)

    sim_view  v   = fx.view();
    sim_store own = fx.store();
    g_log.reset();
    const int32_t result = detail::storage_can_enter(v, own, recording_calls(), PLAYER, UNIT_INDEX,
                                                     (uint32_t)STORAGE_SLOT);
    ck_eq((uint32_t)result, 1u, "S9: shuttle_slot!=0 -> straight to the final gate -> allowed");
    ck_eq((uint32_t)g_log.count(call_log::SHUTTLE_BIND), 0u,
          "S9: shuttle_slot_bind_default is NEVER called when shuttle_slot!=0");
}

void run_storage_can_enter_tests() {
    test_gate_soldier_arm_open_allowed();
    test_gate_door_mutex_rejects_both_arms();
    test_gate_soldier_arm_occupancy_plus_human_boundary();
    test_gate_non_soldier_arm_occupancy_boundary();
    test_gate_online_state_must_be_2();
    test_gate_built_flags_must_be_operational();
    test_shuttle_remaining_zero_foreign_player_no_print();
    test_shuttle_remaining_zero_local_player_prints_reason_then_name();
    test_shuttle_remaining_zero_local_player_wrong_state_no_print();
    test_shuttle_docked_subtraction_by_soldier_type_when_crewed();
    test_shuttle_docked_subtraction_by_proto_when_not_crewed();
    test_shuttle_capacity_full_prints_adjusted_name();
    test_shuttle_slot_zero_binds_and_minus_one_rejects();
    test_shuttle_slot_zero_bind_success_falls_through_to_final_gate();
    test_shuttle_slot_nonzero_never_binds();
}

} // namespace mh::sim::test
